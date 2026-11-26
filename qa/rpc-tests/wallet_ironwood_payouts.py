#!/usr/bin/env python3
"""Qualify Ironwood payouts with a real Zakura regtest peer and a restored wallet."""
import argparse
from contextlib import ExitStack, contextmanager
from decimal import Decimal
import json
import sys
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from urllib.request import Request, urlopen
from urllib.error import HTTPError

# Use the same Base58Check encoder as the project's transparent address vectors.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "contrib" / "testgen"))
from base58 import b58encode_chk

COIN = 100_000_000
UPGRADES = [
    ("Overwinter", "5ba81b19"), ("Sapling", "76b809bb"), ("Blossom", "2bb40e60"),
    ("Heartwood", "f5b9230b"), ("Canopy", "e9ff75a6"), ("NU5", "c2d6d0b4"),
    ("NU6", "c8e71055"), ("NU6.1", "4dec4df0"), ("NU6.2", "5437f330"),
    ("NU6.3", "37a5165b"), ("NU7", "77190ad9"),
]
LOCKBOX = "t2RnBRiqrN1nW4ecZs1Fj3WWjNdnSs4kiX8"
NU63_HEIGHT = 108
NU7_HEIGHT = 135


class RpcError(Exception):
    pass


def rpc(port, method, *params, auth=None):
    payload = json.dumps(dict(jsonrpc="2.0", id=1, method=method, params=params)).encode()
    headers = {"Content-Type": "application/json"}
    if auth:
        import base64
        headers["Authorization"] = "Basic " + base64.b64encode(auth.read_bytes().strip()).decode()
    request = Request(f"http://127.0.0.1:{port}", data=payload, headers=headers)
    try:
        response = urlopen(request, timeout=120)
    except HTTPError as error:
        response = error
    with response:
        reply = json.load(response, parse_float=Decimal)
    if reply.get("error"):
        raise RpcError(reply["error"]["message"])
    return reply["result"]


def wait_for(check, description, timeout=180):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.25)
    raise AssertionError(f"Timed out waiting for {description}")


@contextmanager
def node_process(command, log):
    with log.open("ab") as output:
        proc = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            yield proc
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def ready(proc, call, log):
    def probe():
        if proc.poll() is not None:
            raise RuntimeError(f"Node exited with {proc.returncode}, see {log}")
        try:
            call("getblockcount")
            return True
        except (OSError, RpcError):
            return False
    wait_for(probe, f"RPC startup ({log})")


