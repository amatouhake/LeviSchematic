#include "SchematicLoader.h"

#include "levischematic/LeviSchematic.h"

#include "ll/api/memory/Memory.h"
#include "ll/api/service/Bedrock.h"


#include "mc/dataloadhelper/DataLoadHelper.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/levelgen/structure/StructureBlockPalette.h"
#include "mc/world/level/levelgen/structure/StructureTemplate.h"


namespace levischematic::placement {
namespace {

auto& getLogger() { return levischematic::LeviSchematic::getInstance().getSelf().getLogger(); }

void logFailure(std::string_view operation, std::filesystem::path const& file, LoadPlacementError const& error) {
    getLogger().warn(
        "Placement operation failed [operation={}, file={}]: {}",
        operation,
        file.string(),
        error.describe(file.string())
    );
}

std::optional<verifier::ContainerSlotSnapshot> tryParseContainerSlot(CompoundTag const& tag) {
    if (!tag.contains("Slot") || !tag.contains("Count")) {
        return std::nullopt;
    }

    uint64_t nameHash = 0;
    if (tag.contains("Name")) {
        nameHash = HashedString(static_cast<std::string_view>(tag["Name"])).getHash();
    } else if (tag.contains("id")) {
        nameHash = HashedString(static_cast<std::string_view>(tag["id"])).getHash();
    }

    if (nameHash == 0) {
        return std::nullopt;
    }

    return verifier::ContainerSlotSnapshot{
        .slot         = static_cast<int>(tag["Slot"]),
        .itemNameHash = nameHash,
        .count        = static_cast<int>(tag["Count"]),
    };
}

std::optional<verifier::BlockEntitySnapshot> parseBlockEntitySnapshot(CompoundTag const& tag) {
    verifier::BlockEntitySnapshot snapshot;
    bool                          sawContainerTag = false;

    if (tag.contains("Items", Tag::List)) {
        sawContainerTag = true;
        verifier::ContainerSnapshot container;
        auto const&                 items = tag["Items"].get<ListTag>();
        container.slots.reserve(items.size());
        for (auto const& entry : items) {
            if (!entry || !entry.hold(Tag::Compound)) {
                continue;
            }
            auto parsedSlot = tryParseContainerSlot(entry.get<CompoundTag>());
            if (parsedSlot) {
                container.slots.push_back(*parsedSlot);
            }
        }

        snapshot.container = std::move(container);
    }

    if (!sawContainerTag) {
        return std::nullopt;
    }
    return snapshot;
}

std::optional<verifier::BlockEntitySnapshot> getBlockEntitySnapshot(StructureTemplateData const& data, int flatIndex) {
    auto const* palette = data.getPalette(StructureTemplateData::DEFAULT_PALETTE_NAME());
    if (!palette) {
        return std::nullopt;
    }

    auto const* positionData = palette->getBlockPositionData(static_cast<uint64_t>(flatIndex));
    if (!positionData || !positionData->mBlockEntityData) {
        return std::nullopt;
    }

    return parseBlockEntitySnapshot(*positionData->mBlockEntityData);
}

// Identity data-load helper used when instantiating schematic block actors purely for
// projection rendering. StructureDataLoadHelper's constructor is no longer exported by the
// game, and the block actor position is passed to BlockActor::loadStatic explicitly, so no
// coordinate remapping is needed here.
class ProjectionDataLoadHelper : public DataLoadHelper {
public:
    Vec3            loadPosition(Vec3 const& position) override { return position; }
    BlockPos        loadBlockPosition(BlockPos const& blockPos) override { return blockPos; }
    BlockPos        loadBlockPositionOffset(BlockPos const& blockPosOffset) override { return blockPosOffset; }
    float           loadRotationDegreesX(float x) override { return x; }
    float           loadRotationDegreesY(float y) override { return y; }
    float           loadRotationRadiansX(float x) override { return x; }
    float           loadRotationRadiansY(float y) override { return y; }
    uchar           loadFacingID(uchar facing) override { return facing; }
    Vec3            loadDirection(Vec3 const& direction) override { return direction; }
    Direction::Type loadDirection(Direction::Type direction) override { return direction; }
    Rotation        loadRotation(Rotation rotation) override { return rotation; }
    Mirror          loadMirror(Mirror mirror) override { return mirror; }
    ActorUniqueID   loadActorUniqueID(ActorUniqueID id) override { return id; }
    ActorUniqueID   loadOwnerID(ActorUniqueID id) override { return id; }
    InternalComponentRegistry::ComponentInfo const* loadActorInternalComponentInfo(
        std::unordered_map<HashedString, InternalComponentRegistry::ComponentInfo> const& registry,
        std::string const&                                                                componentName
    ) override {
        auto it = registry.find(HashedString(componentName));
        return it == registry.end() ? nullptr : &it->second;
    }
    DataLoadHelperType getType() const override { return DataLoadHelperType::Default; }
    bool               shouldResetTime() override { return false; }
};

std::shared_ptr<BlockActor> getBlockActor(
    StructureTemplateData const& data,
    Block const&                 block,
    BlockPos const&              localPos,
    int                          flatIndex,
    Level&                       level
) {
    auto const* palette = data.getPalette(StructureTemplateData::DEFAULT_PALETTE_NAME());
    if (!palette) {
        return nullptr;
    }
    auto const* positionData = palette->getBlockPositionData(static_cast<uint64_t>(flatIndex));
    if (positionData && positionData->mBlockEntityData) {
        ProjectionDataLoadHelper helper;
        return BlockActor::loadStatic(block.getBlockType(), localPos, level, *positionData->mBlockEntityData, helper);
    }
    return nullptr;
}

int getFlatIndex(BlockPos const& pos, BlockPos const& size) { return pos.z + size.z * (pos.y + size.y * pos.x); }

} // namespace

LoadAssetResult SchematicLoader::loadMcstructureAsset(std::filesystem::path const& path) const {
    namespace fs = std::filesystem;

    auto fail = [&](LoadPlacementError error) -> LoadAssetResult {
        logFailure("loader.loadMcstructureAsset", path, error);
        return LoadAssetResult::failure(std::move(error));
    };

    try {
        std::error_code ec;
        if (!fs::exists(path, ec) || ec) {
            return fail({
                .code = LoadPlacementError::Code::FileNotFound,
            });
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return fail({
                .code   = LoadPlacementError::Code::FileReadFailed,
                .detail = "unable to open file",
            });
        }

        auto fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::string rawData(fileSize, '\0');
        if (!file.read(rawData.data(), static_cast<std::streamsize>(fileSize))) {
            return fail({
                .code   = LoadPlacementError::Code::FileReadFailed,
                .detail = "read returned incomplete data",
            });
        }

        auto tagResult = CompoundTag::fromBinaryNbt(rawData, true);
        if (!tagResult) {
            return fail({
                .code = LoadPlacementError::Code::NbtParseFailed,
            });
        }

        auto level = ll::service::getLevel();
        if (!level) {
            return fail({
                .code   = LoadPlacementError::Code::RegistryError,
                .detail = "level service is unavailable",
            });
        }

        auto              registry = level->getUnknownBlockTypeRegistry();
        StructureTemplate structureTemplate(path.filename().string(), registry);
        if (!structureTemplate.load(*tagResult)) {
            return fail({
                .code = LoadPlacementError::Code::TemplateLoadFailed,
            });
        }

        auto size = structureTemplate.rawSize();
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) {
            return fail({
                .code = LoadPlacementError::Code::EmptyStructure,
            });
        }

        auto asset         = std::make_shared<SchematicAsset>();
        asset->size        = size;
        asset->defaultName = path.stem().string();
        asset->localBlocks.reserve(static_cast<size_t>(size.x) * size.y * size.z);

        auto const& data = structureTemplate.mStructureTemplateData;

        for (int x = 0; x < size.x; ++x) {
            for (int y = 0; y < size.y; ++y) {
                for (int z = 0; z < size.z; ++z) {
                    BlockPos localPos{x, y, z};
                    auto*    block = StructureTemplate::tryGetBlockAtPos(localPos, data, registry);
                    if (!block || block->isAir()) {
                        continue;
                    }
                    asset->localBlocks.push_back({
                        .localPos    = localPos,
                        .renderBlock = block,
                        .blockActor  = getBlockActor(data, *block, localPos, getFlatIndex(localPos, size), *level),
                        .compareSpec = verifier::buildCompareSpecFromBlock(*block),
                        .blockEntity = getBlockEntitySnapshot(data, getFlatIndex(localPos, size)),
                    });
                }
            }
        }

        return LoadAssetResult::success(std::move(asset));
    } catch (std::exception const& e) {
        return fail({
            .code   = LoadPlacementError::Code::FileReadFailed,
            .detail = e.what(),
        });
    }
}

} // namespace levischematic::placement
