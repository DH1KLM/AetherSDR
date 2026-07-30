#!/usr/bin/env python3
"""Recover the STRUCTURE of VARA's code, not just its generator matrix.

A generator matrix is a lookup table for one block size. The structure — which
output positions are systematic, what the interleaver is, what the constituent
encoders are — is the algorithm, and it generalises to every block size and
frame type. This tool goes after the structure.

WHAT IT WORKS ON. §3.27 showed the transmitted tone is `s = (d + r) mod 16` with
`r` a payload-independent offset. Removing `r` leaves `d`, which is linear in
the payload in ALL FOUR bit planes, so a row is 407*4 = 1628 bits rather than the
814 available before the scrambler was understood.

    row_i = d(e_i) XOR d(0)

and G is those rows stacked. Everything below is a statement about G.

THE FIRST QUESTION IS WHETHER THE CODE IS SYSTEMATIC. Most turbo codes are: the
payload appears verbatim in the output, usually permuted, alongside parity. A
systematic output position depends on exactly ONE input bit, so its column in G
has weight 1. Finding those columns gives the systematic permutation directly,
which is the interleaver or its inverse depending on convention.

A caution that applies throughout: the offset `r` is only determined up to a set
of candidates per position (§3.28), and the rows depend on which member is
chosen. A wrong choice corrupts that position's bits and can mask structure. The
tool reports how constrained each position is so that a structural claim resting
on loosely-constrained positions can be discounted.
"""

import argparse
import os
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_codec as vc  # noqa: E402

SYMBOLS = 407


def nibbles_to_bits(sym):
    """407 tone values -> 1628 bits, most significant bit of each symbol first."""
    s = np.asarray(sym, dtype=int)
    return ((s[:, None] >> np.arange(3, -1, -1)) & 1).ravel().astype(np.uint8)


def build(scratch, length=32):
    names = {"z": "probe_64.wav", "e0": "fec_c1.wav", "e1": "fec_c2.wav",
             "e2": "fec_b2.wav", "e3": "fec_b3.wav", "e32": "v_b32.wav",
             "e255": "v_b255.wav", "e01": "fec_b01.wav", "e02": "d3_b02.wav",
             "e03": "t2_b03.wav", "e12": "d3_b12.wav", "e13": "t2_b13.wav",
             "e23": "fec_b23.wav", "e0_32": "v_b0_32.wav",
             "e2_32": "p_2_32.wav", "e3_32": "p_3_32.wav",
             "e0_255": "v_b0_255.wav", "e1_255": "p_1_255.wav",
             "e2_255": "p_2_255.wav", "e3_255": "p_3_255.wav",
             "e32_255": "p_32_255.wav"}
    caps = {}
    for k, fn in names.items():
        v = vc.symbols_of(os.path.join(scratch, fn))
        if v is not None:
            caps[k] = v
    ids = [t for t in [("e0", "e1", "e01"), ("e0", "e2", "e02"),
                       ("e0", "e3", "e03"), ("e1", "e2", "e12"),
                       ("e1", "e3", "e13"), ("e2", "e3", "e23"),
                       ("e0", "e32", "e0_32"), ("e2", "e32", "e2_32"),
                       ("e3", "e32", "e3_32"), ("e0", "e255", "e0_255"),
                       ("e1", "e255", "e1_255"), ("e2", "e255", "e2_255"),
                       ("e3", "e255", "e3_255"), ("e32", "e255", "e32_255")]
           if all(k in caps for k in t)]
    sets = vc.offset_sets(caps, ids)
    r = vc.choose_offsets(sets)
    base, raw = vc.load_sweep(os.path.join(scratch, "fec_sweep"), length)
    if base is None:
        raise SystemExit("no sweep baseline")
    d0 = (base - r) % 16
    bits = sorted(raw)
    G = np.array([nibbles_to_bits(((raw[i] - r) % 16) ^ d0) for i in bits],
                 dtype=np.uint8)
    return G, np.array(bits), np.array([len(s) for s in sets]), r


