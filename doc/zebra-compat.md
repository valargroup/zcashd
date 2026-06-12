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
-zebra-compat-allow-remote-http=<0|1>
-zebra-compat-rpc-user=<user>
-zebra-compat-rpc-password=<password>
-zebra-compat-cookiefile=<path>
-zebra-compat-no-auth=<0|1>
-zebra-compat-tls-ca-file=<path>
-zebra-compat-poll-interval=<seconds>
-zebra-compat-sync-batch-size=<blocks>
-zebra-compat-sync-response-budget-mb=<MiB>
-zebra-compat-timeout=<seconds>
-zebra-compat-zebra-rpc-max-response-body-bytes=<bytes>
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
When `-zebra-compat` is active, `zcashd` also force-disables `-listen=0`,
`-dnsseed=0`, and `-listenonion=0` in memory, even if a legacy `zcash.conf`
still contains `listen=1` or `p2p=1`. Those values remain on disk but are not
used. Options such as `bind=`, `connect=`, and `addnode=` are not overridden;
startup fails with a clear validation error instead.

When Zebra supervises `zcashd`, it also passes `-p2p=0` and `-listen=0` on the
command line before `zcashd_extra_args`. CLI arguments win over `zcash.conf`.

If you cannot share the cookie file, use static credentials against a loopback
endpoint, for example over an SSH or VPN tunnel:

```sh
ssh -L 8232:127.0.0.1:8232 zebra-host
./src/zcashd -zebra-compat \
  -zebra-compat-url=http://127.0.0.1:8232 \
  -zebra-compat-rpc-user=<user> -zebra-compat-rpc-password=<password>
```

By default, zebra-compat refuses non-loopback `http://` Zebra RPC endpoints
because Basic authentication credentials are sent in cleartext. If an operator
intentionally uses a remote plain-HTTP endpoint, startup requires
`-zebra-compat-allow-remote-http=1`. Treat this as a dangerous escape hatch:
bind Zebra RPC to a private interface, restrict it with a host firewall, or
prefer a tunnel. See **Deployment Topology** below.

For split-host deployments, prefer an `https://` endpoint. This can be Zebra's
TLS-enabled zcashd-compat listener or a TLS-terminating proxy or tunnel in front
of Zebra. If the endpoint uses a certificate from an internal or private CA, pass
that CA certificate to zcashd:

```sh
./src/zcashd -zebra-compat \
  -zebra-compat-url=https://zebra.example.internal:28232 \
  -zebra-compat-cookiefile=/path/to/zebra/.zcashd-compat.cookie \
  -zebra-compat-tls-ca-file=/path/to/internal-ca.pem
```

`-zebra-compat-no-auth=1` disables the Zebra RPC `Authorization` header, but it
is accepted only with `https://` endpoints. Use it only when access control is
provided by another layer such as Cloudflare Access, mTLS, IP allowlists, or a
private network. Cookie or static Basic authentication remains the default and
recommended mode.

Prefer an IP literal in `-zebra-compat-url` where possible. With a DNS
hostname, every Zebra RPC connection — including each forwarded
`sendrawtransaction` — resolves the name again, so a DNS outage fails user
transaction submission even when the Zebra endpoint itself is healthy. (The
plain-HTTP loopback policy decision is cached after the first successful
resolution, but per-connection resolution still applies to the transport.)

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

and a consolidated `zcashd-zebra-compat-manifest-<tag>.json` that includes
artifact URLs and SHA256 values for that target.

## Trust Model

zebra-compat trusts the configured Zebra node for data availability, peer selection,
best-chain discovery, transaction relay, and expensive cryptographic block
verification. zebra-compat does not blindly trust Zebra for local state construction:
Zebra-sourced blocks are still decoded, written to normal local block files,
accepted through the block index, and connected through the local state
transition machinery that maintains chainstate, wallet state, indexes, and ZMQ
notifications.

Operators should run Zebra locally, behind an authenticated HTTPS proxy/tunnel,
or on a private authenticated network. Cookie or static Basic authentication
remains the default. If `-zebra-compat-no-auth=1` is used, the endpoint must be
HTTPS and protected by external access controls. Public unauthenticated Zebra
endpoints must not be used for production zebra-compat mode.

## Configuration

### `zcash.conf` requirements

`zcashd` requires a configuration file in its datadir before startup. The file
can be minimal:

