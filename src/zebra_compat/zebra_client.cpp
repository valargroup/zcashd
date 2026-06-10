// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "zebra_compat/zebra_client.h"

#include "chainparams.h"
#include "consensus/consensus.h"
#include "httpserver.h"
#include "netbase.h"
#include "rpc/protocol.h"
#include "uint256.h"
#include "util/strencodings.h"
#include "util/system.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

namespace unity {
namespace {

const char* const UNITY_JSONRPC_ID = "unity";
const size_t ZEBRA_RPC_RESPONSE_BODY_MARGIN = 1024 * 1024;
const int DEFAULT_UNITY_SYNC_BATCH_SIZE = 8;
const int MAX_UNITY_SYNC_BATCH_SIZE_BY_COUNT = 1000;
const int DEFAULT_UNITY_SYNC_RESPONSE_BUDGET_MB = 128;

// Upper bound on the cumulative size of one getblock batch response. This caps how
// many blocks may be requested per Zebra round-trip (see UnitySyncBatchSize). It is
// runtime-tunable via -zebra-compat-sync-response-budget-mb so the acquisition batch can be
// scaled for throughput experiments without a rebuild; the Zebra server's own
// max_response_body_size must be configured at least as large.
size_t UnitySyncRawBlockResponseBudget()
{
    int64_t megabytes = GetArg("-zebra-compat-sync-response-budget-mb", DEFAULT_UNITY_SYNC_RESPONSE_BUDGET_MB);
    if (megabytes < 1) {
        megabytes = 1;
    }
    return static_cast<size_t>(megabytes) * 1024 * 1024;
}

struct EventBaseDeleter {
    void operator()(event_base* base) const
    {
        event_base_free(base);
    }
};

struct EvhttpConnectionDeleter {
    void operator()(evhttp_connection* connection) const
    {
        evhttp_connection_free(connection);
    }
};

struct EvhttpRequestDeleter {
    void operator()(evhttp_request* request) const
    {
        evhttp_request_free(request);
    }
};

typedef std::unique_ptr<event_base, EventBaseDeleter> UniqueEventBase;
typedef std::unique_ptr<evhttp_connection, EvhttpConnectionDeleter> UniqueEvhttpConnection;
typedef std::unique_ptr<evhttp_request, EvhttpRequestDeleter> UniqueEvhttpRequest;

bool IsValidHashHex(const std::string& value)
{
    return value.size() == 64 && IsHex(value);
}

UniValue NoParams()
{
    return UniValue(UniValue::VARR);
}

UniValue OneParam(const UniValue& value)
{
    UniValue params(UniValue::VARR);
    params.push_back(value);
    return params;
}

UniValue TwoParams(const UniValue& first, const UniValue& second)
{
    UniValue params(UniValue::VARR);
    params.push_back(first);
    params.push_back(second);
    return params;
}

std::string RequireStringResult(const UniValue& result, const std::string& method)
{
    if (!result.isStr()) {
        throw std::runtime_error(strprintf("Zebra RPC %s returned a non-string result", method));
    }
    return result.get_str();
}

int RequireIntResult(const UniValue& result, const std::string& method)
{
    if (!result.isNum()) {
        throw std::runtime_error(strprintf("Zebra RPC %s returned a non-numeric result", method));
    }
    return result.get_int();
}

std::string RequireHashResult(const UniValue& result, const std::string& method)
{
    std::string hash = RequireStringResult(result, method);
    if (!IsValidHashHex(hash)) {
        throw std::runtime_error(strprintf("Zebra RPC %s returned an invalid block hash", method));
    }
    return hash;
}

std::string RequireTxIdResult(const UniValue& result, const std::string& method)
{
    std::string txid = RequireStringResult(result, method);
    if (!IsValidHashHex(txid)) {
        throw std::runtime_error(strprintf("Zebra RPC %s returned an invalid transaction id", method));
    }
    return txid;
}

ZebraIdentity::Failure ClassifyIdentityException(const std::string& error)
{
    if (error.find("authentication failed") != std::string::npos ||
        error.find("HTTP status 401") != std::string::npos ||
        error.find("HTTP status 403") != std::string::npos) {
        return ZebraIdentity::AUTHENTICATION;
    }

    if (error.find("malformed JSON") != std::string::npos ||
        error.find("malformed") != std::string::npos ||
        error.find("missing") != std::string::npos ||
        error.find("invalid") != std::string::npos ||
        error.find("non-object") != std::string::npos ||
        error.find("non-string") != std::string::npos ||
        error.find("non-numeric") != std::string::npos ||
        error.find("exceeded maximum size") != std::string::npos) {
        return ZebraIdentity::MALFORMED_RESPONSE;
    }

    return ZebraIdentity::TRANSIENT;
}

std::string MakeJsonRpcRequest(
    const std::string& method,
    const UniValue& params)
{
    return JSONRPCRequest(method, params, UniValue(UNITY_JSONRPC_ID));
}

std::string MakeJsonRpcBatchRequest(const std::vector<ZebraRpcCall>& calls)
{
    UniValue batch(UniValue::VARR);
    for (size_t i = 0; i < calls.size(); i++) {
        UniValue request(UniValue::VOBJ);
        request.pushKV("jsonrpc", "2.0");
        request.pushKV("method", calls[i].method);
        request.pushKV("params", calls[i].params);
        request.pushKV("id", strprintf("%s-%d", UNITY_JSONRPC_ID, i));
        batch.push_back(request);
    }
    return batch.write();
}

int UnitySyncBatchSizeFromMemoryBudget()
{
    const size_t budget = UnitySyncRawBlockResponseBudget();
    if (budget <= ZEBRA_RPC_RESPONSE_BODY_MARGIN) {
        return 1;
    }

    const size_t maxRawBlocks =
        (budget - ZEBRA_RPC_RESPONSE_BODY_MARGIN) /
        (2 * MAX_BLOCK_SIZE + 1024);
    return std::max<int>(1, std::min<int>(MAX_UNITY_SYNC_BATCH_SIZE_BY_COUNT, maxRawBlocks));
}

struct HttpCallContext {
    int status = 0;
    int error = -1;
    std::string body;
    bool bodyTooLarge = false;
};

const char* HttpErrorString(int code)
{
    switch (code) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    case EVREQ_HTTP_TIMEOUT:
        return "timeout reached";
    case EVREQ_HTTP_EOF:
        return "EOF reached";
    case EVREQ_HTTP_INVALID_HEADER:
        return "error while reading header, or invalid header";
    case EVREQ_HTTP_BUFFER_ERROR:
        return "error encountered while reading or writing";
    case EVREQ_HTTP_REQUEST_CANCEL:
        return "request was canceled";
    case EVREQ_HTTP_DATA_TOO_LONG:
        return "response body is larger than allowed";
#endif
    default:
        return "unknown";
    }
}

void HttpRequestDone(evhttp_request* req, void* arg)
{
    HttpCallContext* context = static_cast<HttpCallContext*>(arg);
    if (req == nullptr) {
        context->status = 0;
        return;
    }

    context->status = evhttp_request_get_response_code(req);
    evbuffer* input = evhttp_request_get_input_buffer(req);
    if (input != nullptr) {
        const size_t len = evbuffer_get_length(input);
        if (len > ZebraRpcMaxResponseBodySize()) {
            context->bodyTooLarge = true;
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
            context->error = EVREQ_HTTP_DATA_TOO_LONG;
#endif
            return;
        }
        context->body.resize(len);
        if (len > 0) {
            evbuffer_copyout(input, &context->body[0], len);
        }
    }
}

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
void HttpErrorCallback(enum evhttp_request_error err, void* arg)
{
    HttpCallContext* context = static_cast<HttpCallContext*>(arg);
    context->error = err;
}
#endif

} // namespace

