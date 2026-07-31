#!/usr/bin/env python3
"""Is the connect frame a linear function of *anything*?

This is the representation-agnostic test. If the frame is any linear map applied
to any encoding of the callsign, then the frames lie in an affine subspace whose
dimension is at most the number of components that encoding carries. Differences
from a reference frame then span exactly that space — regardless of what the
encoding actually is, which is the point: the previous attempts each assumed a
representation (ASCII bits, base-36 packing) and could only ever refute that one.

WHY MOD 5 AND MOD 7. §3.41 established that every symbol's parity is fixed by its
position, so only 35 of the 70 tones are reachable at each. 35 = 5 x 7, so the
information lives in those two components and a bit representation is the wrong
place to look. Differences taken mod 70 also cancel any per-position additive
offset — the trick that turned out to be hiding the DATA frame's linearity.

WHY SAMPLE COUNT MATTERS. A 32-symbol frame spans at most 32 dimensions, so with
fewer than 33 samples the rank is bounded by the sample count and saturation is
unobservable. A run with too few captures proves nothing and says so rather than
reporting a number that looks like a result.

    tools/vara_connect_rank.py --data DIR
"""

import argparse
import glob
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_callsign_analyse as A  # noqa: E402

SYMBOLS = 32


def rank_gfp(matrix, p):
    """Rank over GF(p) by Gaussian elimination."""
    m = matrix.copy() % p
    rows, cols = m.shape
    r = 0
    for col in range(cols):
        piv = None
        for i in range(r, rows):
            if m[i, col] % p:
                piv = i
                break
        if piv is None:
            continue
        m[[r, piv]] = m[[piv, r]]
        inv = pow(int(m[r, col]), p - 2, p)
        m[r] = (m[r] * inv) % p
        for i in range(rows):
            if i != r and m[i, col] % p:
                m[i] = (m[i] - m[i, col] * m[r]) % p
        r += 1
        if r == rows:
            break
    return r


def load_frames(data):
    frames = {}
    for path in sorted(glob.glob(os.path.join(data, "cs_*.wav"))):
        name = re.match(r"cs_(.+)\.wav$", os.path.basename(path)).group(1)
        body = A.connect_body(path)      # applies the tone-purity check
        if body is not None and len(body) == SYMBOLS:
            frames[name] = body
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    a = ap.parse_args()

    frames = load_frames(a.data)
    names = sorted(frames)
    print(f"connect frames accepted: {len(names)}")
    if len(names) < 34:
        print(f"\nNot enough. A {SYMBOLS}-symbol frame spans up to {SYMBOLS}")
        print("dimensions, so the rank cannot saturate below the sample count")
        print(f"until there are more than {SYMBOLS + 1} samples. Running the test")
        print("now would produce a number that means nothing.")
        return 1

    M = np.array([frames[n] for n in names], dtype=int)
    # Drop positions that never vary. Symbol 0 is the frame-type marker and is
    # always 45, so including it costs one rank and reads as a linear relation —
    # which is exactly how an earlier run of this reported "linear structure"
    # when all it had found was a constant.
    varying = [c for c in range(SYMBOLS) if len(set(M[:, c].tolist())) > 1]
    const = SYMBOLS - len(varying)
    print(f"constant symbol positions excluded: {const} "
          f"(the frame-type marker and any other fixed field)")
    M = M[:, varying]
    D = (M[1:] - M[0]) % 70

    # A control that preserves everything except the structure being claimed:
    # same shape, same per-position parity, same value ranges — but random.
    rng = np.random.default_rng(20260730)
    ctrl = np.zeros_like(M)
    for p_ in range(M.shape[1]):
        parity = M[0, p_] % 2
        allowed = np.array([v for v in range(70) if v % 2 == parity])
        ctrl[:, p_] = rng.choice(allowed, size=M.shape[0])
    Dc = (ctrl[1:] - ctrl[0]) % 70

    print("\nRank as samples accumulate. Structure shows as saturation BELOW the")
    print("sample count; no structure means rank tracks it exactly.\n")
    print(f"{'samples':>8} {'GF(5)':>7} {'GF(7)':>7}   {'ctrl GF(5)':>11} {'ctrl GF(7)':>11}")
    steps = [8, 16, 24, 32, 36, 40, D.shape[0]]
    for k in steps:
        if k > D.shape[0]:
            continue
        print(f"{k:>8} {rank_gfp(D[:k], 5):>7} {rank_gfp(D[:k], 7):>7}"
              f"   {rank_gfp(Dc[:k], 5):>11} {rank_gfp(Dc[:k], 7):>11}")

    r5, r7 = rank_gfp(D, 5), rank_gfp(D, 7)
    c5, c7 = rank_gfp(Dc, 5), rank_gfp(Dc, 7)
    n = D.shape[0]
    print("\n" + "=" * 70)
    print(f"  GF(5): rank {r5} of {min(n, M.shape[1])} possible   (control {c5})")
    print(f"  GF(7): rank {r7} of {min(n, M.shape[1])} possible   (control {c7})")
    found = []
    if r5 < min(n, M.shape[1]) and r5 < c5:
        found.append(("GF(5)", r5))
    if r7 < min(n, M.shape[1]) and r7 < c7:
        found.append(("GF(7)", r7))
    if found:
        for field, r in found:
            print(f"\n  LINEAR STRUCTURE in {field}: the frames lie in a "
                  f"{r}-dimensional affine subspace.")
        print("  That dimension bounds how much the callsign field actually")
        print("  carries, and a generator matrix over that field can be solved")
        print("  for and validated by predicting a held-out callsign.")
    else:
        print("\n  No saturation in either component: every new callsign adds a")
        print("  new dimension, exactly as the control does. The frame is not a")
        print("  linear function of the callsign in either component of the")
        print("  35-value alphabet, with a per-position additive offset removed.")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
