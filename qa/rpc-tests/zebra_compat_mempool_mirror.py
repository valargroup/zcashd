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

from zebra_compat_polling_sync import FakePollingZebraServer, wait_until


class ZebraCompatMempoolMirrorTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def zebra_compat_args(self, endpoint, extra_args=None):
        args = [
            '-zebra-compat',
            '-zebra-compat-url=%s' % endpoint,
            '-zebra-compat-rpc-user=user',
            '-zebra-compat-rpc-password=pass',
            '-zebra-compat-poll-interval=1',
            '-zebra-compat-sync-batch-size=2',
        ]
        if extra_args:
            args.extend(extra_args)
        return args

    def wait_for_zebra_compat_tip(self, zebra_compat, source):
        wait_until(lambda: zebra_compat.getblockcount() == source.getblockcount() and
                   zebra_compat.getbestblockhash() == source.getbestblockhash(), timeout=60)
        assert_equal(zebra_compat.getzebracompatinfo()['sync']['state'], 'synced')

    def wait_for_mirror_tx(self, zebra_compat, txid):
        wait_until(lambda: txid in zebra_compat.getrawmempool(), timeout=60)
        info = zebra_compat.getzebracompatinfo()['mempool_mirror']
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

    def wait_for_mirror_txs(self, zebra_compat, txids):
        expected = set(txids)
        wait_until(lambda: expected.issubset(set(zebra_compat.getrawmempool())), timeout=60)
        info = zebra_compat.getzebracompatinfo()['mempool_mirror']
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
            zebra_compat = start_node(1, self.options.tmpdir, self.zebra_compat_args(endpoint))
            self.wait_for_zebra_compat_tip(zebra_compat, source)

            txid = source.sendtoaddress(source.getnewaddress(), 1)
            self.wait_for_mirror_tx(zebra_compat, txid)
            assert_equal(set(zebra_compat.getrawmempool()), set(source.getrawmempool()))
            assert txid in zebra_compat.getrawmempool(True)
            assert_equal(zebra_compat.getmempoolinfo()['size'], 1)

            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)
            wait_until(lambda: txid not in zebra_compat.getrawmempool(), timeout=60)
            assert_equal(zebra_compat.getmempoolinfo()['size'], 0)

            parent_txid, child_txid = self.create_chained_mempool_pair(source)
            self.wait_for_mirror_txs(zebra_compat, [parent_txid, child_txid])
            assert_equal(set(zebra_compat.getrawmempool()), set(source.getrawmempool()))

            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)
            wait_until(lambda: zebra_compat.getmempoolinfo()['size'] == 0, timeout=60)

            stop_node(zebra_compat, 1)

            low_fee_parent_txid, low_fee_child_txid = self.create_chained_mempool_pair(source)
            zebra_compat = start_node(1, self.options.tmpdir, self.zebra_compat_args(
                endpoint,
                ['-minrelaytxfee=1.0'],
            ))
            self.wait_for_zebra_compat_tip(zebra_compat, source)
            wait_until(lambda: zebra_compat.getzebracompatinfo()['mempool_mirror']['divergent'] >= 2, timeout=60)
            assert low_fee_parent_txid not in zebra_compat.getrawmempool()
            assert low_fee_child_txid not in zebra_compat.getrawmempool()
            assert_equal(zebra_compat.getzebracompatinfo()['mempool_mirror']['zebra_size'], 2)

            stop_node(zebra_compat, 1)
            stop_node(source, 0)
        finally:
            fake_zebra.stop()


if __name__ == '__main__':
    ZebraCompatMempoolMirrorTest().main()