ZebraRpcError::ZebraRpcError(
    const std::string& message,
    int httpStatusIn,
    bool hasRpcCodeIn,
    int rpcCodeIn) :
    std::runtime_error(message),
    httpStatus(httpStatusIn),
    hasRpcCode(hasRpcCodeIn),
    rpcCode(rpcCodeIn)
{
}

int ZebraRpcError::HttpStatus() const
{
    return httpStatus;
}

bool ZebraRpcError::HasRpcCode() const
{
    return hasRpcCode;
}

int ZebraRpcError::RpcCode() const
{
    return rpcCode;
}

size_t ZebraRpcMaxResponseBodySize()
{
    return (static_cast<size_t>(UnitySyncBatchSize()) * (2 * MAX_BLOCK_SIZE + 1024)) +
        ZEBRA_RPC_RESPONSE_BODY_MARGIN;
}

int UnitySyncBatchSize()
{
    int64_t configured = GetArg("-zebra-compat-sync-batch-size", DEFAULT_UNITY_SYNC_BATCH_SIZE);
    if (configured < 1) {
        configured = 1;
    }
    const int maxByMemory = UnitySyncBatchSizeFromMemoryBudget();
    if (configured > maxByMemory) {
        configured = maxByMemory;
    }
    return static_cast<int>(configured);
}

