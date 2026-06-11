#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

import base64
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_node,
    stop_node,
)


REGTEST_GENESIS = '029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327'
WRONG_GENESIS = '0' * 63 + '1'


def wait_until(predicate, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError('wait_until() timed out')


class FakeZebraServer:

    def __init__(self, network='regtest', genesis=REGTEST_GENESIS):
        self.network = network
        self.genesis = genesis
        self.server = None
        self.thread = None

    def start(self):
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                expected_auth = 'Basic ' + base64.b64encode(b'user:pass').decode('ascii')
                if self.headers.get('Authorization') != expected_auth:
                    self.send_response(401)
                    self.end_headers()
                    return

                body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
                request = json.loads(body.decode('utf8'))
                method = request['method']
                if method == 'getblockchaininfo':
                    result = {
                        'chain': fake.network,
                        'blocks': 0,
                        'bestblockhash': fake.genesis,
                    }
                elif method == 'getbestblockhash':
                    result = fake.genesis
                elif method == 'getblockcount':
                    result = 0
                elif method == 'getblockhash':
                    result = fake.genesis
                elif method == 'getblock':
                    result = '00'
                elif method == 'sendrawtransaction':
                    result = '0' * 63 + '2'
                else:
                    self._reply({'result': None, 'error': {'code': -32601, 'message': 'method not found'}, 'id': request.get('id')})
                    return
                self._reply({'result': result, 'error': None, 'id': request.get('id')})

            def _reply(self, response):
                data = json.dumps(response).encode('utf8')
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, format, *args):
                pass

        self.server = HTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.daemon = True
        self.thread.start()
        return 'http://127.0.0.1:%d' % self.server.server_address[1]

    def stop(self):
        if self.server is not None:
            self.server.shutdown()
            self.thread.join()
            self.server.server_close()


class ZebraCompatZebraIdentityTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 3

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def zebra_compat_args(self, endpoint):
        return [
            '-zebra-compat',
            '-zebra-compat-url=%s' % endpoint,
            '-zebra-compat-rpc-user=user',
            '-zebra-compat-rpc-password=pass',
        ]

    def run_test(self):
        good_zebra = FakeZebraServer()
        wrong_network_zebra = FakeZebraServer(network='main')
        wrong_genesis_zebra = FakeZebraServer(genesis=WRONG_GENESIS)
        try:
            node = start_node(0, self.options.tmpdir, self.zebra_compat_args(good_zebra.start()))
            wait_until(lambda: node.getzebracompatinfo()['sync']['state'] == 'synced')
            info = node.getzebracompatinfo()
            assert_equal(info['service_state'], 'ready')
            assert_equal(info['zebra']['reachable'], True)
            assert_equal(info['zebra']['identity_verified'], True)
            assert_equal(info['zebra']['network'], 'regtest')
            assert_equal(info['zebra']['genesis'], REGTEST_GENESIS)
            assert_equal(info['zebra']['bestblockhash'], REGTEST_GENESIS)
            assert_equal(info['zebra']['blocks'], 0)
            assert_equal(info['local']['blocks'], 0)
            assert_equal(info['sync']['state'], 'synced')
            assert_equal(info['sync']['detail'], 'zebra_tip_matched')
            stop_node(node, 0)

            node = start_node(1, self.options.tmpdir, self.zebra_compat_args(wrong_network_zebra.start()))
            wait_until(lambda: node.getzebracompatinfo()['sync']['state'] == 'failed')
            info = node.getzebracompatinfo()
            assert_equal(info['service_state'], 'failed')
            assert_equal(info['zebra']['reachable'], True)
            assert_equal(info['zebra']['identity_verified'], False)
            assert_equal(info['sync']['state'], 'failed')
            assert 'network mismatch' in info['sync']['last_error']
            stop_node(node, 1)

            node = start_node(2, self.options.tmpdir, self.zebra_compat_args(wrong_genesis_zebra.start()))
            wait_until(lambda: node.getzebracompatinfo()['sync']['state'] == 'failed')
            info = node.getzebracompatinfo()
            assert_equal(info['service_state'], 'failed')
            assert_equal(info['zebra']['reachable'], True)
            assert_equal(info['zebra']['identity_verified'], False)
            assert_equal(info['sync']['state'], 'failed')
            assert 'genesis mismatch' in info['sync']['last_error']
            stop_node(node, 2)
        finally:
            good_zebra.stop()
            wrong_network_zebra.stop()
            wrong_genesis_zebra.stop()


if __name__ == '__main__':
    ZebraCompatZebraIdentityTest().main()
