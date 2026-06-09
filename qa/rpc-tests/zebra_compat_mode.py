#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

import socket

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_start_raises_init_error,
    assert_raises_message,
    p2p_port,
    start_nodes,
)


class UnityModeTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2

    def setup_network(self, split=False):
        self.nodes = start_nodes(1, self.options.tmpdir, [['-zebra-compat']])
        self.is_network_split = False

    def run_test(self):
        node = self.nodes[0]

        info = node.getzebracompatinfo()
        assert_equal(info['enabled'], True)
        assert_equal(info['service_state'], 'waiting')
        assert_equal(info['readiness'], 'degraded')
        assert_equal(info['blocksource'], 'zebra')
        assert_equal(info['p2p'], False)
        assert_equal(info['blockvalidation'], 'trusted-zebra')
        assert_equal(info['zebra']['configured'], False)
        assert_equal(info['sync']['state'], 'degraded')
        assert_equal(info['sync']['detail'], 'waiting_for_zebra_endpoint')
        assert_equal(info['sync']['retry_count'], 0)
        assert_equal(info['sync']['current_backoff_seconds'], 0)
        assert_equal(info['sync']['next_retry'], None)
        assert_equal(info['metrics']['mempool_lag'], 0)
        assert_equal(info['metrics']['mempool_ready'], False)
        assert_equal(info['metrics']['tx_forwarding_pending'], 0)
        assert_equal(info['metrics']['tx_forwarding_transport_ready'], True)
        assert_equal(info['limits']['max_retry_backoff_seconds'], 60)
        assert info['limits']['sync_batch_size'] >= 1
        assert info['limits']['zebra_rpc_max_response_body_bytes'] > 0

        assert_equal(node.getblockcount(), 0)
        assert_equal(node.getpeerinfo(), [])
        assert_equal(node.getconnectioncount(), 0)
        assert_equal(node.getnetworkinfo()['connections'], 0)

        assert_raises_message(JSONRPCException, 'unavailable when Zcash P2P is disabled', node.addnode, '127.0.0.1', 'onetry')
        assert_raises_message(JSONRPCException, 'unavailable when Zcash P2P is disabled', node.disconnectnode, '127.0.0.1')
        assert_raises_message(JSONRPCException, 'unavailable when Zcash P2P is disabled', node.getaddednodeinfo, False)
        assert_raises_message(JSONRPCException, 'unavailable when Zcash P2P is disabled', node.setban, '127.0.0.0', 'add')
        assert_raises_message(JSONRPCException, 'unavailable when Zcash P2P is disabled', node.listbanned)
        assert_raises_message(JSONRPCException, 'unavailable when Zcash P2P is disabled', node.clearbanned)
        assert_raises_message(JSONRPCException, 'unavailable in unity mode', node.getblocktemplate)
        assert_raises_message(JSONRPCException, 'unavailable in unity mode', node.submitblock, '00')
        assert_raises_message(JSONRPCException, 'unavailable in unity mode', node.generate, 1)
        assert_raises_message(JSONRPCException, 'unavailable in unity mode', node.setgenerate, True)

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(1)
            try:
                s.connect(('127.0.0.1', p2p_port(0)))
            except OSError:
                pass
            else:
                raise AssertionError('Unity mode started a Zcash P2P listener')

        assert_start_raises_init_error(
            1,
            self.options.tmpdir,
            ['-blocksource=zebra', '-p2p=0', '-blockvalidation=full', '-listen=1'],
            '-p2p=0 is incompatible with -listen=1',
        )


if __name__ == '__main__':
    UnityModeTest().main()
