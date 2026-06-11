#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

import base64
import json
import os
import urllib.request

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_node,
    stop_node,
)


REGTEST_GENESIS = '029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327'


class ZebraCompatZebraIdentityLiveTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 1

    def add_options(self, parser):
        parser.add_option('--zebra-rpc-url', dest='zebra_rpc_url',
                          default=os.getenv('ZEBRA_COMPAT_ZEBRA_RPC_URL', ''),
                          help='Real regtest Zebra JSON-RPC URL for the live zebra-compat identity gate')
        parser.add_option('--zebra-rpc-user', dest='zebra_rpc_user',
                          default=os.getenv('ZEBRA_COMPAT_ZEBRA_RPC_USER', ''),
                          help='Zebra JSON-RPC username')
        parser.add_option('--zebra-rpc-password', dest='zebra_rpc_password',
                          default=os.getenv('ZEBRA_COMPAT_ZEBRA_RPC_PASSWORD', ''),
                          help='Zebra JSON-RPC password')
        parser.add_option('--zebra-rpc-cookiefile', dest='zebra_rpc_cookiefile',
                          default=os.getenv('ZEBRA_COMPAT_ZEBRA_RPC_COOKIEFILE', ''),
                          help='Zebra JSON-RPC cookie file')

    def setup_chain(self):
        if not self.options.zebra_rpc_url:
            print('Skipping live Zebra identity gate: --zebra-rpc-url not provided')
            return
        super().setup_chain()

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def zebra_auth(self):
        if self.options.zebra_rpc_cookiefile:
            with open(self.options.zebra_rpc_cookiefile, 'r', encoding='utf8') as cookie:
                return cookie.readline().strip()
        if not self.options.zebra_rpc_user or not self.options.zebra_rpc_password:
            raise AssertionError('live Zebra gate requires RPC credentials or a cookie file')
        return '%s:%s' % (self.options.zebra_rpc_user, self.options.zebra_rpc_password)

    def zebra_rpc(self, method, params=None):
        payload = json.dumps({
            'jsonrpc': '1.0',
            'id': 'zebra-compat-live-test',
            'method': method,
            'params': [] if params is None else params,
        }).encode('utf8')
        request = urllib.request.Request(
            self.options.zebra_rpc_url,
            data=payload,
            headers={
                'Authorization': 'Basic ' + base64.b64encode(self.zebra_auth().encode('utf8')).decode('ascii'),
                'Content-Type': 'application/json',
            },
            method='POST')
        with urllib.request.urlopen(request, timeout=30) as response:
            body = json.loads(response.read().decode('utf8'))
        if body.get('error') is not None:
            raise AssertionError('Zebra RPC %s returned error: %r' % (method, body['error']))
        return body['result']

    def zebra_compat_args(self):
        args = [
            '-zebra-compat',
            '-zebra-compat-url=%s' % self.options.zebra_rpc_url,
        ]
        if self.options.zebra_rpc_cookiefile:
            args.append('-zebra-compat-cookiefile=%s' % self.options.zebra_rpc_cookiefile)
        else:
            args.extend([
                '-zebra-compat-rpc-user=%s' % self.options.zebra_rpc_user,
                '-zebra-compat-rpc-password=%s' % self.options.zebra_rpc_password,
            ])
        return args

    def run_test(self):
        if not self.options.zebra_rpc_url:
            return

        zebra_chain = self.zebra_rpc('getblockchaininfo')
        zebra_best_hash = self.zebra_rpc('getbestblockhash')
        zebra_count = self.zebra_rpc('getblockcount')
        zebra_genesis = self.zebra_rpc('getblockhash', [0])

        assert_equal(zebra_chain['chain'], 'regtest')
        assert_equal(zebra_genesis, REGTEST_GENESIS)

        node = start_node(0, self.options.tmpdir, self.zebra_compat_args())
        try:
            info = node.getzebracompatinfo()
            assert_equal(info['zebra']['reachable'], True)
            assert_equal(info['zebra']['identity_verified'], True)
            assert_equal(info['zebra']['network'], zebra_chain['chain'])
            assert_equal(info['zebra']['genesis'], zebra_genesis)
            assert_equal(info['zebra']['bestblockhash'], zebra_best_hash)
            assert_equal(info['zebra']['blocks'], zebra_count)
        finally:
            stop_node(node, 0)


if __name__ == '__main__':
    ZebraCompatZebraIdentityLiveTest().main()
