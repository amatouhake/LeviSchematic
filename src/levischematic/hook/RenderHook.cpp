#include "RenderHook.h"

#include "levischematic/app/AppKernel.h"
#include "levischematic/render/BlockActorProjectionRenderer.h"
#include "levischematic/render/ProjectionRenderer.h"
#include "levischematic/schematic/block_actor/BlockActorRenderSchematic.h"
#include "levischematic/schematic/block_actor/SchematicChestRenderer.h"
#include "levischematic/util/PositionUtils.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Memory.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/model/GeometryGroup.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/renderer/actor/ActorResourceDefinitionGroup.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/chunks/AirAndSimpleBlockBits.h"
#include "mc/client/renderer/chunks/RenderChunkBuilder.h"
#include "mc/client/renderer/chunks/RenderChunkGeometry.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/deps/core/container/Blob.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core_graphics/ImageDescription.h"
#include "mc/deps/core_graphics/ImageResource.h"
#include "mc/deps/core_graphics/TextureSetDefinition.h"
#include "mc/deps/core_graphics/TextureSetImageContainer.h"
#include "mc/deps/core_graphics/TextureSetLayerImageMipList.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/ChunkViewSource.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionType.h"


struct BlockQueueEntry {
    BlockPos     pos;
    Block const& blockInfo;
};

namespace levischematic::hook {

using namespace levischematic::render;
using namespace levischematic::util;
namespace block_actor = ::levischematic::schematic::block_actor;

namespace {

bool gRenderHooksRegistered = false;

// Chunk build currently running on this worker thread. Set for the duration of
// RenderChunkBuilder::build; cleared again by the injection so it only runs once per build.
thread_local RenderChunkBuilder*  tl_activeBuilder  = nullptr;
thread_local RenderChunkGeometry* tl_activeGeometry = nullptr;

// Scopes every projection thread-local to one RenderChunkBuilder::build call: the state is
// reset on entry and again when the call leaves (normally or by exception), so a build that
// never reaches the injection point, or later BlockTessellator work on the same worker
// thread, can never observe stale projection state.
struct ProjectionBuildScope {
    ProjectionBuildScope(RenderChunkBuilder& builder, RenderChunkGeometry& geometry) {
        reset();
        tl_activeBuilder  = &builder;
        tl_activeGeometry = &geometry;
    }
    ~ProjectionBuildScope() { reset(); }
    ProjectionBuildScope(ProjectionBuildScope const&)            = delete;
    ProjectionBuildScope& operator=(ProjectionBuildScope const&) = delete;

