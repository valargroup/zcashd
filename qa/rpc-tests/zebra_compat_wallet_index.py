#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

from decimal import Decimal

from test_framework.mininode import COIN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    get_coinbase_address,
    start_node,
    stop_node,
    wait_and_assert_operationid_status,
    wait_bitcoinds,
)
from test_framework.zip317 import conventional_fee
from zebra_compat_polling_sync import FakePollingZebraServer, wait_until


class UnityWalletIndexTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.cache_behavior = 'clean'
        self.num_nodes = 2

    def setup_network(self, split=False):
        self.nodes = []
        self.is_network_split = False

    def unity_args(self, endpoint=None):
        args = [
            '-zebra-compat',
            '-allowdeprecated=getnewaddress',
            '-allowdeprecated=z_getnewaddress',
            '-allowdeprecated=z_getbalance',
            '-experimentalfeatures',
            '-lightwalletd',
            '-txindex',
            '-zebra-compat-poll-interval=1',
            '-zebra-compat-sync-batch-size=25',
        ]
        if endpoint is not None:
            args.extend([
                '-zebra-compat-url=%s' % endpoint,
                '-zebra-compat-rpc-user=user',
                '-zebra-compat-rpc-password=pass',
            ])
        return args

    def wait_for_unity_tip(self, node, source):
        wait_until(lambda: node.getblockcount() == source.getblockcount() and
                   node.getbestblockhash() == source.getbestblockhash(), timeout=60)
        info = node.getzebracompatinfo()
        assert_equal(info['sync']['state'], 'synced')
        assert_equal(info['local']['bestblockhash'], source.getbestblockhash())

    def wait_for_wallet_credit(self, node):
        wait_until(lambda: node.getbalance() > Decimal('0'), timeout=60)
        assert_greater_than(len(node.listtransactions('*', 200)), 0)

    def wait_for_shielded_credit(self, node, sapling_addr, amount):
        wait_until(lambda: Decimal(node.z_getbalance(sapling_addr)) == amount, timeout=60)
        notes = node.z_listunspent(1, 999999, False, [sapling_addr])
        assert_equal(len(notes), 1)
        assert_equal(notes[0]['address'], sapling_addr)
        assert_equal(notes[0]['pool'], 'sapling')
        assert_equal(Decimal(notes[0]['amount']), amount)

    def find_vout_for_address(self, tx, address):
        for vout in tx['vout']:
            if address in vout['scriptPubKey'].get('addresses', []):
                return vout['n']
        raise AssertionError('transaction does not pay expected transparent address')

    def assert_local_compatibility_state(
            self, unity, miner_addr, sapling_addr, mined_hashes,
            transparent_txid, transparent_amount, sapling_txid, sapling_amount):
        assert_equal(unity.getblockchaininfo()['blocks'], len(mined_hashes))
        assert_equal(unity.getbestblockhash(), mined_hashes[-1])
        assert_equal(unity.getblockcount(), len(mined_hashes))
        assert_equal(unity.getblockhash(1), mined_hashes[0])

        first_block = unity.getblock(mined_hashes[0])
        assert_equal(first_block['hash'], mined_hashes[0])
        assert_equal(first_block['height'], 1)

        raw_tx = unity.getrawtransaction(transparent_txid, 1)
        assert_equal(raw_tx['txid'], transparent_txid)
        assert_equal(raw_tx['confirmations'], 1)
        transparent_vout = self.find_vout_for_address(raw_tx, miner_addr)

        txout = unity.gettxout(transparent_txid, transparent_vout)
        assert txout is not None
        assert_equal(txout['bestblock'], mined_hashes[-1])
        assert_equal(Decimal(txout['value']), transparent_amount)

        txoutset = unity.gettxoutsetinfo()
        assert_equal(txoutset['height'], len(mined_hashes))
        assert_equal(txoutset['bestblock'], mined_hashes[-1])

        addr_txids = unity.getaddresstxids(miner_addr)
        assert_equal(addr_txids, [transparent_txid])

        balance = unity.getaddressbalance(miner_addr)
        assert_equal(balance['received'], int(transparent_amount * COIN))
        assert_equal(balance['balance'], int(transparent_amount * COIN))

        utxos = unity.getaddressutxos(miner_addr)
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]['txid'], transparent_txid)
        assert_equal(utxos[0]['outputIndex'], transparent_vout)

        deltas = unity.getaddressdeltas({
            'addresses': [miner_addr],
            'start': 1,
            'end': len(mined_hashes),
        })
        assert_equal(len(deltas), 1)
        assert_equal(deltas[0]['txid'], transparent_txid)
        assert_equal(deltas[0]['satoshis'], int(transparent_amount * COIN))

        shielded_tx = unity.getrawtransaction(sapling_txid, 1)
        assert_equal(shielded_tx['txid'], sapling_txid)
        assert_greater_than(len(shielded_tx['vShieldedOutput']), 0)

        tip_block = unity.getblock(mined_hashes[-1])
        assert_greater_than(tip_block['trees']['sapling']['size'], 0)

        treestate = unity.z_gettreestate(str(len(mined_hashes)))
        assert_equal(treestate['height'], len(mined_hashes))
        assert_equal(treestate['hash'], mined_hashes[-1])
        assert_equal(
            treestate['sapling']['commitments']['finalRoot'],
            tip_block['finalsaplingroot'])

        self.wait_for_shielded_credit(unity, sapling_addr, sapling_amount)

        sapling_subtrees = unity.z_getsubtreesbyindex('sapling', 0)
        assert_equal(sapling_subtrees['pool'], 'sapling')
        assert_equal(sapling_subtrees['start_index'], 0)
        assert_equal(len(sapling_subtrees['subtrees']), 0)

        orchard_subtrees = unity.z_getsubtreesbyindex('orchard', 0)
        assert_equal(orchard_subtrees['pool'], 'orchard')
        assert_equal(orchard_subtrees['start_index'], 0)
        assert_equal(len(orchard_subtrees['subtrees']), 0)

    def run_test(self):
        unity = start_node(1, self.options.tmpdir, self.unity_args())
        wait_until(lambda: unity.getzebracompatinfo()['sync']['detail'] == 'waiting_for_zebra_endpoint')
        miner_addr = unity.getnewaddress()
        sapling_addr = unity.z_getnewaddress('sapling')
        stop_node(unity, 1)
        wait_bitcoinds()

        source = start_node(0, self.options.tmpdir, [
            '-allowdeprecated=getnewaddress',
            '-allowdeprecated=z_getnewaddress',
            '-allowdeprecated=z_getbalance',
            '-txindex',
        ])
        mined_hashes = source.generate(105)

        transparent_amount = Decimal('3.21')
        transparent_txid = source.sendtoaddress(miner_addr, transparent_amount)

        sapling_amount = Decimal('1.23')
        fee = conventional_fee(3)
        opid = source.z_sendmany(
            get_coinbase_address(source),
            [{'address': sapling_addr, 'amount': sapling_amount}],
            1,
            fee,
            'AllowRevealedSenders')
        sapling_txid = wait_and_assert_operationid_status(source, opid, timeout=1200)
        mined_hashes.extend(source.generate(1))

        fake_zebra = FakePollingZebraServer(source)
        endpoint = fake_zebra.start()
        try:
            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint))
            self.wait_for_unity_tip(unity, source)
            self.wait_for_wallet_credit(unity)
            self.assert_local_compatibility_state(
                unity, miner_addr, sapling_addr, mined_hashes,
                transparent_txid, transparent_amount, sapling_txid, sapling_amount)

            stop_node(unity, 1)
            wait_bitcoinds()

            unity = start_node(1, self.options.tmpdir, self.unity_args(endpoint) + ['-rescan'])
            self.wait_for_unity_tip(unity, source)
            self.wait_for_wallet_credit(unity)
            self.assert_local_compatibility_state(
                unity, miner_addr, sapling_addr, mined_hashes,
                transparent_txid, transparent_amount, sapling_txid, sapling_amount)

            stop_node(unity, 1)
            stop_node(source, 0)
        finally:
            fake_zebra.stop()


if __name__ == '__main__':
    UnityWalletIndexTest().main()
