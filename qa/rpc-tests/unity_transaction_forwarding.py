#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_node,
    stop_node,
)

from unity_polling_sync import FakePollingZebraServer, wait_until


class UnityTransactionForwardingTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def unity_args(self, endpoint):
        return [
            '-unity',
            '-unityzebra=%s' % endpoint,
            '-unityzebrarpcuser=user',
            '-unityzebrarpcpassword=pass',
            '-unitypollinterval=1',
            '-unitysyncbatchsize=2',
        ]

    def wait_for_unity_tip(self, unity, source):
        wait_until(lambda: unity.getblockcount() == source.getblockcount() and
                   unity.getbestblockhash() == source.getbestblockhash(), timeout=60)
        assert_equal(unity.getunityinfo()['sync']['state'], 'synced')

    def make_signed_tx(self, source):
        utxo = source.listunspent()[0]
        amount = utxo['amount'] - Decimal('0.0001')
        raw = source.createrawtransaction(
            [{'txid': utxo['txid'], 'vout': utxo['vout']}],
            {source.getnewaddress(): amount},
        )
        signed = source.signrawtransaction(raw)
        assert signed['complete']
        txid = source.decoderawtransaction(signed['hex'])['txid']
        return signed['hex'], txid

    def wait_for_mirror_poll_after(self, fake_zebra, previous_count, polls=1):
        wait_until(lambda: fake_zebra.mempool_poll_count() >= previous_count + polls, timeout=60)

    def run_test(self):
        source = start_node(0, self.options.tmpdir, [])
        source.generate(101)

        fake_zebra = FakePollingZebraServer(source)
        endpoint = fake_zebra.start()
        try:
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            self.wait_for_unity_tip(unity, source)

            tx_hex, txid = self.make_signed_tx(source)
            assert_equal(unity.sendrawtransaction(tx_hex), txid)
            wait_until(lambda: txid in source.getrawmempool(), timeout=60)
            wait_until(lambda: txid in unity.getrawmempool(), timeout=60)
            wait_until(lambda: unity.getunityinfo()['tx_forwarding']['pending'] == 0, timeout=60)

            source.generate(1)
            self.wait_for_unity_tip(unity, source)
            wait_until(lambda: txid not in unity.getrawmempool(), timeout=60)

            unity_receive = unity.getnewaddress()
            source.sendtoaddress(unity_receive, Decimal('1.0'))
            source.generate(1)
            self.wait_for_unity_tip(unity, source)
            wait_until(lambda: unity.getbalance() >= Decimal('1.0'), timeout=60)

            wallet_txids_before = [entry['txid'] for entry in unity.listtransactions('*', 1000, 0)]
            unity_mempool_before = set(unity.getrawmempool())
            source_mempool_before = set(source.getrawmempool())
            fake_zebra.set_reject_sendraw(True)
            try:
                unity.sendtoaddress(source.getnewaddress(), Decimal('0.1'))
                raise AssertionError('sendtoaddress unexpectedly succeeded')
            except JSONRPCException as e:
                assert_equal(e.error['code'], -4)
                assert 'Unity transaction forwarding failed' in e.error['message']
                assert 'zebra rejected transaction' in e.error['message']
            fake_zebra.set_reject_sendraw(False)
            wallet_txids_after = [entry['txid'] for entry in unity.listtransactions('*', 1000, 0)]
            assert_equal(wallet_txids_after, wallet_txids_before)
            assert_equal(set(unity.getrawmempool()), unity_mempool_before)
            assert_equal(set(source.getrawmempool()), source_mempool_before)

            reject_hex, reject_txid = self.make_signed_tx(source)
            fake_zebra.set_reject_sendraw(True)
            try:
                unity.sendrawtransaction(reject_hex)
                raise AssertionError('sendrawtransaction unexpectedly succeeded')
            except JSONRPCException as e:
                assert_equal(e.error['code'], -26)
                assert 'zebra rejected transaction' in e.error['message']
            fake_zebra.set_reject_sendraw(False)
            assert reject_txid not in unity.getrawmempool()
            assert reject_txid not in source.getrawmempool()

            grace_hex, grace_txid = self.make_signed_tx(source)
            fake_zebra.hide_mempool_txid(grace_txid)
            before_polls = fake_zebra.mempool_poll_count()
            assert_equal(unity.sendrawtransaction(grace_hex), grace_txid)
            wait_until(lambda: grace_txid in source.getrawmempool(), timeout=60)
            wait_until(lambda: grace_txid in unity.getrawmempool(), timeout=60)
            self.wait_for_mirror_poll_after(fake_zebra, before_polls, polls=2)
            assert grace_txid in unity.getrawmempool()
            assert_equal(unity.getunityinfo()['tx_forwarding']['pending'], 1)

            fake_zebra.unhide_mempool_txid(grace_txid)
            wait_until(lambda: unity.getunityinfo()['tx_forwarding']['pending'] == 0, timeout=60)
            assert grace_txid in unity.getrawmempool()

            stop_node(unity, 1)
            stop_node(source, 0)
        finally:
            fake_zebra.stop()


if __name__ == '__main__':
    UnityTransactionForwardingTest().main()
