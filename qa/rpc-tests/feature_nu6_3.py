#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Integration test for the NU6.3 (Ironwood) network upgrade.

NU6.3 introduces the Ironwood shielded pool (ZIP 229 / ZIP 258): a second
Orchard-protocol pool with its own note commitment tree, nullifier set, and
chain value pool, deployed together with the v6 transaction format.

This test exercises the activation machinery end to end:
- the upgrade activates cleanly at the configured height;
- the Ironwood chain value pool is tracked from activation (and starts at 0);
- the Ironwood note commitment tree state is exposed via z_gettreestate;
- pre-NU6.3 transaction formats (v4/v5) remain usable across the boundary;
- post-NU6.3 coinbase (transparent) remains valid, including the
  empty-Orchard-component rule;
- the node survives a restart across the activation (block index and
  chainstate round-trip the new Ironwood fields).
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    NU6_3_BRANCH_ID,
    assert_equal,
    assert_true,
    connect_nodes_bi,
    get_coinbase_address,
    nuparams,
    start_nodes,
    stop_nodes,
    wait_bitcoinds,
    wait_and_assert_operationid_status,
)
from test_framework.zip317 import conventional_fee

ACTIVATION_HEIGHT = 205


class NU6_3Test(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 4

    def setup_nodes(self):
        self.extra_args = [[
            nuparams(NU6_3_BRANCH_ID, ACTIVATION_HEIGHT),
            '-allowdeprecated=getnewaddress',
            '-allowdeprecated=z_getnewaddress',
            '-allowdeprecated=z_getbalance',
        ]] * self.num_nodes
        return start_nodes(self.num_nodes, self.options.tmpdir, extra_args=self.extra_args)

    def ironwood_pool(self, node):
        pools = node.getblockchaininfo()['valuePools']
        pool = next((p for p in pools if p['id'] == 'ironwood'), None)
        assert pool is not None, "ironwood value pool missing from getblockchaininfo"
        return pool

    def run_test(self):
        # The cached regtest chain starts at height 200.
        assert_equal(self.nodes[0].getblockcount(), 200)

        # The Ironwood value pool is tracked (and empty) before activation.
        assert_equal(self.ironwood_pool(self.nodes[0])['chainValue'], Decimal('0'))

        # NU6.3 should be listed as pending.
        upgrades = self.nodes[0].getblockchaininfo()['upgrades']
        nu6_3 = upgrades['%08x' % NU6_3_BRANCH_ID]
        assert_equal(nu6_3['name'], 'NU6.3')
        assert_equal(nu6_3['status'], 'pending')
        assert_equal(nu6_3['activationheight'], ACTIVATION_HEIGHT)

        # Create a v4 (Sapling) shielded transaction just before activation, to
        # confirm legacy formats keep working across the boundary. Shielding
        # coinbase requires consuming the whole (10 ZEC) coinbase output.
        shielded_amount = Decimal('10') - conventional_fee(3)
        sapling_addr = self.nodes[1].z_getnewaddress('sapling')
        recipients = [{"address": sapling_addr, "amount": shielded_amount}]
        myopid = self.nodes[0].z_sendmany(get_coinbase_address(self.nodes[0]), recipients, 1, conventional_fee(3), 'AllowRevealedSenders')
        wait_and_assert_operationid_status(self.nodes[0], myopid)

        self.sync_all()
        self.nodes[0].generate(4)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), 204)
        assert_equal(Decimal(self.nodes[1].z_getbalance(sapling_addr)), shielded_amount)

        # Mine the activation block.
        self.nodes[0].generate(1)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), ACTIVATION_HEIGHT)

        # NU6.3 is now active.
        upgrades = self.nodes[0].getblockchaininfo()['upgrades']
        nu6_3 = upgrades['%08x' % NU6_3_BRANCH_ID]
        assert_equal(nu6_3['status'], 'active')

        # The consensus branch id reported for the next block is NU6.3's.
        blockchaininfo = self.nodes[0].getblockchaininfo()
        assert_equal(int(blockchaininfo['consensus']['nextblock'], 16), NU6_3_BRANCH_ID)

        # The Ironwood pool exists, has monitored=true, and is still empty
        # (nothing can move into it until wallets can build v6 transactions).
        pool = self.ironwood_pool(self.nodes[0])
        assert_equal(pool['chainValue'], Decimal('0'))
        assert_true(pool['monitored'])

        # z_gettreestate exposes the (empty) Ironwood note commitment tree.
        treestate = self.nodes[0].z_gettreestate(str(ACTIVATION_HEIGHT))
        assert 'ironwood' in treestate, "ironwood treestate missing from z_gettreestate"
        # The final root of the empty Ironwood tree equals the empty Orchard
        # tree root, as the pools share the Orchard tree structure.
        assert_equal(
            treestate['ironwood']['commitments']['finalRoot'],
            treestate['orchard']['commitments']['finalRoot'],
        )

        # getblock exposes the ironwood tree size on post-activation blocks.
        block = self.nodes[0].getblock(str(ACTIVATION_HEIGHT), 2)
        assert_equal(block['trees']['ironwood']['size'], 0)

        # Post-activation, v4/v5 transactions remain valid: send another
        # Sapling transaction and a transparent transaction, and mine them.
        myopid = self.nodes[0].z_sendmany(get_coinbase_address(self.nodes[0]), [
            {"address": sapling_addr, "amount": shielded_amount},
        ], 1, conventional_fee(3), 'AllowRevealedSenders')
        wait_and_assert_operationid_status(self.nodes[0], myopid)

        taddr = self.nodes[2].getnewaddress()
        self.nodes[0].sendtoaddress(taddr, Decimal('0.5'))

        self.sync_all()
        self.nodes[0].generate(2)
        self.sync_all()

        assert_equal(Decimal(self.nodes[1].z_getbalance(sapling_addr)), 2 * shielded_amount)
        assert_equal(Decimal(self.nodes[2].z_getbalance(taddr)), Decimal('0.5'))

        # Mine a few more blocks inside the NU6.3 epoch (exercises the V3
        # history tree nodes and the post-activation coinbase rules).
        self.nodes[0].generate(5)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), ACTIVATION_HEIGHT + 7)

        # Restart all nodes to confirm the block index and chainstate
        # round-trip the Ironwood fields (hashFinalIronwoodRoot, nIronwoodValue,
        # the Ironwood anchors/nullifiers, and the V3 history tree nodes).
        stop_nodes(self.nodes)
        wait_bitcoinds()
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir, extra_args=self.extra_args)
        connect_nodes_bi(self.nodes, 0, 1)
        connect_nodes_bi(self.nodes, 1, 2)
        connect_nodes_bi(self.nodes, 2, 3)

        assert_equal(self.nodes[0].getblockcount(), ACTIVATION_HEIGHT + 7)
        assert_equal(self.ironwood_pool(self.nodes[0])['chainValue'], Decimal('0'))

        # The chain still extends after the restart.
        self.nodes[0].generate(1)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), ACTIVATION_HEIGHT + 8)


if __name__ == '__main__':
    NU6_3Test().main()
