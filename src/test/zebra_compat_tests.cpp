// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "test/test_bitcoin.h"
#include "test/test_util.h"
#include "arith_uint256.h"
#include "core_io.h"
#include "zebra_compat/mempool_mirror.h"
#include "zebra_compat/zebra_compat.h"
#include "zebra_compat/metadata.h"
#include "zebra_compat/tx_forwarder.h"
#include "zebra_compat/zebra_client.h"
#ifdef ENABLE_MINING
#include "crypto/equihash.h"
#include "miner.h"
#endif
#include "main.h"
#include "pow.h"
#include "primitives/block.h"
#include "rpc/protocol.h"
#include "script/script.h"
#include "streams.h"
#include "txdb.h"
#include "util/system.h"

#include <atomic>
#include <map>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/thread.hpp>

#include <univalue.h>

namespace {

struct ArgsSnapshot {
    std::map<std::string, std::string> args;
    std::map<std::string, std::vector<std::string> > multiArgs;

    ArgsSnapshot() : args(mapArgs), multiArgs(mapMultiArgs) {}
    ~ArgsSnapshot()
    {
        mapArgs = args;
        mapMultiArgs = multiArgs;
    }
};

void ResetArgs(const std::string& strArg)
{
    std::vector<std::string> vecArg;
    if (strArg.size()) {
        boost::split(vecArg, strArg, boost::is_any_of(" \t\n\r\f\v"), boost::token_compress_on);
    }
    for (std::string& arg : vecArg) {
        auto replacePrefix = [&arg](const std::string& legacy, const std::string& current) {
            if (boost::algorithm::starts_with(arg, legacy)) {
                arg.replace(0, legacy.size(), current);
            }
        };

        if (arg == "-zebra-compat") arg = "-zebra-compat";
        replacePrefix("-zebra-compat-url=", "-zebra-compat-url=");
        replacePrefix("-zebra-compat-cookiefile=", "-zebra-compat-cookiefile=");
        replacePrefix("-zebra-compat-rpc-user=", "-zebra-compat-rpc-user=");
        replacePrefix("-zebra-compat-rpc-password=", "-zebra-compat-rpc-password=");
        replacePrefix("-zebra-compat-poll-interval=", "-zebra-compat-poll-interval=");
        replacePrefix("-zebra-compat-sync-batch-size=", "-zebra-compat-sync-batch-size=");
        replacePrefix("-zebra-compat-sync-drive-batches=", "-zebra-compat-sync-drive-batches=");
        replacePrefix("-zebra-compat-sync-response-budget-mb=", "-zebra-compat-sync-response-budget-mb=");
        replacePrefix("-zebra-compat-zebra-rpc-max-response-body-bytes=", "-zebra-compat-zebra-rpc-max-response-body-bytes=");
        replacePrefix("-zebra-compat-allow-remote-http=", "-zebra-compat-allow-remote-http=");
        replacePrefix("-zebra-compat-trusted-validation-fixture", "-zebra-compat-trusted-validation-fixture");
        if (arg == "-zebra-compat-fail-trusted-boundary-write=1") arg = "-zebra-compat-fail-trusted-boundary-write=1";
    }

    vecArg.insert(vecArg.begin(), "testbitcoin");

    std::vector<const char*> vecChar;
    for (std::string& s : vecArg) {
        vecChar.push_back(s.c_str());
    }

    ParseParameters(vecChar.size(), &vecChar[0]);
}

void ApplyZebraCompatArgs(const std::string& strArg)
{
    ResetArgs(strArg);
    zebra_compat::InitParameterInteraction();
}

// test_bitcoin does not link init.o (duplicate globals with test_bitcoin.cpp), so
// mirror init.cpp's listen-related interactions that run after zebra_compat::InitParameterInteraction().
void ApplyInitCppListenInteractions()
{
    if (mapArgs.count("-bind")) {
        SoftSetBoolArg("-listen", true);
    }
    if (mapArgs.count("-whitebind")) {
        SoftSetBoolArg("-listen", true);
    }
}

void ApplyFullParameterInteraction(const std::string& strArg)
{
    ResetArgs(strArg);
    zebra_compat::InitParameterInteraction();
    ApplyInitCppListenInteractions();
}

std::string HashWithLastChar(char last)
{
    return std::string(63, '0') + last;
}

UniValue RpcResult(const UniValue& result)
{
    return JSONRPCReplyObj(result, NullUniValue, UniValue("zebra-compat"));
}

UniValue RpcErrorResult(int code, const std::string& message)
{
    return JSONRPCReplyObj(NullUniValue, JSONRPCError(code, message), UniValue("zebra-compat"));
}

class MockZebraTransport : public zebra_compat::ZebraRpcTransport {
public:
    std::map<std::string, zebra_compat::ZebraRpcResponse> responses;
    std::vector<std::string> calls;
    std::vector<std::vector<std::string> > batchCalls;
    bool throwOnCall = false;

    zebra_compat::ZebraRpcResponse Call(
        const zebra_compat::ZebraClientConfig& config,
        const std::string& method,
        const UniValue& params) override
    {
        (void)config;
        (void)params;
        calls.push_back(method);
        if (throwOnCall) {
            throw std::runtime_error("transport unavailable");
        }
        return responses[method];
    }

    zebra_compat::ZebraRpcResponse CallBatch(
        const zebra_compat::ZebraClientConfig& config,
        const std::vector<zebra_compat::ZebraRpcCall>& callsIn) override
    {
        (void)config;
        std::vector<std::string> methods;
        UniValue batch(UniValue::VARR);
        if (throwOnCall) {
            throw std::runtime_error("transport unavailable");
        }
        for (size_t i = 0; i < callsIn.size(); i++) {
            methods.push_back(callsIn[i].method);
            auto it = responses.find(callsIn[i].method);
            if (it == responses.end()) {
                return {HTTP_INTERNAL_SERVER_ERROR, ""};
            }
            if (it->second.httpStatus != HTTP_OK || !it->second.transportError.empty()) {
                return it->second;
            }

            UniValue single;
            if (!single.read(it->second.body) || !single.isObject()) {
                return it->second;
            }

            UniValue item(UniValue::VOBJ);
            item.pushKV("jsonrpc", "2.0");
            item.pushKV("id", strprintf("zebra-compat-%d", i));
            item.pushKV("result", find_value(single.get_obj(), "result"));
            item.pushKV("error", find_value(single.get_obj(), "error"));
            batch.push_back(item);
        }
        batchCalls.push_back(methods);
        return {HTTP_OK, batch.write()};
    }
};

zebra_compat::ZebraClientConfig MockZebraConfig()
{
    zebra_compat::ZebraClientConfig config;
    std::string error;
    BOOST_CHECK(zebra_compat::ParseZebraEndpoint("http://127.0.0.1:8232", config.endpoint, error));
    config.auth.user = "user";
    config.auth.password = "pass";
    return config;
}

std::unique_ptr<MockZebraTransport> HealthyMainnetTransport(const CChainParams& chainparams)
{
    const std::string genesis = chainparams.GetConsensus().hashGenesisBlock.GetHex();
    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());

    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", chainparams.NetworkIDString());
    blockchainInfo.pushKV("blocks", 123);
    blockchainInfo.pushKV("bestblockhash", HashWithLastChar('1'));
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getbestblockhash"] = {HTTP_OK, RpcResult(UniValue(HashWithLastChar('1'))).write()};
    transport->responses["getblockcount"] = {HTTP_OK, RpcResult(UniValue(123)).write()};
    transport->responses["getblockhash"] = {HTTP_OK, RpcResult(UniValue(genesis)).write()};
    transport->responses["getblock"] = {HTTP_OK, RpcResult(UniValue("00")).write()};
    UniValue rawMempool(UniValue::VARR);
    rawMempool.push_back(HashWithLastChar('3'));
    transport->responses["getrawmempool"] = {HTTP_OK, RpcResult(rawMempool).write()};
    UniValue mempoolInfo(UniValue::VOBJ);
    mempoolInfo.pushKV("size", 1);
    mempoolInfo.pushKV("bytes", 100);
    mempoolInfo.pushKV("usage", 200);
    transport->responses["getmempoolinfo"] = {HTTP_OK, RpcResult(mempoolInfo).write()};
    transport->responses["getrawtransaction"] = {HTTP_OK, RpcResult(UniValue("00")).write()};
    transport->responses["sendrawtransaction"] = {HTTP_OK, RpcResult(UniValue(HashWithLastChar('2'))).write()};
    return transport;
}

