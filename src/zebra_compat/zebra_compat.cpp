// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "zebra_compat/zebra_compat.h"

#include "chainparams.h"
#include "chainparamsbase.h"
#include "core_io.h"
#include "main.h"
#include "rpc/protocol.h"
#include "scheduler.h"
#include "sync.h"
#include "zebra_compat/mempool_mirror.h"
#include "zebra_compat/metadata.h"
#include "zebra_compat/tx_forwarder.h"
#include "zebra_compat/zebra_client.h"
#include "uint256.h"
#include "util/system.h"
#include "util/time.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include <boost/bind/bind.hpp>
#include <boost/chrono.hpp>
#include <boost/thread.hpp>
#include <univalue.h>

namespace zebra_compat {
namespace {

std::atomic<bool> g_zebra_compat_started(false);
std::atomic<bool> g_zebra_compat_interrupt(false);
std::unique_ptr<boost::thread> g_zebra_compat_worker;
CCriticalSection cs_zebra_compat_status;
boost::condition_variable g_zebra_compat_retry_cv;
boost::mutex g_zebra_compat_retry_mutex;
uint64_t g_zebra_compat_retry_wakeups = 0;

static const int DEFAULT_ZEBRA_COMPAT_POLL_INTERVAL_SECONDS = 5;
static const int MAX_ZEBRA_COMPAT_RETRY_BACKOFF_SECONDS = 60;
// How many acquisition batches one forward-sync pass drives before returning to the
// outer loop to refresh Zebra's tip/identity. Batches inside a pass are pipelined
// (the next batch is fetched from Zebra while the current one is applied), so this
// only bounds how often the cheap identity round-trip is amortized, not memory use
// (at most two batches are ever held in flight).
static const int DEFAULT_ZEBRA_COMPAT_FORWARD_DRIVE_BATCHES = 64;

struct ZebraCompatStatus {
    std::string serviceState = "stopped";
    std::string syncState = "degraded";
    std::string syncDetail = "waiting_for_zebra_endpoint";
    bool tipMatchedZebra = false;
    std::string lastError;
    ZebraIdentity zebra;
    BlockIngestionResult lastIngestion;
    int syncTargetHeight = -1;
    std::string syncTargetHash;
    int lastSyncedHeight = -1;
    std::string lastSyncedHash;
    int lastCommonAncestorHeight = -1;
    std::string lastCommonAncestorHash;
    int consecutiveRetryCount = 0;
    int currentBackoffSeconds = 0;
    int64_t nextRetryTime = 0;
    bool readinessWasReady = false;
    int64_t readinessDegradedSince = 0;
    bool stickyFault = false;
    bool retryRequested = false;
};

struct ZebraSourceView {
    int bestHeight = -1;
    std::string bestHash;
    std::map<int, std::string> bestChainHashes;
};

ZebraCompatStatus g_status;
ZebraSourceView g_source_view;

bool IsTrustedValidationTestFixtureEnabled()
{
    return GetBoolArg("-zebra-compat-trusted-validation-fixture", false);
}

bool IsExplicitlySet(const std::string& arg)
{
    return mapArgs.count(arg) > 0;
}

std::string ValidateNoMainnetTestFlags()
{
    if (Params().NetworkIDString() != CBaseChainParams::MAIN) {
        return "";
    }
    if (IsExplicitlySet("-zebra-compat-trusted-validation-fixture")) {
        return "-zebra-compat-trusted-validation-fixture may not be used on mainnet";
    }
    return "";
}

bool HasExplicitValues(const std::string& arg)
{
    return mapMultiArgs.count(arg) > 0 && !mapMultiArgs[arg].empty();
}

void ForceBoolArg(const std::string& arg, bool value)
{
    mapArgs[arg] = value ? "1" : "0";
}

void ForceOffP2POption(const std::string& arg)
{
    if (SoftSetBoolArg(arg, false)) {
        LogPrintf("zebra-compat parameter interaction: -p2p=0 -> setting %s=0\n", arg);
    }
}

std::string P2PDisabledConflictPrefix()
{
    return GetBoolArg("-zebra-compat", false) ? "-zebra-compat" : "-p2p=0";
}

bool IsOneOf(const std::string& value, const std::vector<std::string>& allowed)
{
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

std::string ValidateP2PDisabledConflicts()
{
    if (IsP2PEnabled()) {
        return "";
    }

    const std::string prefix = P2PDisabledConflictPrefix();
    if (IsExplicitlySet("-listen") && GetBoolArg("-listen", true)) {
        return prefix + " is incompatible with -listen=1";
    }

    const std::vector<std::pair<std::string, std::string>> valueConflicts = {
        {"-bind", "-bind"},
        {"-whitebind", "-whitebind"},
        {"-connect", "-connect"},
        {"-addnode", "-addnode"},
        {"-seednode", "-seednode"},
    };

    for (const auto& conflict : valueConflicts) {
        if (HasExplicitValues(conflict.first)) {
            return prefix + " is incompatible with " + conflict.second;
        }
    }

    const std::vector<std::pair<std::string, std::string>> boolConflicts = {
        {"-dnsseed", "-dnsseed=1"},
        {"-listenonion", "-listenonion=1"},
    };

    for (const auto& conflict : boolConflicts) {
        if (IsExplicitlySet(conflict.first) && GetBoolArg(conflict.first, true)) {
            return prefix + " is incompatible with " + conflict.second;
        }
    }

    return "";
}

std::string ValidateZebraCompatPreset()
{
    if (!GetBoolArg("-zebra-compat", false)) {
        return "";
    }
    if (GetArg("-blocksource", BLOCK_SOURCE_P2P) != BLOCK_SOURCE_ZEBRA) {
        return "-zebra-compat requires -blocksource=zebra";
    }
    if (IsP2PEnabled()) {
        return "-zebra-compat requires -p2p=0";
    }
    if (GetArg("-blockvalidation", BLOCK_VALIDATION_FULL) != BLOCK_VALIDATION_TRUSTED_ZEBRA) {
        return "-zebra-compat requires -blockvalidation=trusted-zebra";
    }
    return "";
}

int ZebraCompatPollIntervalSeconds()
{
    return std::max<int64_t>(1, GetArg("-zebra-compat-poll-interval", DEFAULT_ZEBRA_COMPAT_POLL_INTERVAL_SECONDS));
}

int ZebraCompatForwardDriveBatches()
{
    return std::max<int64_t>(1, GetArg("-zebra-compat-sync-drive-batches", DEFAULT_ZEBRA_COMPAT_FORWARD_DRIVE_BATCHES));
}

struct LocalTipSnapshot {
    int height = -1;
    std::string hash;
};

LocalTipSnapshot GetLocalTipSnapshot()
{
    LocalTipSnapshot snapshot;
    LOCK(cs_main);
    snapshot.height = chainActive.Height();
    if (chainActive.Tip() != nullptr) {
        snapshot.hash = chainActive.Tip()->GetBlockHash().GetHex();
    }
    return snapshot;
}

std::vector<std::string> GetLocalChainHashes(int startHeight, int endHeight)
{
    std::vector<std::string> hashes;
    if (startHeight > endHeight) {
        return hashes;
    }

    LOCK(cs_main);
    hashes.reserve(endHeight - startHeight + 1);
    for (int height = startHeight; height <= endHeight; height++) {
        CBlockIndex* index = chainActive[height];
        if (index == nullptr) {
            return std::vector<std::string>();
        }
        hashes.push_back(index->GetBlockHash().GetHex());
    }
    return hashes;
}

bool GetLocalChainHash(int height, std::string& hash)
{
    if (height < 0) {
        return false;
    }

    LOCK(cs_main);
    CBlockIndex* index = chainActive[height];
    if (index == nullptr) {
        return false;
    }
    hash = index->GetBlockHash().GetHex();
    return true;
}

// Classifies the case where Zebra's best height is below the normal comparison
// window. Matching local/Zebra hashes mean Zebra is only behind; mismatch means
// the divergence is deeper than zebra-compat's reorg policy.
CommonAncestorSearchResult ClassifyZebraTipBelowReorgWindow(
    const LocalTipSnapshot& localTip,
    int zebraBestHeight,
    const std::string& zebraBestHash,
    bool haveLocalHashAtZebraHeight,
    const std::string& localHashAtZebraHeight)
{
    CommonAncestorSearchResult result;
    if (haveLocalHashAtZebraHeight &&
        localHashAtZebraHeight == zebraBestHash) {
        result.found = true;
        result.height = zebraBestHeight;
        result.hash = zebraBestHash;
        result.disconnectLength = localTip.height - zebraBestHeight;
        return result;
    }

    result.overLimit = true;
    result.disconnectLength = localTip.height - zebraBestHeight;
    result.error = strprintf(
        "Zebra best chain is below zebra-compat reorg policy: local height %d, Zebra height %d, max reorg %u",
        localTip.height,
        zebraBestHeight,
        MAX_REORG_LENGTH);
    return result;
}

void UpdateZebraStatus(const ZebraIdentity& identity)
{
    LOCK(cs_zebra_compat_status);
    g_status.zebra = identity;
    if (identity.blocks >= 0) {
        g_status.syncTargetHeight = identity.blocks;
    }
    g_status.syncTargetHash = identity.bestBlockHash;
    if (g_source_view.bestHeight != identity.blocks ||
        g_source_view.bestHash != identity.bestBlockHash) {
        g_source_view.bestChainHashes.clear();
    }
    g_source_view.bestHeight = identity.blocks;
    g_source_view.bestHash = identity.bestBlockHash;
}

bool GetCachedZebraBestChainHash(
    int height,
    int zebraBestHeight,
    const std::string& zebraBestHash,
    std::string& hash)
{
    LOCK(cs_zebra_compat_status);
    if (g_source_view.bestHeight != zebraBestHeight ||
        g_source_view.bestHash != zebraBestHash) {
        return false;
    }

    auto it = g_source_view.bestChainHashes.find(height);
    if (it == g_source_view.bestChainHashes.end()) {
        return false;
    }

    hash = it->second;
    return true;
}

void RecordZebraBestChainHash(int height, const std::string& hash)
{
    LOCK(cs_zebra_compat_status);
    g_source_view.bestChainHashes[height] = hash;
    if (g_source_view.bestHeight >= 0) {
        const int retainFrom = std::max(
            0,
            g_source_view.bestHeight - static_cast<int>(MAX_REORG_LENGTH) - ZebraCompatSyncBatchSize());
        for (auto it = g_source_view.bestChainHashes.begin(); it != g_source_view.bestChainHashes.end();) {
            if (it->first < retainFrom) {
                it = g_source_view.bestChainHashes.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void RecordZebraBestChainHashes(int startHeight, const std::vector<std::string>& hashes)
{
    for (size_t i = 0; i < hashes.size(); i++) {
        RecordZebraBestChainHash(startHeight + static_cast<int>(i), hashes[i]);
    }
}

void UpdateCommonAncestorStatus(const CommonAncestorSearchResult& ancestor)
{
    LOCK(cs_zebra_compat_status);
    if (ancestor.found && ancestor.disconnectLength > 0) {
        g_status.lastCommonAncestorHeight = ancestor.height;
        g_status.lastCommonAncestorHash = ancestor.hash;
    }
}

std::vector<std::string> GetZebraBestChainHashes(
    ZebraCompatClient& client,
    int startHeight,
    int endHeight)
{
    std::vector<std::string> hashes = client.GetBlockHashes(startHeight, endHeight);
    RecordZebraBestChainHashes(startHeight, hashes);
    return hashes;
}

void UpdateSyncStatus(
    const std::string& serviceState,
    const std::string& syncState,
    const std::string& detail,
    const std::string& error = "",
    bool tipMatchedZebra = false)
{
    LOCK(cs_zebra_compat_status);
    g_status.serviceState = serviceState;
    g_status.syncState = syncState;
    g_status.syncDetail = detail;
    g_status.tipMatchedZebra = tipMatchedZebra;
    g_status.lastError = error;
    if (syncState == "failed") {
        LogPrintf(
            "zebra-compat sync status: service_state=%s sync_state=%s detail=%s error=\"%s\"\n",
            serviceState.c_str(),
            syncState.c_str(),
            detail.c_str(),
            error.c_str());
    } else if (!error.empty()) {
        LogPrint(
            "zebra-compat",
            "zebra-compat sync status: service_state=%s sync_state=%s detail=%s error=\"%s\"\n",
            serviceState.c_str(),
            syncState.c_str(),
            detail.c_str(),
            error.c_str());
    }
}

void UpdateSyncedTip(const LocalTipSnapshot& snapshot)
{
    LOCK(cs_zebra_compat_status);
    g_status.lastSyncedHeight = snapshot.height;
    g_status.lastSyncedHash = snapshot.hash;
}

void UpdateRetryStatus(int consecutiveRetryCount, int backoffSeconds)
{
    LOCK(cs_zebra_compat_status);
    g_status.consecutiveRetryCount = consecutiveRetryCount;
    g_status.currentBackoffSeconds = backoffSeconds;
    g_status.nextRetryTime = backoffSeconds > 0 ? GetTime() + backoffSeconds : 0;
}

// Publishes whether the worker is parked on a sticky fault. Any fresh worker
// outcome consumes a pending manual retry request.
void RecordStickyFault(bool stickyFault)
{
    LOCK(cs_zebra_compat_status);
    g_status.stickyFault = stickyFault;
    g_status.retryRequested = false;
}

// Consumes one manual retry request and clears the exposed sticky latch.
// Returns false when no operator retry is pending.
bool ConsumeStickyFaultRetryRequest()
{
    LOCK(cs_zebra_compat_status);
    if (!g_status.retryRequested) {
        return false;
    }
    g_status.retryRequested = false;
    g_status.stickyFault = false;
    return true;
}

bool DecodeFetchedBlocks(
    const std::vector<std::string>& hashes,
    const std::vector<std::string>& rawBlocks,
    const std::string& expectedPrevHash,
    std::vector<CBlock>& blocks,
    std::string& error)
{
    if (hashes.size() != rawBlocks.size()) {
        error = "Zebra returned mismatched hash/raw-block batch sizes";
        return false;
    }

    blocks.clear();
    blocks.reserve(rawBlocks.size());
    uint256 expectedPrev = uint256S(expectedPrevHash);
    for (size_t i = 0; i < rawBlocks.size(); i++) {
        CBlock block;
        if (!DecodeHexBlk(block, rawBlocks[i])) {
            error = strprintf("Zebra returned malformed block data for %s", hashes[i]);
            return false;
        }
        const std::string decodedHash = block.GetHash().GetHex();
        if (decodedHash != hashes[i]) {
            error = strprintf("Zebra block hash mismatch: requested %s, decoded %s", hashes[i], decodedHash);
            return false;
        }
        if (block.hashPrevBlock != expectedPrev) {
            error = strprintf("Zebra returned non-contiguous block %s", decodedHash);
            return false;
        }
        expectedPrev = block.GetHash();
        blocks.push_back(block);
    }
    return true;
}

struct SyncOutcome {
    bool progressed = false;
    bool stickyFault = false;
    bool transientFailure = false;
};

bool IsTransientIdentityFailure(const ZebraIdentity& identity);
const char* IdentityFailureDetail(const ZebraIdentity& identity, bool transient);

CommonAncestorSearchResult FindCommonAncestorWithZebra(
    ZebraCompatClient& client,
    const LocalTipSnapshot& localTip,
    int zebraBestHeight,
    const std::string& zebraBestHash)
{
    CommonAncestorSearchResult result;
    if (localTip.height < 0) {
        result.error = "local chain tip is unavailable";
        return result;
    }

    const int maxCompareHeight = std::min(localTip.height, zebraBestHeight);
    const int firstAllowedHeight = std::max(
        0,
        localTip.height - static_cast<int>(MAX_REORG_LENGTH));
    if (maxCompareHeight < firstAllowedHeight) {
        std::string localHashAtZebraHeight;
        const bool haveLocalHashAtZebraHeight =
            GetLocalChainHash(zebraBestHeight, localHashAtZebraHeight);
        return ClassifyZebraTipBelowReorgWindow(
            localTip,
            zebraBestHeight,
            zebraBestHash,
            haveLocalHashAtZebraHeight,
            localHashAtZebraHeight);
    }

    if (localTip.height <= zebraBestHeight) {
        std::string zebraHashAtLocalHeight;
        if (localTip.height == zebraBestHeight) {
            zebraHashAtLocalHeight = zebraBestHash;
        } else if (!GetCachedZebraBestChainHash(
                       localTip.height,
                       zebraBestHeight,
                       zebraBestHash,
                       zebraHashAtLocalHeight)) {
            zebraHashAtLocalHeight = client.GetBlockHash(localTip.height);
            RecordZebraBestChainHash(localTip.height, zebraHashAtLocalHeight);
        }
        if (zebraHashAtLocalHeight == localTip.hash) {
            result.found = true;
            result.height = localTip.height;
            result.hash = localTip.hash;
            result.disconnectLength = 0;
            return result;
        }
    }

    const int lastFallbackHeight = localTip.height <= zebraBestHeight ?
        localTip.height - 1 :
        maxCompareHeight;
    if (lastFallbackHeight < firstAllowedHeight) {
        result.error = "local tip is not on Zebra's best chain and no lower height remains inside the reorg window";
        return result;
    }

    std::vector<std::string> zebraHashes =
        GetZebraBestChainHashes(client, firstAllowedHeight, lastFallbackHeight);
    std::vector<std::string> localHashes =
        GetLocalChainHashes(firstAllowedHeight, lastFallbackHeight);
    return FindCommonAncestorInHashRange(
        localTip.height,
        firstAllowedHeight,
        localHashes,
        zebraHashes,
        MAX_REORG_LENGTH);
}

SyncOutcome ValidatePostIngestionTipOnZebraBestChain(
    ZebraCompatClient& client,
    const CChainParams& chainparams,
    const LocalTipSnapshot& localTip,
    int expectedHeight,
    const std::string& expectedHash,
    const std::string& mismatchError,
    const std::string& offChainDetail,
    bool reorgContext)
{
    ZebraIdentity current = client.CheckIdentity(chainparams);
    UpdateZebraStatus(current);
    if (!current.identityVerified) {
        const bool transient = IsTransientIdentityFailure(current);
        UpdateSyncStatus(
            transient ? "waiting" : "failed",
            transient ? "degraded" : "failed",
            IdentityFailureDetail(current, transient),
            current.lastError);
        return {false, !transient, transient};
    }

    std::string zebraHashAtLocalHeight;
    bool localTipOnZebraBest = false;
    if (localTip.height >= 0 && localTip.height <= current.blocks) {
        if (localTip.height == current.blocks) {
            zebraHashAtLocalHeight = current.bestBlockHash;
        
            // If we don't have the zebra hash at the local height, we need to get it from the client
        } else if (!GetCachedZebraBestChainHash(
                       localTip.height,
                       current.blocks,
                       current.bestBlockHash,
                       zebraHashAtLocalHeight)) {
            zebraHashAtLocalHeight = client.GetBlockHash(localTip.height);
            RecordZebraBestChainHash(localTip.height, zebraHashAtLocalHeight);
        }
        // Check if the zebra hash at the local height matches the local tip hash
        localTipOnZebraBest = (zebraHashAtLocalHeight == localTip.hash);
    }

    // If the local tip is on the Zebra best chain, we can update the synced tip and status
    // and continue. Otherwise, fall through and fail.
    if (localTipOnZebraBest) {
        UpdateSyncedTip(localTip);
        if (localTip.height == current.blocks && localTip.hash == current.bestBlockHash) {
            UpdateSyncStatus("ready", "synced", "zebra_tip_matched", "", true);
        } else {
            UpdateSyncStatus("ready", "syncing", "zebra_backfill_in_progress");
        }
        return {true, false};
    }

    // Zebra's best chain temporarily shrank after the reorg (e.g. Zebra itself
    // is mid-reorg): local tip is strictly ahead. Not a hard fault; retry with
    // backoff so the worker loop waits for Zebra to catch up. Check before the
    // tip-changed branch because the expected height is irrelevant here.
    if (reorgContext && localTip.height > current.blocks) {
        UpdateSyncStatus(
            "ready",
            "degraded",
            "zebra_tip_temporarily_behind_local_after_reorg",
            strprintf(
                "local tip %s at height %d is ahead of Zebra best tip %s at height %d after reorg; "
                "retrying after Zebra best chain refresh",
                localTip.hash,
                localTip.height,
                current.bestBlockHash,
                current.blocks));
        return {false, false, true};
    }

    if (current.blocks != expectedHeight ||
        current.bestBlockHash != expectedHash) {
        UpdateSyncStatus(
            "ready",
            "degraded",
            "zebra_tip_changed_during_sync",
            strprintf("Zebra tip changed from %s at height %d to %s at height %d during zebra-compat sync",
                      expectedHash, expectedHeight, current.bestBlockHash, current.blocks));
        return {false, false};
    }

    // Equal-height reorg race: Zebra advertised a competing block at the local
    // tip's height, the replacement branch was ingested, but ActivateBestChain
    // kept the previously received equal-work local tip active. This is not a
    // hard fault: either Zebra extends its branch (making it strictly more
    // work) or Zebra reorgs back to the local branch. Degrade non-sticky so
    // the worker retries after refreshing Zebra's best chain.
    if (reorgContext && localTip.height == current.blocks) {
        UpdateSyncStatus(
            "ready",
            "degraded",
            "zebra_equal_work_reorg_not_activated",
            strprintf(
                "ActivateBestChain kept local equal-work tip %s at height %d instead of Zebra tip %s; "
                "retrying after Zebra best chain refresh",
                localTip.hash,
                localTip.height,
                current.bestBlockHash));
        return {false, false};
    }

    const std::string localVsZebraError = localTip.height > current.blocks
        ? strprintf(
              "local tip %s at height %d exceeds Zebra best tip %s at height %d",
              localTip.hash,
              localTip.height,
              current.bestBlockHash,
              current.blocks)
        : strprintf(
              "local tip %s at height %d is not on Zebra best chain (Zebra hash at that height: %s)",
              localTip.hash,
              localTip.height,
              zebraHashAtLocalHeight);
    UpdateSyncStatus(
        "failed",
        "failed",
        offChainDetail,
        mismatchError + "; " + localVsZebraError);
    return {false, true};
}

SyncOutcome SyncZebraCompatReorgToZebraBest(
    ZebraCompatClient& client,
    const CChainParams& chainparams,
    const LocalTipSnapshot& localTip,
    int zebraBestHeight,
    const std::string& zebraBestHash,
    const CommonAncestorSearchResult& ancestor)
{
    if (!ancestor.found) {
        UpdateSyncStatus(
            "failed",
            "failed",
            ancestor.overLimit ? "over_policy_reorg" : "no_common_ancestor",
            ancestor.error);
        return {false, true};
    }
    UpdateCommonAncestorStatus(ancestor);

    if (zebraBestHeight <= ancestor.height) {
        UpdateSyncStatus(
            "waiting",
            "degraded",
            "zebra_tip_behind_local",
            strprintf("Zebra best tip %s at height %d would require a disconnect-only rollback from local height %d",
                      zebraBestHash, zebraBestHeight, localTip.height));
        return {false, false};
    }

    if (ancestor.overLimit ||
        ancestor.disconnectLength > static_cast<int>(MAX_REORG_LENGTH)) {
        UpdateSyncStatus(
            "failed",
            "failed",
            "over_policy_reorg",
            strprintf("Zebra reorg would disconnect %d blocks, exceeding max reorg %u",
                      ancestor.disconnectLength, MAX_REORG_LENGTH));
        return {false, true};
    }

    bool zebraTipAlreadyIndexed = false;
    {
        LOCK(cs_main);
        auto it = mapBlockIndex.find(uint256S(zebraBestHash));
        zebraTipAlreadyIndexed =
            it != mapBlockIndex.end() &&
            it->second != nullptr &&
            it->second->nHeight == zebraBestHeight;
    }
    if (zebraTipAlreadyIndexed) {
        LocalTipSnapshot newTip = GetLocalTipSnapshot();
        if (newTip.hash != zebraBestHash || newTip.height != zebraBestHeight) {
            UpdateSyncStatus("ready", "syncing", "validating_indexed_zebra_reorg_branch");
            return ValidatePostIngestionTipOnZebraBestChain(
                client,
                chainparams,
                newTip,
                zebraBestHeight,
                zebraBestHash,
                strprintf("local tip after indexed Zebra reorg is %s at height %d, expected %s at height %d",
                          newTip.hash, newTip.height, zebraBestHash, zebraBestHeight),
                "local_tip_not_on_zebra_best_chain_after_reorg",
                /*reorgContext=*/true);
        }

        UpdateSyncedTip(newTip);
        UpdateSyncStatus("ready", "synced", "zebra_tip_matched", "", true);
        return {true, false};
    }

    UpdateSyncStatus("ready", "syncing", "fetching_zebra_reorg_branch");
    const int startHeight = ancestor.height + 1;
    const int branchLength = zebraBestHeight - ancestor.height;
    const int batch = ZebraCompatSyncBatchSize();
    std::vector<CBlock> blocks;
    blocks.reserve(branchLength);
    std::string expectedPrevHash = ancestor.hash;
    // Keep each Zebra RPC response bounded, but preserve the no-partial-reorg
    // contract by handing the complete replacement branch to ingestion once.
    for (int chunkStart = startHeight; chunkStart <= zebraBestHeight; chunkStart += batch) {
        const int chunkEnd = std::min(zebraBestHeight, chunkStart + batch - 1);
        const std::vector<std::string> hashes =
            GetZebraBestChainHashes(client, chunkStart, chunkEnd);
        if (hashes.size() != static_cast<size_t>(chunkEnd - chunkStart + 1)) {
            UpdateSyncStatus(
                "failed",
                "failed",
                "zebra_block_data_error",
                strprintf("Zebra returned %d hashes for reorg branch heights %d-%d",
                          static_cast<int>(hashes.size()), chunkStart, chunkEnd));
            return {false, true};
        }
        if (chunkEnd == zebraBestHeight && hashes.back() != zebraBestHash) {
            UpdateSyncStatus(
                "ready",
                "degraded",
                "zebra_tip_changed_during_sync",
                strprintf("Zebra replacement branch no longer ends at expected tip %s at height %d",
                          zebraBestHash, zebraBestHeight));
            return {false, false};
        }
        const std::vector<std::string> rawBlocks = client.GetRawBlocks(hashes);

        std::vector<CBlock> chunkBlocks;
        std::string decodeError;
        if (!DecodeFetchedBlocks(hashes, rawBlocks, expectedPrevHash, chunkBlocks, decodeError)) {
            UpdateSyncStatus("failed", "failed", "zebra_block_data_error", decodeError);
            return {false, true};
        }
        expectedPrevHash = chunkBlocks.back().GetHash().GetHex();
        blocks.insert(blocks.end(), chunkBlocks.begin(), chunkBlocks.end());
    }

    BlockIngestionResult result = IngestBlockBatch(blocks, chainparams);
    if (!result.success) {
        UpdateSyncStatus(
            result.hardFailure ? "failed" : "ready",
            result.hardFailure ? "failed" : "degraded",
            result.hardFailure ? "hard_sync_fault" : "block_ingestion_error",
            result.error);
        return {false, result.hardFailure};
    }

    LocalTipSnapshot newTip = GetLocalTipSnapshot();
    if (newTip.hash != zebraBestHash || newTip.height != zebraBestHeight) {
        return ValidatePostIngestionTipOnZebraBestChain(
            client,
            chainparams,
            newTip,
            zebraBestHeight,
            zebraBestHash,
            strprintf("local tip after Zebra reorg is %s at height %d, expected %s at height %d",
                      newTip.hash, newTip.height, zebraBestHash, zebraBestHeight),
            "local_tip_not_on_zebra_best_chain_after_reorg",
            /*reorgContext=*/true);
    }

    UpdateSyncedTip(newTip);
    UpdateSyncStatus("ready", "synced", "zebra_tip_matched", "", true);
    LogPrintf(
        "zebra-compat followed Zebra best-chain reorg: ancestor=%s height=%d disconnected=%d new_tip=%s height=%d\n",
        ancestor.hash.c_str(),
        ancestor.height,
        ancestor.disconnectLength,
        newTip.hash.c_str(),
        newTip.height);
    return {true, false};
}

bool IsTransientIdentityFailure(const ZebraIdentity& identity)
{
    return identity.failure == ZebraIdentity::TRANSIENT ||
        (identity.failure == ZebraIdentity::AUTHENTICATION &&
         !GetArg("-zebra-compat-cookiefile", "").empty());
}

// Returns the sync status detail for a Zebra identity failure.
// Authentication is retryable only when cookie auth is configured, because the
// next worker pass can reload a rotated cookie from disk.
const char* IdentityFailureDetail(const ZebraIdentity& identity, bool transient)
{
    if (transient) {
        return identity.failure == ZebraIdentity::AUTHENTICATION ?
            "zebra_authentication_retry" :
            "zebra_unreachable";
    }
    return "zebra_identity_error";
}

// Returns true for client-configuration failures that can clear without a
// zcashd restart, such as Zebra writing or replacing the configured cookie file.
bool IsRetryableZebraClientConfigError(const std::string& error)
{
    return error == "waiting_for_zebra_endpoint" ||
        error.find("Unable to open Zebra RPC cookie file") != std::string::npos ||
        error == "Zebra RPC cookie must be in user:password format";
}

// Loads the Zebra RPC config for one worker pass.
//
// Returns true when `config` is ready to use. On failure, updates zebra-compat
// sync status and sets `stickyFault` to indicate whether the worker should stop
// retrying this configuration until restart.
bool LoadZebraClientConfigForWorker(ZebraClientConfig& config, bool& stickyFault)
{
    std::string error;
    if (LoadZebraClientConfig(config, error)) {
        stickyFault = false;
        return true;
    }

    stickyFault = !IsRetryableZebraClientConfigError(error);
    if (stickyFault) {
        UpdateSyncStatus("failed", "failed", "zebra_configuration_error", error);
        LogPrintf("zebra-compat Zebra configuration failed: %s\n", error);
        return false;
    }

    const std::string statusError = error == "waiting_for_zebra_endpoint" ? "" : error;
    UpdateSyncStatus(
        "waiting",
        "degraded",
        error == "waiting_for_zebra_endpoint" ?
            "waiting_for_zebra_endpoint" :
            "zebra_configuration_unavailable",
        statusError);
    if (error == "waiting_for_zebra_endpoint") {
        LogPrint(
            "zebra-compat",
            "zebra-compat node waiting for Zebra endpoint configuration (-zebra-compat-url)\n");
    }
    return false;
}

struct ForwardFetch {
    int startHeight = -1;
    int endHeight = -1;
    std::vector<std::string> hashes;
    std::vector<std::string> rawBlocks;
};

// Fetch the best-chain hashes and raw block data for [startHeight, endHeight] from
// Zebra. Used for the priming batch and, on a worker via std::async, to prefetch the
// next batch while the current one is applied.
ForwardFetch FetchForwardRange(ZebraCompatClient& client, int startHeight, int endHeight)
{
    ForwardFetch fetch;
    fetch.startHeight = startHeight;
    fetch.endHeight = endHeight;
    fetch.hashes = GetZebraBestChainHashes(client, startHeight, endHeight);
    fetch.rawBlocks = client.GetRawBlocks(fetch.hashes);
    return fetch;
}

// Drive forward (append-only) sync from the local tip toward Zebra's best chain,
// overlapping Zebra block acquisition with local block application: while the current
// batch is decoded and ingested, the next batch is fetched on a worker thread, turning
// the per-batch cost from fetch+apply into max(fetch, apply). Correctness is unchanged
// from the serial path: every batch is still verified to chain onto the actual local
// tip and to match the requested hashes (DecodeFetchedBlocks), so a batch invalidated
// by a Zebra reorg fails those checks and is rejected rather than applied. At most two
// batches are ever held in flight. Preconditions, checked by the caller: the local tip
// is on Zebra's best chain and strictly below Zebra's tip.
SyncOutcome RunForwardSyncPipelined(
    ZebraCompatClient& client,
    ZebraCompatClient& prefetchClient,
    const CChainParams& chainparams,
    const LocalTipSnapshot& startTip,
    int zebraBestHeight,
    const std::string& zebraBestHash)
{
    const int batch = ZebraCompatSyncBatchSize();
    const int64_t windowEnd = std::min<int64_t>(
        zebraBestHeight,
        static_cast<int64_t>(startTip.height) +
            static_cast<int64_t>(batch) * ZebraCompatForwardDriveBatches());

    std::string expectedPrevHash = startTip.hash;
    bool progressedAny = false;

    UpdateSyncStatus("ready", "syncing", "fetching_zebra_blocks");

    // Prime the pipeline with the first batch synchronously.
    ForwardFetch current;
    try {
        current = FetchForwardRange(
            client, startTip.height + 1, std::min(zebraBestHeight, startTip.height + batch));
    } catch (const std::exception& e) {
        UpdateSyncStatus("waiting", "degraded", "zebra_rpc_error", e.what());
        return {progressedAny, false, true};
    }

    while (true) {
        boost::this_thread::interruption_point();
        if (g_zebra_compat_interrupt.load()) {
            return {progressedAny, false};
        }

        // If this batch reaches Zebra's tip, confirm Zebra still ends where we expect;
        // otherwise Zebra moved under us and the outer loop should refresh identity.
        if (current.endHeight == zebraBestHeight &&
            (current.hashes.empty() || current.hashes.back() != zebraBestHash)) {
            UpdateSyncStatus(
                "ready",
                "degraded",
                "zebra_tip_changed_during_sync",
                strprintf("Zebra block batch no longer ends at expected tip %s at height %d",
                          zebraBestHash, zebraBestHeight));
            return {progressedAny, false};
        }

        // Start acquiring the next batch so it overlaps with applying the current one.
        // The future's destructor joins the worker on every early return below, so the
        // referenced prefetchClient always outlives the task.
        const bool hasNext = current.endHeight < windowEnd;
        std::future<ForwardFetch> nextFetch;
        if (hasNext) {
            const int nextStart = current.endHeight + 1;
            const int nextEnd = std::min(zebraBestHeight, current.endHeight + batch);
            nextFetch = std::async(
                std::launch::async,
                [&prefetchClient, nextStart, nextEnd]() {
                    return FetchForwardRange(prefetchClient, nextStart, nextEnd);
                });
        }

        std::vector<CBlock> blocks;
        std::string decodeError;
        if (!DecodeFetchedBlocks(current.hashes, current.rawBlocks, expectedPrevHash, blocks, decodeError)) {
            UpdateSyncStatus("failed", "failed", "zebra_block_data_error", decodeError);
            return {progressedAny, true};
        }

        BlockIngestionResult result = IngestBlockBatch(blocks, chainparams);
        if (!result.success) {
            UpdateSyncStatus(
                result.hardFailure ? "failed" : "ready",
                result.hardFailure ? "failed" : "degraded",
                result.hardFailure ? "hard_sync_fault" : "block_ingestion_error",
                result.error);
            return {progressedAny, result.hardFailure};
        }

        LocalTipSnapshot newTip = GetLocalTipSnapshot();
        if (newTip.hash != current.hashes.back() || newTip.height != current.endHeight) {
            return ValidatePostIngestionTipOnZebraBestChain(
                client,
                chainparams,
                newTip,
                zebraBestHeight,
                zebraBestHash,
                strprintf("local tip after zebra-compat chunk is %s at height %d, expected %s at height %d",
                          newTip.hash, newTip.height, current.hashes.back(), current.endHeight),
                "local_tip_not_on_zebra_best_chain_after_chunk",
                /*reorgContext=*/false);
        }

        UpdateSyncedTip(newTip);
        progressedAny = true;
        expectedPrevHash = newTip.hash;

        if (newTip.height == zebraBestHeight && newTip.hash == zebraBestHash) {
            UpdateSyncStatus("ready", "synced", "zebra_tip_matched", "", true);
            return {true, false};
        }
        UpdateSyncStatus("ready", "syncing", "zebra_backfill_in_progress");

        if (!hasNext) {
            // Reached the drive window without reaching Zebra's tip; return so the outer
            // loop refreshes Zebra's tip/identity and continues from the new local tip.
            return {true, false};
        }

        try {
            current = nextFetch.get();
        } catch (const std::exception& e) {
            UpdateSyncStatus("waiting", "degraded", "zebra_rpc_error", e.what());
            return {progressedAny, false, true};
        }
    }
}

SyncOutcome SyncZebraCompatOnce(
    ZebraCompatClient& client,
    ZebraCompatClient& prefetchClient,
    const CChainParams& chainparams)
{
    ZebraIdentity identity = client.CheckIdentity(chainparams);
    UpdateZebraStatus(identity);
    if (!identity.identityVerified) {
        const bool transient = IsTransientIdentityFailure(identity);
        UpdateSyncStatus(
            transient ? "waiting" : "failed",
            transient ? "degraded" : "failed",
            IdentityFailureDetail(identity, transient),
            identity.lastError);
        return {false, !transient, transient};
    }

    const std::string zebraBestHash = identity.bestBlockHash;
    const int zebraBestHeight = identity.blocks;

    LocalTipSnapshot localTip = GetLocalTipSnapshot();
    CommonAncestorSearchResult ancestor =
        FindCommonAncestorWithZebra(client, localTip, zebraBestHeight, zebraBestHash);
    if (!ancestor.found || ancestor.height != localTip.height) {
        return SyncZebraCompatReorgToZebraBest(
            client,
            chainparams,
            localTip,
            zebraBestHeight,
            zebraBestHash,
            ancestor);
    }
    UpdateCommonAncestorStatus(ancestor);

    if (localTip.height == zebraBestHeight) {
        // FindCommonAncestorWithZebra only reports the local tip as the common
        // ancestor when Zebra's hash at that height matched the local tip hash.
        if (localTip.hash != zebraBestHash) {
            UpdateSyncStatus(
                "failed",
                "failed",
                "zebra_common_ancestor_invariant_violation",
                "common ancestor search reported the local tip as shared but hashes differ");
            return {false, true};
        }
        UpdateSyncedTip(localTip);
        UpdateSyncStatus("ready", "synced", "zebra_tip_matched", "", true);
        return {false, false};
    }

    // Forward, append-only sync: the local tip is on Zebra's best chain and strictly
    // below it. Drive it with acquisition/application pipelining.
    return RunForwardSyncPipelined(
        client, prefetchClient, chainparams, localTip, zebraBestHeight, zebraBestHash);
}

bool IsSyncedForMempoolMirror()
{
    LOCK(cs_zebra_compat_status);
    return g_status.tipMatchedZebra &&
        g_status.zebra.identityVerified;
}

bool IsZebraCompatSynced()
{
    LOCK(cs_zebra_compat_status);
    return g_status.syncState == "synced" && g_status.tipMatchedZebra;
}

bool IsMempoolMirrorReady(const MempoolMirrorStatus& status, int64_t now)
{
    if (status.lastUpdate <= 0 || !status.lastError.empty() ||
        status.lag != 0) {
        return false;
    }

    const int64_t maxAge = std::max<int64_t>(ZebraCompatPollIntervalSeconds() * 2, 1);
    return now - status.lastUpdate <= maxAge;
}

std::string ComputeRawReadiness(
    bool enabled,
    const ZebraCompatStatus& status,
    bool initialBlockDownload,
    bool txForwardingTransportReady,
    bool mirrorReady,
    bool notificationsCaughtUp)
{
    if (!enabled) {
        return "disabled";
    }
    if (status.serviceState == "failed" || status.syncState == "failed") {
        return "failed";
    }
    if (status.zebra.identityVerified &&
        status.tipMatchedZebra &&
        !initialBlockDownload &&
        txForwardingTransportReady &&
        mirrorReady &&
        notificationsCaughtUp) {
        return "ready";
    }
    return "degraded";
}

std::string ApplyReadinessHysteresis(const std::string& rawReadiness, int64_t now, bool allowHysteresis)
{
    LOCK(cs_zebra_compat_status);
    if (!allowHysteresis && rawReadiness == "degraded") {
        g_status.readinessWasReady = false;
        g_status.readinessDegradedSince = 0;
        return rawReadiness;
    }
    if (rawReadiness == "ready") {
        g_status.readinessWasReady = true;
        g_status.readinessDegradedSince = 0;
        return rawReadiness;
    }
    if (rawReadiness != "degraded") {
        g_status.readinessWasReady = false;
        g_status.readinessDegradedSince = 0;
        return rawReadiness;
    }
    if (!g_status.readinessWasReady) {
        return rawReadiness;
    }
    if (g_status.readinessDegradedSince <= 0 || now < g_status.readinessDegradedSince) {
        g_status.readinessDegradedSince = now;
    }

    const int64_t degradeAfter = std::max<int64_t>(ZebraCompatPollIntervalSeconds() * 2, 1);
    return now - g_status.readinessDegradedSince >= degradeAfter ? "degraded" : "ready";
}

// Wakes retry/backoff sleepers and marks the wake so spurious condition-variable
// wakeups do not shorten retry intervals.
void WakeZebraCompatRetryWaiters()
{
    {
        boost::unique_lock<boost::mutex> lock(g_zebra_compat_retry_mutex);
        g_zebra_compat_retry_wakeups++;
    }
    g_zebra_compat_retry_cv.notify_all();
}

// Sleeps until the requested retry/backoff interval elapses or an operator retry
// wakes the worker. Does not consume the retry request.
void WaitForZebraCompatRetryOrTimeout(int seconds)
{
    boost::unique_lock<boost::mutex> lock(g_zebra_compat_retry_mutex);
    const uint64_t observedWakeups = g_zebra_compat_retry_wakeups;
    const boost::chrono::steady_clock::time_point deadline =
        boost::chrono::steady_clock::now() + boost::chrono::seconds(seconds);
    while (g_zebra_compat_retry_wakeups == observedWakeups) {
        if (g_zebra_compat_retry_cv.wait_until(lock, deadline) == boost::cv_status::timeout) {
            return;
        }
    }
}

void SleepZebraCompatRetry(int& consecutiveRetryCount, bool countTransientFailure)
{
    if (countTransientFailure) {
        consecutiveRetryCount++;
    } else {
        consecutiveRetryCount = 0;
    }
    const int backoffSeconds = countTransientFailure ?
        ZebraCompatRetryBackoffSeconds(consecutiveRetryCount) : 0;
    UpdateRetryStatus(consecutiveRetryCount, backoffSeconds);
    WaitForZebraCompatRetryOrTimeout(backoffSeconds > 0 ? backoffSeconds : ZebraCompatPollIntervalSeconds());
}

void ZebraCompatBlockSourceThread(std::string chainName)
{
    RenameThread("zcash-zebra-compat");
    try {
        const CChainParams& chainparams = Params(chainName);
        bool stickyFault = false;
        int consecutiveRetryCount = 0;
        while (!g_zebra_compat_interrupt.load()) {
            boost::this_thread::interruption_point();
            try {
                if (stickyFault) {
                    UpdateRetryStatus(consecutiveRetryCount, 0);
                    bool retryRequested = ConsumeStickyFaultRetryRequest();
                    if (!retryRequested) {
                        WaitForZebraCompatRetryOrTimeout(ZebraCompatPollIntervalSeconds());
                        retryRequested = ConsumeStickyFaultRetryRequest();
                    }
                    if (!retryRequested) {
                        continue;
                    }
                    stickyFault = false;
                    consecutiveRetryCount = 0;
                    UpdateRetryStatus(0, 0);
                    LogPrintf("zebra-compat retrying parked sticky fault after RPC request\n");
                }
                ZebraClientConfig config;
                if (!LoadZebraClientConfigForWorker(config, stickyFault)) {
                    RecordStickyFault(stickyFault);
                    SleepZebraCompatRetry(consecutiveRetryCount, !stickyFault);
                    continue;
                }
                // Dedicated client for prefetching the next batch concurrently with applying the
                // current one. A second client keeps acquisition and application from sharing any
                // per-call transport state. Rebuilding both clients each pass reloads cookie auth.
                ZebraCompatClient client(config, std::unique_ptr<ZebraRpcTransport>(new LibeventZebraRpcTransport()));
                ZebraCompatClient prefetchClient(config, std::unique_ptr<ZebraRpcTransport>(new LibeventZebraRpcTransport()));
                SyncOutcome outcome = SyncZebraCompatOnce(client, prefetchClient, chainparams);
                stickyFault = outcome.stickyFault;
                RecordStickyFault(stickyFault);
                const bool synced = IsZebraCompatSynced();
                if (outcome.progressed || stickyFault || synced || !outcome.transientFailure) {
                    consecutiveRetryCount = 0;
                }
                if (!stickyFault && IsSyncedForMempoolMirror()) {
                    MempoolMirrorResult mirrorResult = SyncMempoolMirrorOnce(client, chainparams);
                    if (!mirrorResult.success) {
                        LogPrintf("zebra-compat mempool mirror polling failed: %s\n", mirrorResult.error.c_str());
                    }
                }
                if (!outcome.progressed || stickyFault) {
                    SleepZebraCompatRetry(consecutiveRetryCount, !stickyFault && !synced && outcome.transientFailure);
                } else {
                    UpdateRetryStatus(0, 0);
                }
            } catch (const boost::thread_interrupted&) {
                throw;
            } catch (const std::exception& e) {
                if (!g_zebra_compat_interrupt.load()) {
                    UpdateSyncStatus("waiting", "degraded", "zebra_rpc_error", e.what());
                    SleepZebraCompatRetry(consecutiveRetryCount, true);
                }
            }
        }
    } catch (const boost::thread_interrupted&) {
        LogPrintf("zebra-compat block source thread interrupted\n");
    }
    LogPrintf("zebra-compat block source thread stopped\n");
}

void InterruptZebraCompatWorker()
{
    g_zebra_compat_interrupt = true;
    WakeZebraCompatRetryWaiters();
    if (g_zebra_compat_worker) {
        g_zebra_compat_worker->interrupt();
    }
}

void JoinZebraCompatWorker()
{
    if (g_zebra_compat_worker) {
        g_zebra_compat_worker->join();
        g_zebra_compat_worker.reset();
    }
}

} // namespace

CommonAncestorSearchResult FindCommonAncestorInHashRange(
    int localTipHeight,
    int firstHeight,
    const std::vector<std::string>& localHashes,
    const std::vector<std::string>& zebraHashes,
    int maxReorgLength)
{
    CommonAncestorSearchResult result;
    if (maxReorgLength < 0) {
        result.error = "max reorg length must be non-negative";
        return result;
    }
    if (firstHeight < 0 || localTipHeight < firstHeight) {
        result.error = "invalid common-ancestor height range";
        return result;
    }
    if (localHashes.size() != zebraHashes.size()) {
        result.error = "local and Zebra ancestor hash ranges have different lengths";
        return result;
    }
    if (localHashes.empty()) {
        result.error = "no common-ancestor hash range was provided";
        return result;
    }

    for (int offset = static_cast<int>(localHashes.size()) - 1; offset >= 0; offset--) {
        if (localHashes[offset] == zebraHashes[offset]) {
            result.found = true;
            result.height = firstHeight + offset;
            result.hash = localHashes[offset];
            result.disconnectLength = localTipHeight - result.height;
            if (result.disconnectLength > maxReorgLength) {
                result.overLimit = true;
                result.error = strprintf(
                    "Zebra reorg would disconnect %d blocks, exceeding max reorg %d",
                    result.disconnectLength,
                    maxReorgLength);
            }
            return result;
        }
    }

    result.disconnectLength = localTipHeight - firstHeight + 1;
    result.overLimit = result.disconnectLength > maxReorgLength;
    result.error = result.overLimit ?
        strprintf("no common ancestor found within max reorg %d", maxReorgLength) :
        "no common ancestor found in provided hash range";
    return result;
}

bool IsEnabled()
{
    // After validation, every zebra-compat configuration has local Zcash P2P disabled.
    // The explicit checks keep pre-validation status reporting robust.
    return GetBoolArg("-zebra-compat", false) ||
        GetArg("-blocksource", BLOCK_SOURCE_P2P) == BLOCK_SOURCE_ZEBRA ||
        !IsP2PEnabled();
}

int ZebraCompatRetryBackoffSeconds(int consecutiveFailures)
{
    if (consecutiveFailures <= 0) {
        return 0;
    }

    int backoff = std::min(ZebraCompatPollIntervalSeconds(), MAX_ZEBRA_COMPAT_RETRY_BACKOFF_SECONDS);
    for (int i = 1; i < consecutiveFailures && backoff < MAX_ZEBRA_COMPAT_RETRY_BACKOFF_SECONDS; i++) {
        backoff = std::min(MAX_ZEBRA_COMPAT_RETRY_BACKOFF_SECONDS, backoff * 2);
    }
    return backoff;
}

bool IsP2PEnabled()
{
    return GetBoolArg("-p2p", true);
}

bool IsTrustedValidationEnabled()
{
    return GetArg("-blockvalidation", BLOCK_VALIDATION_FULL) == BLOCK_VALIDATION_TRUSTED_ZEBRA &&
        (GetArg("-blocksource", BLOCK_SOURCE_P2P) == BLOCK_SOURCE_ZEBRA ||
         IsTrustedValidationTestFixtureEnabled());
}

void InitParameterInteraction()
{
    if (GetBoolArg("-zebra-compat", false)) {
        if (SoftSetArg("-blocksource", BLOCK_SOURCE_ZEBRA)) {
            LogPrintf("zebra-compat parameter interaction: -zebra-compat=1 -> setting -blocksource=zebra\n");
        }
        if (SoftSetArg("-blockvalidation", BLOCK_VALIDATION_TRUSTED_ZEBRA)) {
            LogPrintf("zebra-compat parameter interaction: -zebra-compat=1 -> setting -blockvalidation=trusted-zebra\n");
        }
        ForceBoolArg("-p2p", false);
        LogPrintf("zebra-compat parameter interaction: -zebra-compat=1 -> forcing -p2p=0\n");
        ForceBoolArg("-listen", false);
        LogPrintf("zebra-compat parameter interaction: -zebra-compat=1 -> forcing -listen=0\n");
        ForceBoolArg("-dnsseed", false);
        LogPrintf("zebra-compat parameter interaction: -zebra-compat=1 -> forcing -dnsseed=0\n");
        ForceBoolArg("-listenonion", false);
        LogPrintf("zebra-compat parameter interaction: -zebra-compat=1 -> forcing -listenonion=0\n");
    }

    if (!IsP2PEnabled()) {
        ForceOffP2POption("-listen");
        ForceOffP2POption("-dnsseed");
        ForceOffP2POption("-listenonion");
    }
}

std::string ValidateParameterInteraction()
{
    std::string optionError = ValidateNoMainnetTestFlags();
    if (!optionError.empty()) {
        return optionError;
    }

    const std::string blockSource = GetArg("-blocksource", BLOCK_SOURCE_P2P);
    if (!IsOneOf(blockSource, {BLOCK_SOURCE_P2P, BLOCK_SOURCE_ZEBRA})) {
        return strprintf("Invalid -blocksource value '%s'. Expected 'p2p' or 'zebra'.", blockSource);
    }

    const std::string blockValidation = GetArg("-blockvalidation", BLOCK_VALIDATION_FULL);
    if (!IsOneOf(blockValidation, {BLOCK_VALIDATION_FULL, BLOCK_VALIDATION_TRUSTED_ZEBRA})) {
        return strprintf("Invalid -blockvalidation value '%s'. Expected 'full' or 'trusted-zebra'.", blockValidation);
    }

    if (blockValidation == BLOCK_VALIDATION_TRUSTED_ZEBRA &&
        blockSource != BLOCK_SOURCE_ZEBRA &&
        !IsTrustedValidationTestFixtureEnabled()) {
        return "-blockvalidation=trusted-zebra requires -blocksource=zebra";
    }

    if (blockSource == BLOCK_SOURCE_P2P && !IsP2PEnabled()) {
        return "-blocksource=p2p requires -p2p=1";
    }

    if (blockSource == BLOCK_SOURCE_ZEBRA && IsP2PEnabled()) {
        return "-blocksource=zebra requires -p2p=0";
    }

    optionError = ValidateP2PDisabledConflicts();
    if (!optionError.empty()) {
        return optionError;
    }

    optionError = ValidateZebraCompatPreset();
    if (!optionError.empty()) {
        return optionError;
    }

    const int64_t configuredSyncBatchSize = GetArg("-zebra-compat-sync-batch-size", ZebraCompatSyncBatchSize());
    if (configuredSyncBatchSize < 1) {
        return "-zebra-compat-sync-batch-size must be at least 1";
    }
    const int effectiveSyncBatchSize = ZebraCompatSyncBatchSize();
    if (configuredSyncBatchSize > effectiveSyncBatchSize) {
        return "-zebra-compat-sync-batch-size=" + std::to_string(configuredSyncBatchSize) +
            " exceeds zebra-compat's raw block response memory budget; use " +
            std::to_string(effectiveSyncBatchSize) + " or lower";
    }

    const int64_t configuredTimeout = GetArg("-zebra-compat-timeout", ZebraCompatTimeoutSeconds());
    if (configuredTimeout < 1) {
        return "-zebra-compat-timeout must be at least 1";
    }

    const std::string zebraRpcMaxResponseBodyArg = "-zebra-compat-zebra-rpc-max-response-body-bytes";
    if (IsExplicitlySet(zebraRpcMaxResponseBodyArg)) {
        const int64_t configuredZebraRpcMaxResponseBodySize =
            GetArg(zebraRpcMaxResponseBodyArg, int64_t{0});
        if (configuredZebraRpcMaxResponseBodySize < 1) {
            return zebraRpcMaxResponseBodyArg + " must be at least 1";
        }

        const size_t requiredZebraRpcMaxResponseBodySize = ZebraRpcMaxResponseBodySize();
        if (static_cast<uint64_t>(configuredZebraRpcMaxResponseBodySize) <
            static_cast<uint64_t>(requiredZebraRpcMaxResponseBodySize)) {
            return zebraRpcMaxResponseBodyArg + "=" +
                std::to_string(configuredZebraRpcMaxResponseBodySize) +
                " is too small for zebra-compat's sync batch; "
                "set Zebra rpc.max_response_body_size to " +
                std::to_string(requiredZebraRpcMaxResponseBodySize) + " or higher";
        }
    }

    return "";
}

bool StartZebraCompatNode(boost::thread_group& threadGroup, CScheduler& scheduler, const CChainParams& chainparams)
{
    (void)threadGroup;
    (void)scheduler;
    InterruptZebraCompatWorker();
    JoinZebraCompatWorker();
    g_zebra_compat_started = true;
    g_zebra_compat_interrupt = false;
    LogPrintf("zebra-compat node mode enabled on %s\n",
              chainparams.NetworkIDString().c_str());

    ZebraCompatStatus status;
    status.serviceState = "waiting";
    status.syncState = "degraded";
    status.syncDetail = "waiting_for_zebra_endpoint";

    if (IsTrustedValidationEnabled() && !InitZebraCompatMetadata()) {
        status.serviceState = "failed";
        status.syncState = "failed";
        status.syncDetail = "zebra_compat_metadata_error";
        status.lastError = GetZebraCompatMetadataLastError();
        if (status.lastError.empty()) {
            status.lastError = "failed to initialize zebra-compat metadata database";
        }
        LogPrintf("zebra-compat metadata initialization failed: %s\n", status.lastError.c_str());
        LOCK(cs_zebra_compat_status);
        g_status = status;
        return true;
    }

    {
        LOCK(cs_zebra_compat_status);
        g_status = status;
    }
    LogPrintf("zebra-compat node starting Zebra polling worker\n");
    g_zebra_compat_worker.reset(new boost::thread(boost::bind(
        &ZebraCompatBlockSourceThread,
        chainparams.NetworkIDString())));
    return true;
}

void InterruptZebraCompatNode()
{
    InterruptZebraCompatWorker();
}

void StopZebraCompatNode()
{
    InterruptZebraCompatNode();
    JoinZebraCompatWorker();
    if (g_zebra_compat_started.exchange(false)) {
        LogPrintf("zebra-compat node stopped\n");
    }
    StopZebraCompatMetadata();
    {
        LOCK(cs_zebra_compat_status);
        g_status.serviceState = "stopped";
    }
}

void RecordBlockIngestionResult(const BlockIngestionResult& result)
{
    LOCK(cs_zebra_compat_status);
    g_status.lastIngestion = result;
    if (result.success) {
        g_status.syncState = "degraded";
        g_status.syncDetail = "last_block_ingested";
        g_status.lastError.clear();
    } else {
        g_status.tipMatchedZebra = false;
        g_status.syncState = result.hardFailure ? "failed" : "degraded";
        g_status.syncDetail = result.hardFailure ? "hard_sync_fault" : "block_ingestion_error";
        g_status.lastError = result.error;
    }
}

UniValue GetZebraCompatInfo()
{
    UniValue obj(UniValue::VOBJ);
    const bool enabled = IsEnabled();
    const std::string zebraUrl = GetArg("-zebra-compat-url", "");
    ZebraCompatStatus status;
    {
        LOCK(cs_zebra_compat_status);
        status = g_status;
    }
    const bool serviceStarted = g_zebra_compat_started.load();
    const MempoolMirrorStatus mirrorStatus = GetMempoolMirrorStatus();
    const TxForwardingStatus txStatus = GetTxForwardingStatus();

    obj.pushKV("enabled", enabled);
    obj.pushKV("service_state", enabled ? (serviceStarted ? status.serviceState : "stopped") : "disabled");
    obj.pushKV("blocksource", GetArg("-blocksource", BLOCK_SOURCE_P2P));
    obj.pushKV("p2p", IsP2PEnabled());
    obj.pushKV("blockvalidation", GetArg("-blockvalidation", BLOCK_VALIDATION_FULL));

    UniValue zebra(UniValue::VOBJ);
    zebra.pushKV("configured", !zebraUrl.empty());
    zebra.pushKV("reachable", status.zebra.reachable);
    zebra.pushKV("identity_verified", status.zebra.identityVerified);
    zebra.pushKV("streaming", false);
    if (!zebraUrl.empty()) {
        zebra.pushKV("url", zebraUrl);
    }
    if (!status.zebra.network.empty()) {
        zebra.pushKV("network", status.zebra.network);
    }
    if (!status.zebra.genesisHash.empty()) {
        zebra.pushKV("genesis", status.zebra.genesisHash);
    }
    if (!status.zebra.bestBlockHash.empty()) {
        zebra.pushKV("bestblockhash", status.zebra.bestBlockHash);
    }
    if (status.zebra.blocks >= 0) {
        zebra.pushKV("blocks", status.zebra.blocks);
    }
    obj.pushKV("zebra", zebra);

    UniValue local(UniValue::VOBJ);
    bool initialBlockDownload = true;
    {
        LOCK(cs_main);
        local.pushKV("blocks", chainActive.Height());
        if (chainActive.Tip() != nullptr) {
            local.pushKV("bestblockhash", chainActive.Tip()->GetBlockHash().GetHex());
        } else {
            local.pushKV("bestblockhash", NullUniValue);
        }
        initialBlockDownload = IsInitialBlockDownload(Params().GetConsensus());
        local.pushKV("initial_block_download_complete", !initialBlockDownload);
    }
    const bool notificationsCaughtUp = ChainIsFullyNotified(Params());
    local.pushKV("validation_notifications_caught_up", notificationsCaughtUp);
    obj.pushKV("local", local);

    const int64_t now = GetTime();
    const bool mirrorReady = IsMempoolMirrorReady(mirrorStatus, now);
    const bool txForwardingTransportReady = txStatus.lastTransportError.empty();
    const std::string rawReadiness = serviceStarted || !enabled ?
        ComputeRawReadiness(
            enabled,
            status,
            initialBlockDownload,
            txForwardingTransportReady,
            mirrorReady,
            notificationsCaughtUp) :
        "degraded";
    const std::string readiness = ApplyReadinessHysteresis(rawReadiness, now, serviceStarted);
    obj.pushKV("readiness", readiness);

    UniValue ingestion(UniValue::VOBJ);
    ingestion.pushKV("last_success", status.lastIngestion.success);
    ingestion.pushKV("last_hard_failure", status.lastIngestion.hardFailure);
    if (status.lastIngestion.height >= 0) {
        ingestion.pushKV("last_height", status.lastIngestion.height);
    } else {
        ingestion.pushKV("last_height", NullUniValue);
    }
    if (!status.lastIngestion.hash.empty()) {
        ingestion.pushKV("last_hash", status.lastIngestion.hash);
    } else {
        ingestion.pushKV("last_hash", NullUniValue);
    }
    if (!status.lastIngestion.error.empty()) {
        ingestion.pushKV("last_error", status.lastIngestion.error);
    } else {
        ingestion.pushKV("last_error", NullUniValue);
    }
    obj.pushKV("ingestion", ingestion);

    UniValue trustedBoundary(UniValue::VOBJ);
    TrustedBlockBoundary boundary;
    if (ReadTrustedBlockBoundary(boundary) &&
        TrustedBoundaryMatchesConfiguredSource(boundary, Params())) {
        trustedBoundary.pushKV("active", true);
        trustedBoundary.pushKV("height", boundary.nHeight);
        trustedBoundary.pushKV("hash", boundary.hash.GetHex());
        trustedBoundary.pushKV("network", boundary.network);
        trustedBoundary.pushKV("genesis", boundary.genesisHash);
        trustedBoundary.pushKV("zebra_url", boundary.zebraEndpoint);
    } else {
        trustedBoundary.pushKV("active", false);
    }
    obj.pushKV("trusted_boundary", trustedBoundary);

    obj.pushKV("mempool_mirror", MempoolMirrorStatusToJSON());
    obj.pushKV("tx_forwarding", TxForwardingStatusToJSON());

    UniValue sync(UniValue::VOBJ);
    if (!enabled) {
        sync.pushKV("state", "disabled");
        sync.pushKV("detail", "zebra-compat mode is not enabled");
    } else if (zebraUrl.empty()) {
        sync.pushKV("state", "degraded");
        sync.pushKV("detail", "waiting_for_zebra_endpoint");
    } else {
        sync.pushKV("state", status.syncState);
        sync.pushKV("detail", status.syncDetail);
    }
    if (!status.lastError.empty()) {
        sync.pushKV("last_error", status.lastError);
    } else {
        sync.pushKV("last_error", NullUniValue);
    }
    if (status.syncTargetHeight >= 0) {
        sync.pushKV("target_height", status.syncTargetHeight);
    } else {
        sync.pushKV("target_height", NullUniValue);
    }
    if (!status.syncTargetHash.empty()) {
        sync.pushKV("target_hash", status.syncTargetHash);
    } else {
        sync.pushKV("target_hash", NullUniValue);
    }
    if (status.lastSyncedHeight >= 0) {
        sync.pushKV("last_synced_height", status.lastSyncedHeight);
    } else {
        sync.pushKV("last_synced_height", NullUniValue);
    }
    if (!status.lastSyncedHash.empty()) {
        sync.pushKV("last_synced_hash", status.lastSyncedHash);
    } else {
        sync.pushKV("last_synced_hash", NullUniValue);
    }
    if (status.lastCommonAncestorHeight >= 0) {
        sync.pushKV("last_common_ancestor_height", status.lastCommonAncestorHeight);
    } else {
        sync.pushKV("last_common_ancestor_height", NullUniValue);
    }
    if (!status.lastCommonAncestorHash.empty()) {
        sync.pushKV("last_common_ancestor_hash", status.lastCommonAncestorHash);
    } else {
        sync.pushKV("last_common_ancestor_hash", NullUniValue);
    }
    UniValue syncLag;
    if (status.syncTargetHeight >= 0) {
        int lag = status.syncTargetHeight - std::max(status.lastSyncedHeight, -1);
        if (lag < 0) {
            lag = 0;
        }
        syncLag = UniValue(lag);
    } else {
        syncLag = NullUniValue;
    }
    sync.pushKV("lag", syncLag);
    sync.pushKV("sticky_fault", status.stickyFault);
    sync.pushKV("retry_requested", status.retryRequested);
    sync.pushKV("retry_count", status.consecutiveRetryCount);
    sync.pushKV("current_backoff_seconds", status.currentBackoffSeconds);
    if (status.nextRetryTime > 0) {
        sync.pushKV("next_retry", status.nextRetryTime);
    } else {
        sync.pushKV("next_retry", NullUniValue);
    }
    obj.pushKV("sync", sync);

    UniValue metrics(UniValue::VOBJ);
    metrics.pushKV("sync_lag", syncLag);
    metrics.pushKV("mempool_lag", mirrorStatus.lag);
    metrics.pushKV("mempool_ready", mirrorReady);
    if (mirrorStatus.lastUpdate > 0) {
        metrics.pushKV("mempool_last_update_age_seconds", now - mirrorStatus.lastUpdate);
    } else {
        metrics.pushKV("mempool_last_update_age_seconds", NullUniValue);
    }
    metrics.pushKV("mempool_divergent", static_cast<int64_t>(mirrorStatus.divergent));
    metrics.pushKV("tx_forwarding_pending", static_cast<int64_t>(txStatus.pending));
    metrics.pushKV("tx_forwarding_transport_ready", txForwardingTransportReady);
    metrics.pushKV("validation_notifications_caught_up", notificationsCaughtUp);
    metrics.pushKV("retry_count", status.consecutiveRetryCount);
    metrics.pushKV("current_backoff_seconds", status.currentBackoffSeconds);
    obj.pushKV("metrics", metrics);

    UniValue limits(UniValue::VOBJ);
    limits.pushKV("poll_interval_seconds", ZebraCompatPollIntervalSeconds());
    limits.pushKV("max_retry_backoff_seconds", MAX_ZEBRA_COMPAT_RETRY_BACKOFF_SECONDS);
    limits.pushKV("sync_batch_size", ZebraCompatSyncBatchSize());
    limits.pushKV("forward_drive_batches", ZebraCompatForwardDriveBatches());
    limits.pushKV("zebra_rpc_timeout_seconds", ZebraCompatTimeoutSeconds());
    limits.pushKV("zebra_rpc_max_response_body_bytes", static_cast<int64_t>(ZebraRpcMaxResponseBodySize()));
    limits.pushKV("mempool_txids_per_poll", static_cast<int64_t>(MaxMempoolMirrorTxIdsPerPoll()));
    limits.pushKV("mempool_divergence_details", static_cast<int64_t>(MaxMempoolMirrorDivergenceDetails()));
    limits.pushKV("pending_forwarded_transactions", static_cast<int64_t>(MaxPendingForwardedTransactions()));
    obj.pushKV("limits", limits);

    return obj;
}

// Requests a single sync retry from a parked sticky fault. The request is
// best-effort and observable through `getzebracompatinfo` until the worker consumes it.
ZebraCompatRetryResult RetryZebraCompatStickyFault()
{
    ZebraCompatRetryResult result;
    result.workerRunning = g_zebra_compat_started.load();
    bool shouldNotify = false;
    {
        LOCK(cs_zebra_compat_status);
        result.stickyFault = g_status.stickyFault;
        result.state = g_status.syncState;
        result.detail = g_status.syncDetail;
        if (result.workerRunning && g_status.stickyFault) {
            g_status.retryRequested = true;
            g_status.stickyFault = false;
            g_status.consecutiveRetryCount = 0;
            g_status.currentBackoffSeconds = 0;
            g_status.nextRetryTime = 0;
            result.retryRequested = true;
            shouldNotify = true;
        }
    }

    if (shouldNotify) {
        WakeZebraCompatRetryWaiters();
    }
    return result;
}

void ThrowIfP2PDisabled(const std::string& method)
{
    if (!IsP2PEnabled()) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("%s is unavailable when Zcash P2P is disabled by zebra-compat mode", method));
    }
}

void ThrowIfMiningDisabled(const std::string& method)
{
    if (IsEnabled()) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("%s is unavailable in zebra-compat mode", method));
    }
}

ZebraCompatSyncTestOutcome TEST_ValidatePostIngestionTipOnZebraBestChain(
    ZebraCompatClient& client,
    const CChainParams& chainparams,
    int localTipHeight,
    const std::string& localTipHash,
    int expectedHeight,
    const std::string& expectedHash,
    const std::string& mismatchError,
    const std::string& offChainDetail,
    bool reorgContext)
{
    LocalTipSnapshot localTip;
    localTip.height = localTipHeight;
    localTip.hash = localTipHash;

    SyncOutcome outcome = ValidatePostIngestionTipOnZebraBestChain(
        client,
        chainparams,
        localTip,
        expectedHeight,
        expectedHash,
        mismatchError,
        offChainDetail,
        reorgContext);

    ZebraCompatSyncTestOutcome testOutcome;
    testOutcome.progressed = outcome.progressed;
    testOutcome.stickyFault = outcome.stickyFault;
    testOutcome.transientFailure = outcome.transientFailure;
    RecordStickyFault(outcome.stickyFault);
    return testOutcome;
}

ZebraCompatSyncTestOutcome TEST_SyncZebraTipBelowReorgWindow(
    ZebraCompatClient& client,
    const CChainParams& chainparams,
    int localTipHeight,
    const std::string& localTipHash,
    int zebraBestHeight,
    const std::string& zebraBestHash,
    bool haveLocalHashAtZebraHeight,
    const std::string& localHashAtZebraHeight)
{
    LocalTipSnapshot localTip;
    localTip.height = localTipHeight;
    localTip.hash = localTipHash;

    CommonAncestorSearchResult ancestor = ClassifyZebraTipBelowReorgWindow(
        localTip,
        zebraBestHeight,
        zebraBestHash,
        haveLocalHashAtZebraHeight,
        localHashAtZebraHeight);
    SyncOutcome outcome = SyncZebraCompatReorgToZebraBest(
        client,
        chainparams,
        localTip,
        zebraBestHeight,
        zebraBestHash,
        ancestor);

    ZebraCompatSyncTestOutcome testOutcome;
    testOutcome.progressed = outcome.progressed;
    testOutcome.stickyFault = outcome.stickyFault;
    testOutcome.transientFailure = outcome.transientFailure;
    RecordStickyFault(outcome.stickyFault);
    return testOutcome;
}

ZebraCompatSyncTestOutcome TEST_SyncZebraCompatReorgToZebraBest(
    ZebraCompatClient& client,
    const CChainParams& chainparams,
    int localTipHeight,
    const std::string& localTipHash,
    int zebraBestHeight,
    const std::string& zebraBestHash,
    int ancestorHeight,
    const std::string& ancestorHash,
    int disconnectLength)
{
    LocalTipSnapshot localTip;
    localTip.height = localTipHeight;
    localTip.hash = localTipHash;

    CommonAncestorSearchResult ancestor;
    ancestor.found = true;
    ancestor.height = ancestorHeight;
    ancestor.hash = ancestorHash;
    ancestor.disconnectLength = disconnectLength;

    SyncOutcome outcome = SyncZebraCompatReorgToZebraBest(
        client,
        chainparams,
        localTip,
        zebraBestHeight,
        zebraBestHash,
        ancestor);

    ZebraCompatSyncTestOutcome testOutcome;
    testOutcome.progressed = outcome.progressed;
    testOutcome.stickyFault = outcome.stickyFault;
    testOutcome.transientFailure = outcome.transientFailure;
    return testOutcome;
}

bool TEST_LoadZebraClientConfigForWorker(bool& stickyFault)
{
    ZebraClientConfig config;
    return LoadZebraClientConfigForWorker(config, stickyFault);
}

ZebraCompatSyncTestOutcome TEST_SyncZebraCompatOnce(
    ZebraCompatClient& client,
    ZebraCompatClient& prefetchClient,
    const CChainParams& chainparams)
{
    SyncOutcome outcome = SyncZebraCompatOnce(client, prefetchClient, chainparams);

    ZebraCompatSyncTestOutcome testOutcome;
    testOutcome.progressed = outcome.progressed;
    testOutcome.stickyFault = outcome.stickyFault;
    testOutcome.transientFailure = outcome.transientFailure;
    RecordStickyFault(outcome.stickyFault);
    return testOutcome;
}

void TEST_ResetReadinessHysteresis()
{
    LOCK(cs_zebra_compat_status);
    g_status.readinessWasReady = false;
    g_status.readinessDegradedSince = 0;
}

void TEST_SetZebraCompatStatusForReadiness(
    bool identityVerified,
    bool tipMatchedZebra,
    const std::string& serviceState,
    const std::string& syncState)
{
    LOCK(cs_zebra_compat_status);
    g_status.serviceState = serviceState;
    g_status.syncState = syncState;
    g_status.zebra.identityVerified = identityVerified;
    g_status.tipMatchedZebra = tipMatchedZebra;
}

std::string TEST_ComputeZebraCompatReadiness(
    bool enabled,
    bool initialBlockDownload,
    bool txForwardingTransportReady,
    const MempoolMirrorStatus& mirrorStatus,
    bool notificationsCaughtUp,
    int64_t now)
{
    ZebraCompatStatus status;
    {
        LOCK(cs_zebra_compat_status);
        status = g_status;
    }
    const std::string rawReadiness = ComputeRawReadiness(
        enabled,
        status,
        initialBlockDownload,
        txForwardingTransportReady,
        IsMempoolMirrorReady(mirrorStatus, now),
        notificationsCaughtUp);
    return ApplyReadinessHysteresis(rawReadiness, now, /*allowHysteresis=*/true);
}

void TEST_SetZebraCompatStickyFault(bool stickyFault)
{
    RecordStickyFault(stickyFault);
}

void TEST_SetZebraCompatStarted(bool started)
{
    g_zebra_compat_started = started;
}

} // namespace zebra_compat
