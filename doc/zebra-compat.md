# zebra-compat Node Operation

zebra-compat mode is an explicit `zcashd` operating mode for deployments that keep the
`zcashd` wallet, RPC, block-file, chainstate, index, and notification surfaces
while replacing local Zcash P2P with a trusted Zebra source.

Use `-zebra-compat` to select the preset:

```text
-blocksource=zebra -p2p=0 -blockvalidation=trusted-zebra
```

The lower-level knobs are available for testing and staged rollout:

```text
-blocksource=<p2p|zebra>
-p2p=<1|0>
-blockvalidation=<full|trusted-zebra>
-zebra-compat-url=http://127.0.0.1:8232
-zebra-compat-rpc-user=<user>
-zebra-compat-rpc-password=<password>
-zebra-compat-cookiefile=<path>
-zebra-compat-poll-interval=<seconds>
-zebra-compat-sync-batch-size=<blocks>
```

## Quick Start

zebra-compat mode is `zcashd` driven by a Zebra JSON-RPC endpoint. You need a running
Zebra node with RPC enabled, and a `zcashd` built from this tree.

### 1. Run Zebra with authenticated JSON-RPC

In Zebra's `zebrad.toml`, enable the RPC endpoint. Zebra authenticates RPC
callers with a cookie file (recommended); keep it enabled and never expose the
endpoint publicly:

```toml
[rpc]
listen_addr = "127.0.0.1:8232"
# enable_cookie_auth defaults to true; Zebra writes a `.cookie` file
# (user:password format) into its cache/cookie dir on startup.
```

