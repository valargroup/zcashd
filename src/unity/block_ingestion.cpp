// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "unity/block_ingestion.h"

#include "main.h"
#include "unity/unity.h"

namespace unity {
namespace {

void SetResultError(BlockIngestionResult& result, const CValidationState& state)
{
    result.hardFailure = !result.success && (state.IsInvalid() || state.IsError());
    result.error = state.GetRejectReason();
    if (result.error.empty()) {
        result.error = state.GetDebugMessage();
    }
    if (!result.success && result.error.empty()) {
        result.error = "block ingestion failed";
    }
}

bool ProcessBlockBatchWithFullValidation(
    CValidationState& state,
    BlockIngestionResult& result,
    const CChainParams& chainparams,
    const std::vector<CBlock>& blocks)
{
    for (const CBlock& block : blocks) {
        CValidationState blockState;
        result.hash = block.GetHash().GetHex();
        if (!ProcessNewBlock(blockState, chainparams, nullptr, &block, true, nullptr)) {
            state = blockState;
            return false;
        }
        state = blockState;
    }

    return state.IsValid();
}

void SetConnectedHeight(BlockIngestionResult& result, const CBlock& block)
{
    LOCK(cs_main);
    auto it = mapBlockIndex.find(block.GetHash());
    if (it != mapBlockIndex.end()) {
        result.height = it->second->nHeight;
    }
}

} // namespace

BlockIngestionResult IngestBlock(const CBlock& block, const CChainParams& chainparams)
{
    return IngestBlockBatch(std::vector<CBlock>{block}, chainparams);
}

BlockIngestionResult IngestBlockBatch(const std::vector<CBlock>& blocks, const CChainParams& chainparams)
{
    BlockIngestionResult result;
    if (blocks.empty()) {
        result.success = true;
        return result;
    }

    const CBlock& lastBlock = blocks.back();
    result.hash = lastBlock.GetHash().GetHex();

    CValidationState state;
    result.success = IsTrustedValidationEnabled()
        ? ProcessNewTrustedBlockBatch(state, chainparams, blocks)
        : ProcessBlockBatchWithFullValidation(state, result, chainparams, blocks);

    SetResultError(result, state);
    SetConnectedHeight(result, lastBlock);

    RecordBlockIngestionResult(result);
    return result;
}

} // namespace unity
