#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

import base64
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

from test_framework.authproxy import AuthServiceProxy
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_node,
    stop_node,
)


def wait_until(predicate, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError('wait_until() timed out')


class FakePollingZebraServer:

    def __init__(self, source_node):
        self.source_url = source_node.url
        self.server = None
        self.thread = None
        self.lock = threading.Lock()
        self.batch_calls = []
        self.fail_getblock_batch = False
        self.reject_sendraw = False
        self.hidden_mempool_txids = set()
        self.getrawmempool_calls = 0
        self.reorg_tip_on_next_getblockhash_batch = False

    def start(self, port=0):
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
                if isinstance(request, list):
                    with fake.lock:
                        fake.batch_calls.append([entry['method'] for entry in request])
                        fail_getblock_batch = fake.fail_getblock_batch and any(
                            entry['method'] == 'getblock' for entry in request)
                        reorg_tip = fake.reorg_tip_on_next_getblockhash_batch and any(
                            entry['method'] == 'getblockhash' for entry in request)
                        if reorg_tip:
                            fake.reorg_tip_on_next_getblockhash_batch = False
                    if fail_getblock_batch:
                        self.send_response(500)
                        self.end_headers()
                        return
                    source = fake.source_rpc()
                    if reorg_tip:
                        source.invalidateblock(source.getbestblockhash())
                        source.generate(1)
                    self._reply([self._handle_one(entry, source) for entry in request])
                else:
                    source = fake.source_rpc()
                    self._reply(self._handle_one(request, source))

            def _handle_one(self, request, source):
                method = request['method']
                params = request.get('params', [])
                if method == 'getblockchaininfo':
                    result = {
                        'chain': 'regtest',
                        'blocks': source.getblockcount(),
                        'bestblockhash': source.getbestblockhash(),
                    }
                elif method == 'getbestblockhash':
                    result = source.getbestblockhash()
                elif method == 'getblockcount':
                    result = source.getblockcount()
                elif method == 'getblockhash':
                    result = source.getblockhash(params[0])
                elif method == 'getblock':
                    result = source.getblock(params[0], 0)
                elif method == 'getrawmempool':
                    result = source.getrawmempool()
                    with fake.lock:
                        fake.getrawmempool_calls += 1
                        hidden = set(fake.hidden_mempool_txids)
                    result = [txid for txid in result if txid not in hidden]
                elif method == 'getmempoolinfo':
                    result = source.getmempoolinfo()
                    with fake.lock:
                        hidden = set(fake.hidden_mempool_txids)
                    if hidden:
                        visible = [txid for txid in source.getrawmempool() if txid not in hidden]
                        result['size'] = len(visible)
                elif method == 'getrawtransaction':
                    result = source.getrawtransaction(params[0], 0)
                elif method == 'sendrawtransaction':
                    with fake.lock:
                        reject_sendraw = fake.reject_sendraw
                    if reject_sendraw:
                        return {
                            'result': None,
                            'error': {'code': -26, 'message': 'zebra rejected transaction'},
                            'id': request.get('id'),
                        }
                    result = source.sendrawtransaction(params[0])
                else:
                    return {
                        'result': None,
                        'error': {'code': -32601, 'message': 'method not found'},
                        'id': request.get('id'),
                    }
                return {'result': result, 'error': None, 'id': request.get('id')}

            def _reply(self, response):
                data = json.dumps(response).encode('utf8')
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, format, *args):
                pass

        self.server = HTTPServer(('127.0.0.1', port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.daemon = True
        self.thread.start()
        return 'http://127.0.0.1:%d' % self.server.server_address[1]

    def source_rpc(self):
        return AuthServiceProxy(self.source_url)

    def stop(self):
        if self.server is not None:
            self.server.shutdown()
            self.thread.join()
            self.server.server_close()

    def saw_batched_block_fetch(self):
        with self.lock:
            return any(len(methods) > 1 and methods[0] == 'getblock' for methods in self.batch_calls)

    def set_fail_getblock_batch(self, value):
        with self.lock:
            self.fail_getblock_batch = value

    def set_reject_sendraw(self, value):
        with self.lock:
            self.reject_sendraw = value

    def hide_mempool_txid(self, txid):
        with self.lock:
            self.hidden_mempool_txids.add(txid)

    def unhide_mempool_txid(self, txid):
        with self.lock:
            self.hidden_mempool_txids.discard(txid)

    def mempool_poll_count(self):
        with self.lock:
            return self.getrawmempool_calls

    def set_reorg_tip_on_next_getblockhash_batch(self):
        with self.lock:
            self.reorg_tip_on_next_getblockhash_batch = True


class UnityPollingSyncTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def unity_args(self, endpoint, password='pass'):
        return [
            '-zebra-compat',
            '-zebra-compat-url=%s' % endpoint,
            '-zebra-compat-rpc-user=user',
            '-zebra-compat-rpc-password=%s' % password,
            '-zebra-compat-poll-interval=1',
            '-zebra-compat-sync-batch-size=2',
        ]

    def wait_for_unity_tip(self, node, source):
        wait_until(lambda: node.getblockcount() == source.getblockcount() and
                   node.getbestblockhash() == source.getbestblockhash())
        wait_until(lambda: node.getzebracompatinfo()['readiness'] == 'ready')
        info = node.getzebracompatinfo()
        assert_equal(info['sync']['state'], 'synced')
        assert_equal(info['sync']['detail'], 'zebra_tip_matched')
        assert_equal(info['readiness'], 'ready')
        assert_equal(info['sync']['retry_count'], 0)
        assert_equal(info['sync']['current_backoff_seconds'], 0)
        assert_equal(info['metrics']['sync_lag'], 0)
        assert_equal(info['metrics']['mempool_ready'], True)
        assert_equal(info['metrics']['tx_forwarding_transport_ready'], True)
        assert_equal(info['metrics']['validation_notifications_caught_up'], True)
        assert_equal(info['zebra']['bestblockhash'], source.getbestblockhash())
        assert_equal(info['local']['bestblockhash'], source.getbestblockhash())

    def reserve_port(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
        sock.close()
        return port

    def run_test(self):
        source = start_node(0, self.options.tmpdir, [])
        source.generate(5)

        fake_zebra = FakePollingZebraServer(source)
        port = self.reserve_port()
        endpoint = 'http://127.0.0.1:%d' % port
        unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
        wait_until(lambda: unity.getzebracompatinfo()['sync']['detail'] in [
            'zebra_unreachable',
            'zebra_rpc_error',
        ] and unity.getzebracompatinfo()['sync']['retry_count'] >= 1)
        unreachable_info = unity.getzebracompatinfo()
        assert_equal(unreachable_info['readiness'], 'degraded')
        assert unreachable_info['sync']['current_backoff_seconds'] >= 1
        assert unreachable_info['sync']['next_retry'] is not None
        assert_equal(unity.getblockcount(), 0)

        fake_zebra.start(port)
        try:
            self.wait_for_unity_tip(unity, source)
            stop_node(unity, 1)

            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint, password='wrong'))
            wait_until(lambda: unity.getzebracompatinfo()['sync']['detail'] == 'zebra_identity_error')
            info = unity.getzebracompatinfo()
            assert_equal(info['readiness'], 'failed')
            assert_equal(info['sync']['state'], 'failed')
            assert 'authentication failed' in info['sync']['last_error']
            assert_equal(unity.getblockcount(), source.getblockcount())
            stop_node(unity, 1)

            fake_zebra.set_fail_getblock_batch(True)
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            source.generate(2)
            wait_until(lambda: unity.getzebracompatinfo()['sync']['detail'] == 'zebra_rpc_error')
            rpc_error_info = unity.getzebracompatinfo()
            assert_equal(rpc_error_info['readiness'], 'degraded')
            assert rpc_error_info['sync']['retry_count'] >= 1
            assert rpc_error_info['sync']['current_backoff_seconds'] >= 1
            assert_equal(unity.getblockcount(), 5)
            stop_node(unity, 1)

            fake_zebra.set_fail_getblock_batch(False)
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            self.wait_for_unity_tip(unity, source)
            assert fake_zebra.saw_batched_block_fetch()

            block_hash = source.getblockhash(3)
            assert_equal(unity.getblock(block_hash)['hash'], block_hash)

            source.generate(2)
            self.wait_for_unity_tip(unity, source)

            source.generate(1)
            fake_zebra.set_reorg_tip_on_next_getblockhash_batch()
            wait_until(lambda: unity.getzebracompatinfo()['sync']['detail'] == 'zebra_tip_changed_during_sync')
            self.wait_for_unity_tip(unity, source)

            fork_height = source.getblockcount() - 2
            fork_hash = source.getblockhash(fork_height)
            old_branch_child = source.getblockhash(fork_height + 1)
            source.invalidateblock(old_branch_child)
            source.generate(2)
            self.wait_for_unity_tip(unity, source)
            assert_equal(unity.getblockhash(fork_height), fork_hash)
            assert_equal(unity.getblockhash(fork_height + 1), source.getblockhash(fork_height + 1))
            reorg_info = unity.getzebracompatinfo()
            assert_equal(reorg_info['sync']['last_common_ancestor_height'], fork_height)
            assert_equal(reorg_info['sync']['last_common_ancestor_hash'], fork_hash)

            stop_node(unity, 1)
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            self.wait_for_unity_tip(unity, source)

            stop_node(unity, 1)
            source.generate(2)
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            self.wait_for_unity_tip(unity, source)

            local_tip_before_large_branch = unity.getbestblockhash()
            large_branch_fork_height = source.getblockcount() - 1
            large_branch_child = source.getblockhash(large_branch_fork_height + 1)
            source.invalidateblock(large_branch_child)
            source.generate(3)
            wait_until(lambda: unity.getzebracompatinfo()['sync']['detail'] == 'reorg_branch_too_large')
            large_branch_info = unity.getzebracompatinfo()
            assert_equal(large_branch_info['readiness'], 'failed')
            assert_equal(large_branch_info['sync']['state'], 'failed')
            assert_equal(unity.getbestblockhash(), local_tip_before_large_branch)
            stop_node(unity, 1)
            stop_node(source, 0)
        finally:
            fake_zebra.stop()


if __name__ == '__main__':
    UnityPollingSyncTest().main()
