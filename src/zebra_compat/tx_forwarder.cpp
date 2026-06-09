// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "zebra_compat/tx_forwarder.h"

#include "rpc/protocol.h"
#include "sync.h"
#include "zebra_compat/zebra_client.h"
#include "util/system.h"
#include "util/time.h"

#include <deque>
#include <map>
#include <memory>
#include <stdexcept>

#include <univalue.h>

namespace unity {
namespace {

CCriticalSection cs_tx_forwarding;
TxForwardingStatus g_tx_forwarding_status;
std::map<std::string, int64_t> g_pending_forwarded_transactions;
std::deque<std::string> g_pending_forwarded_order;

static const size_t MAX_PENDING_FORWARDED_TXIDS = 1024;
static const int64_t FORWARDED_TX_GRACE_SECONDS = 10 * 60;

int MapZebraRpcErrorCode(int zebraCode)
{
    // Zebra's legacy compatibility RPC codes are intentionally drawn from
    // zcashd's RPCErrorCode values. Pass through the transaction/deserialize
    // classes we can map exactly, and treat any unexpected Zebra JSON-RPC
    // error object from sendrawtransaction as a transaction rejection rather
    // than a transport failure.
    switch (zebraCode) {
    case RPC_DESERIALIZATION_ERROR:
    case RPC_TRANSACTION_ERROR:
    case RPC_TRANSACTION_REJECTED:
    case RPC_TRANSACTION_ALREADY_IN_CHAIN:
        return zebraCode;
    default:
        return RPC_TRANSACTION_REJECTED;
    }
}

TxForwardingResult Failure(int rpcErrorCode, const std::string& error, bool transportFailure)
{
    TxForwardingResult result;
    result.rpcErrorCode = rpcErrorCode;
    result.error = error;

    LOCK(cs_tx_forwarding);
    g_tx_forwarding_status.lastError = error;
    if (transportFailure) {
        g_tx_forwarding_status.lastTransportError = error;
    }
    LogPrintf(
        "Unity transaction forwarding failed: rpc_error_code=%d error=\"%s\" pending=%d\n",
        rpcErrorCode,
        error.c_str(),
        static_cast<int>(g_tx_forwarding_status.pending));
    return result;
}

void PrunePendingForwardedTransactions(int64_t now)
{
    while (!g_pending_forwarded_order.empty()) {
        const std::string txid = g_pending_forwarded_order.front();
        auto it = g_pending_forwarded_transactions.find(txid);
        if (it == g_pending_forwarded_transactions.end()) {
            g_pending_forwarded_order.pop_front();
            continue;
        }
        if (it->second > now) {
            break;
        }
        g_pending_forwarded_transactions.erase(it);
        g_pending_forwarded_order.pop_front();
    }

    while (g_pending_forwarded_transactions.size() > MAX_PENDING_FORWARDED_TXIDS &&
           !g_pending_forwarded_order.empty()) {
        const std::string txid = g_pending_forwarded_order.front();
        g_pending_forwarded_order.pop_front();
        g_pending_forwarded_transactions.erase(txid);
        g_tx_forwarding_status.lastError =
            "Unity transaction forwarding grace set exceeded its bound; evicted oldest pending transaction";
        g_tx_forwarding_status.lastTransportError =
            g_tx_forwarding_status.lastError;
    }

    g_tx_forwarding_status.pending = g_pending_forwarded_transactions.size();
}

void RemovePendingForwardedOrderEntry(const std::string& txid)
{
    for (auto it = g_pending_forwarded_order.begin(); it != g_pending_forwarded_order.end(); ++it) {
        if (*it == txid) {
            g_pending_forwarded_order.erase(it);
            return;
        }
    }
}

void RecordSuccess()
{
    LOCK(cs_tx_forwarding);
    g_tx_forwarding_status.lastSuccess = GetTime();
    g_tx_forwarding_status.lastError.clear();
    g_tx_forwarding_status.lastTransportError.clear();
}

} // namespace

TxForwardingResult ForwardRawTransaction(
    UnityZebraClient& client,
    const std::string& txHex,
    const uint256& expectedTxId)
{
    try {
        const std::string zebraTxId = client.SendRawTransaction(txHex);
        if (zebraTxId != expectedTxId.GetHex()) {
            return Failure(
                RPC_TRANSACTION_ERROR,
                strprintf("Zebra returned txid %s for forwarded transaction %s",
                          zebraTxId, expectedTxId.GetHex()),
                false);
        }

        TxForwardingResult result;
        result.success = true;
        result.txid = zebraTxId;
        RecordSuccess();
        RecordForwardedTransaction(expectedTxId);
        return result;
    } catch (const ZebraRpcError& e) {
        const int rpcCode = e.HasRpcCode() ?
            MapZebraRpcErrorCode(e.RpcCode()) :
            RPC_CLIENT_NOT_CONNECTED;
        return Failure(rpcCode, e.what(), rpcCode == RPC_CLIENT_NOT_CONNECTED);
    } catch (const std::exception& e) {
        return Failure(RPC_CLIENT_NOT_CONNECTED, e.what(), true);
    }
}

TxForwardingResult ForwardRawTransaction(
    const std::string& txHex,
    const uint256& expectedTxId)
{
    ZebraClientConfig config;
    std::string error;
    if (!LoadZebraClientConfig(config, error)) {
        if (error == "waiting_for_zebra_endpoint") {
            error = "Unity Zebra endpoint is not configured";
        }
        return Failure(RPC_CLIENT_NOT_CONNECTED, error, true);
    }

    UnityZebraClient client(config, std::unique_ptr<ZebraRpcTransport>(new LibeventZebraRpcTransport()));
    return ForwardRawTransaction(client, txHex, expectedTxId);
}

void RecordForwardedTransaction(const uint256& txid)
{
    LOCK(cs_tx_forwarding);
    const std::string txidHex = txid.GetHex();
    const bool isNew = g_pending_forwarded_transactions.count(txidHex) == 0;
    const int64_t now = GetTime();
    g_pending_forwarded_transactions[txidHex] = now + FORWARDED_TX_GRACE_SECONDS;
    if (isNew) {
        g_pending_forwarded_order.push_back(txidHex);
    }
    PrunePendingForwardedTransactions(now);
}

bool ShouldKeepForwardedTransaction(const std::string& txid)
{
    LOCK(cs_tx_forwarding);
    PrunePendingForwardedTransactions(GetTime());
    return g_pending_forwarded_transactions.count(txid) > 0;
}

void MarkForwardedTransactionObserved(const std::string& txid)
{
    LOCK(cs_tx_forwarding);
    if (g_pending_forwarded_transactions.erase(txid) > 0) {
        RemovePendingForwardedOrderEntry(txid);
    }
    g_tx_forwarding_status.pending = g_pending_forwarded_transactions.size();
}

void ExpireForwardedTransactions()
{
    LOCK(cs_tx_forwarding);
    PrunePendingForwardedTransactions(GetTime());
}

TxForwardingStatus GetTxForwardingStatus()
{
    LOCK(cs_tx_forwarding);
    PrunePendingForwardedTransactions(GetTime());
    return g_tx_forwarding_status;
}

UniValue TxForwardingStatusToJSON()
{
    const TxForwardingStatus status = GetTxForwardingStatus();
    UniValue obj(UniValue::VOBJ);
    if (status.lastSuccess > 0) {
        obj.pushKV("last_success", status.lastSuccess);
    } else {
        obj.pushKV("last_success", NullUniValue);
    }
    if (!status.lastError.empty()) {
        obj.pushKV("last_error", status.lastError);
    } else {
        obj.pushKV("last_error", NullUniValue);
    }
    if (!status.lastTransportError.empty()) {
        obj.pushKV("last_transport_error", status.lastTransportError);
    } else {
        obj.pushKV("last_transport_error", NullUniValue);
    }
    obj.pushKV("pending", static_cast<int64_t>(status.pending));
    return obj;
}

void ResetTxForwardingForTesting()
{
    LOCK(cs_tx_forwarding);
    g_tx_forwarding_status = TxForwardingStatus();
    g_pending_forwarded_transactions.clear();
    g_pending_forwarded_order.clear();
}

size_t PendingForwardedOrderSizeForTesting()
{
    LOCK(cs_tx_forwarding);
    return g_pending_forwarded_order.size();
}

size_t MaxPendingForwardedTransactions()
{
    return MAX_PENDING_FORWARDED_TXIDS;
}

} // namespace unity
