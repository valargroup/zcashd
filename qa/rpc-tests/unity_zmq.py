#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

import struct

import zmq

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    bytes_to_hex_str,
    start_node,
    stop_node,
)
from unity_polling_sync import FakePollingZebraServer, wait_until


class UnityZMQTest(BitcoinTestFramework):

    port = 28342

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
            '-zmqpubhashtx=tcp://127.0.0.1:%d' % self.port,
            '-zmqpubhashblock=tcp://127.0.0.1:%d' % self.port,
        ]

    def recv_zmq(self):
        if not self.poller.poll(30000):
            raise AssertionError('timed out waiting for Unity ZMQ notification')
        msg = self.zmq_sub.recv_multipart()
        return msg[0], bytes_to_hex_str(msg[1]), struct.unpack('<I', msg[-1])[-1]

    def run_test(self):
        source = start_node(0, self.options.tmpdir, [])
        fake_zebra = FakePollingZebraServer(source)
        endpoint = fake_zebra.start()

        context = zmq.Context()
        self.zmq_sub = context.socket(zmq.SUB)
        self.zmq_sub.setsockopt(zmq.LINGER, 0)
        self.zmq_sub.setsockopt(zmq.SUBSCRIBE, b'hashblock')
        self.zmq_sub.setsockopt(zmq.SUBSCRIBE, b'hashtx')
        self.zmq_sub.connect('tcp://127.0.0.1:%d' % self.port)
        self.poller = zmq.Poller()
        self.poller.register(self.zmq_sub, zmq.POLLIN)

        try:
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            wait_until(lambda: unity.getunityinfo()['sync']['state'] == 'synced')

            block_hash = source.generate(1)[0]
            coinbase_txid = source.getblock(block_hash)['tx'][0]
            wait_until(lambda: unity.getbestblockhash() == block_hash)

            seen_block = False
            seen_tx = False
            for _ in range(2):
                topic, body, sequence = self.recv_zmq()
                assert sequence >= 0
                if topic == b'hashblock':
                    assert_equal(body, block_hash)
                    seen_block = True
                elif topic == b'hashtx':
                    assert_equal(body, coinbase_txid)
                    seen_tx = True

            assert seen_block
            assert seen_tx

            stop_node(unity, 1)
            stop_node(source, 0)
        finally:
            self.zmq_sub.close()
            context.term()
            fake_zebra.stop()


if __name__ == '__main__':
    UnityZMQTest().main()