    static void reset() {
        tl_activeBuilder  = nullptr;
        tl_activeGeometry = nullptr;
        tl_hasProjection  = false;
        tl_currentScene.reset();
    }
};

// Set while ProjectionTextureUploadHook re-uploads a texture itself; the upload overloads may
// forward to each other, so the hook must ignore its own nested call instead of installing
// and removing itself around it (which would also blind it on every other thread).
thread_local bool tl_inProjectionTextureUpload = false;

int resolveBuilderDimensionId(RenderChunkBuilder const& builder) {
    if (builder.mBlockTessellator && builder.mBlockTessellator->mRegion) {
        return static_cast<int>(builder.mBlockTessellator->mRegion->getDimensionId());
    }
    if (builder.mLocalSource && builder.mLocalSource->mDimension) {
        return static_cast<int>(builder.mLocalSource->mDimension->getDimensionId());
    }
    return -1;
}

// Appends the projected blocks of the sub-chunk being built to the builder's blend-layer
// queue. Since game 1.26.32 RenderChunkBuilder::_sortBlocks is inlined into build(), so this
// cannot run after it returns any more. Instead it runs from the first
// checkNeighborBlockIsAirOrSimpleBlock call of a build: that border pass happens after the
// interior blocks have been sorted into the per-layer queues and before the per-layer
// tessellation loop, which only visits layers whose queue is non-empty when it reaches them.
void injectProjectionIntoBuilder(RenderChunkBuilder& builder, RenderChunkGeometry const& renderChunkGeometry) {
    tl_hasProjection = false;
    tl_currentScene.reset();

    if (!app::hasAppKernel()) {
        return;
    }

    BlockSource* region      = builder.mBlockTessellator ? builder.mBlockTessellator->mRegion : nullptr;
    int          dimensionId = resolveBuilderDimensionId(builder);
    if (dimensionId < 0 || !builder.mQueues) {
        return;
    }

    auto& projection = app::getAppKernel().projection();
    (void)projection.flushRefresh(nullptr);

    tl_currentScene = projection.sceneForDimension(dimensionId);
    if (!tl_currentScene || tl_currentScene->empty()) {
        return;
    }

    // Render chunks are 16x16x16 sub-chunks, so a build normally only draws the entries of its
    // own sub-chunk. Sub-chunks above the highest non-air block of a column never get render
    // geometry, so the topmost renderable sub-chunk of the column additionally draws every entry
    // above it (the refresh logic marks that sub-chunk dirty for those entries).
    auto const& renderPosition = renderChunkGeometry.mPosition.get();
    auto        subChunkKey    = subChunkKeyFromWorldPos(renderPosition.x, renderPosition.y, renderPosition.z);
    auto        topY           = topRenderableSubChunkOriginY(region, renderPosition);
    bool        drawsAirAbove  = topY && *topY == renderPosition.y;
    bool        ownsOwnEntries = !topY || renderPosition.y <= *topY;

    // Every entry is owned by exactly one sub-chunk: its own one while that lies at or below
    // the top renderable sub-chunk of the column, otherwise the top renderable sub-chunk. A
    // build of a sub-chunk above that top therefore injects nothing.
    std::vector<ProjEntry const*> entries;
    if (ownsOwnEntries) {
        if (auto it = tl_currentScene->bySubChunk.find(subChunkKey); it != tl_currentScene->bySubChunk.end()) {
            for (auto const& entry : it->second) {
                entries.push_back(&entry);
            }
        }
    }
    if (drawsAirAbove) {
        auto columnIt =
            tl_currentScene->byRenderColumn.find(renderColumnKeyFromWorldPos(renderPosition.x, renderPosition.z));
        if (columnIt != tl_currentScene->byRenderColumn.end()) {
            for (auto const& entry : columnIt->second) {
                if (subChunkOrigin(entry.pos.x, entry.pos.y, entry.pos.z).y > *topY) {
                    entries.push_back(&entry);
                }
            }
        }
    }

    bool hasColorOverride = tl_currentScene->subChunksWithColorOverrides.contains(subChunkKey);
    if (entries.empty() && !hasColorOverride) {
        return;
    }

    tl_hasProjection = true;
    for (auto const* entry : entries) {
        BlockQueueEntry queueEntry{entry->pos, *entry->block};
        builder.mQueues[RENDERLAYER_BLEND].push_back(queueEntry);
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    ProjectionBuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkBuilder,
    &RenderChunkBuilder::build,
    void,
    ::RenderChunkGeometry&                                     renderChunkGeometry,
    bool                                                       transparentLeaves,
    ::BakedBlockLightType                                      lightingType,
    bool                                                       forExport,
    ::mce::framebuilder::FrameLightingModelCapabilities const& lightingModelCapabilities
) {
    ProjectionBuildScope scope(*this, renderChunkGeometry);
    origin(renderChunkGeometry, transparentLeaves, lightingType, forExport, lightingModelCapabilities);
}

// Injection point. On this client the border pass below is the last exported call before the
// per-layer tessellation loop; it is skipped only for GUI block previews
// (RenderChunkBuilder::mGUIRendering), which must not show projections anyway. A build that
// does not reach it simply injects nothing; ProjectionBuildScope clears the state afterwards.

LL_TYPE_STATIC_HOOK(
    ProjectionSortBorderHook,
    ll::memory::HookPriority::Normal,
    RenderChunkBuilder,
    &RenderChunkBuilder::checkNeighborBlockIsAirOrSimpleBlock,
    void,
    ::Block const&           block,
    uint64 const             blockBitsetIndex,
    ::AirAndSimpleBlockBits& airAndSimpleBlocks
) {
    if (tl_activeBuilder) {
        auto* builder    = tl_activeBuilder;
        tl_activeBuilder = nullptr; // inject once per build, on the first border check
        injectProjectionIntoBuilder(*builder, *tl_activeGeometry);
    }
    origin(block, blockBitsetIndex, airAndSimpleBlocks);
}

LL_TYPE_INSTANCE_HOOK(
    ProjectionTessellateHook,
    ll::memory::HookPriority::Normal,
    BlockTessellator,
    &BlockTessellator::tessellateInWorld,
    bool,
    Tessellator&    tessellator,
    Block const&    block,
    BlockPos const& pos,
    bool            useCalcWithCache
) {
    if (!tl_hasProjection) {
        return origin(tessellator, block, pos, useCalcWithCache);
    }

    auto it = tl_currentScene->posColorMap.find(encodePosKey(pos));
    if (it == tl_currentScene->posColorMap.end()) {
        return origin(tessellator, block, pos, useCalcWithCache);
    }

    this->mColorOverride = it->second;
    bool result          = origin(tessellator, block, pos, useCalcWithCache);
    this->mColorOverride->reset();
    return result;
}

// Simple opaque blocks take a fast path in the chunk builder that skips tessellateInWorld
// and calls this overload with the precomputed visible faces instead, so the verifier colour
// override has to be applied here as well for mismatched full blocks to be tinted.
LL_TYPE_INSTANCE_HOOK(
    ProjectionTessellateSimpleHook,
    ll::memory::HookPriority::Normal,
    BlockTessellator,
    &BlockTessellator::tessellateBlockInWorld,
    bool,
    Tessellator&                   tessellator,
    Block const&                   block,
    BlockPos const&                pos,
    ::std::bitset<6> const         faces,
    ::AirAndSimpleBlockBits const* airAndSimpleBlocks
) {
    if (!tl_hasProjection || this->mColorOverride->has_value()) {
        return origin(tessellator, block, pos, faces, airAndSimpleBlocks);
    }

    auto it = tl_currentScene->posColorMap.find(encodePosKey(pos));
    if (it == tl_currentScene->posColorMap.end()) {
        return origin(tessellator, block, pos, faces, airAndSimpleBlocks);
    }

    this->mColorOverride = it->second;
    bool result          = origin(tessellator, block, pos, faces, airAndSimpleBlocks);
    this->mColorOverride->reset();
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    ProjectionBlockEntityHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);

    if (!renderAlphaLayer || !app::hasAppKernel()) {
        return;
    }

    auto& manager = block_actor::BlockActorRenderSchematic::getInstance();
    if (manager.renderersIsEmpty()) {
        return;
    }

    auto& projection = app::getAppKernel().blockActorProjection();
    (void)projection.flushRefresh(nullptr);

    auto scene = projection.sceneForDimension(mViewRegion->get()->getDimensionId());
    if (!scene || scene->empty()) {
        return;
    }

    Vec3 cameraTargetPos = renderContext.mImpl->mCameraTargetPosition;
    AABB renderBounds(cameraTargetPos, cameraTargetPos);
    renderBounds.min.x -= 72.0f;
    renderBounds.min.y -= 72.0f;
    renderBounds.min.z -= 72.0f;
    renderBounds.max.x += 72.0f;
    renderBounds.max.y += 72.0f;
    renderBounds.max.z += 72.0f;

    for (auto const* entry : collectBlockActorsInAabb(*scene, renderBounds)) {
        if (!entry || !entry->block || !entry->blockActor) {
            continue;
        }

        Vec3 renderPosition;
        renderPosition.x = entry->pos.x - cameraTargetPos.x;
        renderPosition.y = entry->pos.y - cameraTargetPos.y;
        renderPosition.z = entry->pos.z - cameraTargetPos.z;

        block_actor::BlockActorRenderDataForSchematic data{
            renderPosition,
            entry->pos,
            *entry->block,
            entry->blockActor.get(),
        };
        manager.renderSchematic(entry->rendererId, renderContext, data);
    }
}

LL_TYPE_INSTANCE_HOOK(
    ProjectionTextureUploadHook,
    ll::memory::HookPriority::Normal,
    mce::TextureGroup,
    &mce::TextureGroup::uploadTexture,
    BedrockTexture&,
    ResourceLocation const&                                      resourceLocation,
    gsl::not_null<::std::shared_ptr<::cg::TextureSetDefinition>> textureSetDefinition
) {
    auto& manager = block_actor::BlockActorRenderSchematic::getInstance();
    if (tl_inProjectionTextureUpload || manager.renderersIsEmpty()) {
        return origin(resourceLocation, textureSetDefinition);
    }

    auto target = manager.findTextureUploadTarget(resourceLocation.getHashedPath());
    if (target.resource && !textureSetDefinition->_getImageContainer()->mLayerImageList->empty()) {
        int  transparency = static_cast<int>(target.renderer->mTransparency * 255);
        auto old = textureSetDefinition->_getImageContainer()->mLayerImageList.get()[0].mImageList->getImage(0);
        if (old) {
            // Upload a copy of the vanilla colour layer with a constant alpha under the
            // renderer's "blend" resource location. TextureSetDefinitionLoader is no longer
            // exported, so the raw image buffer overload of uploadTexture is used instead.
            cg::ImageBuffer newBuf(*old);
            auto const&     desc   = newBuf.mImageDescription.get();
            auto            stride = cg::ImageDescription::getStrideFromFormat(desc.mTextureFormat);
            if (stride == 4 && newBuf.mStorage->size() >= static_cast<size_t>(desc.mWidth) * desc.mHeight * 4) {
                auto* pixels = newBuf.mStorage->get();
                for (uint32 i = 3; i < desc.mWidth * desc.mHeight * 4; i += 4) {
                    pixels[i] = static_cast<mce::Blob::value_type>(transparency);
                }
            }

            tl_inProjectionTextureUpload = true;
            try {
                uploadTexture(target.resource->blendRes, std::move(newBuf));
                manager.onTextureUploaded(target, this);
            } catch (...) {
                tl_inProjectionTextureUpload = false;
                throw;
            }
            tl_inProjectionTextureUpload = false;
        }
    }

    return origin(resourceLocation, textureSetDefinition);
}

LL_TYPE_INSTANCE_HOOK(
    ProjectionBlockActorRendererInitHook,
    ll::memory::HookPriority::Normal,
    BlockActorRenderDispatcher,
    &BlockActorRenderDispatcher::initializeBlockEntityRenderers,
    void,
    Bedrock::NotNullNonOwnerPtr<::GeometryGroup> const&                        geometryGroup,
    ::std::shared_ptr<::mce::TextureGroup>                                     textureGroup,
    ::BlockTessellator&                                                        blockTessellator,
    ::Bedrock::NotNullNonOwnerPtr<::ActorResourceDefinitionGroup const> const& actorResourceDefinitionGroup,
    ::ResourcePackManager&                                                     resourcePackManager,
    ::Bedrock::NotNullNonOwnerPtr<::ResourceLoadManager>                       resourceLoadManager,
    ::BaseGameVersion const&                                                   baseGameVersion,
    ::Experiments const&                                                       experiments
) {
    origin(
        geometryGroup,
        std::move(textureGroup),
        blockTessellator,
        actorResourceDefinitionGroup,
        resourcePackManager,
        resourceLoadManager,
        baseGameVersion,
        experiments
    );

    auto& manager = block_actor::BlockActorRenderSchematic::getInstance();
    if (!manager.hasRenderer(BlockActorRendererId::Chest)) {
        auto textureGroupFromClient = ll::service::getClientInstance()
                                        ? ll::service::getClientInstance()->getTextureGroup()
                                        : std::shared_ptr<::mce::TextureGroup>{};
        if (textureGroupFromClient) {
            manager.registerRenderer(
                BlockActorRendererId::Chest,
                std::make_unique<block_actor::SchematicChestRenderer>(std::move(textureGroupFromClient), 0.5f)
            );
        }
    }
}
using RenderHook = ll::memory::HookRegistrar<
    ProjectionBuildHook,
    ProjectionSortBorderHook,
    ProjectionTessellateHook,
    ProjectionTessellateSimpleHook,
    ProjectionBlockEntityHook,
    ProjectionTextureUploadHook,
    ProjectionBlockActorRendererInitHook>;

void registerRenderHooks() {
    if (gRenderHooksRegistered) {
        return;
    }
    RenderHook::hook();
    gRenderHooksRegistered = true;
}

void unregisterRenderHooks() {
    if (!gRenderHooksRegistered) {
        return;
    }
    RenderHook::unhook();
    gRenderHooksRegistered = false;
}

} // namespace levischematic::hook