#ifdef ENABLE_MINING
void SolveBlock(CBlock& block, const CChainParams& chainparams)
{
    unsigned int n = chainparams.GetConsensus().nEquihashN;
    unsigned int k = chainparams.GetConsensus().nEquihashK;

    eh_HashState eh_state = EhInitialiseState(n, k);
    CEquihashInput I{block};
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << I;
    eh_state.Update((unsigned char*)&ss[0], ss.size());

    bool found = false;
    do {
        block.nNonce = ArithToUint256(UintToArith256(block.nNonce) + 1);

        eh_HashState curr_state(eh_state);
        curr_state.Update(block.nNonce.begin(), block.nNonce.size());

        std::function<bool(std::vector<unsigned char>)> validBlock =
                [&block, &chainparams](std::vector<unsigned char> soln) {
            block.nSolution = soln;
            return CheckProofOfWork(block.GetHash(), block.nBits, chainparams.GetConsensus());
        };
        found = EhBasicSolveUncancellable(n, k, curr_state, validBlock);
    } while (!found);
}

CBlock CreateSolvedBlock(const CChainParams& chainparams, const CScript& scriptPubKey)
{
    boost::shared_ptr<CReserveScript> mAddr(new CReserveScript());
    mAddr->reserveScript = scriptPubKey;

    std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(chainparams).CreateNewBlock(mAddr));
    CBlock& block = pblocktemplate->block;

    unsigned int extraNonce = 0;
    IncrementExtraNonce(pblocktemplate.get(), chainActive.Tip(), extraNonce, chainparams.GetConsensus());

    SolveBlock(block, chainparams);
    return block;
}

CScript RandomCoinbaseScript()
{
    CKey coinbaseKey = CKey::TestOnlyRandomKey(true);
    return CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
}
#endif

struct ZebraCompatRegtestSetup : public TestingSetup {
    ZebraCompatRegtestSetup() : TestingSetup(CBaseChainParams::REGTEST) {}
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(zebra_compat_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(zebra_compat_preset_expands_to_lower_level_knobs)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat");

    BOOST_CHECK(zebra_compat::IsEnabled());
    BOOST_CHECK(!zebra_compat::IsP2PEnabled());
    BOOST_CHECK_EQUAL(GetArg("-blocksource", ""), "zebra");
    BOOST_CHECK_EQUAL(GetArg("-blockvalidation", ""), "trusted-zebra");
    BOOST_CHECK(!GetBoolArg("-listen", true));
    BOOST_CHECK(!GetBoolArg("-dnsseed", true));
    BOOST_CHECK(!GetBoolArg("-listenonion", true));
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_compat_accepts_zebra_source_with_full_validation)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full");

    BOOST_CHECK(zebra_compat::IsEnabled());
    BOOST_CHECK(!zebra_compat::IsP2PEnabled());
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_compat_rejects_invalid_option_values)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-blocksource=bad -p2p=0");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blockvalidation=bad");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_client_config_accepts_loopback_http_endpoints)
{
    ArgsSnapshot snapshot;
    const std::vector<std::string> urls = {
        "http://127.0.0.1:8232",
        "http://[::1]:8232",
    };

    for (const std::string& url : urls) {
        ApplyZebraCompatArgs(
            "-zebra-compat-url=" + url +
            " -zebra-compat-rpc-user=user -zebra-compat-rpc-password=pass");
        zebra_compat::ZebraClientConfig config;
        std::string error;
        BOOST_CHECK_MESSAGE(zebra_compat::LoadZebraClientConfig(config, error), error);
        BOOST_CHECK_EQUAL(config.endpoint.url, url);
    }
}

BOOST_AUTO_TEST_CASE(zebra_client_config_rejects_remote_plain_http_by_default)
{
    ArgsSnapshot snapshot;
    const std::vector<std::string> urls = {
        "http://10.0.0.2:8232",
        "http://192.168.0.2:8232",
    };

    for (const std::string& url : urls) {
        ApplyZebraCompatArgs(
            "-zebra-compat-url=" + url +
            " -zebra-compat-rpc-user=user -zebra-compat-rpc-password=pass");
        zebra_compat::ZebraClientConfig config;
        std::string error;
        BOOST_CHECK(!zebra_compat::LoadZebraClientConfig(config, error));
        BOOST_CHECK(error.find("-zebra-compat-allow-remote-http") != std::string::npos);
        BOOST_CHECK(error.find("Basic authentication") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(zebra_client_config_allows_remote_plain_http_with_override)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs(
        "-zebra-compat-url=http://10.0.0.2:8232"
        " -zebra-compat-rpc-user=user -zebra-compat-rpc-password=pass"
        " -zebra-compat-allow-remote-http=1");

    zebra_compat::ZebraClientConfig config;
    std::string error;
    BOOST_CHECK_MESSAGE(zebra_compat::LoadZebraClientConfig(config, error), error);
    BOOST_CHECK_EQUAL(config.endpoint.host, "10.0.0.2");
}

BOOST_AUTO_TEST_CASE(zebra_client_config_fails_closed_for_unresolved_plain_http_names)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs(
        "-zebra-compat-url=http://zebra-compat-unresolved.invalid:8232"
        " -zebra-compat-rpc-user=user -zebra-compat-rpc-password=pass");

    zebra_compat::ZebraClientConfig config;
    std::string error;
    BOOST_CHECK(!zebra_compat::LoadZebraClientConfig(config, error));
    BOOST_CHECK(error.find("-zebra-compat-allow-remote-http") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_compat_rejects_trusted_zebra_without_zebra_source)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-blockvalidation=trusted-zebra");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blockvalidation=trusted-zebra -zebra-compat-trusted-validation-fixture=1");
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_compat_forces_p2p_off_despite_legacy_config)
{
    ArgsSnapshot snapshot;

    ApplyFullParameterInteraction("-zebra-compat -listen=1");
    BOOST_CHECK(!GetBoolArg("-listen", true));
    BOOST_CHECK(!zebra_compat::IsP2PEnabled());
    BOOST_CHECK(!GetBoolArg("-dnsseed", true));
    BOOST_CHECK(!GetBoolArg("-listenonion", true));
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");

    ApplyFullParameterInteraction("-zebra-compat -p2p=1");
    BOOST_CHECK(!GetBoolArg("-listen", true));
    BOOST_CHECK(!zebra_compat::IsP2PEnabled());
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");

    ApplyFullParameterInteraction("-zebra-compat -listen=1 -dnsseed=1 -listenonion=1");
    BOOST_CHECK(!GetBoolArg("-listen", true));
    BOOST_CHECK(!GetBoolArg("-dnsseed", true));
    BOOST_CHECK(!GetBoolArg("-listenonion", true));
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");

    ApplyFullParameterInteraction("-zebra-compat -listen=1 -bind=127.0.0.1:8233");
    BOOST_CHECK(!GetBoolArg("-listen", true));
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyFullParameterInteraction("-zebra-compat -listen=1 -connect=127.0.0.1");
    BOOST_CHECK(!GetBoolArg("-listen", true));
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_compat_bind_interaction_does_not_enable_listen)
{
    ArgsSnapshot snapshot;
    ApplyFullParameterInteraction("-zebra-compat -bind=127.0.0.1:8233");

    BOOST_CHECK(!GetBoolArg("-listen", true));
    BOOST_CHECK(!zebra_compat::IsP2PEnabled());
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_compat_rejects_p2p_conflicts)
{
    ArgsSnapshot snapshot;

    ApplyZebraCompatArgs("-zebra-compat -connect=127.0.0.1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-zebra-compat -addnode=127.0.0.1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-zebra-compat -seednode=127.0.0.1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_compat_rejects_lower_level_p2p_conflicts)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -listen=1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -bind=127.0.0.1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -whitebind=127.0.0.1:8233");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -connect=127.0.0.1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -addnode=127.0.0.1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -seednode=127.0.0.1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -dnsseed=1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");

    ApplyZebraCompatArgs("-blocksource=zebra -p2p=0 -blockvalidation=full -listenonion=1");
    BOOST_CHECK_NE(zebra_compat::ValidateParameterInteraction(), "");
}

BOOST_AUTO_TEST_CASE(zebra_client_successfully_checks_identity_and_basic_calls)
{
    const CChainParams& chainparams = Params();
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(chainparams);
    MockZebraTransport* rawTransport = transport.get();
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(chainparams);
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.network, chainparams.NetworkIDString());
    BOOST_CHECK_EQUAL(identity.blocks, 123);
    BOOST_CHECK_EQUAL(identity.genesisHash, chainparams.GetConsensus().hashGenesisBlock.GetHex());
    BOOST_CHECK_EQUAL(identity.bestBlockHash, HashWithLastChar('1'));

    BOOST_CHECK_EQUAL(client.GetBlockHash(0), chainparams.GetConsensus().hashGenesisBlock.GetHex());
    BOOST_CHECK_EQUAL(client.GetRawBlock(chainparams.GetConsensus().hashGenesisBlock.GetHex()), "00");
    BOOST_CHECK_EQUAL(client.SendRawTransaction("00"), HashWithLastChar('2'));
    BOOST_CHECK_EQUAL(rawTransport->calls.size(), 5);
}

