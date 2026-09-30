#!/usr/bin/env python3
"""Compare the zcashd and Zakura NU7 differential dumps (see SPEC.md).

Usage: compare.py [--fail-on-diff] ZCASHD_JSONL ZAKURA_JSONL[.gz]

Prints every differing series. Change-point quantities are compared at every height either
side changes; other quantities at the heights both sides emitted. A value that one side
reports as MISSING (no such function) is listed but not counted as a difference.
"""
import bisect
import gzip
import json
import sys
from collections import defaultdict

# Change-point quantities: the value at h is the value of the last record at or below h.
RLE = {
    "nu", "branch_id", "target_spacing", "averaging_window", "halving_index",
    "block_subsidy", "funding_stream_value", "lockbox_value", "miner_subsidy",
    "testnet_min_difficulty_gap_secs",
}


def load(path):
    """Returns {(scenario, quantity, key): {height: value}} from a JSONL file."""
    opener = gzip.open if path.endswith(".gz") else open
    recs = defaultdict(dict)
    with opener(path, "rt") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if line:
                r = json.loads(line)
                recs[(r["scenario"], r["quantity"], r.get("key", ""))][r.get("height")] = str(r["value"])
    return recs


def value_at(points, heights, h):
    """The change-point value in effect at height h."""
    i = bisect.bisect_right(heights, h) - 1
    return points[heights[i]] if i >= 0 else None


def compare(a, b):
    """Returns (differences, notes) between the zcashd dump a and the Zakura dump b."""
    diffs, notes = [], []
    for k in sorted(set(a) ^ set(b)):
        notes.append(f"only in {'zcashd' if k in a else 'zakura'}: {k}")
    for k in sorted(set(a) & set(b)):
        scenario, quantity, key = k
        va, vb = a[k], b[k]
        if quantity in RLE and None not in va and None not in vb:
            ha, hb = sorted(va), sorted(vb)
            lo = max(ha[0], hb[0])
            pairs = [(h, value_at(va, ha, h), value_at(vb, hb, h)) for h in sorted(set(ha) | set(hb)) if h >= lo]
        else:
            common = sorted(set(va) & set(vb), key=lambda h: (h is None, h))
            pairs = [(h, va[h], vb[h]) for h in common]
        bad = [(h, x, y) for h, x, y in pairs if x != y]
        missing = [p for p in bad if p[1].startswith("MISSING") or p[2].startswith("MISSING")]
        bad = [p for p in bad if p not in missing]
        if missing:
            notes.append(f"MISSING on one side: {scenario} {quantity} [{key}] at {len(missing)} point(s)")
        if bad:
            first = "; ".join(f"h={h}: zcashd={x} zakura={y}" for h, x, y in bad[:6])
            diffs.append(f"DIFF {scenario} {quantity} [{key}] at {len(bad)}/{len(pairs)} point(s); first: {first}")
    return diffs, notes


def main():
    args = sys.argv[1:]
    fail_on_diff = "--fail-on-diff" in args
    args = [x for x in args if x != "--fail-on-diff"]
    if len(args) != 2:
        sys.exit(__doc__)
    a, b = load(args[0]), load(args[1])
    diffs, notes = compare(a, b)
    for line in notes + diffs:
        print(line)
    series = len(set(a) & set(b))
    print(f"{series} common series, {len(diffs)} differing, {len(notes)} notes")
    if fail_on_diff and (diffs or not series):
        sys.exit(1)


if __name__ == "__main__":
    main()
