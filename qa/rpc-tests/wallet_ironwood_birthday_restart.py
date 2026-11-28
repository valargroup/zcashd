#!/usr/bin/env python3
"""Restart a new wallet that caught up before saving its position, with a real Zakura peer.

A new wallet saves its position (best block and note commitment trees) at the tip
it was created on. Later blocks reach disk only every ten minutes, so a quick
catch-up followed by a restart leaves the saved position behind. On that restart
the wallet rebuilds its Ironwood tree at the saved block, while its Orchard tree
has never been saved. The rescan then skips the blocks mined before the wallet's
keys were created. That skip must not run past the rebuilt Ironwood tree, or the
tree cannot be moved back to where the rescan starts and the node fails to start
on every attempt until it is run once with -rescan.

The wallet under test does not pass -regtestwalletsetbestchaineveryblock, which
would save its position on every block and hide the problem. Zakura regtest blocks
are timestamped in 2011, so every block is older than the new wallet's keys.
"""
import argparse
from contextlib import ExitStack
from decimal import Decimal
from pathlib import Path
import re
import tempfile

from wallet_ironwood_payouts import (
    LOCKBOX,
    NU63_HEIGHT,
    UPGRADES,
    free_ports,
    node_process,
    ready,
    rpc,
    wait_for,
)

COIN = 100_000_000
# NU7 stays well above every height this test reaches.
NU7_HEIGHT = 1000
# The saved position of the new wallet, past NU6.3 activation and past coinbase maturity.
SAVED_HEIGHT = NU63_HEIGHT + 2
# Blocks the new wallet follows live before its restart.
UNSAVED_BLOCKS = 5