bool ZebraAuth::IsConfigured() const
{
    return !user.empty() && !password.empty();
}

std::string ZebraAuth::BasicAuthHeader() const
{
    return "Basic " + EncodeBase64(user + ":" + password);
}

bool ParseZebraEndpoint(const std::string& url, ZebraEndpoint& endpoint, std::string& error)
{
    endpoint = ZebraEndpoint();
    endpoint.url = url;

    const std::string httpPrefix = "http://";
    const std::string httpsPrefix = "https://";
    std::string rest;
    int defaultPort = 0;
    if (boost::algorithm::starts_with(url, httpPrefix)) {
        endpoint.scheme = "http";
        rest = url.substr(httpPrefix.size());
        defaultPort = 80;
    } else if (boost::algorithm::starts_with(url, httpsPrefix)) {
        error = "Unity Zebra JSON-RPC currently supports http:// endpoints only";
        return false;
    } else {
        error = "-zebra-compat-url must be an http:// URL";
        return false;
    }

    const size_t slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    endpoint.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (endpoint.path.empty()) {
        endpoint.path = "/";
    }

    if (hostPort.empty()) {
        error = "-zebra-compat-url is missing a host";
        return false;
    }

    int port = defaultPort;
    std::string host;
    SplitHostPort(hostPort, port, host);
    if (host.empty()) {
        error = "-zebra-compat-url is missing a host";
        return false;
    }

    endpoint.host = host;
    endpoint.port = port;
    return true;
}

bool LoadZebraClientConfig(ZebraClientConfig& config, std::string& error)
{
    const std::string url = GetArg("-zebra-compat-url", "");
    if (url.empty()) {
        error = "waiting_for_zebra_endpoint";
        return false;
    }

    if (!ParseZebraEndpoint(url, config.endpoint, error)) {
        return false;
    }

    const std::string user = GetArg("-zebra-compat-rpc-user", "");
    const std::string password = GetArg("-zebra-compat-rpc-password", "");
    const std::string cookieFile = GetArg("-zebra-compat-cookiefile", "");
    if (!cookieFile.empty() && (!user.empty() || !password.empty())) {
        error = "-zebra-compat-cookiefile is incompatible with -zebra-compat-rpc-user/-zebra-compat-rpc-password";
        return false;
    }

    if (!cookieFile.empty()) {
        fs::path path = AbsPathForConfigVal(fs::path(cookieFile));
        std::ifstream file(path.string().c_str());
        if (!file.is_open()) {
            error = strprintf("Unable to open Zebra RPC cookie file %s", path.string());
            return false;
        }
        std::string cookie;
        std::getline(file, cookie);
        const size_t colon = cookie.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= cookie.size()) {
            error = "Zebra RPC cookie must be in user:password format";
            return false;
        }
        config.auth.user = cookie.substr(0, colon);
        config.auth.password = cookie.substr(colon + 1);
    } else {
        if (user.empty() || password.empty()) {
            error = "zebra-compat Zebra JSON-RPC requires -zebra-compat-cookiefile or both -zebra-compat-rpc-user and -zebra-compat-rpc-password";
            return false;
        }
        config.auth.user = user;
        config.auth.password = password;
    }

    config.timeoutSeconds = DEFAULT_HTTP_SERVER_TIMEOUT;
    return true;
}

ZebraRpcResponse LibeventZebraRpcTransport::Call(
    const ZebraClientConfig& config,
    const std::string& method,
    const UniValue& params)
{
    const std::string requestBody = MakeJsonRpcRequest(method, params);
    return CallJsonRpc(config, requestBody);
}

ZebraRpcResponse LibeventZebraRpcTransport::CallBatch(
    const ZebraClientConfig& config,
    const std::vector<ZebraRpcCall>& calls)
{
    if (calls.empty()) {
        throw std::runtime_error("Zebra JSON-RPC batch requires at least one call");
    }
    const std::string requestBody = MakeJsonRpcBatchRequest(calls);
    return CallJsonRpc(config, requestBody);
}

