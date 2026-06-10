// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_ZEBRA_COMPAT_ZEBRA_COMPAT_H
#define BITCOIN_ZEBRA_COMPAT_ZEBRA_COMPAT_H

#include <string>
#include <vector>
#include <stdint.h>

#include "zebra_compat/block_ingestion.h"

class CBlock;
class CChainParams;
class CScheduler;
class CRPCTable;
class UniValue;

namespace boost {
class thread_group;
}

namespace unity {

class UnityZebraClient;

static const char* const BLOCK_SOURCE_P2P = "p2p";
static const char* const BLOCK_SOURCE_ZEBRA = "zebra";
static const char* const BLOCK_VALIDATION_FULL = "full";
static const char* const BLOCK_VALIDATION_TRUSTED_ZEBRA = "trusted-zebra";

bool IsEnabled();
bool IsP2PEnabled();
bool IsTrustedValidationEnabled();

void InitParameterInteraction();
std::string ValidateParameterInteraction();

bool StartUnityNode(boost::thread_group& threadGroup, CScheduler& scheduler, const CChainParams& chainparams);
void InterruptUnityNode();
void StopUnityNode();

struct CommonAncestorSearchResult {
    bool found = false;
    bool overLimit = false;
    int height = -1;
    int disconnectLength = 0;
    std::string hash;
    std::string error;
};

CommonAncestorSearchResult FindCommonAncestorInHashRange(
    int localTipHeight,
    int firstHeight,
    const std::vector<std::string>& localHashes,
    const std::vector<std::string>& zebraHashes,
    int maxReorgLength);

UniValue GetUnityInfo();
void ThrowIfP2PDisabled(const std::string& method);
void ThrowIfMiningDisabled(const std::string& method);

void RegisterUnityRPCCommands(CRPCTable& tableRPC);

int UnityRetryBackoffSeconds(int consecutiveFailures);

struct UnitySyncTestOutcome {
    bool progressed = false;
    bool stickyFault = false;
    bool transientFailure = false;
};

UnitySyncTestOutcome TEST_ValidatePostIngestionTipOnZebraBestChain(
    UnityZebraClient& client,
    const CChainParams& chainparams,
    int localTipHeight,
    const std::string& localTipHash,
    int expectedHeight,
    const std::string& expectedHash,
    const std::string& mismatchError,
    const std::string& offChainDetail,
    bool reorgContext = false);

} // namespace unity

#endif // BITCOIN_ZEBRA_COMPAT_ZEBRA_COMPAT_H
