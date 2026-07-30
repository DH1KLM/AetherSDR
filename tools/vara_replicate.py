#!/usr/bin/env python3
"""Re-derive the recorded findings from freshly captured data.

The original captures were destroyed by a reboot (§3.32). Re-capturing them is
not merely recovery: the clean-room note recorded exact numbers for every
finding, so re-deriving those numbers from a NEW set of captures is an
independent replication of the whole chain. If the modem is deterministic across
sessions — which §3.14 and later work claim — these must match exactly, not
approximately.

Anything that fails to reproduce is far more interesting than anything that
passes, and is reported first.

    tools/vara_replicate.py --data /home/jeremy/aethersdr-vara-data
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_codec as vc  # noqa: E402

# Every expectation below is quoted from docs/vara-cleanroom-design.md, so a
# mismatch means either the document is wrong or the bench is not reproducing.
EXPECT = {
    "3.23 row weights (bits 0,1,2 vs Encode(0)), of 1628 bits": [547, 587, 577],
    "3.23 symbols differing for those rows, of 407": [260, 270, 259],
    "3.25 D2 at base 0, per bit plane": [0, 0, 38, 48],
    "3.25 D2 at base e2, per bit plane": [0, 0, 38, 42],
    "3.25 D3 over {e0,e1,e2}, per bit plane": [0, 0, 0, 10],
    "3.26 D2 at base e3, per bit plane": [0, 0, 38, 44],
    "3.26 D3 over {e0,e1,e3}, per bit plane": [0, 0, 0, 6],
}


def load(data, name):
    p = os.path.join(data, name)
    if not os.path.exists(p) or os.path.getsize(p) < 1_000_000:
        return None
    try:
        return vc.symbols_of(p)
    except Exception:
        return None


def planes(sym):
    s = np.asarray(sym, dtype=int)
    return [(s >> b) & 1 for b in (0, 1, 2, 3)]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    a = ap.parse_args()

    need = {"z": "probe_64.wav", "e0": "fec_c1.wav", "e1": "fec_c2.wav",
            "e2": "fec_b2.wav", "e3": "fec_b3.wav", "e01": "fec_b01.wav",
            "e02": "d3_b02.wav", "e12": "d3_b12.wav", "e012": "v_b012.wav",
            "e03": "t2_b03.wav", "e13": "t2_b13.wav", "e013": "t2_b013.wav"}
    C = {}
    missing = []
    for k, fn in need.items():
        v = load(a.data, fn)
        if v is None:
            missing.append(fn)
        else:
            C[k] = v
    if missing:
        print(f"not yet captured: {missing}")
        return 1

    got = {}

    # §3.23 — generator rows against the all-zero baseline, at 64-byte payloads.
    def tobits(sym):
        s = np.asarray(sym, dtype=int)
        return ((s[:, None] >> np.arange(3, -1, -1)) & 1).ravel()
    wts, diffs = [], []
    for k in ("e0", "e1", "e2"):
        wts.append(int((tobits(C[k]) ^ tobits(C["z"])).sum()))
        diffs.append(int((C[k] != C["z"]).sum()))
    got["3.23 row weights (bits 0,1,2 vs Encode(0)), of 1628 bits"] = wts
    got["3.23 symbols differing for those rows, of 407"] = diffs

    def comb(keys, b):
        v = np.zeros(407, dtype=int)
        for k in keys:
            v = v ^ planes(C[k])[b]
        return int(v.sum())
    got["3.25 D2 at base 0, per bit plane"] = [
        comb(["z", "e0", "e1", "e01"], b) for b in range(4)]
    got["3.25 D2 at base e2, per bit plane"] = [
        comb(["e2", "e02", "e12", "e012"], b) for b in range(4)]
    got["3.25 D3 over {e0,e1,e2}, per bit plane"] = [
        comb(["z", "e0", "e1", "e2", "e01", "e02", "e12", "e012"], b)
        for b in range(4)]
    got["3.26 D2 at base e3, per bit plane"] = [
        comb(["e3", "e03", "e13", "e013"], b) for b in range(4)]
    got["3.26 D3 over {e0,e1,e3}, per bit plane"] = [
        comb(["z", "e0", "e1", "e3", "e01", "e03", "e13", "e013"], b)
        for b in range(4)]

    fails = [(k, EXPECT[k], got[k]) for k in EXPECT if got.get(k) != EXPECT[k]]
    print("REPLICATION against docs/vara-cleanroom-design.md\n")
    if fails:
        print(f"  *** {len(fails)} QUANTITY(IES) FAILED TO REPRODUCE ***")
        for k, exp, act in fails:
            print(f"    {k}\n      documented {exp}\n      measured   {act}")
        print()
    for k in EXPECT:
        mark = "ok " if got.get(k) == EXPECT[k] else "FAIL"
        print(f"  [{mark}] {k}\n         documented {EXPECT[k]}  measured {got.get(k)}")
    print(f"\n  {len(EXPECT) - len(fails)}/{len(EXPECT)} quantities reproduced exactly")
    if not fails:
        print("\n  Every documented quantity reproduces from a completely fresh set")
        print("  of captures recorded in new ARQ sessions. This is an independent")
        print("  replication of the scrambler model and the graded-degree law, not")
        print("  merely a restoration of the data.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