ZebraRpcResponse LibeventZebraRpcTransport::CallJsonRpc(
    const ZebraClientConfig& config,
    const std::string& requestBody)
{

    UniqueEventBase base(event_base_new());
    if (base == nullptr) {
        throw std::runtime_error("Unable to create event base for Zebra JSON-RPC request");
    }

    UniqueEvhttpConnection connection(evhttp_connection_base_new(
        base.get(),
        nullptr,
        config.endpoint.host.c_str(),
        config.endpoint.port));
    if (connection == nullptr) {
        throw std::runtime_error("Unable to create Zebra JSON-RPC connection");
    }
    evhttp_connection_set_timeout(connection.get(), config.timeoutSeconds);
    evhttp_connection_set_max_body_size(connection.get(), ZebraRpcMaxResponseBodySize());

    HttpCallContext context;
    UniqueEvhttpRequest request(evhttp_request_new(HttpRequestDone, &context));
    if (request == nullptr) {
        throw std::runtime_error("Unable to create Zebra JSON-RPC request");
    }
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(request.get(), HttpErrorCallback);
#endif

    evkeyvalq* headers = evhttp_request_get_output_headers(request.get());
    evhttp_add_header(headers, "Host", config.endpoint.host.c_str());
    evhttp_add_header(headers, "Connection", "close");
    evhttp_add_header(headers, "Content-Type", "application/json");
    evhttp_add_header(headers, "Authorization", config.auth.BasicAuthHeader().c_str());

    evbuffer* output = evhttp_request_get_output_buffer(request.get());
    evbuffer_add(output, requestBody.data(), requestBody.size());

    int requestResult = evhttp_make_request(connection.get(), request.get(), EVHTTP_REQ_POST, config.endpoint.path.c_str());
    request.release(); // ownership moved to connection in the call above, including on failure
    if (requestResult != 0) {
        throw std::runtime_error("Unable to send Zebra JSON-RPC request");
    }

    event_base_dispatch(base.get());

    ZebraRpcResponse response;
    response.httpStatus = context.status;
    response.body = context.body;
    if (context.bodyTooLarge) {
        response.transportError = "response body exceeded maximum size";
    } else if (context.status == 0 && context.error != -1) {
        response.transportError = strprintf(
            "connection failed: %s (code %d)",
            HttpErrorString(context.error),
            context.error);
    }
    return response;
}

UnityZebraClient::UnityZebraClient(ZebraClientConfig config, std::unique_ptr<ZebraRpcTransport> transport) :
    config(std::move(config)),
    transport(std::move(transport))
{
    if (!this->transport) {
        throw std::runtime_error("UnityZebraClient requires a transport");
    }
}

UniValue UnityZebraClient::CallRpc(const std::string& method, const UniValue& params)
{
    ZebraRpcResponse response = transport->Call(config, method, params);
    if (response.body.size() > ZebraRpcMaxResponseBodySize()) {
        throw std::runtime_error(strprintf("Zebra JSON-RPC %s response body exceeded maximum size", method));
    }
    if (!response.transportError.empty()) {
        throw ZebraRpcError(strprintf("Zebra JSON-RPC %s %s", method, response.transportError), response.httpStatus, false, 0);
    }
    if (response.httpStatus == HTTP_UNAUTHORIZED || response.httpStatus == HTTP_FORBIDDEN) {
        throw ZebraRpcError(strprintf("Zebra JSON-RPC authentication failed with HTTP status %d", response.httpStatus), response.httpStatus, false, 0);
    }
    if (response.httpStatus != HTTP_OK) {
        throw ZebraRpcError(strprintf("Zebra JSON-RPC %s failed with HTTP status %d", method, response.httpStatus), response.httpStatus, false, 0);
    }

    UniValue reply;
    if (!reply.read(response.body) || !reply.isObject()) {
        throw std::runtime_error(strprintf("Zebra JSON-RPC %s returned malformed JSON", method));
    }

    const UniValue& error = find_value(reply.get_obj(), "error");
    if (!error.isNull()) {
        if (error.isObject()) {
            const UniValue& code = find_value(error.get_obj(), "code");
            const UniValue& message = find_value(error.get_obj(), "message");
            if (message.isStr()) {
                throw ZebraRpcError(
                    strprintf("Zebra JSON-RPC %s error: %s", method, message.get_str()),
                    response.httpStatus,
                    code.isNum(),
                    code.isNum() ? code.get_int() : 0);
            }
        }
        throw ZebraRpcError(strprintf("Zebra JSON-RPC %s returned an error", method), response.httpStatus, false, 0);
    }

    const UniValue& result = find_value(reply.get_obj(), "result");
    if (result.isNull()) {
        throw std::runtime_error(strprintf("Zebra JSON-RPC %s response is missing result", method));
    }
    return result;
}

