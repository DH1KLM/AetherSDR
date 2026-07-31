#!/usr/bin/env python3
"""How does the connect frame depend on each character of the destination?

§3.43 established that the frame is not injective: some distinct callsigns give
byte-identical frames, the invariance is linear (it holds at every base point
tried) and it is anchored to the last three characters rather than to the start
of the string. What it did not establish is the weighting — how much each
character position contributes, and modulo what.

METHOD. Hold a callsign fixed and sweep ONE character through all 36 values,
capturing a frame for each. Then:

  * Distinct frames within a sweep says whether that position is carried in full.
    36 distinct means the map restricted to that character is injective; fewer
    means characters are being folded together, and exactly which ones fold is
    the structure.
  * A frame appearing in TWO sweeps is a relation between the weights of two
    positions: if changing character p by d gives the same frame as changing
    character q by e, then w_p*d and w_q*e cancel. Enough such relations pin the
    weights down.

Frames are compared for exact equality across all 32 symbols. That is a hard
test, not a threshold, so a coincidence cannot be manufactured by a tolerance.

    tools/vara_connect_scan.py --data DIR --base KK7GWY --positions 5,4,3
"""

import argparse
import os
import string
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_callsign_analyse as A  # noqa: E402

ALPHABET = string.ascii_uppercase + string.digits


def load(data, call):
    path = os.path.join(data, f"cs_{call}.wav")
    if not os.path.exists(path):
        return None
    body = A.connect_body(path)
    return tuple(body) if body else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--base", default="KK7GWY")
    ap.add_argument("--positions", default="5,4,3")
    a = ap.parse_args()

    positions = [int(p) for p in a.positions.split(",")]
    sweeps = {}
    for p in positions:
        got = {}
        for ch in ALPHABET:
            call = a.base[:p] + ch + a.base[p + 1:]
            f = load(a.data, call)
            if f is not None:
                got[ch] = f
        sweeps[p] = got
        print(f"position {p} (base character {a.base[p]!r}): "
              f"{len(got)}/36 captured", flush=True)

    print()
    for p, got in sweeps.items():
        if not got:
            continue
        counts = Counter(got.values())
        folded = {f: sorted(c for c, v in got.items() if v == f)
                  for f, n in counts.items() if n > 1}
        print(f"POSITION {p}: {len(counts)} distinct frames from {len(got)} characters")
        if folded:
            for chars in sorted(folded.values()):
                print(f"    fold: {' = '.join(chars)}")
        else:
            print("    injective — every character gives its own frame")

    # A frame shared between two sweeps relates the weights of those positions.
    print("\nCROSS-SWEEP COINCIDENCES (a frame reachable by changing either of")
    print("two different characters — each one is a relation between weights):")
    index = defaultdict(list)
    for p, got in sweeps.items():
        for ch, f in got.items():
            index[f].append((p, ch))
    found = 0
    for f, where in index.items():
        ps = {p for p, _ in where}
        if len(ps) > 1:
            found += 1
            desc = ", ".join(f"pos {p} -> {ch!r}" for p, ch in sorted(where))
            print(f"    {desc}")
    if not found:
        print("    none — the sweeps reach disjoint sets of frames, so no")
        print("    single-character change at one position is equivalent to one")
        print("    at another. Any relation involves at least two positions at")
        print("    once, which is what the known three-character collision is.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
