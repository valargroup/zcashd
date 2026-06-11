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
    size_t forwardedPending = 0;
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

// Reconciles the local mempool with Zebra's mempool once. Returns success for a
// completed poll, while policy divergence is reported in the result and status.
MempoolMirrorResult SyncMempoolMirrorOnce(ZebraCompatClient& client, const CChainParams& chainparams);

// Returns the latest mirror status snapshot under the mirror lock.
MempoolMirrorStatus GetMempoolMirrorStatus();

// Serializes the latest mirror status for `getzebracompatinfo`.
UniValue MempoolMirrorStatusToJSON();

// Clears mirror status and divergence samples for unit tests.
void ResetMempoolMirrorForTesting();

// Returns the per-poll cap on Zebra mempool txids reconciled into zcashd.
size_t MaxMempoolMirrorTxIdsPerPoll();

// Returns the maximum number of divergent txids retained for status details.
size_t MaxMempoolMirrorDivergenceDetails();

int TEST_ComputeMempoolMirrorLag(
    int zebraSize,
    int localSize,
    size_t divergent,
    size_t retainedForwarded);

} // namespace zebra_compat

#endif // ZCASH_ZEBRA_COMPAT_MEMPOOL_MIRROR_H
