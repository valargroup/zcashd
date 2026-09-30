#include "arith_uint256.h"
#include "chainparams.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "crypto/equihash.h"
#include "keystore.h"
#include "main.h"
#include "miner.h"
#include "pow.h"
#include "script/sign.h"
#include "test/test_bitcoin.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

// NU7 on a regtest chain: ZIP 235 and ZIP 237 in ConnectBlock, with blocks mined across
// NU7 with fees, NSM reissuance, invalid coinbase claims, and a block index reload.

#ifdef ENABLE_MINING

namespace {

const int NU7_HEIGHT = 105;
const CAmount FEE = 10001;

/** A regtest chain with every upgrade through NU6.3 at height 1 and NU7 at NU7_HEIGHT. */
struct Nu7ChainSetup : public TestingSetup {
    std::vector<int> originalHeights;
    CBasicKeyStore keystore;
    CScript coinbaseScript;
    /** The coinbase transaction of the block at each height, starting at height 1. */
    std::vector<CTransaction> coinbaseTxns;

    Nu7ChainSetup() : TestingSetup(CBaseChainParams::REGTEST) {
        for (int idx = Consensus::BASE_SPROUT; idx < Consensus::MAX_NETWORK_UPGRADES; idx++) {
            originalHeights.push_back(Params().GetConsensus().vUpgrades[idx].nActivationHeight);
        }
        for (int idx = Consensus::UPGRADE_OVERWINTER; idx <= Consensus::UPGRADE_NU6_3; idx++) {
            UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex(idx), 1);
        }
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_NU7, NU7_HEIGHT);

        const CKey coinbaseKey = CKey::TestOnlyRandomKey(true);
        keystore.AddKey(coinbaseKey);
        coinbaseScript = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    }

    ~Nu7ChainSetup() {
        UpdateRegtestNSMReissuanceHeightForTesting(std::nullopt);
        UpdateRegtestInitialNSMValueBalance(std::nullopt);
        for (int idx = Consensus::BASE_SPROUT + 1; idx < Consensus::MAX_NETWORK_UPGRADES; idx++) {
            UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex(idx), originalHeights[idx]);
        }
    }

    /** A template for the next block, with the mempool's transactions, paying coinbaseScript. */
    std::unique_ptr<CBlockTemplate> Template() {
        boost::shared_ptr<CReserveScript> script(new CReserveScript());
        script->reserveScript = coinbaseScript;
        return std::unique_ptr<CBlockTemplate>(BlockAssembler(Params()).CreateNewBlock(script));
    }

    /** Solves the Equihash proof of work for `block`. */
    static void Solve(CBlock& block) {
        const auto& consensus = Params().GetConsensus();
        eh_HashState eh_state = EhInitialiseState(consensus.nEquihashN, consensus.nEquihashK);
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
                [&block, &consensus](std::vector<unsigned char> soln) {
                    block.nSolution = soln;
                    return CheckProofOfWork(block.GetHash(), block.nBits, consensus);
                };
            found = EhBasicSolveUncancellable(consensus.nEquihashN, consensus.nEquihashK, curr_state, validBlock);
        } while (!found);
    }

    /** Mines the next block with the mempool's transactions, and requires it to become the tip. */
    CBlock MineBlock() {
        auto tmpl = Template();
        CBlock& block = tmpl->block;
        unsigned int extraNonce = 0;
        IncrementExtraNonce(tmpl.get(), chainActive.Tip(), extraNonce, Params().GetConsensus());
        Solve(block);

        CValidationState state;
        ProcessNewBlock(state, Params(), NULL, &block, true, NULL);
        BOOST_REQUIRE(chainActive.Tip()->GetBlockHash() == block.GetHash());
        coinbaseTxns.push_back(block.vtx[0]);
        return block;
    }

    /** Mines blocks until the tip is at nHeight. */
    void MineTo(int nHeight) {
        while (chainActive.Height() < nHeight) {
            MineBlock();
        }
    }

    /** The next block template, with its first coinbase output changed by delta. */
    std::unique_ptr<CBlockTemplate> TemplateWithCoinbaseDelta(CAmount delta) {
        auto tmpl = Template();
        CMutableTransaction coinbase(tmpl->block.vtx[0]);
        coinbase.vout[0].nValue += delta;
        tmpl->block.vtx[0] = coinbase;
        unsigned int extraNonce = 0;
        IncrementExtraNonce(tmpl.get(), chainActive.Tip(), extraNonce, Params().GetConsensus());
        return tmpl;
    }

    /** The reason TestNewBlockAtTipValidity rejects TemplateWithCoinbaseDelta(delta). */
    std::string RejectReasonWithCoinbaseDelta(CAmount delta) {
        auto tmpl = TemplateWithCoinbaseDelta(delta);
        LOCK(cs_main);
        CValidationState state;
        BOOST_CHECK(!TestNewBlockAtTipValidity(state, Params(), tmpl->block, true));
        return state.GetRejectReason();
    }

    /**
     * A transaction for the next block that spends the coinbase output of the block at
     * `height` back to coinbaseScript, paying `fee`, with the default expiry or `expiry`.
     */
    CMutableTransaction FeeTransaction(int height, CAmount fee, std::optional<uint32_t> expiry = std::nullopt) {
        const auto& consensus = Params().GetConsensus();
        const CTransaction& prev = coinbaseTxns.at(height - 1);
        const int nextHeight = chainActive.Height() + 1;

        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(consensus, nextHeight, false);
        if (expiry.has_value()) {
            mtx.nExpiryHeight = expiry.value();
        }
        mtx.vin.emplace_back(COutPoint(prev.GetHash(), 0));
        mtx.vout.emplace_back(prev.vout[0].nValue - fee, coinbaseScript);
        const PrecomputedTransactionData txdata(mtx, {prev.vout[0]});
        BOOST_REQUIRE(SignSignature(
            keystore, prev.vout[0].scriptPubKey, mtx, txdata, 0, prev.vout[0].nValue, SIGHASH_ALL,
            CurrentEpochBranchId(nextHeight, consensus)));
        return mtx;
    }

    /** Adds FeeTransaction(height, fee) to the mempool without checking it. */
    void AddFeeTransaction(int height, CAmount fee) {
        CMutableTransaction mtx = FeeTransaction(height, fee);
        const uint32_t branchId = CurrentEpochBranchId(chainActive.Height() + 1, Params().GetConsensus());
        TestMemPoolEntryHelper entry;
        mempool.addUnchecked(
            mtx.GetHash(), entry.Fee(fee).Time(GetTime()).SpendsCoinbase(true).BranchId(branchId).FromTx(mtx));
    }

    /** Submits `mtx` to the mempool, and returns the reject reason if it is refused. */
    std::optional<std::string> Submit(const CMutableTransaction& mtx) {
        LOCK(cs_main);
        CValidationState state;
        if (AcceptToMemoryPool(Params(), mempool, state, CTransaction(mtx), false, nullptr)) {
            return std::nullopt;
        }
        int dos = -1;
        BOOST_CHECK(state.IsInvalid(dos));
        BOOST_CHECK_EQUAL(dos, 0);
        return state.GetRejectReason();
    }
};