BOOST_AUTO_TEST_CASE(zebra_client_uses_batch_calls_for_block_ranges)
{
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    MockZebraTransport* rawTransport = transport.get();
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    std::vector<std::string> hashes = client.GetBlockHashes(1, 3);
    BOOST_CHECK_EQUAL(hashes.size(), 3);
    BOOST_CHECK_EQUAL(rawTransport->batchCalls.size(), 1);
    BOOST_CHECK_EQUAL(rawTransport->batchCalls[0].size(), 3);
    BOOST_CHECK_EQUAL(rawTransport->batchCalls[0][0], "getblockhash");

    std::vector<std::string> rawBlocks = client.GetRawBlocks(hashes);
    BOOST_CHECK_EQUAL(rawBlocks.size(), 3);
    BOOST_CHECK_EQUAL(rawTransport->batchCalls.size(), 2);
    BOOST_CHECK_EQUAL(rawTransport->batchCalls[1].size(), 3);
    BOOST_CHECK_EQUAL(rawTransport->batchCalls[1][0], "getblock");
}

BOOST_AUTO_TEST_CASE(zebra_compat_common_ancestor_search_finds_highest_shared_height)
{
    std::vector<std::string> localHashes{
        HashWithLastChar('1'),
        HashWithLastChar('2'),
        HashWithLastChar('3'),
        HashWithLastChar('4')};
    std::vector<std::string> zebraHashes{
        HashWithLastChar('1'),
        HashWithLastChar('2'),
        HashWithLastChar('9'),
        HashWithLastChar('a')};

    zebra_compat::CommonAncestorSearchResult result =
        zebra_compat::FindCommonAncestorInHashRange(3, 0, localHashes, zebraHashes, 10);
    BOOST_CHECK(result.found);
    BOOST_CHECK(!result.overLimit);
    BOOST_CHECK_EQUAL(result.height, 1);
    BOOST_CHECK_EQUAL(result.disconnectLength, 2);
    BOOST_CHECK_EQUAL(result.hash, HashWithLastChar('2'));
}

