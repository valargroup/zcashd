// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "zebra_compat/mempool_mirror.h"

#include "chainparams.h"
#include "core_io.h"
#include "main.h"
#include "primitives/transaction.h"
#include "sync.h"
#include "txmempool.h"
#include "zebra_compat/tx_forwarder.h"
#include "zebra_compat/zebra_client.h"
#include "uint256.h"
#include "util/strencodings.h"
#include "util/time.h"

#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include <univalue.h>

namespace unity {
namespace {

CCriticalSection cs_mempool_mirror;
MempoolMirrorStatus g_mempool_mirror_status;
std::map<std::string, std::string> g_divergent_transactions;

static const size_t MAX_MIRROR_TXIDS_PER_POLL = 1024;
static const size_t MAX_DEFERRED_TXIDS = MAX_MIRROR_TXIDS_PER_POLL;
static const size_t MAX_DEFERRED_PASSES = 2;
static const size_t MAX_DIVERGENCE_DETAILS = 128;

bool IsValidTxIdHex(const std::string& value)
{
    return value.size() == 64 && IsHex(value);
}

std::set<std::string> ToTxIdSet(const std::vector<std::string>& txids)
{
    std::set<std::string> result;
    for (const std::string& txid : txids) {
        if (!IsValidTxIdHex(txid)) {
            throw std::runtime_error("Zebra RPC getrawmempool returned an invalid transaction id");
        }
        result.insert(txid);
    }
    return result;
}

std::set<std::string> LocalMempoolTxIds()
{
    std::vector<uint256> hashes;
    mempool.queryHashes(hashes);

    std::set<std::string> result;
    for (const uint256& hash : hashes) {
        result.insert(hash.GetHex());
    }
    return result;
}

bool DecodeMirroredTransaction(
    const std::string& expectedTxId,
    const std::string& rawTx,
    CTransaction& tx,
    std::string& error)
{
    if (!IsHex(rawTx)) {
        error = "raw transaction is not hex";
        return false;
    }

    try {
        DecodeHexTx(tx, rawTx);
    } catch (const std::exception& e) {
        error = std::string("transaction decode failed: ") + e.what();
        return false;
    }

    const std::string decodedTxId = tx.GetHash().GetHex();
    if (decodedTxId != expectedTxId) {
        error = strprintf("raw transaction hash mismatch: expected %s, decoded %s", expectedTxId, decodedTxId);
        return false;
    }
    return true;
}

bool MissingZebraMempoolParent(const CTransaction& tx, const std::set<std::string>& zebraTxIds)
{
    for (const CTxIn& input : tx.vin) {
        const std::string parent = input.prevout.hash.GetHex();
        if (zebraTxIds.count(parent) && !mempool.exists(input.prevout.hash)) {
            return true;
        }
    }
    return false;
}

void RemoveTransactionsNotInZebra(const std::set<std::string>& zebraTxIds, int& removed)
{
    ExpireForwardedTransactions();
    for (const std::string& txid : zebraTxIds) {
        MarkForwardedTransactionObserved(txid);
    }

    const std::set<std::string> localTxIds = LocalMempoolTxIds();
    for (const std::string& txid : localTxIds) {
        if (zebraTxIds.count(txid)) {
            continue;
        }
        if (ShouldKeepForwardedTransaction(txid)) {
            LogPrint("mempool", "Unity mempool mirror retained forwarded txid %s pending Zebra mempool observation\n", txid);
            continue;
        }

        std::shared_ptr<const CTransaction> tx = mempool.get(uint256S(txid));
        if (!tx) {
            continue;
        }

        std::list<CTransaction> removedTxs;
        mempool.remove(*tx, removedTxs, true);
        removed += removedTxs.size();
        LogPrint("mempool", "Unity mempool mirror removed txid %s absent from Zebra mempool\n", txid);
    }
}

void ResetDivergenceSamples()
{
    AssertLockHeld(cs_mempool_mirror);
    g_divergent_transactions.clear();
}

void RecordDivergence(const std::string& txid, const std::string& reason)
{
    LOCK(cs_mempool_mirror);
    if (g_divergent_transactions.count(txid)) {
        g_divergent_transactions[txid] = reason;
    } else if (g_divergent_transactions.size() < MAX_DIVERGENCE_DETAILS) {
        g_divergent_transactions.insert(std::make_pair(txid, reason));
    }
    LogPrintf("Unity mempool mirror divergence for %s: %s\n", txid, reason);
}

void ClearDivergence(const std::string& txid)
{
    LOCK(cs_mempool_mirror);
    g_divergent_transactions.erase(txid);
}

void UpdateMirrorStatus(
    const std::set<std::string>& zebraTxIds,
    int zebraReportedSize,
    size_t divergent,
    const std::string& error)
{
    MempoolMirrorStatus status;
    status.source = "zebra-poll";
    status.lastUpdate = GetTime();
    status.zebraSize = zebraReportedSize >= 0 ? zebraReportedSize : static_cast<int>(zebraTxIds.size());
    status.localSize = mempool.size();
    status.divergent = divergent;
    status.lastError = error;

    const int localMissing = std::max<int>(0, status.zebraSize - status.localSize);
    const int localExtra = std::max<int>(0, status.localSize - status.zebraSize);
    const size_t explainedMissing = std::min<size_t>(static_cast<size_t>(localMissing), status.divergent);
    status.lag = static_cast<int>(static_cast<size_t>(localMissing) - explainedMissing) + localExtra;

    LOCK(cs_mempool_mirror);
    status.divergentDetails = g_divergent_transactions.size();
    status.divergentDetailOverflow = status.divergent > status.divergentDetails ?
        status.divergent - status.divergentDetails : 0;
    g_mempool_mirror_status = status;
}

void UpdateMirrorFailure(const std::string& error)
{
    LOCK(cs_mempool_mirror);
    g_mempool_mirror_status.source = "zebra-poll";
    g_mempool_mirror_status.lastFailure = GetTime();
    g_mempool_mirror_status.lastError = error;
}

void AppendDeferred(const std::string& txid, std::vector<std::string>& deferred, MempoolMirrorResult& result)
{
    if (deferred.size() < MAX_DEFERRED_TXIDS) {
        deferred.push_back(txid);
    } else {
        RecordDivergence(txid, "too many deferred Zebra mempool dependencies");
        result.divergent++;
    }
}

void ApplyDecodedTransactions(
    const std::map<std::string, CTransaction>& decoded,
    const std::set<std::string>& zebraTxIds,
    const CChainParams& chainparams,
    std::vector<std::string>& deferred,
    MempoolMirrorResult& result)
{
    for (const auto& entry : decoded) {
        const std::string& txid = entry.first;
        const CTransaction& tx = entry.second;

        CValidationState state;
        bool missingInputs = false;
        bool accepted = false;
        bool alreadyInMempool = false;
        bool defer = false;
        std::string rejectReason;
        {
            LOCK(cs_main);
            if (mempool.exists(tx.GetHash())) {
                accepted = true;
                alreadyInMempool = true;
            } else if (MissingZebraMempoolParent(tx, zebraTxIds)) {
                defer = true;
            } else if (AcceptToMemoryPool(chainparams, mempool, state, tx, true, &missingInputs, false)) {
                accepted = true;
            } else if (missingInputs) {
                rejectReason = "missing inputs";
            } else {
                rejectReason = FormatStateMessage(state);
                if (rejectReason.empty()) {
                    rejectReason = "local mempool policy rejected transaction";
                }
            }
        }

        if (defer) {
            AppendDeferred(txid, deferred, result);
            continue;
        }

        if (accepted) {
            ClearDivergence(txid);
            if (!alreadyInMempool) {
                result.added++;
            }
        } else {
            RecordDivergence(txid, rejectReason);
            result.divergent++;
        }
    }
}

void ProcessTransactionIdChunks(
    UnityZebraClient& client,
    const std::vector<std::string>& txids,
    const std::set<std::string>& zebraTxIds,
    const CChainParams& chainparams,
    std::vector<std::string>& deferred,
    MempoolMirrorResult& result)
{
    const size_t batchSize = static_cast<size_t>(UnitySyncBatchSize());
    for (size_t start = 0; start < txids.size(); start += batchSize) {
        const size_t end = std::min(txids.size(), start + batchSize);
        const std::vector<std::string> chunk(txids.begin() + start, txids.begin() + end);
        const std::vector<std::string> rawTransactions = client.GetRawTransactions(chunk);
        if (rawTransactions.size() != chunk.size()) {
            throw std::runtime_error("Zebra returned mismatched raw transaction batch size");
        }

        std::map<std::string, CTransaction> decoded;
        for (size_t i = 0; i < chunk.size(); i++) {
            CTransaction tx;
            std::string decodeError;
            if (!DecodeMirroredTransaction(chunk[i], rawTransactions[i], tx, decodeError)) {
                RecordDivergence(chunk[i], decodeError);
                result.divergent++;
                continue;
            }
            decoded[chunk[i]] = tx;
        }
        ApplyDecodedTransactions(decoded, zebraTxIds, chainparams, deferred, result);
    }
}

} // namespace

MempoolMirrorResult SyncMempoolMirrorOnce(UnityZebraClient& client, const CChainParams& chainparams)
{
    MempoolMirrorResult result;

    try {
        const std::vector<std::string> zebraRawMempool = client.GetRawMempool();
        const ZebraMempoolInfo zebraInfo = client.GetMempoolInfo();
        const std::set<std::string> zebraTxIds = ToTxIdSet(zebraRawMempool);

        {
            LOCK(cs_mempool_mirror);
            ResetDivergenceSamples();
        }

        std::vector<std::string> missingTxIds;
        std::set<std::string> localTxIdsSnapshot;
        size_t missingCount = 0;
        {
            LOCK(cs_main);
            RemoveTransactionsNotInZebra(zebraTxIds, result.removed);

            const std::set<std::string> localTxIds = LocalMempoolTxIds();
            localTxIdsSnapshot = localTxIds;
            for (const std::string& txid : zebraTxIds) {
                if (!localTxIds.count(txid)) {
                    missingCount++;
                    if (missingTxIds.size() < MAX_MIRROR_TXIDS_PER_POLL) {
                        missingTxIds.push_back(txid);
                    }
                }
            }
        }

        if (missingCount > missingTxIds.size()) {
            result.divergent += missingCount - missingTxIds.size();
            size_t sampled = 0;
            bool pastReconciledPrefix = missingTxIds.empty();
            for (const std::string& txid : zebraTxIds) {
                if (!pastReconciledPrefix) {
                    pastReconciledPrefix = txid == missingTxIds.back();
                    continue;
                }
                if (localTxIdsSnapshot.count(txid)) {
                    continue;
                }
                RecordDivergence(txid, "not reconciled: Zebra mempool exceeds Unity mirror per-poll cap");
                sampled++;
                if (sampled >= MAX_DIVERGENCE_DETAILS) {
                    break;
                }
            }
        }

        std::vector<std::string> deferred;
        ProcessTransactionIdChunks(client, missingTxIds, zebraTxIds, chainparams, deferred, result);

        for (size_t pass = 0; pass < MAX_DEFERRED_PASSES && !deferred.empty(); pass++) {
            std::vector<std::string> retry;
            retry.swap(deferred);
            ProcessTransactionIdChunks(client, retry, zebraTxIds, chainparams, deferred, result);
        }

        for (const std::string& txid : deferred) {
            RecordDivergence(txid, "missing unaccepted Zebra mempool parent");
            result.divergent++;
        }

        std::string statusError;
        if (missingCount > missingTxIds.size()) {
            statusError = strprintf(
                "Zebra mempool has %d missing transactions; Unity reconciled %d this poll",
                static_cast<int>(missingCount),
                static_cast<int>(missingTxIds.size()));
        }
        UpdateMirrorStatus(zebraTxIds, zebraInfo.size, result.divergent, statusError);
        result.success = true;
        return result;
    } catch (const std::exception& e) {
        result.error = e.what();
        UpdateMirrorFailure(result.error);
        return result;
    }
}

MempoolMirrorStatus GetMempoolMirrorStatus()
{
    LOCK(cs_mempool_mirror);
    return g_mempool_mirror_status;
}

UniValue MempoolMirrorStatusToJSON()
{
    const MempoolMirrorStatus status = GetMempoolMirrorStatus();
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("source", status.source);
    obj.pushKV("lag", status.lag);
    if (status.lastUpdate > 0) {
        obj.pushKV("last_update", status.lastUpdate);
    } else {
        obj.pushKV("last_update", NullUniValue);
    }
    if (status.lastFailure > 0) {
        obj.pushKV("last_failure", status.lastFailure);
    } else {
        obj.pushKV("last_failure", NullUniValue);
    }
    obj.pushKV("divergent", static_cast<int64_t>(status.divergent));
    obj.pushKV("divergent_detail_sample_size", static_cast<int64_t>(status.divergentDetails));
    obj.pushKV("divergent_detail_overflow", static_cast<int64_t>(status.divergentDetailOverflow));
    if (status.zebraSize >= 0) {
        obj.pushKV("zebra_size", status.zebraSize);
    } else {
        obj.pushKV("zebra_size", NullUniValue);
    }
    obj.pushKV("local_size", status.localSize);
    if (!status.lastError.empty()) {
        obj.pushKV("last_error", status.lastError);
    } else {
        obj.pushKV("last_error", NullUniValue);
    }
    return obj;
}

void ResetMempoolMirrorForTesting()
{
    LOCK(cs_mempool_mirror);
    g_mempool_mirror_status = MempoolMirrorStatus();
    g_divergent_transactions.clear();
}

size_t MaxMempoolMirrorTxIdsPerPoll()
{
    return MAX_MIRROR_TXIDS_PER_POLL;
}

size_t MaxMempoolMirrorDivergenceDetails()
{
    return MAX_DIVERGENCE_DETAILS;
}

} // namespace unity
