// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_ZEBRA_COMPAT_ZEBRA_CLIENT_H
#define ZCASH_ZEBRA_COMPAT_ZEBRA_CLIENT_H

#include "fs.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <univalue.h>

class CChainParams;

namespace zebra_compat {

struct ZebraEndpoint {
    std::string url;
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path = "/";
};

struct ZebraAuth {
    std::string user;
    std::string password;

    // Returns true when both Basic auth fields are present.
    bool IsConfigured() const;

    // Builds the HTTP Basic Authorization header value from configured auth.
    std::string BasicAuthHeader() const;
};

struct ZebraClientConfig {
    ZebraEndpoint endpoint;
    ZebraAuth auth;
    int timeoutSeconds = 30;
};

struct ZebraRpcResponse {
    int httpStatus = 0;
    std::string body = {};
    std::string transportError = {};
};

struct ZebraRpcCall {
    std::string method;
    UniValue params;
};

struct ZebraBlockchainInfo {
    std::string network;
    int blocks = -1;
    std::string bestBlockHash;
};

struct ZebraMempoolInfo {
    int size = -1;
    int64_t bytes = -1;
    int64_t usage = -1;
};

struct ZebraIdentity {
    enum Failure {
        NONE,
        TRANSIENT,
        AUTHENTICATION,
        MALFORMED_RESPONSE,
        NETWORK_MISMATCH,
        GENESIS_MISMATCH,
    };

    bool reachable = false;
    bool identityVerified = false;
    Failure failure = NONE;
    std::string network;
    std::string genesisHash;
    std::string bestBlockHash;
    int blocks = -1;
    std::string lastError;
};

class ZebraRpcTransport {
public:
    virtual ~ZebraRpcTransport() {}

    // Sends one JSON-RPC method call. Implementations return HTTP and transport
    // details without interpreting the JSON-RPC response body.
    virtual ZebraRpcResponse Call(
        const ZebraClientConfig& config,
        const std::string& method,
        const UniValue& params) = 0;

    // Sends a JSON-RPC batch. The caller owns response validation and result
    // ordering checks.
    virtual ZebraRpcResponse CallBatch(
        const ZebraClientConfig& config,
        const std::vector<ZebraRpcCall>& calls) = 0;
};

class ZebraRpcError : public std::runtime_error {
public:
    enum Kind {
        TRANSPORT,
        AUTHENTICATION,
        HTTP_STATUS,
        RPC_ERROR,
        MALFORMED_RESPONSE,
    };

    // Creates an RPC-layer error with HTTP status and optional JSON-RPC code.
    ZebraRpcError(
        const std::string& message,
        int httpStatus,
        bool hasRpcCode,
        int rpcCode);

    // Creates a typed RPC-layer error with HTTP status and optional JSON-RPC code.
    ZebraRpcError(
        const std::string& message,
        Kind kind,
        int httpStatus,
        bool hasRpcCode,
        int rpcCode);

    // Returns the coarse error kind used by identity classification.
    Kind ErrorKind() const;

    // Returns the HTTP status associated with the RPC response, or zero when
    // the transport did not receive a response.
    int HttpStatus() const;

    // Returns true when the Zebra error object included a numeric RPC code.
    bool HasRpcCode() const;

    // Returns the Zebra RPC code when present, otherwise the stored default.
    int RpcCode() const;

private:
    Kind kind;
    int httpStatus;
    bool hasRpcCode;
    int rpcCode;
};

class LibeventZebraRpcTransport : public ZebraRpcTransport {
public:
    // Sends one HTTP JSON-RPC POST using libevent and configured Basic auth.
    ZebraRpcResponse Call(
        const ZebraClientConfig& config,
        const std::string& method,
        const UniValue& params) override;