/** The NSM value balance after `pindex`, which must be known. */
CAmount NSMValueBalance(const CBlockIndex* pindex) {
    BOOST_REQUIRE(pindex->nChainNSMValueBalance.has_value());
    return pindex->nChainNSMValueBalance.value();
}

/** The chain supply delta of the tip. */
CAmount TipSupplyDelta() {
    return chainActive.Tip()->nChainTotalSupply.value() - chainActive.Tip()->pprev->nChainTotalSupply.value();
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(nu7_chain_tests, Nu7ChainSetup)

BOOST_AUTO_TEST_CASE(fees_burn_from_nu7)
{
    const auto& consensus = Params().GetConsensus();
    MineTo(NU7_HEIGHT - 2);

    // The last block before NU7 still pays the miner the whole fee.
    AddFeeTransaction(1, FEE);
    CBlock last = MineBlock();
    BOOST_CHECK_EQUAL(last.vtx.size(), size_t(2));
    BOOST_CHECK_EQUAL(last.vtx[0].GetValueOut(), consensus.GetBlockSubsidy(NU7_HEIGHT - 1) + FEE);

    // Every regtest block claimed exactly its subsidy and fees, so the derived initial
    // balance is zero.
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), 0);

    // From NU7 the miner keeps 8,001 of the two 10,001 fees, and 12,001 is burned.
    AddFeeTransaction(2, FEE);
    AddFeeTransaction(3, FEE);
    BOOST_CHECK_EQUAL(RejectReasonWithCoinbaseDelta(2 * FEE - 8001), "bad-cb-amount");
    BOOST_CHECK_EQUAL(RejectReasonWithCoinbaseDelta(1), "bad-cb-amount");
    BOOST_CHECK_EQUAL(RejectReasonWithCoinbaseDelta(-1), "bad-cb-not-exact");

    CBlock first = MineBlock();
    BOOST_CHECK_EQUAL(first.vtx.size(), size_t(3));
    BOOST_CHECK_EQUAL(first.vtx[0].GetValueOut(), consensus.GetBlockSubsidy(NU7_HEIGHT) + 8001);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), 12001);
    BOOST_CHECK_EQUAL(TipSupplyDelta(), consensus.GetBlockSubsidy(NU7_HEIGHT) - 12001);

    // Without fees the balance is unchanged.
    MineBlock();
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), 12001);
}

