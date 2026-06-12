// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_ZEBRA_COMPAT_TX_FORWARDER_H
#define ZCASH_ZEBRA_COMPAT_TX_FORWARDER_H

#include "uint256.h"

#include <cstddef>
#include <stdint.h>

#include <string>

class UniValue;

namespace zebra_compat {

class ZebraCompatClient;

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

// Forwards raw transaction hex through an existing Zebra client and verifies
// Zebra returns the expected txid before recording forwarding success.
TxForwardingResult ForwardRawTransaction(
    ZebraCompatClient& client,
    const std::string& txHex,
    const uint256& expectedTxId);

// Loads Zebra RPC configuration, forwards raw transaction hex, and records
// transport/configuration failures in forwarding status.
TxForwardingResult ForwardRawTransaction(
    const std::string& txHex,
    const uint256& expectedTxId);

// Adds or refreshes a transaction in the grace set retained until Zebra's
// mempool mirror observes it or the grace window expires.
void RecordForwardedTransaction(const uint256& txid);

// Returns whether a local transaction absent from Zebra should be retained
// because it was recently forwarded.
bool ShouldKeepForwardedTransaction(const std::string& txid);

// Removes a forwarded transaction from the pending grace set after Zebra
// reports it in its mempool.
void MarkForwardedTransactionObserved(const std::string& txid);

// Expires old forwarded transactions and updates forwarding status counters.
void ExpireForwardedTransactions();

// Clears the transport-readiness latch after independent Zebra health checks recover.
void ClearTxForwardingTransportError();

// Returns the latest transaction forwarding status snapshot.
TxForwardingStatus GetTxForwardingStatus();

// Serializes forwarding status for `getzebracompatinfo`.
UniValue TxForwardingStatusToJSON();

// Clears forwarding status and pending transaction state for unit tests.
void ResetTxForwardingForTesting();

// Returns the pending-order deque size for tests of duplicate and pruning logic.
size_t PendingForwardedOrderSizeForTesting();

// Returns the hard cap on pending forwarded transaction IDs.
size_t MaxPendingForwardedTransactions();

} // namespace zebra_compat

#endif // ZCASH_ZEBRA_COMPAT_TX_FORWARDER_H
