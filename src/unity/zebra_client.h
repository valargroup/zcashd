// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_UNITY_ZEBRA_CLIENT_H
#define BITCOIN_UNITY_ZEBRA_CLIENT_H

#include "fs.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <univalue.h>

class CChainParams;

namespace unity {

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

    bool IsConfigured() const;
    std::string BasicAuthHeader() const;
};

struct ZebraClientConfig {
    ZebraEndpoint endpoint;
    ZebraAuth auth;
    int timeoutSeconds = 30;
};

struct ZebraRpcResponse {
    int httpStatus = 0;
    std::string body;
    std::string transportError;
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
    virtual ZebraRpcResponse Call(
        const ZebraClientConfig& config,
        const std::string& method,
        const UniValue& params) = 0;
    virtual ZebraRpcResponse CallBatch(
        const ZebraClientConfig& config,
        const std::vector<ZebraRpcCall>& calls) = 0;
};

class ZebraRpcError : public std::runtime_error {
public:
    ZebraRpcError(
        const std::string& message,
        int httpStatus,
        bool hasRpcCode,
        int rpcCode);

    int HttpStatus() const;
    bool HasRpcCode() const;
    int RpcCode() const;

private:
    int httpStatus;
    bool hasRpcCode;
    int rpcCode;
};

class LibeventZebraRpcTransport : public ZebraRpcTransport {
public:
    ZebraRpcResponse Call(
        const ZebraClientConfig& config,
        const std::string& method,
        const UniValue& params) override;
    ZebraRpcResponse CallBatch(
        const ZebraClientConfig& config,
        const std::vector<ZebraRpcCall>& calls) override;

private:
    ZebraRpcResponse CallJsonRpc(
        const ZebraClientConfig& config,
        const std::string& requestBody);
};

class UnityZebraClient {
public:
    UnityZebraClient(ZebraClientConfig config, std::unique_ptr<ZebraRpcTransport> transport);

    ZebraBlockchainInfo GetBlockchainInfo();
    std::string GetBestBlockHash();
    int GetBlockCount();
    std::string GetBlockHash(int height);
    std::vector<std::string> GetBlockHashes(int startHeight, int endHeight);
    std::string GetRawBlock(const std::string& hash);
    std::vector<std::string> GetRawBlocks(const std::vector<std::string>& hashes);
    std::vector<std::string> GetRawMempool();
    ZebraMempoolInfo GetMempoolInfo();
    std::string GetRawTransaction(const std::string& txid);
    std::vector<std::string> GetRawTransactions(const std::vector<std::string>& txids);
    std::string SendRawTransaction(const std::string& txHex);
    ZebraIdentity CheckIdentity(const CChainParams& chainparams);

private:
    UniValue CallRpc(const std::string& method, const UniValue& params);
    std::vector<UniValue> CallRpcBatch(const std::vector<ZebraRpcCall>& calls);

    ZebraClientConfig config;
    std::unique_ptr<ZebraRpcTransport> transport;
};

bool ParseZebraEndpoint(const std::string& url, ZebraEndpoint& endpoint, std::string& error);
bool LoadZebraClientConfig(ZebraClientConfig& config, std::string& error);
int UnitySyncBatchSize();
size_t ZebraRpcMaxResponseBodySize();

} // namespace unity

#endif // BITCOIN_UNITY_ZEBRA_CLIENT_H