Start Zebra and note the path to the generated `.cookie` file (printed at
startup, under Zebra's cache dir by default). Let Zebra finish syncing to the
network tip before pointing zebra-compat at it.

### 2. Build `zcashd`

```sh
./zcutil/build.sh -j$(nproc)
```

### 3. Run `zcashd` in zebra-compat mode

Point zebra-compat at the Zebra endpoint and supply the cookie file for
authentication:

```sh
./src/zcashd -zebra-compat \
  -zebra-compat-url=http://127.0.0.1:8232 \
  -zebra-compat-cookiefile=/path/to/zebra/.cookie
```

`-zebra-compat` expands to `-blocksource=zebra -p2p=0 -blockvalidation=trusted-zebra`.
If you cannot share the cookie file (for example a remote Zebra host), use
static credentials instead:

```sh
./src/zcashd -zebra-compat \
  -zebra-compat-url=http://10.0.0.2:8232 \
  -zebra-compat-rpc-user=<user> -zebra-compat-rpc-password=<password>
```

When Zebra and `zcashd` are on different hosts, do not expose the Zebra RPC
port to the public internet: bind it to a private interface, restrict it with a
host firewall, or tunnel it. See **Deployment Topology** below.

### 4. Verify the connection

```sh
./src/zcash-cli getzebracompatinfo
```

Confirm `zebra.identity_verified` is `true` and watch `readiness` move from
`degraded` (syncing) to `ready` once the local tip catches Zebra's best tip.
`sync.retry_count` and `sync.current_backoff_seconds` surface connectivity
problems; see **Readiness And Diagnostics** below for the full surface.

## Release Artifacts For Zebra Integration

`zcashd -zebra-compat` is released independently from Zebra. Zebra release CI
consumes these release artifacts to build `zfnd/zebra-zcashd-compat` images and
to update managed download metadata.

Each compat release should publish Linux runtime tarballs for:

- `x86_64-pc-linux-gnu` (`linux-x86_64`)
- `aarch64-linux-gnu` (`linux-aarch64`)

and a consolidated `zcashd-zebra-compat-manifest-<tag>.json` that includes
artifact URLs and SHA256 values for those targets.

## Trust Model

zebra-compat trusts the configured Zebra node for data availability, peer selection,
best-chain discovery, transaction relay, and expensive cryptographic block
verification. zebra-compat does not blindly trust Zebra for local state construction:
Zebra-sourced blocks are still decoded, written to normal local block files,
accepted through the block index, and connected through the local state
transition machinery that maintains chainstate, wallet state, indexes, and ZMQ
notifications.

Operators should run Zebra locally or over a private authenticated network.
Production zebra-compat mode requires Zebra RPC authentication. Public unauthenticated
Zebra endpoints must not be used for production zebra-compat mode.

## Deployment Topology

Recommended single-host topology:

```text
Zebra JSON-RPC 127.0.0.1:8232  <---authenticated HTTP---  zcashd -zebra-compat
Zebra P2P enabled                                      zcashd P2P disabled
```

Recommended split-host topology:

```text
Zebra host on private network  <---authenticated, firewalled HTTP---  zcashd -zebra-compat host
```

Do not expose the Zebra JSON-RPC endpoint publicly. Use host firewalls,
private addressing, or a mutually authenticated tunnel if the Zebra and zebra-compat
processes are not on the same machine.

## Readiness And Diagnostics

Use `getzebracompatinfo` as the primary monitoring surface. The top-level
`readiness` value is:

- `ready`: Zebra identity is verified, the local tip matches Zebra's best tip,
  the local tip has left IBD, validation notifications have caught up, Zebra
  transaction forwarding has no transport failure, and the Zebra mempool mirror
  has a fresh successful poll with no lag, divergence, or current error.
- `degraded`: zebra-compat can keep running but is waiting, syncing, retrying,
  lagging, waiting for notification catch-up, has a stale or failed mempool
  mirror, or has non-fatal mempool/forwarding diagnostics.
- `failed`: zebra-compat has hit a hard sync or identity fault and will not advance
  past that fault without operator action.
- `disabled`: zebra-compat mode is not enabled.

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

Existing network RPCs remain scriptable: zebra-compat reports no local Zcash peers and
P2P-control RPCs are unavailable while `-p2p=0`.

## Recovery Procedures

For transient Zebra outages:

1. Keep `zcashd -zebra-compat` running.
2. Restore Zebra availability or authentication.
3. Watch `getzebracompatinfo.sync.retry_count`,
   `getzebracompatinfo.sync.current_backoff_seconds`, and
   `getzebracompatinfo.readiness`.
4. zebra-compat should return to `ready` after it verifies Zebra identity and catches
   the local tip up to Zebra's best tip.

For Zebra endpoint changes:

1. Stop `zcashd`.
2. Update `-zebra-compat-url` and its credentials.
3. Restart `zcashd -zebra-compat`.
4. Confirm `getzebracompatinfo.zebra.identity_verified` is true.
5. Confirm the trusted boundary in `getzebracompatinfo.trusted_boundary` is either
   inactive or matches the configured endpoint, network, and genesis.

If the trusted boundary does not match, zebra-compat must not silently apply the old
trusted-source decision to a different source. Reconfirm the local data
against the intended Zebra source or rebuild using the configured trusted
source policy.

For hard sync faults:

1. Stop zebra-compat block ingestion by stopping `zcashd`.
2. Preserve `debug.log`, `getzebracompatinfo`, and the local datadir for diagnosis.
3. Confirm Zebra is on the expected network and best chain.
4. If Zebra served a block that failed retained local checks, do not skip ahead
   locally. Use a different Zebra source or investigate the local/Zebra
   mismatch before restarting.
5. If local block files or chainstate are corrupt, recover from backup or run
   the documented reindex strategy for the selected trusted-source policy.

For reindex or reindex-chainstate:

zebra-compat uses a trusted boundary tied to network, genesis, and Zebra endpoint
identity. Reindex workflows must preserve or intentionally re-establish that
boundary; otherwise blocks are rebuilt with full local validation or require
live Zebra reconfirmation according to the configured recovery policy. Always
check `getzebracompatinfo.trusted_boundary` after recovery.

## Compatibility Expectations

Wallet behavior, local block files, chainstate, optional indexes, ZMQ
notifications, and local RPC response semantics continue to come from
`zcashd`. Zebra replaces only block acquisition, best-chain source data, and
transaction relay. Retained local validation failures are fail-closed.