def free_ports(count):
    with ExitStack() as stack:
        sockets = [stack.enter_context(socket.socket()) for _ in range(count)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        return [sock.getsockname()[1] for sock in sockets]


def exercise(args, root):
    peer, node_port, wallet_port = free_ports(3)
    datadir = root / "wallet"
    datadir.mkdir()
    (datadir / "zcash.conf").touch()
    cookie = datadir / "regtest" / ".cookie"
    activation = dict.fromkeys((name for name, _ in UPGRADES), 1)
    activation.update({"NU6.3": NU63_HEIGHT, "NU7": NU7_HEIGHT})
    flags = [f"-nuparams={branch}:{activation[name]}" for name, branch in UPGRADES]
    flags += [f"-onetimelockboxdisbursement=0:4dec4df0:0:{LOCKBOX}", "-regtestnsminitialbalance=1000000000"]

    def wallet(method, *params):
        return rpc(wallet_port, method, *params, auth=cookie)

    def zakura(method, *params):
        return rpc(node_port, method, *params)

    def start_wallet(stack, binary, extra):
        log = root / "wallet.log"
        cmd = [binary, "-regtest", f"-datadir={datadir}", f"-exportdir={root}",
               f"-rpcport={wallet_port}", f"-connect=127.0.0.1:{peer}", "-listen=0",
               "-walletrequirebackup=0", "-allowdeprecated=getnewaddress", "-maxtipage=3153600000",
               "-regtestwalletsetbestchaineveryblock", "-regtestshieldcoinbase", "-regtestacceptunvalidatedpow", "-txindex=1", *extra]
        if binary != args.zcashd:
            # Older compat releases disable z_getbalance by default; this build allows it.
            cmd.append("-allowdeprecated=z_getbalance")
        proc = stack.enter_context(node_process(cmd, log))
        ready(proc, wallet, log)
        return proc

    # A second wallet supplies receivers outside the mining wallet's accounts.
    with ExitStack() as receiver:
        start_wallet(receiver, args.zcashd, ["-wallet=receiver.dat", "-mocktime=1296688000"])
        receiver_account = wallet("z_getnewaccount")["account"]
        foreign_mixed = wallet("z_getaddressforaccount", receiver_account, ["sapling", "orchard"])["address"]
        foreign_orchard = wallet("z_getaddressforaccount", receiver_account, ["orchard"])["address"]

    # Create the backup before either operation. An older compat binary also tests
    # loading wallet.dat records written by a version without Ironwood support.
    with ExitStack() as initial:
        # Zakura regtest blocks start in 2011. Give the keys a birthday before those blocks.
        start_wallet(initial, args.previous_zcashd or args.zcashd, ["-mocktime=1296688000"])
        mining = wallet("getnewaddress")
        first, second = wallet("getnewaddress"), wallet("getnewaddress")
        account = wallet("z_getnewaccount")["account"]
        sweep = wallet("z_getaddressforaccount", account, ["orchard"])["address"]
        mixed = wallet("z_getaddressforaccount", account, ["sapling", "orchard"])["address"]
        sapling = wallet("z_getaddressforaccount", account, ["sapling"])["address"]
        backup = Path(wallet("backupwallet", "beforeIronwood"))

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
listen_addr = "127.0.0.1:{node_port}"
enable_cookie_auth = false
[mining]
miner_address = "{mining}"
[mempool]
debug_enable_at_height = 0
''')

    def mine(count):
        hashes = zakura("generate", count)
        wait_for(lambda: wallet("getbestblockhash") == hashes[-1], "wallet to follow mined blocks")
        wait_for(lambda: wallet("getblockchaininfo")["fullyNotified"], "wallet block notifications")
        return hashes[-1]

    def result(opid):
        return wait_for(lambda: wallet("z_getoperationresult", [opid]), f"operation {opid}", 360)[0]

    def mine_wallet_transaction(opid):
        outcome = result(opid)
        assert outcome["status"] == "success", outcome.get("error")
        txid = outcome["result"]["txid"]
        wait_for(lambda: txid in zakura("getrawmempool"), "independent mempool acceptance")
        block = mine(1)
        assert txid in zakura("getblock", block, 1)["tx"]
        return wallet("getrawtransaction", txid, 1)

    def confirm(opid):
        tx = mine_wallet_transaction(opid)
        txid = tx["txid"]
        assert tx["version"] == 6 and not tx["orchard"]["actions"] and tx["ironwood"]["actions"]
        wire = bytes.fromhex(zakura("getrawtransaction", txid))
        assert wire.hex() == tx["hex"]
        branch = UPGRADES[-1 if zakura("getblockcount") >= NU7_HEIGHT else -2][1]
        assert wire[8:12][::-1].hex() == branch
        return tx

    def check_shield(tx, sweep_result):
        assert len(tx["vin"]) == sweep_result["shieldingUTXOs"] and not tx["vout"]
        inputs = 0
        for vin in tx["vin"]:
            reward = wallet("getrawtransaction", vin["txid"], 1)
            assert "coinbase" in reward["vin"][0] and reward["confirmations"] >= 101
            inputs += int(reward["vout"][vin["vout"]]["value"] * COIN)
        assert inputs == int(sweep_result["shieldingValue"] * COIN)
        fee = 5000 * (len(tx["vin"]) + len(tx["ironwood"]["actions"]))
        assert -tx["ironwood"]["valueBalanceZat"] == inputs - fee
        return inputs - fee

    def balance():
        return wallet("z_getbalanceforaccount", account)["pools"].get("ironwood", {}).get("valueZat", 0)

    def disconnect_and_reconnect(block):
        wallet("invalidateblock", block)
        wait_for(lambda: wallet("getbestblockhash") != block and
                 wallet("getblockchaininfo")["fullyNotified"], "wallet to disconnect block")
        wallet("reconsiderblock", block)
        wait_for(lambda: wallet("getbestblockhash") == block and
                 wallet("getblockchaininfo")["fullyNotified"], "wallet to reconnect block")

    def payout(outputs, *options, source=None):
        return wallet("z_sendmany", source or sweep, [{"address": addr, "amount": amount} for addr, amount in outputs],
                      *options)

    def check_payout(tx, outputs, fee_zat=None):
        assert not tx["vin"]
        actual = {v["scriptPubKey"]["addresses"][0]: int(v["value"] * COIN) for v in tx["vout"]}
        for v in tx["vout"]:
            assert zakura("gettxout", tx["txid"], v["n"])["value"] == v["value"]
        assert len(tx["vout"]) == len(outputs)
        assert actual == {addr: int(Decimal(str(amount)) * COIN) for addr, amount in outputs}
        # One shielded spend and change pad to two actions. Transparent outputs
        # contribute another logical action per standard P2PKH destination.
        fee = fee_zat if fee_zat is not None else 5000 * (len(tx["ironwood"]["actions"]) + len(outputs))
        assert tx["ironwood"]["valueBalanceZat"] == sum(actual.values()) + fee

    with ExitStack() as nodes:
        zlog = root / "zakura.log"
        ready(nodes.enter_context(node_process([args.zakurad, "-c", config, "start"], zlog)), zakura, zlog)
        sidecar = nodes.enter_context(ExitStack())
        start_wallet(sidecar, args.zcashd, flags)
        # s-nomp only pays from a shielded address that the wallet reports as its own.
        assert wallet("z_validateaddress", sweep)["ismine"]
        mine(100)
        try:
            wallet("z_shieldcoinbase", mining, sweep)
        except RpcError:
            pass
        else:
            raise AssertionError("Wallet selected an immature reward")
        mine(1)
        try:
            wallet("sendmany", "", {first: 1})
        except RpcError as error:
            assert "Coinbase funds can only be sent to a zaddr" in str(error), error
        else:
            raise AssertionError("Wallet spent coinbase to a transparent address")
        legacy = wallet("z_shieldcoinbase", mining, sweep, None, 1)
        legacy_result = result(legacy["opid"])
        assert legacy_result["status"] == "success", legacy_result
        historical_id = legacy_result["result"]["txid"]
        wait_for(lambda: historical_id in zakura("getrawmempool"), "Orchard shield before activation")
        mine(1)
        historical = wallet("getrawtransaction", historical_id, 1)
        assert historical["version"] == 5 and historical["orchard"]["actions"]
        frozen = -historical["orchard"]["valueBalanceZat"]
        assert wallet("z_getbalanceforaccount", account)["pools"]["orchard"]["valueZat"] == frozen
        assert wallet("z_getbalance", sweep) * COIN == frozen
        private = result(payout([(first, 1)]))
        assert private["status"] == "failed" and "AllowRevealedRecipients" in private["error"]["message"], private
        mine(NU63_HEIGHT - 1 - zakura("getblockcount"))
        assert wallet("z_getbalance", sweep) == 0
        unsupported = result(payout([(first, 1)], 1, None, "AllowRevealedRecipients"))
        assert unsupported["status"] == "failed" and "Historical Orchard" in unsupported["error"]["message"]
        one = wallet("z_shieldcoinbase", mining, sweep, None, 1)
        assert one["shieldingUTXOs"] == 1 and one["remainingUTXOs"] == NU63_HEIGHT - 103
        shield = confirm(one["opid"])
        assert not shield["vout"] and len(shield["vin"]) == 1
        received = check_shield(shield, one)
        wait_for(lambda: balance() == received, "confirmed Ironwood balance")
        assert wallet("z_getbalance", sweep) * COIN == received
        assert wallet("z_getbalance", sweep, 8) == 0
        # The sweep and mixed addresses share an incoming viewing key. Only the sweep was paid.
        assert wallet("z_listreceivedbyaddress", mixed) == []
        receipts = wallet("z_listreceivedbyaddress", sweep)
        assert [n["txid"] for n in receipts if n["pool"] == "ironwood"] == [shield["txid"]]
        unspent = wallet("z_listunspent", 1, 999999, False, [sweep, mixed])
        assert [(n["txid"], n["address"]) for n in unspent if n["pool"] == "ironwood"] == [(shield["txid"], sweep)]
        disconnect_and_reconnect(wallet("getbestblockhash"))
        assert balance() == received
        mine(2)
        batch = wallet("z_shieldcoinbase", mining, sweep, None, 2)
        assert batch["shieldingUTXOs"] == 2 and batch["remainingUTXOs"] > 0
        shield = confirm(batch["opid"])
        assert len(shield["vin"]) == 2 and not shield["vout"]
        received += check_shield(shield, batch)
        remainder = wallet("z_shieldcoinbase", mining, sweep)
        assert remainder["remainingUTXOs"] == 0
        received += check_shield(confirm(remainder["opid"]), remainder)
        wait_for(lambda: balance() == received, "all rewards credited")

        sidecar.close()
        start_wallet(sidecar, args.zcashd, flags)
        assert balance() == received
        notes = wallet("z_listunspent", 1)
        assert sum(int(n["amount"] * COIN) for n in notes if n["pool"] == "ironwood") == received
        sidecar.close()
        if args.previous_zcashd:
            # Sync the old wallet past a payment it cannot see. Its backup now
            # has a recent block locator but no Ironwood witnesses or note hints.
            current = root / "current"
            datadir.rename(current)
            (datadir / "regtest").mkdir(parents=True)
            (datadir / "zcash.conf").touch()
            shutil.copy2(backup, datadir / "regtest" / "wallet.dat")
            # The older wallet only needs the upgrades through NU6.3.
            start_wallet(sidecar, args.previous_zcashd, [f for f in flags if not f.startswith("-nuparams=77190ad9")])
            wait_for(lambda: wallet("getbestblockhash") == zakura("getbestblockhash") and
                     wallet("getblockchaininfo")["fullyNotified"], "pre-feature wallet to sync")
            assert "ironwood" not in wallet("z_getbalanceforaccount", account)["pools"]
            backup = Path(wallet("backupwallet", "afterIronwood"))
            sidecar.close()
            datadir.rename(root / "previous")
            current.rename(datadir)
        shutil.copy2(backup, datadir / "regtest" / "wallet.dat")
        start_wallet(sidecar, args.zcashd, flags)
        wait_for(lambda: balance() == received, "backup to recover shielded funds")
        wait_for(lambda: any(p.get("version", 0) for p in wallet("getpeerinfo")), "restored wallet to reconnect")
        if args.previous_zcashd:
            # The old binary advances the saved wallet tip without updating
            # the existing Ironwood tree. Upgrading must rebuild its witnesses.
            sidecar.close()
            start_wallet(sidecar, args.previous_zcashd, [f for f in flags if not f.startswith("-nuparams=77190ad9")])
            mine(1)
            sidecar.close()
            start_wallet(sidecar, args.zcashd, flags)
            assert wallet("z_getbalance", sweep) * COIN == received
            wait_for(lambda: any(p.get("version", 0) for p in wallet("getpeerinfo")), "upgraded wallet to reconnect")
        outputs = [(first, 1)]
        mine(NU63_HEIGHT + 8 - zakura("getblockcount"))
        immature = result(payout(outputs))
        assert immature["status"] == "failed" and "Insufficient funds" in immature["error"]["message"], immature
        # The two-argument z_sendmany uses ten confirmations, even though z_getbalance defaults to one.
        mine(1)
        mixed_recipients = result(payout([(first, 1), (sapling, 1)]))
        assert (mixed_recipients["status"] == "failed" and
                "AllowRevealedRecipients" in mixed_recipients["error"]["message"]), mixed_recipients
        tx = confirm(payout(outputs))
        check_payout(tx, outputs)
        remaining = received - tx["ironwood"]["valueBalanceZat"]
        assert balance() == remaining
        # A transparent send to one UA funds Ironwood, as pool sweepers do instead of z_shieldcoinbase.
        swept_in = confirm(wallet("z_sendmany", first, [{"address": sweep, "amount": 0.99985}],
                                  1, 0.00015, "AllowRevealedSenders"))
        assert len(swept_in["vin"]) == 1 and not swept_in["vout"] and len(swept_in["ironwood"]["actions"]) == 2
        assert -swept_in["ironwood"]["valueBalanceZat"] == 99_985_000
        remaining += 99_985_000
        wait_for(lambda: balance() == remaining, "transparent funds credited to Ironwood")
        sapling_op = wallet("z_shieldcoinbase", mining, sapling, None, 1)
        sapling_result = result(sapling_op["opid"])
        assert sapling_result["status"] == "success", sapling_result
        sapling_txid = sapling_result["result"]["txid"]
        wait_for(lambda: sapling_txid in zakura("getrawmempool"), "Sapling shield to relay")
        mine(1)
        sapling_reserve = wallet("z_getbalanceforaccount", account)["pools"]["sapling"]["valueZat"]
        assert sapling_reserve > 0
        mine(NU7_HEIGHT - zakura("getblockcount") + 1)
        # Prove both directions on the new branch ID as well.
        sweep_result = wallet("z_shieldcoinbase", "*", sweep, None, 1)
        before = remaining + check_shield(confirm(sweep_result["opid"]), sweep_result)
        assert balance() == before
        outputs = [(first, 2), (second, 1)]
        for policy in ("FullPrivacy", "LegacyCompat"):
            private = result(payout(outputs, 1, None, policy))
            assert private["status"] == "failed" and "AllowRevealedRecipients" in private["error"]["message"], private
        # The notes cover the requested amount exactly, leaving no funds for the fee.
        insufficient = result(payout([(first, float(Decimal(before) / COIN))], 1, source=mixed))
        assert insufficient["status"] == "failed" and "Insufficient funds" in insufficient["error"]["message"]
        # 60,000 standard transparent outputs exceed the two-megabyte transaction limit.
        oversized = [(b58encode_chk(b"\x1d\x25" + n.to_bytes(20, "big")), 0.0001) for n in range(60_000)]
        try:
            payout(oversized)
        except RpcError as error:
            assert "Too many outputs" in str(error), error
        else:
            raise AssertionError("Wallet accepted an oversized batch")
        assert balance() == before
        assert wallet("z_getbalance", sweep, 8) * COIN < before
        mine(7)
        assert wallet("z_getbalance", sweep, 8) * COIN == before
        tx = confirm(payout(outputs, 8, 0.0004, "AllowRevealedRecipients"))
        check_payout(tx, outputs, fee_zat=40_000)
        remaining = before - tx["ironwood"]["valueBalanceZat"]
        wait_for(lambda: balance() == remaining, "change to be credited exactly once")
        disconnect_and_reconnect(wallet("getbestblockhash"))
        assert balance() == remaining
        details = wallet("z_viewtransaction", tx["txid"])
        assert any(s["pool"] == "ironwood" for s in details["spends"])
        assert any(o["pool"] == "ironwood" and o["walletInternal"] for o in details["outputs"])
        sidecar.close()
        start_wallet(sidecar, args.zcashd, [*flags, "-rescan"])
        assert balance() == remaining
        wait_for(lambda: any(p.get("version", 0) for p in wallet("getpeerinfo")), "rescanned wallet to reconnect")
        # After a hard kill the saved wallet tip is ahead of the node's last flushed
        # block index. The restart must resume from that tip, not rewind it.
        sidecar.close()
        proc = start_wallet(sidecar, args.zcashd, flags)
        mine(3)
        proc.kill()
        proc.wait()
        sidecar.close()
        start_wallet(sidecar, args.zcashd, flags)
        wait_for(lambda: wallet("getbestblockhash") == zakura("getbestblockhash") and
                 wallet("getblockchaininfo")["fullyNotified"], "wallet to resume after a hard kill")
        assert balance() == remaining
        # A fixed fee must cover the batch's logical actions under the default relay policy.
        mine(8)
        workers = [(addr, 0.001) for addr, _ in oversized[:80]]
        underpaid = result(payout(workers, 8, 0.0001, "AllowRevealedRecipients"))
        assert underpaid["status"] == "failed" and "unpaid action limit exceeded" in underpaid["error"]["message"], underpaid
        assert balance() == remaining
        large = confirm(payout(workers, 8, 0.0041, "AllowRevealedRecipients"))
        assert len(large["ironwood"]["actions"]) == 2
        check_payout(large, workers, fee_zat=410_000)
        remaining -= large["ironwood"]["valueBalanceZat"]
        assert balance() == remaining
        # Both operations request almost all remaining notes and cannot both succeed.
        almost_all = float(Decimal(remaining - 500000) / COIN)
        ops = [payout([(first, almost_all)], 1, source=mixed), payout([(second, almost_all)], 1)]
        results = [result(op) for op in ops]
        assert sorted(item["status"] for item in results) == ["failed", "success"], results
        winner = next(item["result"]["txid"] for item in results if item["status"] == "success")
        wait_for(lambda: winner in zakura("getrawmempool"), "change payout to reach Zakura")
        assert winner in zakura("getblock", mine(1), 1)["tx"]
        spent = wallet("getrawtransaction", winner, 1)
        destination = first if results[0]["status"] == "success" else second
        check_payout(spent, [(destination, almost_all)])
        assert balance() == remaining - spent["ironwood"]["valueBalanceZat"]
        pools = wallet("z_getbalanceforaccount", account)["pools"]
        assert pools["orchard"]["valueZat"] == frozen and pools["sapling"]["valueZat"] == sapling_reserve
        # s-nomp moves its whole sweep balance with the 2-argument z_sendmany and leaves
        # 0.0001 ZEC for the fee, less than the conventional fee for its spends and one
        # transparent output. That is refused by default and pays the 0.0001 ZEC when the
        # operator allows the unpaid actions, as a Sapling sweep of that shape already does.
        mine(10)
        swept = balance()
        snomp = [(first, float(Decimal(swept - 10_000) / COIN))]
        refused = result(payout(snomp))
        assert refused["status"] == "failed" and "Insufficient funds" in refused["error"]["message"], refused
        # Pools run with legacy_privacy and a nonzero unpaid-action limit.
        sidecar.close()
        start_wallet(sidecar, args.zcashd, [*flags, "-txunpaidactionlimit=50", "-allowdeprecated=legacy_privacy"])
        wait_for(lambda: any(p.get("version", 0) for p in wallet("getpeerinfo")), "wallet to reconnect")
        check_payout(confirm(payout(snomp)), snomp, fee_zat=10_000)
        assert balance() == 0
        # With legacy_privacy, a sweeper's transparent send to the UA needs no policy: it
        # reveals the inputs and returns change to a transparent address, as for Sapling.
        swept_in = confirm(wallet("z_sendmany", second, [{"address": sweep, "amount": 0.5}]))
        assert swept_in["vin"] and len(swept_in["vout"]) == 1 and len(swept_in["ironwood"]["actions"]) == 2
        assert -swept_in["ironwood"]["valueBalanceZat"] == 50_000_000
        wait_for(lambda: balance() == 50_000_000, "swept funds credited to Ironwood")

        try:
            wallet("z_shieldcoinbase", mining, foreign_orchard, None, 1)
        except RpcError as error:
            assert "Use a wallet-owned Unified Address" in str(error), error
        else:
            raise AssertionError("Accepted a foreign Orchard-only shielding destination")

        shield = wallet("z_shieldcoinbase", mining, foreign_mixed, None, 1)
        tx = mine_wallet_transaction(shield["opid"])
        sidecar.close()
        start_wallet(sidecar, args.zcashd, [*flags, "-wallet=receiver.dat"])
        wait_for(lambda: wallet("getbestblockhash") == zakura("getbestblockhash") and
                 wallet("getblockchaininfo")["fullyNotified"], "recipient wallet to sync")
        receipts = wallet("z_listreceivedbyaddress", foreign_mixed)
        assert [(n["pool"], n["txid"]) for n in receipts] == [("sapling", tx["txid"])]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zcashd", required=True, type=Path)
    parser.add_argument("--zakurad", required=True, type=Path)
    parser.add_argument("--previous-zcashd", type=Path,
                        help="Pre-feature compat binary with NU6.3 consensus support for the upgrade test")
    parser.add_argument("--workdir", type=Path, help="Retain logs and regtest data in a fresh directory")
    args = parser.parse_args(argv)
    if args.workdir:
        args.workdir.mkdir(exist_ok=False)
        exercise(args, args.workdir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="ironwood-payouts-") as root:
            exercise(args, Path(root))
    print("Confirmed Ironwood shielding, wallet recovery, and transparent payouts with Zakura.")


if __name__ == "__main__":
    main()