```conf
i-am-aware-zcashd-will-be-replaced-by-zebrad-and-zallet-in-2025=1
```

Zebra supervised deployments create this minimal `zcash.conf` atomically when
the datadir has no config. Existing operator configs are never overwritten; for
migrated datadirs, remove legacy P2P peer options before the first supervised
start.

### P2P flags in compat mode

These flags are P2P-only. None are needed for zebra-compat operation:

| Flag | Compat behavior |
|---|---|
| `-p2p=0` | Master switch: no `StartNode()`, blocks come from Zebra RPC |
| `-listen=0` | No inbound peer connections or network P2P port bind |
| `-dnsseed=0` | No DNS peer discovery |
| `-listenonion=0` | No Tor hidden service for inbound P2P |

With `-zebra-compat`, `zcashd` forces all four off regardless of `zcash.conf`.
Supervised starts add `-p2p=0` and `-listen=0` on the CLI as defense in depth.

Peer-directing options (`bind=`, `whitebind=`, `connect=`, `addnode=`,
`seednode=`, etc.) are not silently ignored; remove them from `zcash.conf` or
startup fails.

`-dns` (general hostname resolution) is separate from `-dnsseed` and is not
forced off. Wallet RPC (`-rpcbind`, `-rpcport`) is unrelated to `-listen`.

### Migration rollout

For existing zcashd datadirs, keep the datadir in place and migrate the config
instead of starting from a fresh wallet directory. Remove legacy P2P
peer-directing options first, then do a staged rollout by starting with
`-blocksource=zebra -p2p=0 -blockvalidation=full` if you want local full block
validation before switching to the `-zebra-compat` trusted-validation preset.

### Sync batch size, response budget, and reorg depth

Three settings must agree when increasing zebra-compat sync batch size:

- `-zebra-compat-sync-batch-size=<blocks>`: how many raw blocks zcashd asks
  Zebra for in one JSON-RPC batch. It defaults to `30`.
- `-zebra-compat-sync-response-budget-mb=<MiB>`: zcashd's memory budget for one
  batched raw-block response. It defaults to `128` MiB and bounds the effective
  sync batch size.
- `-zebra-compat-timeout=<seconds>`: how long zcashd waits for each Zebra RPC
  response. It defaults to `30` seconds.
- Zebra `rpc.max_response_body_size`: Zebra's own HTTP response-body limit. It
  must be large enough for the same batch response.

zcashd computes its memory-clamped maximum batch size as:

```text
effective max = floor((budget - 1 MiB) / (2 * MAX_BLOCK_SIZE + 1024))
```

With the default budget, the memory-clamped maximum is `33`. If the configured
batch size exceeds that maximum, startup fails with a memory-budget validation
error and reports the largest usable value.

The batch size bounds each Zebra RPC acquisition request, not the total
replacement branch length for a reorg. When Zebra's best chain reorgs, zcashd
fetches the replacement branch in bounded batches and then ingests the complete
branch once. Followable reorg depth is still capped by zcashd's absolute
`MAX_REORG_LENGTH` of `99` disconnected blocks.

When `-zebra-compat-zebra-rpc-max-response-body-bytes=<bytes>` is set, zcashd
also validates that Zebra's configured `rpc.max_response_body_size` can carry
the effective sync batch. Zebra sets this flag automatically when it supervises
zcashd. If zcashd is managed externally, set it explicitly to get the same
fail-early validation.

On split-host links, large valid batch responses can also exceed the default
timeout even when the response-size limits are high enough. Raise
`-zebra-compat-timeout` together with the batch and response-budget settings if
large ingest responses are timing out before Zebra finishes sending them.

For an externally managed 80-block batch, raise zcashd's response budget and
pass Zebra's configured response limit to zcashd:

```sh
./src/zcashd -zebra-compat \
  -zebra-compat-sync-batch-size=80 \
  -zebra-compat-sync-response-budget-mb=320 \
  -zebra-compat-timeout=120 \
  -zebra-compat-zebra-rpc-max-response-body-bytes=335544320 \
  -zebra-compat-url=http://127.0.0.1:8232 \
  -zebra-compat-cookiefile=/path/to/zebra/.cookie
```

Configure Zebra with the matching response-body limit:

```toml
[rpc]
max_response_body_size = 335544320
```

