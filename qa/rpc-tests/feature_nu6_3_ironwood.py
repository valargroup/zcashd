#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    NU6_3_BRANCH_ID,
    assert_equal,
    nuparams,
    start_nodes,
)


IRONWOOD_TREE_EMPTY_ROOT = "ae2935f1dfd8a24aed7c70df7de3a668eb7a49b1319880dde2bbd9031ae5d82f"


class IronwoodRpcSurfaceTest(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 1

    def setup_nodes(self):
        # The cached regtest chain starts at height 200. Activate NU6.3 at the
        # next block so the test can assert both sides of the RPC boundary.
        return start_nodes(self.num_nodes, self.options.tmpdir, extra_args=[[
            nuparams(NU6_3_BRANCH_ID, 201),
            '-experimentalfeatures',
            '-lightwalletd',
        ]] * self.num_nodes)

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


if __name__ == '__main__':
    IronwoodRpcSurfaceTest().main()
