// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_UNITY_TX_FORWARDER_H
#define BITCOIN_UNITY_TX_FORWARDER_H

#include "uint256.h"

#include <cstddef>
#include <stdint.h>

#include <string>

class UniValue;

namespace unity {

class UnityZebraClient;

struct TxForwardingResult {
    bool success = false;
    int rpcErrorCode = 0;
    std::string txid;
    std::string error;
};

struct TxForwardingStatus {
    int64_t lastSuccess = 0;
    std::string lastError;
    std::string lastTransportError;
    size_t pending = 0;
};

TxForwardingResult ForwardRawTransaction(
    UnityZebraClient& client,
    const std::string& txHex,
    const uint256& expectedTxId);
TxForwardingResult ForwardRawTransaction(
    const std::string& txHex,
    const uint256& expectedTxId);

void RecordForwardedTransaction(const uint256& txid);
bool ShouldKeepForwardedTransaction(const std::string& txid);
void MarkForwardedTransactionObserved(const std::string& txid);
void ExpireForwardedTransactions();

TxForwardingStatus GetTxForwardingStatus();
UniValue TxForwardingStatusToJSON();
void ResetTxForwardingForTesting();
size_t PendingForwardedOrderSizeForTesting();
size_t MaxPendingForwardedTransactions();

} // namespace unity

#endif // BITCOIN_UNITY_TX_FORWARDER_H
