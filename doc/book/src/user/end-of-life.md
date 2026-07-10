# End of Life

> ## ⚠️ Upstream `zcashd` is reaching its End of Life
>
> Upstream `zcashd` is **deprecated**.
> **If you do not need the `zcashd` wallet, migrate to [Zebra](https://github.com/ZcashFoundation/zebra) now.**
> If you depend on the `zcashd` wallet, start testing [Zallet](https://zcash.github.io/zallet/)
> migrating your `wallet.dat` as soon as possible. This Valargroup compatibility build's
> **End-of-Support marker** is estimated for **September 4th 2026** at block height
> 3470000. Its automatic halt is disabled because it runs as a P2P sidecar behind Zebra.

## Two key dates: NU6.3 activation and End-of-Support

`zcashd`'s End of Life involves two distinct milestones. Don't conflate them:

- **NU6.3 mainnet activation — estimated July 21st 2026.**
- **Valargroup compatibility build End-of-Support — estimated September 4th 2026, at
  block height 3470000.** This is hard-coded as the deprecation height
  (`DEPRECATION_HEIGHT` in `src/deprecation.h`). The date is an estimate that may shift
  with network solution power. Query the exact height with the `getdeprecationinfo`
  JSON-RPC method. The sidecar warns at this height but does not automatically shut down.
  See [Release Support](release-support.md) for more information.

## Context

Zcash organizations continue to work on the different components that are needed to
specify, develop and deploy the Ironwood shielded pool in the next Network Upgrade,
NU6.3. Upstream `zcashd` maintainers do not support this upgrade. Upstream `zcashd` is
deprecated; this Valargroup compatibility build is maintained separately as a Zebra
sidecar.

The decision to accelerate `zcashd` deprecation is specifically related to the recent
Orchard vulnerability disclosed on June 1st 2026 and the associated risks that the C++
codebase poses for future (or present) AI advancements. Deprecating `zcashd` is a
**mandatory** step to conclude the remediation of the reported Orchard bug.

## Timeline

The current timeline estimates that NU6.3 activation will be integrated into testnet
code around July 2nd 2026, and testnet activation will follow on July 3rd (block height 4134000).
The same will follow for mainnet on later dates (see timeline below).

For the Valargroup compatibility build, the **End-of-Support marker** (block height
3470000) is estimated for September 4th, after NU6.3 mainnet activation. The sidecar's
automatic halt is disabled.

| Date | Event |
| ----- | ----- |
| July 2nd 2026 | Ironwood/NU6.3 Testnet deployment |
| July 3rd 2026 | Ironwood/NU6.3 Testnet Activation (block height 4134000) |
| July 9th 2026 | Ironwood/NU6.3 **Mainnet** deployment |
| ~July 21st 2026 | Ironwood/NU6.3 **Mainnet** Activation |
| ~September 4th 2026 | Valargroup compatibility build **End-of-Support marker** (block height 3470000) |

## The Path Forward: Zebra + Zallet

The Zcash Foundation has developed and maintains the
[Zebra full node](https://github.com/ZcashFoundation/zebra), a Rust implementation of the
Zcash protocol. All `zcashd` users that don't need the `zcashd` wallet should migrate to
Zebra **immediately**.

For those who do depend on the `zcashd` wallet, ZODL developers have released Zallet
Alpha.4, which allows migration of `zcashd`'s `wallet.dat` files into Zallet. You can find
the documentation for Zallet [here](https://zcash.github.io/zallet/). We encourage these
users to start testing Zallet and to send feedback to its maintainers as soon as possible.