std::vector<UniValue> UnityZebraClient::CallRpcBatch(const std::vector<ZebraRpcCall>& calls)
{
    if (calls.empty()) {
        return std::vector<UniValue>();
    }

    ZebraRpcResponse response = transport->CallBatch(config, calls);
    if (response.body.size() > ZebraRpcMaxResponseBodySize()) {
        throw std::runtime_error("Zebra JSON-RPC batch response body exceeded maximum size");
    }
    if (!response.transportError.empty()) {
        throw std::runtime_error(strprintf("Zebra JSON-RPC batch %s", response.transportError));
    }
    if (response.httpStatus == HTTP_UNAUTHORIZED || response.httpStatus == HTTP_FORBIDDEN) {
        throw std::runtime_error(strprintf("Zebra JSON-RPC authentication failed with HTTP status %d", response.httpStatus));
    }
    if (response.httpStatus != HTTP_OK) {
        throw std::runtime_error(strprintf("Zebra JSON-RPC batch failed with HTTP status %d", response.httpStatus));
    }

    UniValue reply;
    if (!reply.read(response.body) || !reply.isArray()) {
        throw std::runtime_error("Zebra JSON-RPC batch returned malformed JSON");
    }

    std::map<std::string, UniValue> repliesById;
    for (size_t i = 0; i < reply.size(); i++) {
        const UniValue& item = reply[i];
        if (!item.isObject()) {
            throw std::runtime_error("Zebra JSON-RPC batch returned a non-object response");
        }
        const UniValue& id = find_value(item.get_obj(), "id");
        if (!id.isStr()) {
            throw std::runtime_error("Zebra JSON-RPC batch response is missing string id");
        }
        repliesById[id.get_str()] = item;
    }

    std::vector<UniValue> results;
    results.reserve(calls.size());
    for (size_t i = 0; i < calls.size(); i++) {
        const std::string id = strprintf("%s-%d", UNITY_JSONRPC_ID, i);
        auto it = repliesById.find(id);
        if (it == repliesById.end()) {
            throw std::runtime_error(strprintf("Zebra JSON-RPC batch response is missing id %s", id));
        }

        const UniValue& item = it->second;
        const UniValue& error = find_value(item.get_obj(), "error");
        if (!error.isNull()) {
            if (error.isObject()) {
                const UniValue& message = find_value(error.get_obj(), "message");
                if (message.isStr()) {
                    throw std::runtime_error(strprintf("Zebra JSON-RPC %s error: %s", calls[i].method, message.get_str()));
                }
            }
            throw std::runtime_error(strprintf("Zebra JSON-RPC %s returned an error", calls[i].method));
        }

        const UniValue& result = find_value(item.get_obj(), "result");
        if (result.isNull()) {
            throw std::runtime_error(strprintf("Zebra JSON-RPC %s response is missing result", calls[i].method));
        }
        results.push_back(result);
    }

    return results;
}

ZebraBlockchainInfo UnityZebraClient::GetBlockchainInfo()
{
    UniValue result = CallRpc("getblockchaininfo", NoParams());
    if (!result.isObject()) {
        throw std::runtime_error("Zebra RPC getblockchaininfo returned a non-object result");
    }

    const UniValue& network = find_value(result.get_obj(), "chain");
    const UniValue& blocks = find_value(result.get_obj(), "blocks");
    const UniValue& bestBlockHash = find_value(result.get_obj(), "bestblockhash");
    if (!network.isStr() || !blocks.isNum() || !bestBlockHash.isStr()) {
        throw std::runtime_error("Zebra RPC getblockchaininfo result is missing required chain, blocks, or bestblockhash fields");
    }

    ZebraBlockchainInfo info;
    info.network = network.get_str();
    info.blocks = blocks.get_int();
    info.bestBlockHash = bestBlockHash.get_str();
    if (!IsValidHashHex(info.bestBlockHash)) {
        throw std::runtime_error("Zebra RPC getblockchaininfo returned an invalid bestblockhash");
    }
    return info;
}

