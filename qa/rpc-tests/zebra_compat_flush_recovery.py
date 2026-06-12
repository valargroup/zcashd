#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .
#
# Regression test for the zebra-compat chainstate flush interval: an unclean
# zcashd shutdown (SIGKILL) must not lose ingested chainstate beyond the
# configured flush window. The control leg runs with the flush disabled and
# shows the pre-fix behavior: everything since the last clean shutdown is lost
# and re-fetched from Zebra.

import socket

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    bitcoind_processes,
    start_node,
    stop_node,
)

from zebra_compat_polling_sync import FakePollingZebraServer, wait_until


class ZebraCompatFlushRecoveryTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def zebra_compat_args(self, endpoint, flush_interval):
        return [
            '-zebra-compat',
            '-zebra-compat-url=%s' % endpoint,
            '-zebra-compat-rpc-user=user',
            '-zebra-compat-rpc-password=pass',
            '-zebra-compat-poll-interval=1',
            '-zebra-compat-sync-batch-size=2',
            '-zebra-compat-flush-interval=%d' % flush_interval,
        ]

    def reserve_port(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
        sock.close()
        return port

    def wait_for_synced_tip(self, node, source):
        # Generous timeout: the node may be parked in retry backoff (up to 60s)
        # from a window where the fake Zebra endpoint was down.
        wait_until(lambda: node.getblockcount() == source.getblockcount() and
                   node.getbestblockhash() == source.getbestblockhash(), timeout=120)
        wait_until(lambda: node.getzebracompatinfo()['sync']['state'] == 'synced')

    def kill_node_uncleanly(self, i):
        # SIGKILL models the failure this feature defends against: a crash or a
        # supervisor force-kill that skips zcashd's shutdown flush entirely.
        bitcoind_processes[i].kill()
        bitcoind_processes[i].wait()
        del bitcoind_processes[i]

    def run_test(self):
        source = start_node(0, self.options.tmpdir, [])
        source.generate(60)

        port = self.reserve_port()
        endpoint = 'http://127.0.0.1:%d' % port

        # Leg 1: with a 1-second flush interval, the synced-transition flush
        # persists the full ingested chainstate, so a SIGKILL loses nothing.
        fake_zebra = FakePollingZebraServer(source)
        fake_zebra.start(port)
        try:
            zebra_compat = start_node(
                1, self.options.tmpdir, self.zebra_compat_args(endpoint, flush_interval=1))
            self.wait_for_synced_tip(zebra_compat, source)

            tip_height = source.getblockcount()
            tip_hash = source.getbestblockhash()
            wait_until(lambda: zebra_compat.getzebracompatinfo()
                       ['chainstate_flush']['last_flushed_height'] == tip_height)
            flush_info = zebra_compat.getzebracompatinfo()['chainstate_flush']
            assert_equal(flush_info['interval_seconds'], 1)
            assert_equal(flush_info['last_flushed_hash'], tip_hash)
            assert_equal(flush_info['last_error'], None)

            self.kill_node_uncleanly(1)
        finally:
            fake_zebra.stop()

        # Restart with Zebra unreachable: recovery must come from local disk.
        zebra_compat = start_node(
            1, self.options.tmpdir, self.zebra_compat_args(endpoint, flush_interval=1))
        wait_until(lambda: zebra_compat.getzebracompatinfo()['sync']['detail'] in [
            'zebra_unreachable',
            'zebra_rpc_error',
            'zebra_rpc_error_retry',
        ])
        assert_equal(zebra_compat.getblockcount(), tip_height)
        assert_equal(zebra_compat.getbestblockhash(), tip_hash)
        boundary = zebra_compat.getzebracompatinfo()['trusted_boundary']
        assert_equal(boundary['active'], True)
        assert_equal(boundary['height'], tip_height)
        stop_node(zebra_compat, 1)

        # Leg 2 (control): with the compat flush disabled, the same SIGKILL
        # loses every block ingested since the last clean shutdown, proving the
        # flush interval is what bounds the replay window.
        fake_zebra = FakePollingZebraServer(source)
        fake_zebra.start(port)
        try:
            source.generate(30)
            new_tip_height = source.getblockcount()

            zebra_compat = start_node(
                1, self.options.tmpdir, self.zebra_compat_args(endpoint, flush_interval=0))
            self.wait_for_synced_tip(zebra_compat, source)
            flush_info = zebra_compat.getzebracompatinfo()['chainstate_flush']
            assert_equal(flush_info['interval_seconds'], 0)
            assert_equal(flush_info['last_flush_time'], None)

            self.kill_node_uncleanly(1)
        finally:
            fake_zebra.stop()

        # Restart with Zebra down so the observed height is what local disk
        # recovery produced: only the cleanly-flushed leg-1 chainstate survives.
        zebra_compat = start_node(
            1, self.options.tmpdir, self.zebra_compat_args(endpoint, flush_interval=1))
        wait_until(lambda: zebra_compat.getzebracompatinfo()['sync']['detail'] in [
            'zebra_unreachable',
            'zebra_rpc_error',
            'zebra_rpc_error_retry',
        ])
        assert_equal(zebra_compat.getblockcount(), tip_height)
        assert zebra_compat.getblockcount() < new_tip_height

        # The lost range is re-fetched from Zebra and the node recovers.
        fake_zebra = FakePollingZebraServer(source)
        fake_zebra.start(port)
        try:
            self.wait_for_synced_tip(zebra_compat, source)
            stop_node(zebra_compat, 1)
            stop_node(source, 0)
        finally:
            fake_zebra.stop()


if __name__ == '__main__':
    ZebraCompatFlushRecoveryTest().main()
