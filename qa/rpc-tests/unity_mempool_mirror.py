#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_node,
    stop_node,
)

from decimal import Decimal

from unity_polling_sync import FakePollingZebraServer, wait_until


class UnityMempoolMirrorTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def unity_args(self, endpoint, extra_args=None):
        args = [
            '-unity',
            '-unityzebra=%s' % endpoint,
            '-unityzebrarpcuser=user',
            '-unityzebrarpcpassword=pass',
            '-unitypollinterval=1',
            '-unitysyncbatchsize=2',
        ]
        if extra_args:
            args.extend(extra_args)
        return args

    def wait_for_unity_tip(self, unity, source):
        wait_until(lambda: unity.getblockcount() == source.getblockcount() and
                   unity.getbestblockhash() == source.getbestblockhash(), timeout=60)
        assert_equal(unity.getunityinfo()['sync']['state'], 'synced')

    def wait_for_mirror_tx(self, unity, txid):
        wait_until(lambda: txid in unity.getrawmempool(), timeout=60)
        info = unity.getunityinfo()['mempool_mirror']
        assert_equal(info['source'], 'zebra-poll')
        assert_equal(info['divergent'], 0)
        assert_equal(info['zebra_size'], 1)
        assert_equal(info['local_size'], 1)

    def create_chained_mempool_pair(self, node):
        parent_txid = node.sendtoaddress(node.getnewaddress(), 1)
        parent_outputs = [utxo for utxo in node.listunspent(0, 999999) if utxo['txid'] == parent_txid]
        assert_equal(len(parent_outputs), 1)

        parent_output = parent_outputs[0]
        child_raw = node.createrawtransaction(
            [{'txid': parent_output['txid'], 'vout': parent_output['vout']}],
            {node.getnewaddress(): parent_output['amount'] - Decimal('0.0001')},
        )
        child_signed = node.signrawtransaction(child_raw)
        assert child_signed['complete']
        child_txid = node.sendrawtransaction(child_signed['hex'])
        return parent_txid, child_txid

    def wait_for_mirror_txs(self, unity, txids):
        expected = set(txids)
        wait_until(lambda: expected.issubset(set(unity.getrawmempool())), timeout=60)
        info = unity.getunityinfo()['mempool_mirror']
        assert_equal(info['source'], 'zebra-poll')
        assert_equal(info['divergent'], 0)
        assert_equal(info['zebra_size'], len(expected))
        assert_equal(info['local_size'], len(expected))

    def run_test(self):
        source = start_node(0, self.options.tmpdir, [])
        source.generate(101)

        fake_zebra = FakePollingZebraServer(source)
        endpoint = fake_zebra.start()
        try:
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            self.wait_for_unity_tip(unity, source)

            txid = source.sendtoaddress(source.getnewaddress(), 1)
            self.wait_for_mirror_tx(unity, txid)
            assert_equal(set(unity.getrawmempool()), set(source.getrawmempool()))
            assert txid in unity.getrawmempool(True)
            assert_equal(unity.getmempoolinfo()['size'], 1)

            source.generate(1)
            self.wait_for_unity_tip(unity, source)
            wait_until(lambda: txid not in unity.getrawmempool(), timeout=60)
            assert_equal(unity.getmempoolinfo()['size'], 0)

            parent_txid, child_txid = self.create_chained_mempool_pair(source)
            self.wait_for_mirror_txs(unity, [parent_txid, child_txid])
            assert_equal(set(unity.getrawmempool()), set(source.getrawmempool()))

            source.generate(1)
            self.wait_for_unity_tip(unity, source)
            wait_until(lambda: unity.getmempoolinfo()['size'] == 0, timeout=60)

            stop_node(unity, 1)

            low_fee_parent_txid, low_fee_child_txid = self.create_chained_mempool_pair(source)
            unity = start_node(1, self.options.tmpdir, self.unity_args(
                endpoint,
                ['-minrelaytxfee=1.0'],
            ))
            self.wait_for_unity_tip(unity, source)
            wait_until(lambda: unity.getunityinfo()['mempool_mirror']['divergent'] >= 2, timeout=60)
            assert low_fee_parent_txid not in unity.getrawmempool()
            assert low_fee_child_txid not in unity.getrawmempool()
            assert_equal(unity.getunityinfo()['mempool_mirror']['zebra_size'], 2)

            stop_node(unity, 1)
            stop_node(source, 0)
        finally:
            fake_zebra.stop()


if __name__ == '__main__':
    UnityMempoolMirrorTest().main()