std::string UnityZebraClient::GetBestBlockHash()
{
    return RequireHashResult(CallRpc("getbestblockhash", NoParams()), "getbestblockhash");
}

int UnityZebraClient::GetBlockCount()
{
    return RequireIntResult(CallRpc("getblockcount", NoParams()), "getblockcount");
}

std::string UnityZebraClient::GetBlockHash(int height)
{
    return RequireHashResult(CallRpc("getblockhash", OneParam(UniValue(height))), "getblockhash");
}

std::vector<std::string> UnityZebraClient::GetBlockHashes(int startHeight, int endHeight)
{
    if (startHeight > endHeight) {
        return std::vector<std::string>();
    }
    if (startHeight < 0) {
        throw std::runtime_error("GetBlockHashes requires non-negative heights");
    }

    std::vector<ZebraRpcCall> calls;
    calls.reserve(endHeight - startHeight + 1);
    for (int height = startHeight; height <= endHeight; height++) {
        calls.push_back({"getblockhash", OneParam(UniValue(height))});
    }

    std::vector<UniValue> results = CallRpcBatch(calls);
    std::vector<std::string> hashes;
    hashes.reserve(results.size());
    for (const UniValue& result : results) {
        hashes.push_back(RequireHashResult(result, "getblockhash"));
    }
    return hashes;
}

std::string UnityZebraClient::GetRawBlock(const std::string& hash)
{
    if (!IsValidHashHex(hash)) {
        throw std::runtime_error("GetRawBlock requires a 64-character block hash");
    }
    std::string rawBlock = RequireStringResult(CallRpc("getblock", TwoParams(UniValue(hash), UniValue(0))), "getblock");
    if (!IsHex(rawBlock)) {
        throw std::runtime_error("Zebra RPC getblock returned non-hex block data");
    }
    return rawBlock;
}

std::vector<std::string> UnityZebraClient::GetRawBlocks(const std::vector<std::string>& hashes)
{
    if (hashes.empty()) {
        return std::vector<std::string>();
    }

    std::vector<std::string> rawBlocks;
    rawBlocks.reserve(hashes.size());
    const size_t maxBatchSize = static_cast<size_t>(UnitySyncBatchSize());
    for (size_t start = 0; start < hashes.size(); start += maxBatchSize) {
        const size_t end = std::min(hashes.size(), start + maxBatchSize);
        std::vector<ZebraRpcCall> calls;
        calls.reserve(end - start);
        for (size_t i = start; i < end; i++) {
            if (!IsValidHashHex(hashes[i])) {
                throw std::runtime_error("GetRawBlocks requires 64-character block hashes");
            }
            calls.push_back({"getblock", TwoParams(UniValue(hashes[i]), UniValue(0))});
        }

        std::vector<UniValue> results = CallRpcBatch(calls);
        for (const UniValue& result : results) {
            std::string rawBlock = RequireStringResult(result, "getblock");
            if (!IsHex(rawBlock)) {
                throw std::runtime_error("Zebra RPC getblock returned non-hex block data");
            }
            rawBlocks.push_back(rawBlock);
        }
    }
    return rawBlocks;
}

std::vector<std::string> UnityZebraClient::GetRawMempool()
{
    UniValue result = CallRpc("getrawmempool", NoParams());
    if (!result.isArray()) {
        throw std::runtime_error("Zebra RPC getrawmempool returned a non-array result");
    }

    std::vector<std::string> txids;
    txids.reserve(result.size());
    for (size_t i = 0; i < result.size(); i++) {
        txids.push_back(RequireTxIdResult(result[i], "getrawmempool"));
    }
    return txids;
}