BOOST_AUTO_TEST_CASE(zebra_compat_common_ancestor_search_reports_no_common_ancestor)
{
    std::vector<std::string> localHashes{
        HashWithLastChar('1'),
        HashWithLastChar('2')};
    std::vector<std::string> zebraHashes{
        HashWithLastChar('8'),
        HashWithLastChar('9')};

    zebra_compat::CommonAncestorSearchResult result =
        zebra_compat::FindCommonAncestorInHashRange(3, 2, localHashes, zebraHashes, 10);
    BOOST_CHECK(!result.found);
    BOOST_CHECK(!result.overLimit);
    BOOST_CHECK(result.error.find("no common ancestor") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_compat_common_ancestor_search_reports_over_policy_reorg)
{
    std::vector<std::string> localHashes{
        HashWithLastChar('1'),
        HashWithLastChar('2'),
        HashWithLastChar('3')};
    std::vector<std::string> zebraHashes{
        HashWithLastChar('1'),
        HashWithLastChar('8'),
        HashWithLastChar('9')};

    zebra_compat::CommonAncestorSearchResult result =
        zebra_compat::FindCommonAncestorInHashRange(12, 0, localHashes, zebraHashes, 10);
    BOOST_CHECK(result.found);
    BOOST_CHECK(result.overLimit);
    BOOST_CHECK_EQUAL(result.height, 0);
    BOOST_CHECK_EQUAL(result.disconnectLength, 12);
    BOOST_CHECK(result.error.find("exceeding max reorg") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_client_polls_mempool_and_raw_transactions)
{
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    MockZebraTransport* rawTransport = transport.get();
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    std::vector<std::string> txids = client.GetRawMempool();
    BOOST_REQUIRE_EQUAL(txids.size(), 1);
    BOOST_CHECK_EQUAL(txids[0], HashWithLastChar('3'));

    zebra_compat::ZebraMempoolInfo info = client.GetMempoolInfo();
    BOOST_CHECK_EQUAL(info.size, 1);
    BOOST_CHECK_EQUAL(info.bytes, 100);
    BOOST_CHECK_EQUAL(info.usage, 200);

    BOOST_CHECK_EQUAL(client.GetRawTransaction(HashWithLastChar('3')), "00");

    std::vector<std::string> rawTxs = client.GetRawTransactions(std::vector<std::string>{
        HashWithLastChar('3'),
        HashWithLastChar('4')});
    BOOST_REQUIRE_EQUAL(rawTxs.size(), 2);
    BOOST_CHECK_EQUAL(rawTxs[0], "00");
    BOOST_CHECK_EQUAL(rawTxs[1], "00");
    BOOST_CHECK(!rawTransport->batchCalls.empty());
    BOOST_CHECK_EQUAL(rawTransport->batchCalls.back()[0], "getrawtransaction");
}

BOOST_AUTO_TEST_CASE(zebra_client_rejects_oversized_raw_transaction_batch)
{
    ArgsSnapshot snapshot;
    ResetArgs("-zebra-compat-sync-batch-size=1");

    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    BOOST_CHECK_THROW(client.GetRawTransactions(std::vector<std::string>{
        HashWithLastChar('3'),
        HashWithLastChar('4')}),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(zebra_client_rejects_malformed_mempool_payloads)
{
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    transport->responses["getrawmempool"] = {HTTP_OK, RpcResult(UniValue("not-array")).write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    BOOST_CHECK_THROW(client.GetRawMempool(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(tx_forwarder_records_success_and_pending_until_zebra_mirror_observes)
{
    zebra_compat::ResetTxForwardingForTesting();

    const uint256 txid = uint256S(HashWithLastChar('5'));
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    transport->responses["sendrawtransaction"] = {HTTP_OK, RpcResult(UniValue(txid.GetHex())).write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::TxForwardingResult result = zebra_compat::ForwardRawTransaction(client, "00", txid);
    BOOST_CHECK(result.success);
    BOOST_CHECK_EQUAL(result.txid, txid.GetHex());
    BOOST_CHECK(zebra_compat::ShouldKeepForwardedTransaction(txid.GetHex()));

    zebra_compat::TxForwardingStatus status = zebra_compat::GetTxForwardingStatus();
    BOOST_CHECK_GT(status.lastSuccess, 0);
    BOOST_CHECK_EQUAL(status.lastError, "");
    BOOST_CHECK_EQUAL(status.lastTransportError, "");
    BOOST_CHECK_EQUAL(status.pending, 1);

    zebra_compat::MarkForwardedTransactionObserved(txid.GetHex());
    status = zebra_compat::GetTxForwardingStatus();
    BOOST_CHECK_EQUAL(status.pending, 0);
    BOOST_CHECK_EQUAL(zebra_compat::PendingForwardedOrderSizeForTesting(), 0);
}

BOOST_AUTO_TEST_CASE(tx_forwarder_duplicate_successes_do_not_grow_order_storage)
{
    zebra_compat::ResetTxForwardingForTesting();

    const uint256 txid = uint256S(HashWithLastChar('8'));
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    transport->responses["sendrawtransaction"] = {HTTP_OK, RpcResult(UniValue(txid.GetHex())).write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    for (size_t i = 0; i < 1100; i++) {
        zebra_compat::TxForwardingResult result = zebra_compat::ForwardRawTransaction(client, "00", txid);
        BOOST_REQUIRE(result.success);
    }

    zebra_compat::TxForwardingStatus status = zebra_compat::GetTxForwardingStatus();
    BOOST_CHECK_EQUAL(status.pending, 1);
    BOOST_CHECK_EQUAL(zebra_compat::PendingForwardedOrderSizeForTesting(), 1);
}

BOOST_AUTO_TEST_CASE(tx_forwarder_maps_zebra_rejection_to_transaction_rpc_error)
{
    zebra_compat::ResetTxForwardingForTesting();

    const uint256 txid = uint256S(HashWithLastChar('6'));
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    transport->responses["sendrawtransaction"] = {
        HTTP_OK,
        RpcErrorResult(RPC_TRANSACTION_REJECTED, "bad-txns-inputs-spent").write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::TxForwardingResult result = zebra_compat::ForwardRawTransaction(client, "00", txid);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.rpcErrorCode, RPC_TRANSACTION_REJECTED);
    BOOST_CHECK(result.error.find("bad-txns-inputs-spent") != std::string::npos);
    zebra_compat::TxForwardingStatus status = zebra_compat::GetTxForwardingStatus();
    BOOST_CHECK(status.lastError.find("bad-txns-inputs-spent") != std::string::npos);
    BOOST_CHECK_EQUAL(status.lastTransportError, "");
}

BOOST_AUTO_TEST_CASE(tx_forwarder_maps_zebra_deserialization_error)
{
    zebra_compat::ResetTxForwardingForTesting();

    const uint256 txid = uint256S(HashWithLastChar('a'));
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    transport->responses["sendrawtransaction"] = {
        HTTP_OK,
        RpcErrorResult(RPC_DESERIALIZATION_ERROR, "TX decode failed").write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::TxForwardingResult result = zebra_compat::ForwardRawTransaction(client, "00", txid);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.rpcErrorCode, RPC_DESERIALIZATION_ERROR);
    BOOST_CHECK(result.error.find("TX decode failed") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tx_forwarder_maps_unreachable_zebra_to_not_connected)
{
    zebra_compat::ResetTxForwardingForTesting();

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    transport->throwOnCall = true;
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::TxForwardingResult result =
        zebra_compat::ForwardRawTransaction(client, "00", uint256S(HashWithLastChar('7')));
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.rpcErrorCode, RPC_CLIENT_NOT_CONNECTED);
    BOOST_CHECK(result.error.find("transport unavailable") != std::string::npos);
    zebra_compat::TxForwardingStatus status = zebra_compat::GetTxForwardingStatus();
    BOOST_CHECK(status.lastTransportError.find("transport unavailable") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(mempool_mirror_failure_does_not_record_successful_update)
{
    zebra_compat::ResetMempoolMirrorForTesting();

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    transport->throwOnCall = true;
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::MempoolMirrorResult result = zebra_compat::SyncMempoolMirrorOnce(client, Params());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("transport unavailable") != std::string::npos);

    zebra_compat::MempoolMirrorStatus status = zebra_compat::GetMempoolMirrorStatus();
    BOOST_CHECK_EQUAL(status.lastUpdate, 0);
    BOOST_CHECK_GT(status.lastFailure, 0);
    BOOST_CHECK(status.lastError.find("transport unavailable") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(mempool_mirror_bounds_large_poll_batches_and_divergence_details)
{
    ArgsSnapshot snapshot;
    ResetArgs("-zebra-compat-sync-batch-size=2");
    zebra_compat::ResetMempoolMirrorForTesting();

    const size_t txCount = 1030;
    UniValue rawMempool(UniValue::VARR);
    for (size_t i = 0; i < txCount; i++) {
        rawMempool.push_back(strprintf("%064x", static_cast<unsigned int>(i + 1)));
    }

    UniValue mempoolInfo(UniValue::VOBJ);
    mempoolInfo.pushKV("size", static_cast<int>(txCount));

    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    MockZebraTransport* rawTransport = transport.get();
    transport->responses["getrawmempool"] = {HTTP_OK, RpcResult(rawMempool).write()};
    transport->responses["getmempoolinfo"] = {HTTP_OK, RpcResult(mempoolInfo).write()};
    transport->responses["getrawtransaction"] = {HTTP_OK, RpcResult(UniValue("00")).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::MempoolMirrorResult result = zebra_compat::SyncMempoolMirrorOnce(client, Params());

    BOOST_CHECK(result.success);
    BOOST_CHECK_GT(rawTransport->batchCalls.size(), 1);
    for (const std::vector<std::string>& batch : rawTransport->batchCalls) {
        BOOST_CHECK_LE(batch.size(), 2);
    }

    zebra_compat::MempoolMirrorStatus status = zebra_compat::GetMempoolMirrorStatus();
    BOOST_CHECK_EQUAL(status.zebraSize, static_cast<int>(txCount));
    BOOST_CHECK_EQUAL(status.divergent, txCount);
    BOOST_CHECK_LE(status.divergentDetails, 128);
    BOOST_CHECK_EQUAL(status.divergentDetailOverflow, txCount - status.divergentDetails);
    BOOST_CHECK_EQUAL(status.lag, 0);
    BOOST_CHECK(status.lastError.find("reconciled 1024 this poll") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_compat_sync_batch_size_is_clamped_by_memory_budget)
{
    ArgsSnapshot snapshot;
    ResetArgs("-zebra-compat-sync-batch-size=100000");

    BOOST_CHECK(zebra_compat::ValidateParameterInteraction().find("memory budget") != std::string::npos);
    const int clamped = zebra_compat::ZebraCompatSyncBatchSize();
    BOOST_CHECK_GE(clamped, 1);
    BOOST_CHECK_LT(clamped, 100000);
    BOOST_CHECK_LE(zebra_compat::ZebraRpcMaxResponseBodySize(), 128 * 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(zebra_compat_sync_batch_size_default_is_30)
{
    ArgsSnapshot snapshot;
    ResetArgs("");

    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatSyncBatchSize(), 30);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraRpcMaxResponseBodySize(), 121079296);
}

BOOST_AUTO_TEST_CASE(zebra_compat_sync_batch_size_80_requires_raised_budget)
{
    ArgsSnapshot snapshot;
    ResetArgs("-zebra-compat-sync-batch-size=80");

    BOOST_CHECK(zebra_compat::ValidateParameterInteraction().find("memory budget") != std::string::npos);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatSyncBatchSize(), 33);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraRpcMaxResponseBodySize(), 133082368);

    ResetArgs("-zebra-compat-sync-batch-size=80 -zebra-compat-sync-response-budget-mb=320");
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");
    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatSyncBatchSize(), 80);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraRpcMaxResponseBodySize(), 321130496);
}

BOOST_AUTO_TEST_CASE(zebra_compat_validates_configured_zebra_rpc_response_limit)
{
    ArgsSnapshot snapshot;
    ResetArgs(
        "-zebra-compat-sync-batch-size=80 "
        "-zebra-compat-sync-response-budget-mb=320 "
        "-zebra-compat-zebra-rpc-max-response-body-bytes=134217728");

    const std::string tooLowError = zebra_compat::ValidateParameterInteraction();
    BOOST_CHECK(
        tooLowError.find("-zebra-compat-zebra-rpc-max-response-body-bytes=134217728") !=
        std::string::npos);
    BOOST_CHECK(tooLowError.find("321130496") != std::string::npos);

    ResetArgs(
        "-zebra-compat-sync-batch-size=80 "
        "-zebra-compat-sync-response-budget-mb=320 "
        "-zebra-compat-zebra-rpc-max-response-body-bytes=321130496");
    BOOST_CHECK_EQUAL(zebra_compat::ValidateParameterInteraction(), "");

    ResetArgs("-zebra-compat-zebra-rpc-max-response-body-bytes=0");
    BOOST_CHECK(zebra_compat::ValidateParameterInteraction().find("must be at least 1") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_compat_retry_backoff_is_bounded)
{
    ArgsSnapshot snapshot;
    ResetArgs("-zebra-compat-poll-interval=2");

    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatRetryBackoffSeconds(0), 0);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatRetryBackoffSeconds(1), 2);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatRetryBackoffSeconds(2), 4);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatRetryBackoffSeconds(3), 8);
    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatRetryBackoffSeconds(30), 60);

    ResetArgs("-zebra-compat-poll-interval=120");
    BOOST_CHECK_EQUAL(zebra_compat::ZebraCompatRetryBackoffSeconds(1), 60);
}

BOOST_AUTO_TEST_CASE(zebra_client_fails_closed_on_auth_failure)
{
    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    transport->responses["getblockchaininfo"] = {HTTP_UNAUTHORIZED, ""};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(!identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.failure, zebra_compat::ZebraIdentity::AUTHENTICATION);
    BOOST_CHECK(identity.lastError.find("authentication failed") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_client_fails_closed_on_network_mismatch)
{
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", std::string("not-") + Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", 123);
    blockchainInfo.pushKV("bestblockhash", HashWithLastChar('1'));
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(!identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.failure, zebra_compat::ZebraIdentity::NETWORK_MISMATCH);
    BOOST_CHECK(identity.lastError.find("network mismatch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_client_fails_closed_on_genesis_mismatch)
{
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    transport->responses["getblockhash"] = {HTTP_OK, RpcResult(UniValue(HashWithLastChar('9'))).write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(!identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.failure, zebra_compat::ZebraIdentity::GENESIS_MISMATCH);
    BOOST_CHECK(identity.lastError.find("genesis mismatch") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(regtest_identity_accepts_zebra_test_chain_alias, ZebraCompatRegtestSetup)
{
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", CBaseChainParams::TESTNET);
    blockchainInfo.pushKV("blocks", 123);
    blockchainInfo.pushKV("bestblockhash", HashWithLastChar('1'));
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.network, CBaseChainParams::TESTNET);
    BOOST_CHECK_EQUAL(identity.genesisHash, Params().GetConsensus().hashGenesisBlock.GetHex());
}

BOOST_FIXTURE_TEST_CASE(regtest_identity_rejects_zebra_test_chain_alias_with_wrong_genesis, ZebraCompatRegtestSetup)
{
    std::unique_ptr<MockZebraTransport> transport = HealthyMainnetTransport(Params());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", CBaseChainParams::TESTNET);
    blockchainInfo.pushKV("blocks", 123);
    blockchainInfo.pushKV("bestblockhash", HashWithLastChar('1'));
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {HTTP_OK, RpcResult(UniValue(HashWithLastChar('9'))).write()};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(!identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.failure, zebra_compat::ZebraIdentity::GENESIS_MISMATCH);
    BOOST_CHECK(identity.lastError.find("genesis mismatch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_client_fails_closed_on_unreachable_zebra)
{
    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    transport->throwOnCall = true;
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(!identity.reachable);
    BOOST_CHECK(!identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.failure, zebra_compat::ZebraIdentity::TRANSIENT);
    BOOST_CHECK(identity.lastError.find("transport unavailable") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_client_fails_closed_on_malformed_payload)
{
    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    transport->responses["getblockchaininfo"] = {HTTP_OK, "not json"};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(!identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.failure, zebra_compat::ZebraIdentity::MALFORMED_RESPONSE);
    BOOST_CHECK(identity.lastError.find("malformed JSON") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_client_fails_closed_on_oversized_response)
{
    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    transport->responses["getblockchaininfo"] = {
        HTTP_OK,
        std::string(zebra_compat::ZebraRpcMaxResponseBodySize() + 1, 'x')};
    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));

    zebra_compat::ZebraIdentity identity = client.CheckIdentity(Params());
    BOOST_CHECK(identity.reachable);
    BOOST_CHECK(!identity.identityVerified);
    BOOST_CHECK_EQUAL(identity.failure, zebra_compat::ZebraIdentity::MALFORMED_RESPONSE);
    BOOST_CHECK(identity.lastError.find("response body exceeded maximum size") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(zebra_compat_accepts_tip_ahead_when_on_zebra_best_chain_after_chunk_mismatch)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");

    const std::string currentBestHash = HashWithLastChar('f');
    const std::string localTipHash = currentBestHash;
    const int expectedHeight = 4054025;
    const std::string expectedHash = HashWithLastChar('c');
    const int currentBestHeight = 4054209;
    const int localTipHeight = currentBestHeight;

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", currentBestHeight);
    blockchainInfo.pushKV("bestblockhash", currentBestHash);
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {
        HTTP_OK,
        RpcResult(UniValue(Params().GetConsensus().hashGenesisBlock.GetHex())).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::ZebraCompatSyncTestOutcome outcome = zebra_compat::TEST_ValidatePostIngestionTipOnZebraBestChain(
        client,
        Params(),
        localTipHeight,
        localTipHash,
        expectedHeight,
        expectedHash,
        "local tip mismatch during test",
        "local_tip_not_on_zebra_best_chain_after_chunk");

    BOOST_CHECK(outcome.progressed);
    BOOST_CHECK(!outcome.stickyFault);
    BOOST_CHECK(!outcome.transientFailure);

    UniValue info = zebra_compat::GetZebraCompatInfo();
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "synced");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "zebra_tip_matched");
}

BOOST_AUTO_TEST_CASE(zebra_compat_fails_when_tip_ahead_is_not_on_zebra_best_chain_after_chunk_mismatch)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");

    const int expectedHeight = 4054025;
    const std::string expectedHash = HashWithLastChar('c');
    const std::string localTipHash = HashWithLastChar('e');
    const int localTipHeight = expectedHeight;

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", expectedHeight);
    blockchainInfo.pushKV("bestblockhash", expectedHash);
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {
        HTTP_OK,
        RpcResult(UniValue(Params().GetConsensus().hashGenesisBlock.GetHex())).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::ZebraCompatSyncTestOutcome outcome = zebra_compat::TEST_ValidatePostIngestionTipOnZebraBestChain(
        client,
        Params(),
        localTipHeight,
        localTipHash,
        expectedHeight,
        expectedHash,
        "local tip mismatch during test",
        "local_tip_not_on_zebra_best_chain_after_chunk");

    BOOST_CHECK(!outcome.progressed);
    BOOST_CHECK(outcome.stickyFault);
    BOOST_CHECK(!outcome.transientFailure);

    UniValue info = zebra_compat::GetZebraCompatInfo();
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "failed");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "local_tip_not_on_zebra_best_chain_after_chunk");
}

BOOST_AUTO_TEST_CASE(zebra_compat_fails_when_local_tip_exceeds_zebra_best_after_chunk_mismatch)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");

    const int expectedHeight = 4054025;
    const std::string expectedHash = HashWithLastChar('c');
    const int localTipHeight = 4054026;
    const std::string localTipHash = HashWithLastChar('e');

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", expectedHeight);
    blockchainInfo.pushKV("bestblockhash", expectedHash);
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {
        HTTP_OK,
        RpcResult(UniValue(Params().GetConsensus().hashGenesisBlock.GetHex())).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::ZebraCompatSyncTestOutcome outcome = zebra_compat::TEST_ValidatePostIngestionTipOnZebraBestChain(
        client,
        Params(),
        localTipHeight,
        localTipHash,
        expectedHeight,
        expectedHash,
        "local tip mismatch during test",
        "local_tip_not_on_zebra_best_chain_after_chunk");

    BOOST_CHECK(!outcome.progressed);
    BOOST_CHECK(outcome.stickyFault);
    BOOST_CHECK(!outcome.transientFailure);

    UniValue info = zebra_compat::GetZebraCompatInfo();
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "failed");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "local_tip_not_on_zebra_best_chain_after_chunk");
}

BOOST_AUTO_TEST_CASE(zebra_compat_degrades_non_sticky_for_equal_height_reorg_candidate_not_activated)
{
    // Regression: Zebra advertised a competing block at the local tip's height,
    // the replacement branch was ingested, but ActivateBestChain kept the
    // previously received equal-work local tip. The reorg path must degrade
    // non-sticky so the worker retries, instead of freezing in a failed state.
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");

    const int expectedHeight = 4056120;
    const std::string expectedHash = HashWithLastChar('c');
    const int localTipHeight = expectedHeight;
    const std::string localTipHash = HashWithLastChar('e');

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", expectedHeight);
    blockchainInfo.pushKV("bestblockhash", expectedHash);
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {
        HTTP_OK,
        RpcResult(UniValue(Params().GetConsensus().hashGenesisBlock.GetHex())).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::ZebraCompatSyncTestOutcome outcome = zebra_compat::TEST_ValidatePostIngestionTipOnZebraBestChain(
        client,
        Params(),
        localTipHeight,
        localTipHash,
        expectedHeight,
        expectedHash,
        "local tip mismatch during test",
        "local_tip_not_on_zebra_best_chain_after_reorg",
        /*reorgContext=*/true);

    BOOST_CHECK(!outcome.progressed);
    BOOST_CHECK(!outcome.stickyFault);
    BOOST_CHECK(!outcome.transientFailure);

    UniValue info = zebra_compat::GetZebraCompatInfo();
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "degraded");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "zebra_equal_work_reorg_not_activated");
}

BOOST_AUTO_TEST_CASE(zebra_compat_reorg_context_keeps_sticky_fault_when_local_tip_below_zebra_best)
{
    // In the reorg context, an off-chain local tip strictly below Zebra's best
    // height is not the equal-height race and must remain a sticky fault.
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");

    const int expectedHeight = 4056121;
    const std::string expectedHash = HashWithLastChar('c');
    const int localTipHeight = expectedHeight - 1;
    const std::string localTipHash = HashWithLastChar('e');

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", expectedHeight);
    blockchainInfo.pushKV("bestblockhash", expectedHash);
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {
        HTTP_OK,
        RpcResult(UniValue(Params().GetConsensus().hashGenesisBlock.GetHex())).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::ZebraCompatSyncTestOutcome outcome = zebra_compat::TEST_ValidatePostIngestionTipOnZebraBestChain(
        client,
        Params(),
        localTipHeight,
        localTipHash,
        expectedHeight,
        expectedHash,
        "local tip mismatch during test",
        "local_tip_not_on_zebra_best_chain_after_reorg",
        /*reorgContext=*/true);

    BOOST_CHECK(!outcome.progressed);
    BOOST_CHECK(outcome.stickyFault);
    BOOST_CHECK(!outcome.transientFailure);

    UniValue info = zebra_compat::GetZebraCompatInfo();
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "failed");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "local_tip_not_on_zebra_best_chain_after_reorg");
}

BOOST_AUTO_TEST_CASE(zebra_compat_degrades_transient_when_local_tip_ahead_of_zebra_after_reorg)
{
    // Zebra's best chain temporarily shrank (Zebra mid-reorg): local tip is
    // strictly ahead. Must be a non-sticky transient so the worker applies
    // backoff instead of polling at full rate.
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");

    const int expectedHeight = 4056200;
    const std::string expectedHash = HashWithLastChar('c');
    const int localTipHeight = expectedHeight + 3;
    const std::string localTipHash = HashWithLastChar('e');
    const int zebraCurrentHeight = expectedHeight - 2;
    const std::string zebraCurrentHash = HashWithLastChar('b');

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", zebraCurrentHeight);
    blockchainInfo.pushKV("bestblockhash", zebraCurrentHash);
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {
        HTTP_OK,
        RpcResult(UniValue(Params().GetConsensus().hashGenesisBlock.GetHex())).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::ZebraCompatSyncTestOutcome outcome = zebra_compat::TEST_ValidatePostIngestionTipOnZebraBestChain(
        client,
        Params(),
        localTipHeight,
        localTipHash,
        expectedHeight,
        expectedHash,
        "local tip mismatch during test",
        "local_tip_not_on_zebra_best_chain_after_reorg",
        /*reorgContext=*/true);

    BOOST_CHECK(!outcome.progressed);
    BOOST_CHECK(!outcome.stickyFault);
    BOOST_CHECK(outcome.transientFailure);

    UniValue info = zebra_compat::GetZebraCompatInfo();
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "degraded");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "zebra_tip_temporarily_behind_local_after_reorg");
}

BOOST_AUTO_TEST_CASE(zebra_compat_degrades_non_sticky_when_zebra_tip_changes_during_chunk_mismatch_handling)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");

    const int expectedHeight = 4054025;
    const std::string expectedHash = HashWithLastChar('c');
    const int newZebraHeight = 4054026;
    const std::string newZebraHash = HashWithLastChar('d');
    const int localTipHeight = newZebraHeight + 1;
    const std::string localTipHash = HashWithLastChar('e');

    std::unique_ptr<MockZebraTransport> transport(new MockZebraTransport());
    UniValue blockchainInfo(UniValue::VOBJ);
    blockchainInfo.pushKV("chain", Params().NetworkIDString());
    blockchainInfo.pushKV("blocks", newZebraHeight);
    blockchainInfo.pushKV("bestblockhash", newZebraHash);
    transport->responses["getblockchaininfo"] = {HTTP_OK, RpcResult(blockchainInfo).write()};
    transport->responses["getblockhash"] = {
        HTTP_OK,
        RpcResult(UniValue(Params().GetConsensus().hashGenesisBlock.GetHex())).write()};

    zebra_compat::ZebraCompatClient client(MockZebraConfig(), std::move(transport));
    zebra_compat::ZebraCompatSyncTestOutcome outcome = zebra_compat::TEST_ValidatePostIngestionTipOnZebraBestChain(
        client,
        Params(),
        localTipHeight,
        localTipHash,
        expectedHeight,
        expectedHash,
        "local tip mismatch during test",
        "local_tip_not_on_zebra_best_chain_after_chunk");

    BOOST_CHECK(!outcome.progressed);
    BOOST_CHECK(!outcome.stickyFault);
    BOOST_CHECK(!outcome.transientFailure);

    UniValue info = zebra_compat::GetZebraCompatInfo();
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "degraded");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "zebra_tip_changed_during_sync");
}

BOOST_AUTO_TEST_CASE(trusted_block_uses_checkpoint_expensive_check_lever)
{
    BOOST_CHECK(BlockCheckModeUsesExpensiveChecks(CheckAs::Block, false));
    BOOST_CHECK(BlockCheckModeUsesExpensiveChecks(CheckAs::SlowBenchmark, false));
    BOOST_CHECK(!BlockCheckModeUsesExpensiveChecks(CheckAs::TrustedBlock, false));
    BOOST_CHECK(!BlockCheckModeUsesExpensiveChecks(CheckAs::BlockTemplate, false));
    BOOST_CHECK(!BlockCheckModeUsesExpensiveChecks(CheckAs::Block, true));
    BOOST_CHECK(!BlockCheckModeUsesExpensiveChecks(CheckAs::TrustedBlock, true));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(zebra_compat_rpc_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(getzebracompatinfo_reports_minimal_status)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat");
    zebra_compat::ResetMempoolMirrorForTesting();
    zebra_compat::ResetTxForwardingForTesting();

    UniValue info = CallRPC("getzebracompatinfo");
    BOOST_CHECK(find_value(info.get_obj(), "enabled").get_bool());
    BOOST_CHECK_EQUAL(find_value(info.get_obj(), "service_state").get_str(), "stopped");
    BOOST_CHECK_EQUAL(find_value(info.get_obj(), "readiness").get_str(), "degraded");
    BOOST_CHECK_EQUAL(find_value(info.get_obj(), "blocksource").get_str(), "zebra");
    BOOST_CHECK(!find_value(info.get_obj(), "p2p").get_bool());
    BOOST_CHECK_EQUAL(find_value(info.get_obj(), "blockvalidation").get_str(), "trusted-zebra");

    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "degraded");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "waiting_for_zebra_endpoint");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "retry_count").get_int(), 0);
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "current_backoff_seconds").get_int(), 0);
    BOOST_CHECK(find_value(sync.get_obj(), "next_retry").isNull());

    UniValue ingestion = find_value(info.get_obj(), "ingestion");
    BOOST_CHECK(!find_value(ingestion.get_obj(), "last_success").get_bool());
    BOOST_CHECK(!find_value(ingestion.get_obj(), "last_hard_failure").get_bool());

    UniValue trustedBoundary = find_value(info.get_obj(), "trusted_boundary");
    BOOST_CHECK(!find_value(trustedBoundary.get_obj(), "active").get_bool());

    UniValue mempoolMirror = find_value(info.get_obj(), "mempool_mirror");
    BOOST_CHECK(mempoolMirror.isObject());
    BOOST_CHECK_EQUAL(find_value(mempoolMirror.get_obj(), "source").get_str(), "zebra-poll");
    BOOST_CHECK_EQUAL(find_value(mempoolMirror.get_obj(), "lag").get_int(), 0);
    BOOST_CHECK(find_value(mempoolMirror.get_obj(), "last_update").isNull());
    BOOST_CHECK(find_value(mempoolMirror.get_obj(), "last_failure").isNull());
    BOOST_CHECK_EQUAL(find_value(mempoolMirror.get_obj(), "divergent").get_int(), 0);
    BOOST_CHECK_EQUAL(find_value(mempoolMirror.get_obj(), "divergent_detail_sample_size").get_int(), 0);
    BOOST_CHECK_EQUAL(find_value(mempoolMirror.get_obj(), "divergent_detail_overflow").get_int(), 0);

    UniValue txForwarding = find_value(info.get_obj(), "tx_forwarding");
    BOOST_CHECK(txForwarding.isObject());
    BOOST_CHECK(find_value(txForwarding.get_obj(), "last_success").isNull());
    BOOST_CHECK(find_value(txForwarding.get_obj(), "last_error").isNull());
    BOOST_CHECK(find_value(txForwarding.get_obj(), "last_transport_error").isNull());
    BOOST_CHECK_EQUAL(find_value(txForwarding.get_obj(), "pending").get_int(), 0);

    UniValue metrics = find_value(info.get_obj(), "metrics");
    BOOST_CHECK(metrics.isObject());
    BOOST_CHECK_EQUAL(find_value(metrics.get_obj(), "mempool_lag").get_int(), 0);
    BOOST_CHECK(!find_value(metrics.get_obj(), "mempool_ready").get_bool());
    BOOST_CHECK(find_value(metrics.get_obj(), "mempool_last_update_age_seconds").isNull());
    BOOST_CHECK_EQUAL(find_value(metrics.get_obj(), "mempool_divergent").get_int(), 0);
    BOOST_CHECK_EQUAL(find_value(metrics.get_obj(), "tx_forwarding_pending").get_int(), 0);
    BOOST_CHECK(find_value(metrics.get_obj(), "tx_forwarding_transport_ready").get_bool());
    BOOST_CHECK(find_value(metrics.get_obj(), "validation_notifications_caught_up").isBool());
    BOOST_CHECK_EQUAL(find_value(metrics.get_obj(), "retry_count").get_int(), 0);

    UniValue limits = find_value(info.get_obj(), "limits");
    BOOST_CHECK(limits.isObject());
    BOOST_CHECK_EQUAL(find_value(limits.get_obj(), "poll_interval_seconds").get_int(), 5);
    BOOST_CHECK_EQUAL(find_value(limits.get_obj(), "max_retry_backoff_seconds").get_int(), 60);
    BOOST_CHECK_EQUAL(find_value(limits.get_obj(), "sync_batch_size").get_int(), zebra_compat::ZebraCompatSyncBatchSize());
    BOOST_CHECK_EQUAL(find_value(limits.get_obj(), "mempool_txids_per_poll").get_int(), static_cast<int>(zebra_compat::MaxMempoolMirrorTxIdsPerPoll()));
    BOOST_CHECK_EQUAL(find_value(limits.get_obj(), "mempool_divergence_details").get_int(), static_cast<int>(zebra_compat::MaxMempoolMirrorDivergenceDetails()));
    BOOST_CHECK_EQUAL(find_value(limits.get_obj(), "pending_forwarded_transactions").get_int(), static_cast<int>(zebra_compat::MaxPendingForwardedTransactions()));
}

