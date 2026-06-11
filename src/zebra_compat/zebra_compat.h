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

namespace zebra_compat {

class ZebraCompatClient;
struct MempoolMirrorStatus;

static const char* const BLOCK_SOURCE_P2P = "p2p";
static const char* const BLOCK_SOURCE_ZEBRA = "zebra";
static const char* const BLOCK_VALIDATION_FULL = "full";
static const char* const BLOCK_VALIDATION_TRUSTED_ZEBRA = "trusted-zebra";

// Returns true when zebra-compat mode or Zebra block-source behavior is enabled.
bool IsEnabled();

// Returns true when local Zcash P2P networking remains enabled.
bool IsP2PEnabled();

// Returns true when blocks from Zebra are connected under trusted validation.
bool IsTrustedValidationEnabled();

// Applies zebra-compat argument interactions, including P2P disablement.
void InitParameterInteraction();

// Validates zebra-compat argument combinations. Returns an empty string when
// valid, otherwise a human-readable startup error.
std::string ValidateParameterInteraction();

// Starts the zebra-compat worker and metadata store when configured. Returns
// false after recording status if startup configuration cannot be loaded.
bool StartZebraCompatNode(boost::thread_group& threadGroup, CScheduler& scheduler, const CChainParams& chainparams);

// Requests worker interruption without waiting for shutdown completion.
void InterruptZebraCompatNode();

// Interrupts and joins the worker, closes metadata, and marks service stopped.
void StopZebraCompatNode();

struct CommonAncestorSearchResult {
    bool found = false;
    bool overLimit = false;
    int height = -1;
    int disconnectLength = 0;
    std::string hash;
    std::string error;
};

// Finds the highest shared hash in same-height local and Zebra hash ranges.
// Reports over-limit when the found ancestor exceeds `maxReorgLength`.
CommonAncestorSearchResult FindCommonAncestorInHashRange(
    int localTipHeight,
    int firstHeight,
    const std::vector<std::string>& localHashes,
    const std::vector<std::string>& zebraHashes,
    int maxReorgLength);

// Returns the complete zebra-compat status object used by RPC reporting.
UniValue GetZebraCompatInfo();

// Throws an RPC error when `method` requires local P2P but P2P is disabled.
void ThrowIfP2PDisabled(const std::string& method);

// Throws an RPC error when `method` is unavailable in zebra-compat mode.
void ThrowIfMiningDisabled(const std::string& method);

// Registers zebra-compat RPC methods with the node RPC table.
void RegisterZebraCompatRPCCommands(CRPCTable& tableRPC);

// Returns exponential retry backoff seconds for consecutive transient failures.
int ZebraCompatRetryBackoffSeconds(int consecutiveFailures);

struct ZebraCompatSyncTestOutcome {
    bool progressed = false;
    bool stickyFault = false;
    bool transientFailure = false;
};

// Test seam for post-ingestion Zebra-tip validation without applying blocks.
ZebraCompatSyncTestOutcome TEST_ValidatePostIngestionTipOnZebraBestChain(
    ZebraCompatClient& client,
    const CChainParams& chainparams,
    int localTipHeight,
    const std::string& localTipHash,
    int expectedHeight,
    const std::string& expectedHash,
    const std::string& mismatchError,
    const std::string& offChainDetail,
    bool reorgContext = false);

// Test seam for classifying a Zebra tip below the reorg window without mutating
// chainstate. The supplied local hash models the active chain at Zebra's height.
ZebraCompatSyncTestOutcome TEST_SyncZebraTipBelowReorgWindow(
    ZebraCompatClient& client,
    const CChainParams& chainparams,
    int localTipHeight,
    const std::string& localTipHash,
    int zebraBestHeight,
    const std::string& zebraBestHash,
    bool haveLocalHashAtZebraHeight,
    const std::string& localHashAtZebraHeight);

bool TEST_LoadZebraClientConfigForWorker(bool& stickyFault);

ZebraCompatSyncTestOutcome TEST_SyncZebraCompatOnce(
    ZebraCompatClient& client,
    ZebraCompatClient& prefetchClient,
    const CChainParams& chainparams);

void TEST_ResetReadinessHysteresis();

void TEST_SetZebraCompatStatusForReadiness(
    bool identityVerified,
    bool tipMatchedZebra,
    const std::string& serviceState = "ready",
    const std::string& syncState = "synced");

std::string TEST_ComputeZebraCompatReadiness(
    bool enabled,
    bool initialBlockDownload,
    bool txForwardingTransportReady,
    const MempoolMirrorStatus& mirrorStatus,
    bool notificationsCaughtUp,
    int64_t now);

} // namespace zebra_compat

#endif // BITCOIN_ZEBRA_COMPAT_ZEBRA_COMPAT_H
