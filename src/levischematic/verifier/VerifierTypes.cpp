#include "VerifierTypes.h"

#include "levischematic/LeviSchematic.h"

#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/states/BlockState.h"
#include "mc/world/level/block/states/BlockStateInstance.h"

#include <algorithm>

namespace levischematic::verifier {

    auto& getLogger() {
    return levischematic::LeviSchematic::getInstance().getSelf().getLogger();
}

BlockCompareSpec buildCompareSpecFromBlock(Block const& block) {
    BlockCompareSpec spec;
    spec.nameHash = block.getBlockType().mNameInfo->mFullName->getHash();
    // Block::forEachState is no longer exported by the game; walk the block type's
    // state table directly instead.
    auto const& blockType = block.getBlockType();
    auto        addState  = [&](BlockState const& state) {
        auto value = block.getState<int>(state.mID);
        if (!value) {
            return;
        }
        spec.exactStates.push_back(BlockStateSnapshot{
            .stateId  = state.mID,
            .value    = *value,
            .nameHash = state.mName->getHash(),
            .name     = state.mName->getString(),
        });
    };
    for (auto const& [stateId, instance] : blockType.mStates.get()) {
        addState(*instance.mState);
    }
    for (auto const& collection : blockType.mAlteredStateCollections.get()) {
        if (collection) {
            addState(collection->mBlockState->get());
        }
    }
    std::sort(
        spec.exactStates.begin(),
        spec.exactStates.end(),
        [](BlockStateSnapshot const& lhs, BlockStateSnapshot const& rhs) {
            return lhs.stateId < rhs.stateId;
        }
    );

    spec.compareContainer = block.getBlockType().isContainerBlock();
    return spec;
}

} // namespace levischematic::verifier
