#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    NU6_3_BRANCH_ID,
    assert_equal,
    get_coinbase_address,
    nuparams,
    start_node,
    start_nodes,
    stop_node,
    wait_and_assert_operationid_status,
)
from test_framework.zip317 import conventional_fee

from decimal import Decimal


IRONWOOD_TREE_EMPTY_ROOT = "ae2935f1dfd8a24aed7c70df7de3a668eb7a49b1319880dde2bbd9031ae5d82f"
IRONWOOD_WALLET_UNSUPPORTED = "Failed to build transaction: Ironwood wallet support is not yet available in zcashd; Orchard third-party payments are invalid from NU6.3"
NODE_ARGS = [
    nuparams(NU6_3_BRANCH_ID, 201),
    '-reindex',
    '-experimentalfeatures',
    '-lightwalletd',
]
RESTART_NODE_ARGS = [
    nuparams(NU6_3_BRANCH_ID, 201),
    '-experimentalfeatures',
    '-lightwalletd',
]


class IronwoodRpcSurfaceTest(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 1

    def setup_nodes(self):
        # The cached regtest chain starts at height 200. Activate NU6.3 at the
        # next block so the test can assert both sides of the RPC boundary.
        return start_nodes(self.num_nodes, self.options.tmpdir, extra_args=[NODE_ARGS] * self.num_nodes)

    def setup_network(self, split=False):
        self.nodes = self.setup_nodes()
        self.is_network_split = False
        self.sync_all()

    def assert_has_ironwood_pool(self, value_pools):
        ironwood = next(pool for pool in value_pools if pool['id'] == 'ironwood')
        assert 'monitored' in ironwood
        return ironwood

    def run_test(self):
        node = self.nodes[0]
        assert_equal(node.getblockcount(), 200)

        pre_activation_block = node.getblock('200')
        assert 'finalironwoodroot' not in pre_activation_block
        assert 'ironwood' not in pre_activation_block['trees']
        self.assert_has_ironwood_pool(pre_activation_block['valuePools'])
        self.assert_has_ironwood_pool(node.getblockchaininfo()['valuePools'])

        pre_activation_treestate = node.z_gettreestate('200')
        assert 'ironwood' not in pre_activation_treestate

        node.generate(1)
        assert_equal(node.getblockcount(), 201)

        activation_block = node.getblock('201')
        assert_equal(activation_block['finalironwoodroot'], IRONWOOD_TREE_EMPTY_ROOT)
        assert_equal(activation_block['trees']['ironwood']['size'], 0)
        self.assert_has_ironwood_pool(activation_block['valuePools'])
        self.assert_has_ironwood_pool(node.getblockchaininfo()['valuePools'])

        activation_treestate = node.z_gettreestate('201')
        assert_equal(activation_treestate['ironwood']['commitments']['finalRoot'], IRONWOOD_TREE_EMPTY_ROOT)
        assert_equal(activation_treestate['ironwood']['commitments']['finalState'], "000000")
        assert 'skipHash' not in activation_treestate['ironwood']

        subtrees = node.z_getsubtreesbyindex('ironwood', 0)
        assert_equal(subtrees['pool'], 'ironwood')
        assert_equal(subtrees['start_index'], 0)
        assert_equal(len(subtrees['subtrees']), 0)

        # Exercise disconnect/reconnect across the activation boundary.
        activation_hash = node.getblockhash(201)
        post_activation_hashes = node.generate(2)
        tip_hash = post_activation_hashes[-1]
        assert_equal(node.getblockcount(), 203)
        assert_equal(node.getblock(tip_hash)['finalironwoodroot'], IRONWOOD_TREE_EMPTY_ROOT)

        # Restart with post-NU6.3 chainstate on disk to cover the Ironwood data-version gate.
        stop_node(node, 0)
        self.nodes[0] = start_node(0, self.options.tmpdir, RESTART_NODE_ARGS)
        node = self.nodes[0]
        assert_equal(node.getbestblockhash(), tip_hash)
        assert_equal(node.getblock(tip_hash)['finalironwoodroot'], IRONWOOD_TREE_EMPTY_ROOT)

        node.invalidateblock(activation_hash)
        assert_equal(node.getblockcount(), 200)
        assert 'finalironwoodroot' not in node.getblock('200')
        assert 'ironwood' not in node.z_gettreestate('200')

        node.reconsiderblock(activation_hash)
        assert_equal(node.getbestblockhash(), tip_hash)
        assert_equal(node.getblockcount(), 203)
        assert_equal(node.getblock(tip_hash)['finalironwoodroot'], IRONWOOD_TREE_EMPTY_ROOT)

        # Milestone 1 deliberately has no Ironwood wallet support. Orchard wallet sends
        # after NU6.3 should fail with a clear preparation/build error.
        acct = node.z_getnewaccount()['account']
        orchard_ua = node.z_getaddressforaccount(acct, ['orchard'])['address']
        fee = conventional_fee(3)
        opid = node.z_sendmany(
            get_coinbase_address(node),
            [{"address": orchard_ua, "amount": Decimal('10') - fee}],
            1,
            fee,
            'AllowRevealedSenders')
        wait_and_assert_operationid_status(node, opid, 'failed', IRONWOOD_WALLET_UNSUPPORTED)


if __name__ == '__main__':
    IronwoodRpcSurfaceTest().main()
