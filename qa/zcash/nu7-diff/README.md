# NU7 differential check

zcashd and Zakura must agree on every NU7 consensus value, or the sidecar would reject
blocks that Zakura accepts. This directory checks that by comparing both nodes' values
of each quantity in [`SPEC.md`](SPEC.md), computed with each node's production code.

- `run.sh` writes zcashd's dump with the `NU7Diff.DISABLED_Dump` gtest and compares it
  with Zakura's. CI runs it with `--fail-on-diff` in the `nu7-differential` job.
- `compare.py` compares two dumps. A quantity that one side reports as `MISSING` (Zakura
  has no public function for `tx_version_allowed`) is listed but is not a difference.
- `zakura.jsonl.gz` is Zakura's dump, sorted and gzipped.

## Provenance of `zakura.jsonl.gz`

Written by Zakura's ignored test `crates/zakura-consensus/tests/nu7_diff_dump.rs`, from
commit fad9745af (branch `adam/nu7-diff-dump-harness`) on top of Zakura `main` at 2af66b616:

```sh
NU7_DIFF_OUT=zakura.jsonl cargo test -p zakura-consensus --test nu7_diff_dump -- --ignored
sort zakura.jsonl | gzip -9n > zakura.jsonl.gz
```

It has 344,118 rows. Regenerate it whenever Zakura's NU7 rules or the spec change, and
update this section.