BOOST_AUTO_TEST_CASE(reissuance_pays_the_additional_subsidy)
{
    const auto& consensus = Params().GetConsensus();
    const CAmount initialBalance = 1000000000000;
    UpdateRegtestInitialNSMValueBalance(initialBalance);
    UpdateRegtestNSMReissuanceHeightForTesting(NU7_HEIGHT + 2);

    // The configured initial balance applies after the block before NU7.
    MineTo(NU7_HEIGHT + 1);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive[NU7_HEIGHT - 2]), 0);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive[NU7_HEIGHT - 1]), initialBalance);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), initialBalance);

    // The first reissuance block must also claim ceil(1375 * 10^12 / 10^10) = 137,500.
    BOOST_CHECK_EQUAL(RejectReasonWithCoinbaseDelta(-137500), "bad-cb-not-exact");
    BOOST_CHECK_EQUAL(RejectReasonWithCoinbaseDelta(1), "bad-cb-amount");
    CBlock block = MineBlock();
    BOOST_CHECK_EQUAL(block.vtx[0].GetValueOut(), consensus.GetBlockSubsidy(NU7_HEIGHT + 2) + 137500);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), initialBalance - 137500);
    BOOST_CHECK_EQUAL(TipSupplyDelta(), consensus.GetBlockSubsidy(NU7_HEIGHT + 2) + 137500);

    // The burn and the payout combine: ceil(1375 * (10^12 - 137,500) / 10^10) is still 137,500.
    AddFeeTransaction(1, FEE);
    AddFeeTransaction(2, FEE);
    block = MineBlock();
    BOOST_CHECK_EQUAL(block.vtx.size(), size_t(3));
    BOOST_CHECK_EQUAL(block.vtx[0].GetValueOut(), consensus.GetBlockSubsidy(NU7_HEIGHT + 3) + 137500 + 8001);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), initialBalance - 2 * 137500 + 12001);
}

BOOST_AUTO_TEST_CASE(balance_is_recomputed_on_reload)
{
    UpdateRegtestInitialNSMValueBalance(1000000000000);
    UpdateRegtestNSMReissuanceHeightForTesting(NU7_HEIGHT + 2);
    MineTo(NU7_HEIGHT - 1);
    AddFeeTransaction(1, FEE);
    MineBlock();
    MineTo(NU7_HEIGHT + 2);
    AddFeeTransaction(2, FEE);
    MineBlock();

    std::map<uint256, CAmount> balances;
    for (int height = 0; height <= chainActive.Height(); height++) {
        balances[chainActive[height]->GetBlockHash()] = NSMValueBalance(chainActive[height]);
    }
    // 10^12 + 6,000 burned at NU7, then 137,501 and 137,500 reissued and 6,000 burned.
    const uint256 tipHash = chainActive.Tip()->GetBlockHash();
    BOOST_CHECK_EQUAL(balances[tipHash], 999999736999);

    FlushStateToDisk();
    UnloadBlockIndex();
    BOOST_REQUIRE(LoadBlockIndex());
    BOOST_REQUIRE(chainActive.Tip() != nullptr);
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() == tipHash);
    for (const auto& [hash, balance] : balances) {
        BOOST_CHECK_EQUAL(NSMValueBalance(mapBlockIndex.at(hash)), balance);
    }
}

BOOST_AUTO_TEST_CASE(failed_block_does_not_stop_reload)
{
    MineTo(NU7_HEIGHT);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), 0);
    const uint256 tipHash = chainActive.Tip()->GetBlockHash();

    // A block claiming one zatoshi too many fails in ConnectBlock after its chain supply
    // delta is stored, and that delta implies an NSM value balance of -1.
    auto tmpl = TemplateWithCoinbaseDelta(1);
    CBlock& block = tmpl->block;
    Solve(block);
    CValidationState state;
    ProcessNewBlock(state, Params(), NULL, &block, true, NULL);
    BOOST_REQUIRE(chainActive.Tip()->GetBlockHash() == tipHash);
    BOOST_REQUIRE(mapBlockIndex.at(block.GetHash())->nStatus & BLOCK_FAILED_VALID);
    BOOST_REQUIRE(mapBlockIndex.at(block.GetHash())->nChainSupplyDelta.has_value());

    FlushStateToDisk();
    UnloadBlockIndex();
    BOOST_REQUIRE(LoadBlockIndex());
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() == tipHash);
    BOOST_CHECK_EQUAL(NSMValueBalance(chainActive.Tip()), 0);
    BOOST_CHECK(!mapBlockIndex.at(block.GetHash())->nChainNSMValueBalance.has_value());
}

BOOST_AUTO_TEST_CASE(mempool_refuses_what_zakura_would_reject)
{
    // Zakura may be up to RELAY_HEIGHT_MARGIN blocks ahead, so from next height
    // NU7_HEIGHT - 3 a transaction committing to NU6.3 could reach Zakura after it has
    // activated NU7. (Without an expiry, it is not refused as expiring soon.)
    MineTo(NU7_HEIGHT - 5);
    BOOST_CHECK(Submit(FeeTransaction(1, FEE, 0)) == std::nullopt);
    MineBlock();
    BOOST_CHECK(Submit(FeeTransaction(2, FEE, 0)) == std::string("tx-invalid-at-relay-height"));

    // From NU7, transactions committing to NU7 are accepted again.
    MineTo(NU7_HEIGHT);
    BOOST_CHECK(Submit(FeeTransaction(3, FEE)) == std::nullopt);
}

BOOST_AUTO_TEST_SUITE_END()

#endif // ENABLE_MINING
