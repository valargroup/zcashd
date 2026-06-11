// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_ZEBRA_COMPAT_BLOCK_INGESTION_H
#define ZCASH_ZEBRA_COMPAT_BLOCK_INGESTION_H

#include <string>
#include <vector>

class CBlock;
class CChainParams;

namespace zebra_compat {

struct BlockIngestionResult {
    bool success = false;
    bool hardFailure = false;
    int height = -1;
    std::string hash;
    std::string error;
};

// Ingests one block through the configured zebra-compat validation mode.
// Records the result globally and reports validation or connection failures.
BlockIngestionResult IngestBlock(const CBlock& block, const CChainParams& chainparams);

// Ingests a contiguous block batch and records the result globally. An empty
// batch succeeds without side effects on chainstate.
BlockIngestionResult IngestBlockBatch(const std::vector<CBlock>& blocks, const CChainParams& chainparams);

// Publishes the most recent block ingestion result for status RPC consumers.
void RecordBlockIngestionResult(const BlockIngestionResult& result);

} // namespace zebra_compat

#endif // ZCASH_ZEBRA_COMPAT_BLOCK_INGESTION_H
