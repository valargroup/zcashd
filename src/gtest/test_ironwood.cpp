// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include <gtest/gtest.h>

#include "chainparams.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "main.h"
#include "primitives/transaction.h"
#include "util/test.h"
#include "gtest/utils.h"

// The v6 transaction format (ZIP 229) and NU6.3 deployment (ZIP 258)
// constants.
TEST(IronwoodTest, Nu6_3Constants) {
    EXPECT_EQ(NetworkUpgradeInfo[Consensus::UPGRADE_NU6_3].nBranchId, 0x37a5165b);
    EXPECT_EQ(ZIP229_VERSION_GROUP_ID, 0xD884B698);
    EXPECT_EQ(ZIP229_TX_VERSION, 6);
}

// Constructs a minimal v6 transaction: transparent-only, with empty Sapling,
// Orchard, and Ironwood components.
static CMutableTransaction GetValidV6Transaction() {
    CMutableTransaction mtx;
    mtx.fOverwintered = true;
    mtx.nVersion = ZIP229_TX_VERSION;
    mtx.nVersionGroupId = ZIP229_VERSION_GROUP_ID;
    mtx.nConsensusBranchId = NetworkUpgradeInfo[Consensus::UPGRADE_NU6_3].nBranchId;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();
    mtx.vin[0].scriptSig = CScript() << OP_0 << OP_0;
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1000;
    mtx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    return mtx;
}

// A v6 transaction with empty shielded components round-trips through
// serialization, and its txid and auth digest are computed by the
// consensus-critical Rust parser (which must accept the v6 format, including
// the empty-Ironwood-component digest conventions).
TEST(IronwoodTest, EmptyV6TransactionRoundTrip) {
    CMutableTransaction mtx = GetValidV6Transaction();

    // Conversion to CTransaction invokes UpdateHash, which round-trips the
    // serialized transaction through the Rust parser.
    CTransaction tx(mtx);
    EXPECT_FALSE(tx.GetHash().IsNull());
    // v6 transactions have an auth digest (unlike v1-v4).
    EXPECT_FALSE(tx.GetAuthDigest() == uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));

    EXPECT_FALSE(tx.GetSaplingBundle().IsPresent());
    EXPECT_FALSE(tx.GetOrchardBundle().IsPresent());
    EXPECT_FALSE(tx.GetIronwoodBundle().IsPresent());
    EXPECT_EQ(tx.GetIronwoodBundle().GetValueBalance(), 0);
    EXPECT_EQ(tx.GetIronwoodBundle().GetNumActions(), 0);

    // Byte-level round trip.
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << tx;
    CTransaction tx2;
    ss >> tx2;
    EXPECT_EQ(tx.GetHash(), tx2.GetHash());
    EXPECT_EQ(tx.GetAuthDigest(), tx2.GetAuthDigest());
    EXPECT_EQ(tx2.nVersion, ZIP229_TX_VERSION);
    EXPECT_EQ(tx2.nVersionGroupId, ZIP229_VERSION_GROUP_ID);
}

// A v4 transaction and a v6 transaction have distinct wire formats; the v6
// version group id is rejected by the parser when combined with any other
// version number.
TEST(IronwoodTest, InvalidV6VersionRejected) {
    CMutableTransaction mtx = GetValidV6Transaction();
    mtx.nVersion = 5;

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    EXPECT_THROW(ss << mtx, std::ios_base::failure);
}

// v6 transactions are only valid once NU6.3 is active.
TEST(IronwoodTest, V6GatedOnNu6_3Activation) {
    LoadProofParameters();

    // Activate up to NU6.2 only.
    RegtestActivateNU6point2();

    {
        CMutableTransaction mtx = GetValidV6Transaction();
        // Use the NU6.2 branch id so that only the version-group rule can fail.
        mtx.nConsensusBranchId = NetworkUpgradeInfo[Consensus::UPGRADE_NU6_2].nBranchId;
        CTransaction tx(mtx);

        CValidationState state;
        EXPECT_FALSE(ContextualCheckTransaction(tx, state, Params(), 1, true));
        EXPECT_EQ(state.GetRejectReason(), "bad-nu5-tx-version-group-id");
    }

    // Now activate NU6.3.
    RegtestActivateNU6point3();

    {
        CMutableTransaction mtx = GetValidV6Transaction();
        CTransaction tx(mtx);

        CValidationState state;
        EXPECT_TRUE(ContextualCheckTransaction(tx, state, Params(), 1, true));
    }

    // A v5 transaction remains valid after NU6.3.
    {
        CMutableTransaction mtx = GetValidV6Transaction();
        mtx.nVersion = ZIP225_TX_VERSION;
        mtx.nVersionGroupId = ZIP225_VERSION_GROUP_ID;
        CTransaction tx(mtx);

        CValidationState state;
        EXPECT_TRUE(ContextualCheckTransaction(tx, state, Params(), 1, true));
    }

    RegtestDeactivateNU6point3();
}

// The Ironwood tree and pool state are distinct from Orchard's, but the empty
// tree roots coincide (the Ironwood pool uses the Orchard tree structure).
TEST(IronwoodTest, EmptyTreeRoot) {
    EXPECT_EQ(IronwoodMerkleFrontier::empty_root(), OrchardMerkleFrontier::empty_root());

    IronwoodMerkleFrontier tree;
    EXPECT_EQ(tree.root(), IronwoodMerkleFrontier::empty_root());
    EXPECT_EQ(tree.size(), 0);
}
