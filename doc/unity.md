# Unity Node Operation

Unity mode is an explicit `zcashd` operating mode for deployments that keep the
`zcashd` wallet, RPC, block-file, chainstate, index, and notification surfaces
while replacing local Zcash P2P with a trusted Zebra source.

Use `-unity` to select the preset:

```text
-blocksource=zebra -p2p=0 -blockvalidation=trusted-zebra
```

The lower-level knobs are available for testing and staged rollout:

```text
-blocksource=<p2p|zebra>
-p2p=<1|0>
-blockvalidation=<full|trusted-zebra>
-unityzebra=http://127.0.0.1:8232
-unityzebrarpcuser=<user>
-unityzebrarpcpassword=<password>
-unityzebracookiefile=<path>
-unitypollinterval=<seconds>
-unitysyncbatchsize=<blocks>
```

## Trust Model

Unity trusts the configured Zebra node for data availability, peer selection,
best-chain discovery, transaction relay, and expensive cryptographic block
verification. Unity does not blindly trust Zebra for local state construction:
Zebra-sourced blocks are still decoded, written to normal local block files,
accepted through the block index, and connected through the local state
transition machinery that maintains chainstate, wallet state, indexes, and ZMQ
notifications.

Operators should run Zebra locally or over a private authenticated network.
Production Unity mode requires Zebra RPC authentication. Public unauthenticated
Zebra endpoints must not be used for production Unity mode.

## Deployment Topology

Recommended single-host topology:

```text
Zebra JSON-RPC 127.0.0.1:8232  <---authenticated HTTP---  zcashd -unity
Zebra P2P enabled                                      zcashd P2P disabled
```

Recommended split-host topology:

```text
Zebra host on private network  <---authenticated, firewalled HTTP---  zcashd -unity host
```

Do not expose the Zebra JSON-RPC endpoint publicly. Use host firewalls,
private addressing, or a mutually authenticated tunnel if the Zebra and Unity
processes are not on the same machine.

## Readiness And Diagnostics

Use `getunityinfo` as the primary monitoring surface. The top-level
`readiness` value is:

- `ready`: Zebra identity is verified, the local tip matches Zebra's best tip,
  the local tip has left IBD, validation notifications have caught up, Zebra
  transaction forwarding has no transport failure, and the Zebra mempool mirror
  has a fresh successful poll with no lag, divergence, or current error.
- `degraded`: Unity can keep running but is waiting, syncing, retrying,
  lagging, waiting for notification catch-up, has a stale or failed mempool
  mirror, or has non-fatal mempool/forwarding diagnostics.
- `failed`: Unity has hit a hard sync or identity fault and will not advance
  past that fault without operator action.
- `disabled`: Unity mode is not enabled.

The `sync` object carries the current detailed state, last error, local lag,
and retry/backoff counters. The `metrics` object repeats the script-friendly
counters most useful to dashboards, including mempool freshness, forwarding
transport health, and validation-notification catch-up. Ordinary Zebra
rejections of invalid user transactions remain visible in
`tx_forwarding.last_error`, but they do not make the node degraded; transport
and connectivity failures are reported separately as
`tx_forwarding.last_transport_error`. The `limits` object exposes hard bounds
such as block batch size, Zebra RPC response size, pending forwarded
transactions, and per-poll mempool reconciliation limits.

Existing network RPCs remain scriptable: Unity reports no local Zcash peers and
P2P-control RPCs are unavailable while `-p2p=0`.

## Recovery Procedures

For transient Zebra outages:

1. Keep `zcashd -unity` running.
2. Restore Zebra availability or authentication.
3. Watch `getunityinfo.sync.retry_count`,
   `getunityinfo.sync.current_backoff_seconds`, and
   `getunityinfo.readiness`.
4. Unity should return to `ready` after it verifies Zebra identity and catches
   the local tip up to Zebra's best tip.

For Zebra endpoint changes:

1. Stop `zcashd`.
2. Update `-unityzebra` and its credentials.
3. Restart `zcashd -unity`.
4. Confirm `getunityinfo.zebra.identity_verified` is true.
5. Confirm the trusted boundary in `getunityinfo.trusted_boundary` is either
   inactive or matches the configured endpoint, network, and genesis.

If the trusted boundary does not match, Unity must not silently apply the old
trusted-source decision to a different source. Reconfirm the local data
against the intended Zebra source or rebuild using the configured trusted
source policy.

For hard sync faults:

1. Stop Unity block ingestion by stopping `zcashd`.
2. Preserve `debug.log`, `getunityinfo`, and the local datadir for diagnosis.
3. Confirm Zebra is on the expected network and best chain.
4. If Zebra served a block that failed retained local checks, do not skip ahead
   locally. Use a different Zebra source or investigate the local/Zebra
   mismatch before restarting.
5. If local block files or chainstate are corrupt, recover from backup or run
   the documented reindex strategy for the selected trusted-source policy.

For reindex or reindex-chainstate:

Unity uses a trusted boundary tied to network, genesis, and Zebra endpoint
identity. Reindex workflows must preserve or intentionally re-establish that
boundary; otherwise blocks are rebuilt with full local validation or require
live Zebra reconfirmation according to the configured recovery policy. Always
check `getunityinfo.trusted_boundary` after recovery.

## Compatibility Expectations

Wallet behavior, local block files, chainstate, optional indexes, ZMQ
notifications, and local RPC response semantics continue to come from
`zcashd`. Zebra replaces only block acquisition, best-chain source data, and
transaction relay. Retained local validation failures are fail-closed.