If Zebra supervises zcashd, put only the zcashd batch and response-budget
settings in Zebra's `zcashd_extra_args`; Zebra passes
`-zebra-compat-zebra-rpc-max-response-body-bytes=<effective limit>`
automatically.

### Three different endpoints (supervised deployments)

| Endpoint | Typical address | Purpose |
|---|---|---|
| Zebra user RPC | `127.0.0.1:8232` | Operator JSON-RPC; externally managed `zcashd` can point here |
| Zebra compat RPC | `127.0.0.1:28232` | Backend channel from supervised `zcashd` to Zebra; cookie auth by default, HTTPS optional |
| Zebra network P2P | `0.0.0.0:8233` / `:18233` | Zebra syncs from the Zcash network |
| zcashd network P2P | disabled | Must not bind 8233/18233 in compat mode |

For externally managed `zcashd`, point `-zebra-compat-url` at the Zebra RPC
listener intended for that process. For supervised deployments, Zebra passes the
dedicated compat RPC listener automatically.

When Zebra supervises zcashd, it points `-zebra-compat-url` at the dedicated
compat RPC listener and passes `-zebra-compat-cookiefile` for that listener's
cookie authentication by default. The supervisor builds an `http://` URL unless
the dedicated compat listener has TLS enabled; with TLS enabled, it builds an
`https://` URL and passes `-zebra-compat-tls-ca-file=<tls_ca_file>` so zcashd can
verify Zebra's server certificate.

For supervised loopback deployments, enabling or disabling TLS on the same
`127.0.0.1` or `localhost` compat listener does not invalidate zcashd's trusted
boundary. The persisted boundary still must match the configured host, port,
path, network, and genesis. For remote endpoints, changing between `http://` and
`https://` is treated as a source change and requires the normal endpoint-change
recovery checks.

The dedicated compat listener is controlled by Zebra's `[zcashd_compat]`
settings:

```toml
[zcashd_compat]
listen_addr = "127.0.0.1:28232"
enable_cookie_auth = true
tls_cert_file = "/path/to/zebra.crt"
tls_key_file = "/path/to/zebra.key"
tls_ca_file = "/path/to/internal-ca.pem"
```

`tls_cert_file` and `tls_key_file` must be configured together. Supervised
zcashd-compat with TLS also requires `tls_ca_file`, which is the CA certificate
zcashd uses to verify Zebra, not Zebra's private key. `enable_cookie_auth`
defaults to `true`; setting it to `false` makes supervised zcashd receive
`-zebra-compat-no-auth=1` instead of a cookie file and is allowed only when TLS
is enabled. Use no-auth mode only when another layer controls access, such as
Cloudflare Access, mTLS, IP allowlists, or a private network.

The server certificate must include an IP Subject Alternative Name for the
listener IP (for example `IP:127.0.0.1`). Supervised zcashd connects to the
raw `listen_addr` IP and verifies the certificate against that IP, so a
certificate carrying only DNS names fails hostname verification. The same
applies to externally managed zcashd when `-zebra-compat-url` uses an IP
literal; URLs with DNS hostnames are verified against the hostname instead.

### Validate P2P is disabled

```sh
./src/zcash-cli getzebracompatinfo   # expect "p2p": false, "blocksource": "zebra"
./src/zcash-cli getconnectioncount   # expect 0
./src/zcash-cli getpeerinfo          # expect []
```

Confirm `zcashd` is not listening on the network P2P port:

```sh
ss -ltnp 'sport = :18233'   # testnet
ss -ltnp 'sport = :8233'    # mainnet
```

## Deployment Topology

Recommended single-host topology:

```text
Zebra JSON-RPC 127.0.0.1:8232    <---authenticated HTTP/HTTPS---  external zcashd -zebra-compat
Zebra compat RPC 127.0.0.1:28232 <---cookie auth; optional HTTPS---  supervised zcashd
Zebra P2P enabled                                      zcashd P2P disabled
```

Recommended split-host topology:

```text
Zebra host on private network  <---authenticated HTTPS or private tunnel---  zcashd -zebra-compat host
```

Do not expose the Zebra JSON-RPC endpoint as a public unauthenticated backend
API. For authenticated HTTPS split-host deployments, use Zebra's TLS-enabled
zcashd-compat listener or terminate TLS in a proxy or tunnel in front of Zebra
and point zcashd at that `https://` endpoint. TLS protects the channel and
authenticates the endpoint to zcashd; cookie auth or an external access-control
layer still decides which clients may call the endpoint.

