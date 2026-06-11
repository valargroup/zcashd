#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

import time

from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_node,
    stop_node,
)

from zebra_compat_polling_sync import FakePollingZebraServer, wait_until


class ZebraCompatTransactionForwardingTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2
        self.mock_time = int(time.time())

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def zebra_compat_args(self, endpoint):
        return [
            '-allowdeprecated=getnewaddress',
            '-zebra-compat',
            '-zebra-compat-url=%s' % endpoint,
            '-zebra-compat-rpc-user=user',
            '-zebra-compat-rpc-password=pass',
            '-zebra-compat-poll-interval=1',
            '-zebra-compat-sync-batch-size=2',
            '-mocktime=%d' % self.mock_time,
        ]

    def wait_for_zebra_compat_tip(self, zebra_compat, source):
        wait_until(lambda: zebra_compat.getblockcount() == source.getblockcount() and
                   zebra_compat.getbestblockhash() == source.getbestblockhash(), timeout=60)
        assert_equal(zebra_compat.getzebracompatinfo()['sync']['state'], 'synced')

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

    def make_absurd_fee_tx(self, source):
        utxo = None
        for candidate in source.listunspent():
            if candidate['amount'] == Decimal('10.0'):
                utxo = candidate
                break
        assert utxo is not None

        raw = source.createrawtransaction(
            [{'txid': utxo['txid'], 'vout': utxo['vout']}],
            {source.getnewaddress(): Decimal('1.0')},
        )
        signed = source.signrawtransaction(raw)
        assert signed['complete']
        txid = source.decoderawtransaction(signed['hex'])['txid']
        return signed['hex'], txid

    def wait_for_mirror_poll_after(self, fake_zebra, previous_count, polls=1):
        wait_until(lambda: fake_zebra.mempool_poll_count() >= previous_count + polls, timeout=60)

    def run_test(self):
        source = start_node(0, self.options.tmpdir, ['-allowdeprecated=getnewaddress'])
        source.generate(101)

        fake_zebra = FakePollingZebraServer(source)
        endpoint = fake_zebra.start()
        try:
            zebra_compat = start_node(1, self.options.tmpdir, self.zebra_compat_args(endpoint))
            self.wait_for_zebra_compat_tip(zebra_compat, source)

            tx_hex, txid = self.make_signed_tx(source)
            assert_equal(zebra_compat.sendrawtransaction(tx_hex), txid)
            wait_until(lambda: txid in source.getrawmempool(), timeout=60)
            wait_until(lambda: txid in zebra_compat.getrawmempool(), timeout=60)
            wait_until(lambda: zebra_compat.getzebracompatinfo()['tx_forwarding']['pending'] == 0, timeout=60)

            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)
            wait_until(lambda: txid not in zebra_compat.getrawmempool(), timeout=60)

            zebra_compat_receive = zebra_compat.getnewaddress()
            source.sendtoaddress(zebra_compat_receive, Decimal('1.0'))
            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)
            wait_until(lambda: zebra_compat.getbalance() >= Decimal('1.0'), timeout=60)

            rebroadcast_txid = zebra_compat.sendtoaddress(source.getnewaddress(), Decimal('0.1'))
            wait_until(lambda: rebroadcast_txid in source.getrawmempool(), timeout=60)
            wait_until(lambda: rebroadcast_txid in zebra_compat.getrawmempool(), timeout=60)
            assert_equal(fake_zebra.sendrawtransaction_count(rebroadcast_txid), 1)

            fake_zebra.hide_mempool_txid(rebroadcast_txid)
            rebroadcasted = zebra_compat.resendwallettransactions()
            assert rebroadcast_txid in rebroadcasted
            assert_equal(fake_zebra.sendrawtransaction_count(rebroadcast_txid), 2)

            fake_zebra.set_reject_sendraw(True)
            try:
                failed_rebroadcasted = zebra_compat.resendwallettransactions()
            finally:
                fake_zebra.set_reject_sendraw(False)
            assert rebroadcast_txid not in failed_rebroadcasted
            assert_equal(fake_zebra.sendrawtransaction_count(rebroadcast_txid), 3)

            fake_zebra.unhide_mempool_txid(rebroadcast_txid)
            wait_until(lambda: zebra_compat.getzebracompatinfo()['tx_forwarding']['pending'] == 0, timeout=60)

            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)
            wait_until(lambda: rebroadcast_txid not in zebra_compat.getrawmempool(), timeout=60)

            wallet_txids_before = [entry['txid'] for entry in zebra_compat.listtransactions('*', 1000, 0)]
            zebra_compat_mempool_before = set(zebra_compat.getrawmempool())
            source_mempool_before = set(source.getrawmempool())
            fake_zebra.set_reject_sendraw(True)
            try:
                zebra_compat.sendtoaddress(source.getnewaddress(), Decimal('0.1'))
                raise AssertionError('sendtoaddress unexpectedly succeeded')
            except JSONRPCException as e:
                assert_equal(e.error['code'], -4)
                assert 'zebra-compat transaction forwarding failed' in e.error['message']
                assert 'zebra rejected transaction' in e.error['message']
            fake_zebra.set_reject_sendraw(False)
            wallet_txids_after = [entry['txid'] for entry in zebra_compat.listtransactions('*', 1000, 0)]
            assert_equal(wallet_txids_after, wallet_txids_before)
            assert_equal(set(zebra_compat.getrawmempool()), zebra_compat_mempool_before)
            assert_equal(set(source.getrawmempool()), source_mempool_before)

            reject_hex, reject_txid = self.make_signed_tx(source)
            fake_zebra.set_reject_sendraw(True)
            try:
                zebra_compat.sendrawtransaction(reject_hex)
                raise AssertionError('sendrawtransaction unexpectedly succeeded')
            except JSONRPCException as e:
                assert_equal(e.error['code'], -26)
                assert 'zebra rejected transaction' in e.error['message']
            fake_zebra.set_reject_sendraw(False)
            assert reject_txid not in zebra_compat.getrawmempool()
            assert reject_txid not in source.getrawmempool()

            grace_hex, grace_txid = self.make_signed_tx(source)
            fake_zebra.hide_mempool_txid(grace_txid)
            before_polls = fake_zebra.mempool_poll_count()
            assert_equal(zebra_compat.sendrawtransaction(grace_hex), grace_txid)
            wait_until(lambda: grace_txid in source.getrawmempool(), timeout=60)
            wait_until(lambda: grace_txid in zebra_compat.getrawmempool(), timeout=60)
            self.wait_for_mirror_poll_after(fake_zebra, before_polls, polls=2)
            assert grace_txid in zebra_compat.getrawmempool()
            assert_equal(zebra_compat.getzebracompatinfo()['tx_forwarding']['pending'], 1)

            fake_zebra.unhide_mempool_txid(grace_txid)
            wait_until(lambda: zebra_compat.getzebracompatinfo()['tx_forwarding']['pending'] == 0, timeout=60)
            assert grace_txid in zebra_compat.getrawmempool()

            auto_rebroadcast_txid = zebra_compat.sendtoaddress(source.getnewaddress(), Decimal('0.1'))
            wait_until(lambda: auto_rebroadcast_txid in source.getrawmempool(), timeout=60)
            wait_until(lambda: auto_rebroadcast_txid in zebra_compat.getrawmempool(), timeout=60)
            assert_equal(fake_zebra.sendrawtransaction_count(auto_rebroadcast_txid), 1)

            fake_zebra.hide_mempool_txid(auto_rebroadcast_txid)
            auto_rebroadcast_count = fake_zebra.sendrawtransaction_count(auto_rebroadcast_txid)
            zebra_compat.setmocktime(self.mock_time + 7200)
            before_auto_rebroadcast_polls = fake_zebra.mempool_poll_count()
            self.wait_for_mirror_poll_after(fake_zebra, before_auto_rebroadcast_polls)
            wait_until(lambda: auto_rebroadcast_txid not in zebra_compat.getrawmempool(), timeout=60)

            wait_until(lambda: fake_zebra.sendrawtransaction_count(auto_rebroadcast_txid) > auto_rebroadcast_count, timeout=60)
            fake_zebra.unhide_mempool_txid(auto_rebroadcast_txid)
            wait_until(lambda: auto_rebroadcast_txid in zebra_compat.getrawmempool(), timeout=60)
            zebra_compat.setmocktime(0)

            high_fee_hex, high_fee_txid = self.make_absurd_fee_tx(source)
            for allow_high_fees in [None, False]:
                try:
                    if allow_high_fees is None:
                        zebra_compat.sendrawtransaction(high_fee_hex)
                    else:
                        zebra_compat.sendrawtransaction(high_fee_hex, allow_high_fees)
                    raise AssertionError('high-fee sendrawtransaction unexpectedly succeeded')
                except JSONRPCException as e:
                    assert_equal(e.error['code'], -26)
                    assert_equal(e.error['message'], '256: absurdly-high-fee')
                fake_zebra.assert_tx_not_forwarded(high_fee_txid)
                assert high_fee_txid not in zebra_compat.getrawmempool()
                assert high_fee_txid not in source.getrawmempool()

            fake_zebra.accept_sendraw_without_source_mempool(high_fee_txid)
            assert_equal(zebra_compat.sendrawtransaction(high_fee_hex, True), high_fee_txid)
            fake_zebra.assert_tx_forwarded(high_fee_txid)
            assert high_fee_txid in zebra_compat.getrawmempool()

            stop_node(zebra_compat, 1)
            stop_node(source, 0)
        finally:
            fake_zebra.stop()


if __name__ == '__main__':
    ZebraCompatTransactionForwardingTest().main()
