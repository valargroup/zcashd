(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes at release time)

Notable changes
===============

Sidecar sync after burst mining
------------------------------

The sidecar now fetches headers through its peer's tip after block announcements
arrive while a header request is pending. Out-of-order announcements could
previously leave it one or more blocks behind until another block was announced.

NU7 consensus support
---------------------

This release adds consensus support for the NU7 network upgrade (the ZIP 259
set, consensus branch ID `0x77190ad9`). NU7 activated on Testnet at height
4,465,026 on 2026-10-04, as in Zakura v1.6.0, and v1.1.x sidecars cannot
follow Testnet past that height. Mainnet NU7 is scheduled at height 3,543,405,
matching Zakura's Mainnet schedule. Upgrade before that height to keep following
Mainnet. Run this release behind a Zakura version that supports NU7 on the
selected network.

From NU7:

- **25-second blocks (ZIP 218).** The target spacing drops from 75 to 25
  seconds and the difficulty averaging window grows from 17 to 102 blocks. The
  halving interval triples and the block subsidy is divided by 3, so issuance
  per unit of time is unchanged, and the funding streams and lockbox follow the
  new schedule.
- **Per-block shielded limits (ZIP 218).** A block holds at most 330 Orchard
  actions, 330 Ironwood actions and 300 Sapling spends and outputs, within a
  combined budget of 330, and no Sprout JoinSplits.
- **Fee burning (ZIP 235).** 60% of each block's transaction fees are burned:
  the coinbase may claim only `F - floor(6F/10)` of the block's fees `F`.
- **Network Sustainability Mechanism (ZIP 237).** Burned fees are added to the
  NSM balance, which is paid back out to miners gradually from the reissuance
  height. `getblockchaininfo` reports it as `nsmValueBalanceZat`.
- **No v4 transactions (ZIP 2003).** v4 transactions, coinbase included, are
  invalid.

The protocol version is now 170190, the Mainnet NU7 version. The Rust crates
are Zakura Common 2.2.0. A CI job checks zcashd's NU7 consensus values
against Zakura's.

Action required for Sprout users BEFORE NU7 activation
------------------------------------------------------

- **Sprout funds become permanently unspendable at NU7.** ZIP 2003 makes v4
  transactions, the only ones that can spend Sprout notes, invalid. Move any
  Sprout funds to a Sapling or transparent address well before the activation
  height, with `z_sendmany` or the Sprout-to-Sapling migration
  (`z_setmigration`).
- **Do not rely on the migration's last rounds.** The migration stops creating
  transactions a few blocks before NU7, and skips any round whose transactions
  could still be unmined when NU7 activates.

Wallet changes from NU7
-----------------------

- Wallet transactions using Ironwood are v6. Other wallet transactions are v5, even with `-preferredtxversion=4`.
- The default expiry delta becomes 120 blocks, the same time as 40 blocks at
  the old spacing. `-txexpirydelta` still overrides it.
- Sprout spends and the Sprout-to-Sapling migration are refused with an error.
- A transaction with more than 300 Sapling spends and outputs is refused,
  because no block could hold it.

Deeper reorgs: wallet and pruning impact
----------------------------------------

zcashd now accepts reorgs of up to 1000 blocks (previously 99), matching
Zakura's `MAX_BLOCK_REORG_HEIGHT`, so the sidecar follows every reorg Zakura
does. This has costs for wallets and pruned nodes:

- **Larger wallets.** The wallet keeps 1001 cached witnesses for each unspent
  Sprout or Sapling note (previously 100), about 10 times the memory and disk
  per note, and its Orchard note commitment tree keeps 1001 checkpoints. Since
  anyone who knows a Sapling address can send it notes, a wallet sent many small
  "dust" notes grows accordingly: at least about 600 MiB per ZEC the sender
  spends on fees.
- **One-way wallet upgrade.** zcashd v1.1.x cannot load a wallet written by
  this release. Back up `wallet.dat` before upgrading.
- **Reorg tolerance builds up after upgrading.** An upgraded wallet starts with
  only the last 100 blocks of history and reaches the full 1000 over the next
  900 or so blocks. A reorg as deep as its history or deeper, in that window,
  stops the node; restart with `-rescan`.
- **Pruned nodes keep more blocks.** A pruned node keeps the last 1001 blocks
  (previously 288). With large blocks this can exceed the 550 MiB minimum
  `-prune` target, so allow at least 2 GiB. A recently upgraded pruned node has
  already pruned older blocks, so it cannot follow a reorg deeper than the
  blocks it has kept until it holds 1001 again.

Mempool around network upgrades
-------------------------------

