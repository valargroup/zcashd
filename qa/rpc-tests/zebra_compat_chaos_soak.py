#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

import os
import socket
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_node,
    stop_node,
)

from zebra_compat_polling_sync import FakePollingZebraServer, wait_until


class ZebraCompatChaosSoakTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'current'
        self.num_nodes = 3

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def reserve_port(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
        sock.close()
        return port

    def zebra_compat_args(self, endpoint, extra_args=None, cookiefile=None):
        args = [
            '-allowdeprecated=getnewaddress',
            '-zebra-compat',
            '-zebra-compat-url=%s' % endpoint,
            '-zebra-compat-poll-interval=1',
            '-zebra-compat-sync-batch-size=1',
        ]
        if cookiefile is None:
            args.extend([
                '-zebra-compat-rpc-user=user',
                '-zebra-compat-rpc-password=pass',
            ])
        else:
            args.append('-zebra-compat-cookiefile=%s' % cookiefile)
        if extra_args:
            args.extend(extra_args)
        return args

    def wait_for_zebra_compat_tip(self, zebra_compat, source, timeout=90):
        wait_until(lambda: zebra_compat.getblockcount() == source.getblockcount() and
                   zebra_compat.getbestblockhash() == source.getbestblockhash(), timeout=timeout)
        info = zebra_compat.getzebracompatinfo()
        assert_equal(info['sync']['state'], 'synced')
        assert_equal(info['sync']['detail'], 'zebra_tip_matched')
        assert_equal(info['readiness'], 'ready')

    def write_cookie(self, path, user, password):
        with open(path, 'w', encoding='utf8') as cookie:
            cookie.write('%s:%s\n' % (user, password))

    def run_test(self):
        source = start_node(0, self.options.tmpdir, ['-allowdeprecated=getnewaddress'])

        fake_zebra = FakePollingZebraServer(source)
        port = self.reserve_port()
        endpoint = fake_zebra.start(port)
        zebra_compat = None
        try:
            cookie_path = os.path.join(self.options.tmpdir, 'zebra-compat.cookie')
            self.write_cookie(cookie_path, 'user', 'pass')

            zebra_compat = start_node(1, self.options.tmpdir, self.zebra_compat_args(endpoint, cookiefile=cookie_path))
            self.wait_for_zebra_compat_tip(zebra_compat, source)

            # Zebra restart while zcashd remains live and polling.
            fake_zebra.stop()
            time.sleep(1)
            endpoint = fake_zebra.start(port)
            self.wait_for_zebra_compat_tip(zebra_compat, source)

            # Cookie rotation with externally managed Zebra credentials.
            fake_zebra.set_rpc_credentials('user', 'rotated-pass')
            self.write_cookie(cookie_path, 'user', 'rotated-pass')
            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)

            # Readiness stays ready across ordinary block progress.
            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)
            assert_equal(zebra_compat.getzebracompatinfo()['readiness'], 'ready')

            # Simulated Zebra state loss/unavailability followed by the same source returning.
            fake_zebra.stop()
            time.sleep(1)
            endpoint = fake_zebra.start(port)
            source.generate(1)
            self.wait_for_zebra_compat_tip(zebra_compat, source)

            # SIGTERM during active batch ingest exits cleanly and can resume.
            stop_node(zebra_compat, 1)
            source.generate(2)
            before_batches = fake_zebra.block_fetch_batch_count()
            zebra_compat = start_node(1, self.options.tmpdir, self.zebra_compat_args(endpoint, cookiefile=cookie_path))
            wait_until(lambda: fake_zebra.block_fetch_batch_count() > before_batches, timeout=60)
            stop_node(zebra_compat, 1)
            zebra_compat = start_node(1, self.options.tmpdir, self.zebra_compat_args(endpoint, cookiefile=cookie_path))
            self.wait_for_zebra_compat_tip(zebra_compat, source)

            stop_node(zebra_compat, 1)
            zebra_compat = None
            stop_node(source, 0)
        finally:
            if zebra_compat is not None:
                stop_node(zebra_compat, 1)
            fake_zebra.stop()


if __name__ == '__main__':
    ZebraCompatChaosSoakTest().main()
