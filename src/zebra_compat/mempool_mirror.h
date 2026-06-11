// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_ZEBRA_COMPAT_MEMPOOL_MIRROR_H
#define ZCASH_ZEBRA_COMPAT_MEMPOOL_MIRROR_H

#include <cstddef>
#include <stdint.h>

#include <string>

class CChainParams;
class UniValue;

namespace zebra_compat {

class ZebraCompatClient;

struct MempoolMirrorStatus {
    std::string source = "zebra-poll";
    int lag = 0;
    int64_t lastUpdate = 0;
    int64_t lastFailure = 0;
    size_t divergent = 0;
    size_t divergentDetails = 0;
    size_t divergentDetailOverflow = 0;
    int zebraSize = -1;
    int localSize = 0;
    std::string lastError;
};

struct MempoolMirrorResult {
    bool success = false;
    int added = 0;
    int removed = 0;
    size_t divergent = 0;
    std::string error;
};

MempoolMirrorResult SyncMempoolMirrorOnce(ZebraCompatClient& client, const CChainParams& chainparams);
MempoolMirrorStatus GetMempoolMirrorStatus();
UniValue MempoolMirrorStatusToJSON();
void ResetMempoolMirrorForTesting();
size_t MaxMempoolMirrorTxIdsPerPoll();
size_t MaxMempoolMirrorDivergenceDetails();

} // namespace zebra_compat

#endif // ZCASH_ZEBRA_COMPAT_MEMPOOL_MIRROR_H