Zakura bans a peer that relays a transaction it rejects, and Zakura may be a few
blocks ahead of the sidecar. The mempool therefore also checks each transaction
at the highest height Zakura could check it at (3 blocks past the next block,
or past the block after Zakura's best header), and drops any it holds that are
no longer valid there.

So in the last few blocks before an upgrade activates, the mempool refuses new
transactions with `tx-invalid-at-relay-height`, and drops the ones it holds.
From activation, it accepts transactions for the new upgrade. From NU7 it also
refuses any transaction that exceeds a ZIP 218 per-block limit.

Mining
------

Block templates follow the ZIP 218 per-block limits, the ZIP 235 fee split and
ZIP 237 reissuance, and `getblocksubsidy` reports the NU7 amounts. Once
reissuance is active, `getblocktemplate` no longer builds the next block's
coinbase in advance, since it depends on that block's NSM balance.

Other fixes
-----------

- v6 transactions are standard from NU6.3. Previously the mempool rejected
  every v6 transaction relayed to it as `nu5-version`.
- Fixed a sync stall: when Zakura stopped answering a `getdata` at its 1 MB
  limit, the sidecar waited for the download timeout on each remaining block,
  and each wait was longer than the last.

NU6.3 (Ironwood) consensus support
----------------------------------

This release adds full consensus and node support for the NU6.3 network
upgrade, which introduces the Ironwood shielded pool (testnet activation
height 4134000; mainnet activation height 3428143). zcashd
validates and follows the chain across NU6.3 activation, including blocks
containing v6 transactions with Ironwood bundles, and exposes node-level
observability for the new pool: an "ironwood" value pool in `getblockchaininfo`
and `getblock`, `finalironwoodroot` in `getblock`, an ironwood section in
`z_gettreestate` and `z_getsubtreesbyindex`, and an "ironwood" bundle in
verbose `getrawtransaction`/`decoderawtransaction` output for v6 transactions.

Ironwood wallet shielding and transparent payouts
--------------------------------------------------

From NU6.3, `z_shieldcoinbase` can spend mature transparent mining rewards into
Ironwood. Ironwood uses the Orchard receiver in a Unified Address. The wallet
tracks balances and notes in the two pools separately. Existing Orchard notes
remain unavailable for spending through this wallet after NU6.3.

For example, create a wallet-owned sweep address with `z_getaddressforaccount` using the
receiver list `["orchard"]`. Supply the account's actual address and the
transparent addresses and amounts for the payout job in these commands:

```sh
zcash-cli z_shieldcoinbase "$MINING_ADDRESS" "$SWEEP_ADDRESS" null 50
zcash-cli z_getoperationresult '["<shield-operation-id>"]'
zcash-cli z_getbalanceforaccount "$ACCOUNT"
zcash-cli z_sendmany "$SWEEP_ADDRESS" '[{"address":"<worker-1-taddr>","amount":1.25},{"address":"<worker-2-taddr>","amount":0.75}]' 1 null AllowRevealedRecipients
zcash-cli z_getoperationresult '["<payout-operation-id>"]'
```

Poll each operation until it finishes. Wait for the shield to confirm before
paying recipients. `remainingUTXOs` in the shielding result counts eligible
rewards left for a subsequent sweep. A payout can also have one destination.
The wallet pays the specified amounts, deducts the fee from the sweep account,
and returns change to its internal Ironwood receiver. Restoring an older wallet
backup requires the original blocks to rebuild the notes and their witnesses.

With stock s-nomp, set `zAddress` to the sweep UA. `z_validateaddress` now
reports `ismine` for Unified Addresses (true when the wallet holds the spending
key), which s-nomp requires before it starts payment processing. s-nomp's two-argument
`z_sendmany` refill permits revealed recipients when spending Ironwood
to transparent addresses. This call requires ten confirmations. An explicit
privacy policy still applies. s-nomp leaves 0.0001 ZEC for the fee, less than
the conventional fee for the notes it spends and its transparent output. With
no fee given, the wallet pays what is left when the unpaid actions fit
`-txunpaidactionlimit`, as a Sapling sweep of that shape already does; set it to
at least the number of notes a sweep spends. The default of 0 refuses the
payout with "Insufficient funds".
Pools using the deprecated `z_getbalance` RPC must start the wallet with
`-allowdeprecated=z_getbalance`. On the Orchard-only sweep UA this RPC
reports confirmed Ironwood funds from NU6.3 and accepts a minimum confirmation
count. Historical Orchard remains visible in `z_getbalanceforaccount`.

Miningcore can use `"*"` for the shielding source and request eight
confirmations and an explicit fee for payouts. With default relay policy,
a fixed fee must cover all logical actions, including each standard
transparent output and the padded Ironwood actions. For example, a batch of
80 standard P2PKH outputs and two Ironwood actions needs 410,000 zatoshis.

Size batches using the serialized transaction limit of 2,000,000 bytes, the
wallet's maximum fee, and its Orchard action limit (which also applies to
Ironwood notes). A payout with many recipients or many small input notes can
reach different limits. From NU7, a block can contain at most 330 Ironwood
actions. Check the returned operation result before retrying a batch.

P2P sidecar hard-lock
---------------------

This compatibility build is intended to run as a wallet/RPC sidecar behind
Zakura. It now refuses to start unless exactly one `-connect=<zakura-address>`
peer is configured, never opens a P2P listener, and rejects `-addnode`,
`-seednode`, `-bind`, and `-whitebind`. The `addnode` RPC is not registered and
returns `Method not found`.

This makes the sidecar's P2P isolation a binary guarantee. Zcashd can only dial
the single Zakura peer supplied at startup, and no public peer can connect
inbound to zcashd.

After publishing this sidecar release, update Zakura's pinned compatibility
manifest and installer checksums to this build.

Compatibility support policy
----------------------------

Unlike upstream zcashd, this compatibility build has no scheduled End of Life.
It no longer contains an End-of-Support height, emits height-based support
warnings, requires the upstream deprecation acknowledgment at startup, or
returns End-of-Service data from `getdeprecationinfo`.

Valargroup reserves the right to announce a future End of Life through a Zakura
compatibility release. Integrators are encouraged to migrate to
[Zakura](https://github.com/zakura-core/zakura) and monitor its
[CHANGELOG](https://github.com/zakura-core/zakura/blob/main/CHANGELOG.md).