    // Sends one HTTP JSON-RPC batch POST. Throws if the batch is empty.
    ZebraRpcResponse CallBatch(
        const ZebraClientConfig& config,
        const std::vector<ZebraRpcCall>& calls) override;

private:
    // Executes a prepared JSON-RPC request body and returns raw response data.
    // Throws only for local setup or request-submission failures.
    ZebraRpcResponse CallJsonRpc(
        const ZebraClientConfig& config,
        const std::string& requestBody);
};

class ZebraCompatClient {
public:
    // Creates a Zebra RPC client. Throws if `transport` is null.
    ZebraCompatClient(ZebraClientConfig config, std::unique_ptr<ZebraRpcTransport> transport);

    // Returns Zebra chain identity fields from `getblockchaininfo`.
    ZebraBlockchainInfo GetBlockchainInfo();

    // Returns Zebra's best block hash, validated as a 64-character hex string.
    std::string GetBestBlockHash();

    // Returns Zebra's best block height.
    int GetBlockCount();

    // Returns Zebra's best-chain block hash at `height`; `height` is passed to
    // Zebra and response shape is validated locally.
    std::string GetBlockHash(int height);

    // Returns best-chain block hashes for the inclusive height range. Empty
    // ranges return empty; negative start heights throw.
    std::vector<std::string> GetBlockHashes(int startHeight, int endHeight);

    // Returns raw block hex for `hash`; input and output must be hex.
    std::string GetRawBlock(const std::string& hash);

    // Returns raw block hex for each requested hash, chunked by the configured
    // sync batch size. Invalid hashes or non-hex responses throw.
    std::vector<std::string> GetRawBlocks(const std::vector<std::string>& hashes);

    // Returns Zebra mempool transaction IDs, each validated as 64-character hex.
    std::vector<std::string> GetRawMempool();

    // Returns Zebra mempool size metadata; `size` is required, byte fields are
    // optional and remain negative when absent.
    ZebraMempoolInfo GetMempoolInfo();

    // Returns raw transaction hex for `txid`; input and output must be hex.
    std::string GetRawTransaction(const std::string& txid);

    // Returns raw transaction hex for a batch no larger than the sync batch
    // size. Empty input returns empty.
    std::vector<std::string> GetRawTransactions(const std::vector<std::string>& txids);

    // Sends raw transaction hex to Zebra and returns the txid string Zebra
    // reports. Transaction-policy failures are surfaced as RPC errors.
    std::string SendRawTransaction(const std::string& txHex);

    // Verifies Zebra's network, genesis, best hash, and height against local
    // chain parameters. Transport and malformed-response errors are classified.
    ZebraIdentity CheckIdentity(const CChainParams& chainparams);

private:
    // Calls one RPC method and returns the non-null `result` field. Throws for
    // transport, HTTP, JSON-RPC, malformed, or oversized responses.
    UniValue CallRpc(const std::string& method, const UniValue& params);

    // Calls an RPC batch and returns results in request order. Throws for
    // missing IDs, JSON-RPC errors, malformed, or oversized responses.
    std::vector<UniValue> CallRpcBatch(const std::vector<ZebraRpcCall>& calls);

    ZebraClientConfig config;
    std::unique_ptr<ZebraRpcTransport> transport;
};

// Parses an http:// Zebra RPC URL into endpoint fields. HTTPS and missing hosts
// are rejected with `error` populated.
bool ParseZebraEndpoint(const std::string& url, ZebraEndpoint& endpoint, std::string& error);

// Loads endpoint, auth, and timeout settings from command-line arguments.
// Returns false with `error` for missing URL, invalid auth, or cookie failures.
bool LoadZebraClientConfig(ZebraClientConfig& config, std::string& error);

// Returns the effective block acquisition batch size after memory-budget clamps.
int ZebraCompatSyncBatchSize();

// Returns the Zebra RPC timeout in seconds.
int ZebraCompatTimeoutSeconds();

// Returns the maximum accepted Zebra RPC response body in bytes.
size_t ZebraRpcMaxResponseBodySize();

} // namespace zebra_compat

#endif // ZCASH_ZEBRA_COMPAT_ZEBRA_CLIENT_H