def exercise(args, root):
    peer, zakura_port, funder_port, wallet_port = free_ports(4)
    activation = dict.fromkeys((name for name, _ in UPGRADES), 1)
    activation.update({"NU6.3": NU63_HEIGHT, "NU7": NU7_HEIGHT})
    flags = [f"-nuparams={branch}:{activation[name]}" for name, branch in UPGRADES]
    flags += [f"-onetimelockboxdisbursement=0:4dec4df0:0:{LOCKBOX}", "-regtestnsminitialbalance=1000000000",
              "-regtest", f"-connect=127.0.0.1:{peer}", "-listen=0", "-walletrequirebackup=0",
              "-allowdeprecated=getnewaddress", "-maxtipage=3153600000", "-regtestshieldcoinbase",
              "-regtestacceptunvalidatedpow", "-txindex=1"]

    def sidecar(name, port):
        datadir = root / name
        datadir.mkdir()
        (datadir / "zcash.conf").touch()
        cookie = datadir / "regtest" / ".cookie"

        def call(method, *params):
            return rpc(port, method, *params, auth=cookie)

        def start(stack, *extra):
            log = root / f"{name}.log"
            proc = stack.enter_context(node_process(
                [args.zcashd, f"-datadir={datadir}", f"-rpcport={port}", *flags, *extra], log))
            ready(proc, call, log)
            return proc

        return call, start, datadir / "regtest" / "debug.log"

    funder, start_funder, _ = sidecar("funder", funder_port)
    wallet, start_wallet, wallet_log = sidecar("wallet", wallet_port)

    def zakura(method, *params):
        return rpc(zakura_port, method, *params)

    def follow(call, block):
        wait_for(lambda: call("getbestblockhash") == block and call("getblockchaininfo")["fullyNotified"],
                 "sidecar to follow Zakura")

    def ironwood_balance(call, account):
        return call("z_getbalanceforaccount", account)["pools"].get("ironwood", {}).get("valueZat", 0)

    def submit(call, opid):
        # Wait for a wallet operation and for its transaction to reach Zakura's mempool.
        outcome = wait_for(lambda: call("z_getoperationresult", [opid]), f"operation {opid}", 360)[0]
        assert outcome["status"] == "success", outcome
        txid = outcome["result"]["txid"]
        wait_for(lambda: txid in zakura("getrawmempool"), "transaction to reach Zakura")
        return txid

    def mine(*calls):
        tip = zakura("generate", 1)[0]
        for call in calls:
            follow(call, tip)
        return tip

    # The funding wallet's keys must exist before Zakura starts, to receive its rewards.
    with ExitStack() as offline:
        start_funder(offline, "-regtestwalletsetbestchaineveryblock")
        mining = funder("getnewaddress")
        payee = funder("getnewaddress")
        funder_account = funder("z_getnewaccount")["account"]
        funder_address = funder("z_getaddressforaccount", funder_account, ["orchard"])["address"]

    config = root / "zakurad.toml"
    heights = "\n".join(f'"{name}" = {height}' for name, height in activation.items())
    config.write_text(f'''[network]
network = "Regtest"
listen_addr = "127.0.0.1:{peer}"
p2p_stack = "legacy"
initial_testnet_peers = []
max_connections_per_ip = 10
cache_dir = false
[network.testnet_parameters]
initial_nsm_value_balance = 1000000000
lockbox_disbursements = [{{ address = "{LOCKBOX}", amount = 0 }}]
[network.testnet_parameters.activation_heights]
{heights}
[state]
ephemeral = true
[rpc]
listen_addr = "127.0.0.1:{zakura_port}"
enable_cookie_auth = false
[mining]
miner_address = "{mining}"
[mempool]
debug_enable_at_height = 0
''')

    with ExitStack() as nodes:
        zlog = root / "zakura.log"
        ready(nodes.enter_context(node_process([args.zakurad, "-c", config, "start"], zlog)), zakura, zlog)
        start_funder(nodes, "-regtestwalletsetbestchaineveryblock")
        tip = zakura("generate", SAVED_HEIGHT)[-1]
        follow(funder, tip)

        # Bring the wallet node's chain to the tip before its wallet exists, like a node
        # restored from a snapshot. Its wallet is then created at that tip.
        with ExitStack() as sync:
            start_wallet(sync, "-wallet=sync.dat")
            follow(wallet, tip)
        live = nodes.enter_context(ExitStack())
        start_wallet(live)
        assert wallet("getblockcount") == SAVED_HEIGHT
        account = wallet("z_getnewaccount")["account"]
        address = wallet("z_getaddressforaccount", account, ["orchard"])["address"]
        transparent = wallet("getnewaddress")

        # While the new wallet follows the chain, it receives transparent funds and
        # shields them to its own Unified Address, the only way this build lets a
        # wallet receive Ironwood funds besides shielding its own rewards.
        shield = funder("z_shieldcoinbase", mining, funder_address, None, 1)
        submit(funder, shield["opid"])
        mine(funder, wallet)
        half = Decimal(ironwood_balance(funder, funder_account) // 2) / COIN
        submit(funder, funder("z_sendmany", funder_address, [{"address": transparent, "amount": float(half)}], 1, None,
                              "AllowRevealedRecipients"))
        mine(funder, wallet)
        fee = Decimal("0.0001")
        txid = submit(wallet, wallet("z_sendmany", transparent, [{"address": address, "amount": float(half - fee)}], 1,
                                     float(fee), "AllowRevealedSenders"))
        tip = mine(funder, wallet)
        assert txid in zakura("getblock", tip, 1)["tx"]
        for _ in range(UNSAVED_BLOCKS - 3):
            tip = mine(funder, wallet)
        assert wallet("getblockcount") == SAVED_HEIGHT + UNSAVED_BLOCKS
        received = ironwood_balance(wallet, account)
        assert received == int((half - fee) * COIN), received

        # Restart the way a supervisor does, before the wallet saves its position again.
        # It must start without -rescan, rescanning from its saved block.
        live.close()
        live = nodes.enter_context(ExitStack())
        try:
            start_wallet(live)
        except RuntimeError as error:
            raise AssertionError(f"New wallet failed to restart after catching up: {error}") from error
        rescans = re.findall(r"Rescanning last (\d+) blocks \(from block (\d+)\)", wallet_log.read_text())
        assert rescans[-1] == (str(UNSAVED_BLOCKS), str(SAVED_HEIGHT)), rescans
        follow(wallet, tip)
        assert ironwood_balance(wallet, account) == received

        # The rescanned note still spends, which needs a valid Ironwood witness.
        payout_txid = submit(wallet, wallet("z_sendmany", address, [{"address": payee, "amount": 1}], 1, None,
                                            "AllowRevealedRecipients"))
        tip = mine(funder, wallet)
        assert payout_txid in zakura("getblock", tip, 1)["tx"]
        spent = wallet("getrawtransaction", payout_txid, 1)["ironwood"]["valueBalanceZat"]
        assert spent > COIN
        remaining = received - spent
        assert ironwood_balance(wallet, account) == remaining

        # Later restarts keep the same balance.
        live.close()
        live = nodes.enter_context(ExitStack())
        start_wallet(live)
        follow(wallet, tip)
        assert ironwood_balance(wallet, account) == remaining


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zcashd", required=True, type=Path)
    parser.add_argument("--zakurad", required=True, type=Path)
    parser.add_argument("--workdir", type=Path, help="Retain logs and regtest data in a fresh directory")
    args = parser.parse_args(argv)
    if args.workdir:
        args.workdir.mkdir(exist_ok=False)
        exercise(args, args.workdir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="ironwood-birthday-restart-") as root:
            exercise(args, Path(root))
    print("Restarted a new wallet that caught up before saving its position.")


if __name__ == "__main__":
    main()
