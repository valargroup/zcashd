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

BlockIngestionResult IngestBlock(const CBlock& block, const CChainParams& chainparams);
BlockIngestionResult IngestBlockBatch(const std::vector<CBlock>& blocks, const CChainParams& chainparams);
void RecordBlockIngestionResult(const BlockIngestionResult& result);

} // namespace zebra_compat

#endif // ZCASH_ZEBRA_COMPAT_BLOCK_INGESTION_H