ZebraMempoolInfo UnityZebraClient::GetMempoolInfo()
{
    UniValue result = CallRpc("getmempoolinfo", NoParams());
    if (!result.isObject()) {
        throw std::runtime_error("Zebra RPC getmempoolinfo returned a non-object result");
    }

    ZebraMempoolInfo info;
    const UniValue& size = find_value(result.get_obj(), "size");
    if (size.isNum()) {
        info.size = size.get_int();
    }
    const UniValue& bytes = find_value(result.get_obj(), "bytes");
    if (bytes.isNum()) {
        info.bytes = bytes.get_int64();
    }
    const UniValue& usage = find_value(result.get_obj(), "usage");
    if (usage.isNum()) {
        info.usage = usage.get_int64();
    }
    if (info.size < 0) {
        throw std::runtime_error("Zebra RPC getmempoolinfo result is missing required size field");
    }
    return info;
}

std::string UnityZebraClient::GetRawTransaction(const std::string& txid)
{
    if (!IsValidHashHex(txid)) {
        throw std::runtime_error("GetRawTransaction requires a 64-character transaction id");
    }
    std::string rawTx = RequireStringResult(
        CallRpc("getrawtransaction", TwoParams(UniValue(txid), UniValue(0))),
        "getrawtransaction");
    if (!IsHex(rawTx)) {
        throw std::runtime_error("Zebra RPC getrawtransaction returned non-hex transaction data");
    }
    return rawTx;
}

std::vector<std::string> UnityZebraClient::GetRawTransactions(const std::vector<std::string>& txids)
{
    if (txids.empty()) {
        return std::vector<std::string>();
    }

    const size_t maxBatchSize = static_cast<size_t>(UnitySyncBatchSize());
    if (txids.size() > maxBatchSize) {
        throw std::runtime_error("GetRawTransactions batch exceeds Unity batch size");
    }

    std::vector<std::string> rawTxs;
    rawTxs.reserve(txids.size());
    std::vector<ZebraRpcCall> calls;
    calls.reserve(txids.size());
    for (const std::string& txid : txids) {
        if (!IsValidHashHex(txid)) {
            throw std::runtime_error("GetRawTransactions requires 64-character transaction ids");
        }
        calls.push_back({"getrawtransaction", TwoParams(UniValue(txid), UniValue(0))});
    }

    std::vector<UniValue> results = CallRpcBatch(calls);
    for (const UniValue& result : results) {
        std::string rawTx = RequireStringResult(result, "getrawtransaction");
        if (!IsHex(rawTx)) {
            throw std::runtime_error("Zebra RPC getrawtransaction returned non-hex transaction data");
        }
        rawTxs.push_back(rawTx);
    }
    return rawTxs;
}

std::string UnityZebraClient::SendRawTransaction(const std::string& txHex)
{
    if (!IsHex(txHex)) {
        throw std::runtime_error("SendRawTransaction requires transaction hex");
    }
    return RequireStringResult(CallRpc("sendrawtransaction", OneParam(UniValue(txHex))), "sendrawtransaction");
}

ZebraIdentity UnityZebraClient::CheckIdentity(const CChainParams& chainparams)
{
    ZebraIdentity identity;
    try {
        ZebraBlockchainInfo info = GetBlockchainInfo();
        identity.reachable = true;
        identity.network = info.network;
        identity.bestBlockHash = info.bestBlockHash;
        identity.blocks = info.blocks;
        identity.genesisHash = GetBlockHash(0);

        const std::string expectedGenesis = chainparams.GetConsensus().hashGenesisBlock.GetHex();
        const bool genesisMatches = identity.genesisHash == expectedGenesis;
        const bool regtestChainAlias =
            chainparams.NetworkIDString() == CBaseChainParams::REGTEST &&
            identity.network == CBaseChainParams::TESTNET;

        if (identity.network != chainparams.NetworkIDString() && !regtestChainAlias) {
            identity.failure = ZebraIdentity::NETWORK_MISMATCH;
            identity.lastError = strprintf(
                "Zebra network mismatch: expected %s, got %s",
                chainparams.NetworkIDString(),
                identity.network);
            return identity;
        }

        if (!genesisMatches) {
            identity.failure = ZebraIdentity::GENESIS_MISMATCH;
            identity.lastError = strprintf(
                "Zebra genesis mismatch: expected %s, got %s",
                expectedGenesis,
                identity.genesisHash);
            return identity;
        }

        identity.identityVerified = true;
        return identity;
    } catch (const std::exception& e) {
        identity.lastError = e.what();
        identity.failure = ClassifyIdentityException(identity.lastError);
        identity.reachable = identity.failure != ZebraIdentity::TRANSIENT;
        return identity;
    }
}

} // namespace unity