## Readiness And Diagnostics

Use `getzebracompatinfo` as the primary monitoring surface. The top-level
`readiness` value is:

- `ready`: Zebra identity is verified, the local tip matches Zebra's best tip,
  the local tip has left IBD, validation notifications have caught up, Zebra
  transaction forwarding has no transport failure, and the Zebra mempool mirror
  has a fresh successful poll with no unexplained lag or current error.
- `degraded`: zebra-compat can keep running but is waiting, syncing, retrying,
  persistently lagging, waiting for notification catch-up, has a stale or
  failed mempool mirror, or has non-fatal mempool/forwarding diagnostics.
- `failed`: zebra-compat has hit a hard sync or identity fault and will not advance
  past that fault without operator action.
- `disabled`: zebra-compat mode is not enabled.

After the node has reached `ready`, transient non-failed conditions are held
through a short hysteresis window of roughly two poll intervals before the
top-level value changes to `degraded`. This keeps exchange-facing alerts stable
across ordinary block-ingest and mempool-poll timing windows. Hard `failed`
states are still reported immediately.

The `sync` object carries the current detailed state, last error, local lag,
and retry/backoff counters. The `metrics` object repeats the script-friendly
counters most useful to dashboards, including mempool freshness, forwarding
transport health, and validation-notification catch-up. Ordinary Zebra
rejections of invalid user transactions remain visible in
`tx_forwarding.last_error`, but they do not make the node degraded; transport
and connectivity failures are reported separately as
`tx_forwarding.last_transport_error`. Mempool divergence is reported through
`mempool_mirror.divergent` and `metrics.mempool_divergent` as a diagnostic
rather than a hard readiness gate, because local and Zebra policy can differ
benignly. Recently forwarded transactions that are retained locally while
waiting for Zebra's next mempool observation are reported as pending and are
treated as explained rather than lag. The `limits` object exposes hard bounds
such as `sync_batch_size`, `zebra_rpc_max_response_body_bytes`, pending
forwarded transactions, and per-poll mempool reconciliation limits.

Existing network RPCs remain scriptable: zebra-compat reports no local Zcash peers and
P2P-control RPCs are unavailable while `-p2p=0`.

## Recovery Procedures

For transient Zebra outages:

1. Keep `zcashd -zebra-compat` running.
2. Restore Zebra availability or authentication. During first boot with a
   co-started Zebra, zcashd also stays in this retry path until Zebra has
   committed genesis and can answer identity RPCs such as `getblockhash(0)`.
   The `sync.detail` value distinguishes the two cases: `zebra_unreachable`
   means the endpoint did not answer, while `zebra_rpc_error_retry` means
   Zebra answered but returned a JSON-RPC error (normal during first boot).
3. Watch `getzebracompatinfo.sync.retry_count`,
   `getzebracompatinfo.sync.current_backoff_seconds`, and
   `getzebracompatinfo.readiness`.
4. zebra-compat should return to `ready` after it verifies Zebra identity and catches
   the local tip up to Zebra's best tip.

For Zebra endpoint changes or static credential flag changes:

1. Stop `zcashd`.
2. Update `-zebra-compat-url` or `-zebra-compat-rpc-user` /
   `-zebra-compat-rpc-password`.
3. Restart `zcashd -zebra-compat`.
4. Confirm `getzebracompatinfo.zebra.identity_verified` is true.
5. Confirm the trusted boundary in `getzebracompatinfo.trusted_boundary` is either
   inactive or matches the configured endpoint, network, and genesis.

When using `-zebra-compat-cookiefile`, zcashd rereads the cookie during ingest
polling. Zebra restarts that regenerate the cookie file should recover through
normal retry/backoff without restarting zcashd, as long as the cookie file path
and endpoint URL stay the same.

Changing only the URL scheme between `http://` and `https://` on the same
loopback supervised compat listener is not considered an endpoint change for the
trusted boundary. Port, host, path, network, genesis, and all remote endpoint
changes remain strict boundary checks.

Alert if `getzebracompatinfo.sync.detail` remains
`zebra_authentication_retry` for more than a few minutes. That state is
intentionally retryable for cookie rotation, but a persistently wrong cookie path
or file contents needs operator action.

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