BOOST_AUTO_TEST_CASE(sendrawtransaction_zebra_compat_rejects_local_preflight_before_zebra_forwarding)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat");
    zebra_compat::ResetTxForwardingForTesting();

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1;
    mtx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    CTransaction coinbase(mtx);

    BOOST_CHECK_THROW(CallRPC("sendrawtransaction " + EncodeHexTx(coinbase)), std::runtime_error);
    BOOST_CHECK_EQUAL(zebra_compat::GetTxForwardingStatus().pending, 0);
}

BOOST_AUTO_TEST_CASE(p2p_control_rpcs_reject_when_p2p_disabled)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat");

    BOOST_CHECK_THROW(CallRPC("addnode 127.0.0.1 onetry"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("disconnectnode 127.0.0.1"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("getaddednodeinfo false"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("setban 127.0.0.0 add"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("listbanned"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("clearbanned"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("getblocktemplate"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("generate 1"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("setgenerate true"), std::runtime_error);

    UniValue peers = CallRPC("getpeerinfo");
    BOOST_CHECK(peers.isArray());
    BOOST_CHECK_EQUAL(peers.size(), 0);

    UniValue network = CallRPC("getnetworkinfo");
    BOOST_CHECK_EQUAL(find_value(network.get_obj(), "connections").get_int(), 0);
}

BOOST_AUTO_TEST_CASE(zebra_compat_trusted_boundary_is_identity_scoped)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    zebra_compat::TrustedBlockBoundary boundary =
        zebra_compat::MakeTrustedBlockBoundary(1, uint256S(HashWithLastChar('1')), Params());
    BOOST_CHECK(zebra_compat::WriteTrustedBlockBoundary(boundary));

    zebra_compat::TrustedBlockBoundary readBoundary;
    BOOST_CHECK(zebra_compat::ReadTrustedBlockBoundary(readBoundary));
    BOOST_CHECK(zebra_compat::TrustedBoundaryMatchesConfiguredSource(readBoundary, Params()));

    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:18232");
    BOOST_CHECK(!zebra_compat::TrustedBoundaryMatchesConfiguredSource(readBoundary, Params()));

    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    readBoundary.network = "wrong-network";
    BOOST_CHECK(!zebra_compat::TrustedBoundaryMatchesConfiguredSource(readBoundary, Params()));

    zebra_compat::ClearTrustedBlockBoundary();
}

BOOST_AUTO_TEST_CASE(zebra_compat_metadata_boundary_reads_are_thread_safe)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    zebra_compat::TrustedBlockBoundary boundary =
        zebra_compat::MakeTrustedBlockBoundary(1, uint256S(HashWithLastChar('1')), Params());
    BOOST_REQUIRE(zebra_compat::WriteTrustedBlockBoundary(boundary));

    std::atomic<bool> failed(false);
    boost::thread_group threads;
    for (int i = 0; i < 4; i++) {
        threads.create_thread([&failed, &boundary]() {
            for (int j = 0; j < 50; j++) {
                zebra_compat::TrustedBlockBoundary readBoundary;
                if (!zebra_compat::ReadTrustedBlockBoundary(readBoundary) ||
                    readBoundary.nHeight != boundary.nHeight ||
                    readBoundary.hash != boundary.hash) {
                    failed.store(true);
                }
            }
        });
    }
    threads.join_all();

    BOOST_CHECK(!failed.load());
    zebra_compat::ClearTrustedBlockBoundary();
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef ENABLE_MINING
BOOST_FIXTURE_TEST_SUITE(zebra_compat_ingestion_tests, ZebraCompatRegtestSetup)

BOOST_AUTO_TEST_CASE(zebra_compat_ingests_valid_regtest_block_and_persists_trusted_boundary)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    CKey coinbaseKey = CKey::TestOnlyRandomKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CBlock block = CreateSolvedBlock(Params(), scriptPubKey);

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(block, Params());
    BOOST_CHECK(result.success);
    BOOST_CHECK(!result.hardFailure);

    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainActive.Tip() != nullptr);
        BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash().GetHex(), block.GetHash().GetHex());
    }

    CBlock diskBlock;
    {
        LOCK(cs_main);
        BOOST_CHECK(ReadBlockFromDisk(diskBlock, chainActive.Tip(), Params().GetConsensus()));
    }
    BOOST_CHECK_EQUAL(diskBlock.GetHash().GetHex(), block.GetHash().GetHex());

    zebra_compat::TrustedBlockBoundary boundary;
    BOOST_CHECK(zebra_compat::ReadTrustedBlockBoundary(boundary));
    BOOST_CHECK(zebra_compat::TrustedBoundaryMatchesConfiguredSource(boundary, Params()));
    BOOST_CHECK_EQUAL(boundary.nHeight, result.height);
    BOOST_CHECK_EQUAL(boundary.hash.GetHex(), block.GetHash().GetHex());
}

BOOST_AUTO_TEST_CASE(trusted_zebra_regtest_accepts_zebra_style_difficulty)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    CBlock block = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    block.nBits -= 1;

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(block, Params());
    BOOST_CHECK(result.success);
    BOOST_CHECK(!result.hardFailure);
}

BOOST_AUTO_TEST_CASE(trusted_zebra_regtest_accepts_zebra_style_equihash)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    CBlock block = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    block.nSolution = std::vector<unsigned char>(1344, 0);

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(block, Params());
    BOOST_CHECK(result.success);
    BOOST_CHECK(!result.hardFailure);
}