def gf2_rank(A):
    A = A.copy() % 2
    rows, cols = A.shape
    rk = 0
    for c in range(cols):
        piv = None
        for i in range(rk, rows):
            if A[i, c]:
                piv = i
                break
        if piv is None:
            continue
        A[[rk, piv]] = A[[piv, rk]]
        for i in range(rows):
            if i != rk and A[i, c]:
                A[i] ^= A[rk]
        rk += 1
        if rk == rows:
            break
    return rk


def report(G, bits, setsizes, top=40):
    n, m = G.shape
    print(f"G is {n} rows x {m} bits ({SYMBOLS} symbols x 4 planes)")
    print(f"payload bits present: {bits.min()}..{bits.max()} "
          f"({'contiguous' if len(bits) == bits.max() - bits.min() + 1 else 'gaps'})")
    print(f"rank: {gf2_rank(G)}/{n}")
    w = G.sum(1)
    print(f"row weights: min {w.min()} max {w.max()} mean {w.mean():.1f} "
          f"({100 * w.mean() / m:.1f} %)")

    colw = G.sum(0)
    print(f"\ncolumn weights over {n} rows: "
          f"{sorted(Counter(colw.tolist()).items())[:12]}{' ...' if len(set(colw.tolist())) > 12 else ''}")
    zero = int((colw == 0).sum())
    one = int((colw == 1).sum())
    print(f"  columns untouched by any row : {zero}/{m}")
    print(f"  columns of weight EXACTLY 1  : {one}/{m}   <-- systematic candidates")

    # A weight-1 column is affected by exactly one payload bit. Under a
    # systematic code these are the payload's own bits appearing in the output.
    if one:
        sysmap = {}
        for c in np.flatnonzero(colw == 1):
            i = int(np.flatnonzero(G[:, c])[0])
            sysmap.setdefault(int(bits[i]), []).append(int(c))
        multi = {k: v for k, v in sysmap.items() if len(v) > 1}
        print(f"  payload bits with >=1 systematic column: {len(sysmap)}/{n}")
        print(f"  payload bits with more than one        : {len(multi)}")
        ordered = sorted(sysmap.items())[:top]
        print(f"\n  payload bit -> weight-1 column(s), first {len(ordered)}:")
        for k, v in ordered:
            sym = [c // 4 for c in v]
            plane = [c % 4 for c in v]
            print(f"    bit {k:3d} -> cols {v}  (symbols {sym}, planes {plane}, "
                  f"offset sets {[int(setsizes[s]) for s in sym]})")
        # Is the mapping regular?
        singles = {k: v[0] for k, v in sysmap.items() if len(v) == 1}
        if len(singles) >= 4:
            ks = sorted(singles)
            d = np.diff([singles[k] for k in ks])
            print(f"\n  consecutive-column differences for singly-mapped bits: "
                  f"{d[:24].tolist()}")
            print(f"  distinct differences: {sorted(Counter(d.tolist()).items())[:8]}")
    else:
        print("\n  No weight-1 columns. Either the code is non-systematic, or too")
        print("  few rows are present for a column to be isolated yet — with only")
        print(f"  {n} of the payload's bits captured, a column touched solely by an")
        print("  uncaptured bit currently reads as weight 0, not weight 1.")

    # Where does each row's support sit? Systematic + two parity streams would
    # show up as distinct clusters.
    print("\nper-plane column-weight profile (is one plane special?):")
    for p in range(4):
        cw = colw[p::4]
        print(f"  plane {p}: mean {cw.mean():5.2f}  zeros {int((cw==0).sum()):3d}  "
              f"ones {int((cw==1).sum()):3d}  max {cw.max()}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--length", type=int, default=32)
    a = ap.parse_args()
    G, bits, setsizes, r = build(a.scratch, a.length)
    report(G, bits, setsizes)
    np.savez_compressed(os.path.join(a.scratch, "structure_G.npz"),
                        G=G, bits=bits, setsizes=setsizes, r=r)
    print(f"\nsaved G to {os.path.join(a.scratch, 'structure_G.npz')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
