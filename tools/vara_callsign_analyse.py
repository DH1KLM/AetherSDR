#!/usr/bin/env python3
"""Recover how the connect frame encodes a destination callsign.

§3.38 established that the 41-symbol connect frame is a deterministic function of
the destination callsign alone: identical callsigns give a byte-identical frame,
changing the source changes nothing, changing the destination changes about three
quarters of the symbols. This analyses a differential sweep over destinations to
find out HOW.

The first question decides everything that follows:

  * If changing ONE character moves only a few symbols, the field is a positional
    encoding and can be read off directly.
  * If it moves most of them, the field is coded — diffused by something like the
    FEC and scrambler already recovered for DATA frames — and the attack has to
    be algebraic rather than by inspection.

Both outcomes are useful; the point is not to guess which.

    tools/vara_callsign_analyse.py --data DIR
"""

import argparse
import glob
import os
import re
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap  # noqa: E402

CTRL = 2048
BINS = np.arange(29, 99)
PREAMBLE = 9          # acquisition preamble, never varies
BODY = 32             # the frame proper


def ctrl_symbols(seg):
    return [int(np.argmax(np.abs(np.fft.rfft(seg[i * CTRL:(i + 1) * CTRL]))[BINS]))
            for i in range(len(seg) // CTRL)]


def connect_body(path):
    """The 32-symbol body of the connect frame, preamble stripped."""
    try:
        if os.path.getsize(path) < 1_000_000:
            return None
        x = cap.load_wav(path)
    except Exception:
        return None
    for a, b in cap.split_bursts(x):
        n = b - a
        if n % CTRL or n // CTRL != PREAMBLE + BODY:
            continue
        s = ctrl_symbols(x[a:b].astype(float) / 32768.0)
        return s[PREAMBLE:]
    return None


def diff(a, b):
    return sum(1 for x, y in zip(a, b) if x != y)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    a = ap.parse_args()

    frames = {}
    for path in sorted(glob.glob(os.path.join(a.data, "cs_*.wav"))):
        m = re.match(r"cs_(.+)\.wav$", os.path.basename(path))
        if not m:
            continue
        body = connect_body(path)
        if body is not None:
            frames[m.group(1)] = body
    # The probe's own runs double as a control pair.
    for name, call in (("base_a", "KK7GWY-8"), ("base_b", "KK7GWY-8")):
        p = os.path.join(a.data, f"call_{name}.wav")
        if os.path.exists(p):
            b = connect_body(p)
            if b is not None:
                frames[f"@{name}"] = b

    print(f"connect frames recovered: {len(frames)}")
    if len(frames) < 3:
        print("not enough captures yet")
        return 1

    if "@base_a" in frames and "@base_b" in frames:
        d = diff(frames["@base_a"], frames["@base_b"])
        print(f"\nCONTROL — two runs, identical callsign: {d}/32 symbols differ")
        print("  (anything below is measured against this floor)")

    print("\nLENGTH LADDER — destination 'A' through 'AAAAAA':")
    ladder = [c for c in ("A", "AA", "AAA", "AAAA", "AAAAA", "AAAAAA") if c in frames]
    for i in range(len(ladder) - 1):
        x, y = ladder[i], ladder[i + 1]
        print(f"  {x:8} -> {y:8}: {diff(frames[x], frames[y]):2d}/32 differ")

    base = "AAAAAA"
    if base in frames:
        print(f"\nSINGLE-CHARACTER SUBSTITUTION (one position changed A->B):")
        for i, name in enumerate(("BAAAAA", "ABAAAA", "AABAAA",
                                  "AAABAA", "AAAABA", "AAAAAB")):
            if name not in frames:
                continue
            d = diff(frames[base], frames[name])
            where = [p for p in range(BODY)
                     if frames[base][p] != frames[name][p]]
            print(f"  position {i}: {d:2d}/32 differ  at symbols {where[:12]}"
                  f"{' ...' if len(where) > 12 else ''}")

        print(f"\nALPHABET LADDER at position 0:")
        for name in ("BAAAAA", "CAAAAA", "DAAAAA", "PAAAAA",
                     "ZAAAAA", "0AAAAA", "9AAAAA"):
            if name in frames:
                print(f"  {base} -> {name}: {diff(frames[base], frames[name]):2d}/32 differ")

        print(f"\nSSID SUFFIX:")
        for name in ("AAAAAA-1", "AAAAAA-9"):
            if name in frames:
                print(f"  {base} -> {name}: {diff(frames[base], frames[name]):2d}/32 differ")

    # The discriminator.
    singles = [diff(frames[base], frames[n])
               for n in ("BAAAAA", "ABAAAA", "AABAAA", "AAABAA", "AAAABA", "AAAAAB")
               if base in frames and n in frames]
    print("\n" + "=" * 66)
    if singles:
        mean = sum(singles) / len(singles)
        print(f"A one-character change moves {mean:.1f} of 32 symbols on average.")
        if mean < 6:
            print("LOCALISED — the callsign field is a positional encoding and can")
            print("be read off directly. Map each character to the symbols it moves.")
        else:
            print("DIFFUSED — one character reaches most of the frame, so the field")
            print("is coded, as the DATA payload is. Reading it off by inspection")
            print("will not work; the attack has to be algebraic, and the next step")
            print("is to test whether the map is linear in the callsign bits.")
    else:
        print("no single-character pairs available yet")
    print("=" * 66)
    return 0


if __name__ == "__main__":
    sys.exit(main())