BOOST_AUTO_TEST_CASE(full_validation_still_rejects_zebra_style_header)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -blockvalidation=full");
    zebra_compat::ClearTrustedBlockBoundary();

    CBlock block = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    block.nBits -= 1;
    block.nSolution = std::vector<unsigned char>(1344, 0);

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(block, Params());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.hardFailure);
}

BOOST_AUTO_TEST_CASE(trusted_zebra_regtest_disk_read_skips_work_only_for_indexed_blocks)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    CBlock trustedBlock = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    trustedBlock.nSolution = std::vector<unsigned char>(1344, 0);

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(trustedBlock, Params());
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE(!result.hardFailure);

    CBlock sideBlock = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    sideBlock.nSolution = std::vector<unsigned char>(1344, 0);

    CDiskBlockPos sideBlockPos(9999, 0);
    BOOST_REQUIRE(WriteBlockToDisk(sideBlock, sideBlockPos, Params().MessageStart()));

    uint256 sideBlockHash = sideBlock.GetHash();
    CBlockIndex sideIndex(sideBlock);
    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainActive.Height() >= 1);
        BOOST_REQUIRE(chainActive.Tip() != nullptr);
        BOOST_REQUIRE_EQUAL(chainActive.Tip()->GetBlockHash().GetHex(), trustedBlock.GetHash().GetHex());

        sideIndex.phashBlock = &sideBlockHash;
        sideIndex.pprev = chainActive[0];
        sideIndex.nHeight = 1;
        sideIndex.BuildSkip();
        sideIndex.nStatus = BLOCK_VALID_TREE | BLOCK_HAVE_DATA;
        sideIndex.nFile = sideBlockPos.nFile;
        sideIndex.nDataPos = sideBlockPos.nPos;

        CBlock diskBlock;
        BOOST_CHECK(!ReadBlockFromDisk(diskBlock, &sideIndex, Params().GetConsensus()));
        BOOST_CHECK(ReadBlockFromDisk(diskBlock, chainActive.Tip(), Params().GetConsensus()));
        BOOST_CHECK_EQUAL(diskBlock.GetHash().GetHex(), trustedBlock.GetHash().GetHex());
    }

    zebra_compat::ClearTrustedBlockBoundary();
}

BOOST_AUTO_TEST_CASE(trusted_zebra_regtest_reorg_disconnects_remain_loadable)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    // Two competing height-1 blocks built on genesis. The off-chain block
    // carries Zebra-style header work that deterministically fails zcashd's
    // CheckProofOfWork (target above the regtest powLimit), like the blocks a
    // Zebra-driven reorg leaves disconnected. It must be created before any
    // ingestion so no block template is ever built on its bogus nBits.
    CBlock activeBranchFirst = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    CBlock offChainBlock = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    offChainBlock.nBits = 0x207fffff;
    BOOST_REQUIRE(!CheckProofOfWork(
        offChainBlock.GetHash(), offChainBlock.nBits, Params().GetConsensus()));

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(activeBranchFirst, Params());
    BOOST_REQUIRE(result.success);

    // The Zebra-style block lands in the index without activating (the
    // first-seen branch has more work than its near-zero-work header).
    result = zebra_compat::IngestBlock(offChainBlock, Params());
    BOOST_REQUIRE(result.success);

    // Extend the active branch so the trusted boundary rises above the
    // off-chain block's height.
    CBlock activeBranchSecond = CreateSolvedBlock(Params(), RandomCoinbaseScript());
    result = zebra_compat::IngestBlock(activeBranchSecond, Params());
    BOOST_REQUIRE(result.success);

    const uint256 offChainHash = offChainBlock.GetHash();
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(
            chainActive.Tip()->GetBlockHash().GetHex(), activeBranchSecond.GetHash().GetHex());

        // The off-chain block stays in the index below the boundary and must
        // remain readable despite failing header work.
        auto it = mapBlockIndex.find(offChainHash);
        BOOST_REQUIRE(it != mapBlockIndex.end());
        BOOST_REQUIRE(!chainActive.Contains(it->second));

        CBlock diskBlock;
        BOOST_CHECK(ReadBlockFromDisk(diskBlock, it->second, Params().GetConsensus()));
        BOOST_CHECK_EQUAL(diskBlock.GetHash().GetHex(), offChainHash.GetHex());
    }

    // Reloading the block index from disk must also accept the disconnected
    // block (regression: zcashd restart after a Zebra-driven reorg).
    FlushStateToDisk();
    std::map<uint256, CBlockIndex*> scratchIndex;
    std::function<CBlockIndex*(const uint256&)> insertScratch =
        [&scratchIndex](const uint256& hash) -> CBlockIndex* {
            auto inserted = scratchIndex.emplace(hash, nullptr);
            if (inserted.second) {
                inserted.first->second = new CBlockIndex();
                inserted.first->second->phashBlock = &inserted.first->first;
            }
            return inserted.first->second;
        };
    {
        LOCK(cs_main);
        BOOST_CHECK(pblocktree->LoadBlockIndexGuts(insertScratch, Params()));
    }
    // The reload check is only meaningful if the disconnected block was
    // actually persisted and reloaded.
    BOOST_CHECK_EQUAL(scratchIndex.count(offChainHash), 1);
    for (auto& entry : scratchIndex) {
        delete entry.second;
    }

    zebra_compat::ClearTrustedBlockBoundary();
}

BOOST_AUTO_TEST_CASE(zebra_compat_ingestion_reports_hard_fault_for_wrong_parent_without_advancing_tip)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    uint256 oldTip;
    {
        LOCK(cs_main);
        oldTip = chainActive.Tip()->GetBlockHash();
    }

    CKey coinbaseKey = CKey::TestOnlyRandomKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CBlock block = CreateSolvedBlock(Params(), scriptPubKey);
    block.hashPrevBlock = uint256S(HashWithLastChar('9'));

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(block, Params());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.hardFailure);

    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash().GetHex(), oldTip.GetHex());
    }

    UniValue info = CallRPC("getzebracompatinfo");
    UniValue sync = find_value(info.get_obj(), "sync");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "state").get_str(), "failed");
    BOOST_CHECK_EQUAL(find_value(sync.get_obj(), "detail").get_str(), "hard_sync_fault");
}

BOOST_AUTO_TEST_CASE(zebra_compat_ingesting_known_ancestor_keeps_descendant_tip_active)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    CKey firstKey = CKey::TestOnlyRandomKey(true);
    CScript firstScript = CScript() << ToByteVector(firstKey.GetPubKey()) << OP_CHECKSIG;
    CBlock first = CreateSolvedBlock(Params(), firstScript);

    zebra_compat::BlockIngestionResult firstResult = zebra_compat::IngestBlock(first, Params());
    BOOST_REQUIRE(firstResult.success);

    CKey secondKey = CKey::TestOnlyRandomKey(true);
    CScript secondScript = CScript() << ToByteVector(secondKey.GetPubKey()) << OP_CHECKSIG;
    CBlock second = CreateSolvedBlock(Params(), secondScript);

    zebra_compat::BlockIngestionResult seedResult = zebra_compat::IngestBlock(second, Params());
    BOOST_REQUIRE(seedResult.success);

    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainActive.Tip() != nullptr);
        BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash().GetHex(), second.GetHash().GetHex());
    }

    zebra_compat::BlockIngestionResult replayResult =
        zebra_compat::IngestBlockBatch(std::vector<CBlock>{first}, Params());
    BOOST_CHECK(replayResult.success);
    BOOST_CHECK_EQUAL(replayResult.hash, first.GetHash().GetHex());

    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainActive.Tip() != nullptr);
        BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash().GetHex(), second.GetHash().GetHex());
    }
}

BOOST_AUTO_TEST_CASE(zebra_compat_failed_non_contiguous_batch_clears_trusted_candidates)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    BOOST_REQUIRE_EQUAL(TEST_GetZebraCompatTrustedBlockCandidateCount(), 0);

    for (int i = 0; i < 3; i++) {
        CKey firstKey = CKey::TestOnlyRandomKey(true);
        CScript firstScript = CScript() << ToByteVector(firstKey.GetPubKey()) << OP_CHECKSIG;
        CBlock first = CreateSolvedBlock(Params(), firstScript);

        CKey secondKey = CKey::TestOnlyRandomKey(true);
        CScript secondScript = CScript() << ToByteVector(secondKey.GetPubKey()) << OP_CHECKSIG;
        CBlock second = CreateSolvedBlock(Params(), secondScript);

        zebra_compat::BlockIngestionResult result =
            zebra_compat::IngestBlockBatch(std::vector<CBlock>{first, second}, Params());
        BOOST_CHECK(!result.success);
        BOOST_CHECK(result.hardFailure);
        BOOST_CHECK_EQUAL(TEST_GetZebraCompatTrustedBlockCandidateCount(), 0);
    }
}

BOOST_AUTO_TEST_CASE(zebra_compat_batch_middle_block_failure_does_not_process_later_blocks)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232");
    zebra_compat::ClearTrustedBlockBoundary();

    int oldHeight;
    {
        LOCK(cs_main);
        oldHeight = chainActive.Height();
    }

    CKey firstKey = CKey::TestOnlyRandomKey(true);
    CScript firstScript = CScript() << ToByteVector(firstKey.GetPubKey()) << OP_CHECKSIG;
    CBlock first = CreateSolvedBlock(Params(), firstScript);

    CKey secondKey = CKey::TestOnlyRandomKey(true);
    CScript secondScript = CScript() << ToByteVector(secondKey.GetPubKey()) << OP_CHECKSIG;
    CBlock second = CreateSolvedBlock(Params(), secondScript);
    second.hashPrevBlock = first.GetHash();
    SolveBlock(second, Params());

    CKey thirdKey = CKey::TestOnlyRandomKey(true);
    CScript thirdScript = CScript() << ToByteVector(thirdKey.GetPubKey()) << OP_CHECKSIG;
    CBlock third = CreateSolvedBlock(Params(), thirdScript);
    third.hashPrevBlock = second.GetHash();
    SolveBlock(third, Params());

    zebra_compat::BlockIngestionResult result =
        zebra_compat::IngestBlockBatch(std::vector<CBlock>{first, second, third}, Params());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.hardFailure);

    {
        LOCK(cs_main);
        BOOST_CHECK_LE(chainActive.Height(), oldHeight + 1);
        BOOST_CHECK_NE(chainActive.Tip()->GetBlockHash().GetHex(), second.GetHash().GetHex());
        BOOST_CHECK_NE(chainActive.Tip()->GetBlockHash().GetHex(), third.GetHash().GetHex());
        BOOST_CHECK(mapBlockIndex.find(third.GetHash()) == mapBlockIndex.end());
    }
    BOOST_CHECK_EQUAL(TEST_GetZebraCompatTrustedBlockCandidateCount(), 0);
}

BOOST_AUTO_TEST_CASE(zebra_compat_trusted_boundary_write_failure_reports_fault_before_activation)
{
    ArgsSnapshot snapshot;
    ApplyZebraCompatArgs("-zebra-compat -zebra-compat-url=http://127.0.0.1:8232 -zebra-compat-fail-trusted-boundary-write=1");
    zebra_compat::ClearTrustedBlockBoundary();
    int oldHeight = -1;
    uint256 oldTip;
    {
        LOCK(cs_main);
        oldHeight = chainActive.Height();
        oldTip = chainActive.Tip()->GetBlockHash();
    }

    CKey coinbaseKey = CKey::TestOnlyRandomKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CBlock block = CreateSolvedBlock(Params(), scriptPubKey);

    zebra_compat::BlockIngestionResult result = zebra_compat::IngestBlock(block, Params());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.hardFailure);
    BOOST_CHECK(result.error.find("failed to persist zebra-compat trusted block boundary before activation") != std::string::npos);

    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainActive.Tip() != nullptr);
        BOOST_CHECK_EQUAL(chainActive.Height(), oldHeight);
        BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash().GetHex(), oldTip.GetHex());
        BOOST_CHECK_NE(chainActive.Tip()->GetBlockHash().GetHex(), block.GetHash().GetHex());
    }

    zebra_compat::TrustedBlockBoundary boundary;
    BOOST_CHECK(!zebra_compat::ReadTrustedBlockBoundary(boundary));
    BOOST_CHECK_EQUAL(TEST_GetZebraCompatTrustedBlockCandidateCount(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
#endif
