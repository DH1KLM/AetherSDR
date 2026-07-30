#!/usr/bin/env python3
"""Pin the scrambler offsets with every multi-bit capture, then validate.

Pair identities only constrain positions their two bits actually move, leaving
blind spots where the offset is arbitrary. The encoder tolerates that (405/407);
a linear solve does not, which is why 7 of 8 multi-bit captures failed to decode.
Multi-bit captures constrain every position at once.

Held-out discipline: fit on all but four captures, then report both encode and
decode accuracy on the four the fit never saw.
"""
import json
import os
import sys
from collections import Counter

import numpy as np

S = "/tmp/claude-1000/-home-jeremy-AetherSDR/dba67c7c-7678-4762-97b3-32d25143fd29/scratchpad"
sys.path.insert(0, os.path.join(S, "wt-varac", "tools"))
import vara_codec as vc  # noqa: E402

PAIR_FILES = {
    "z": "probe_64.wav", "e0": "fec_c1.wav", "e1": "fec_c2.wav",
    "e2": "fec_b2.wav", "e3": "fec_b3.wav", "e32": "v_b32.wav",
    "e255": "v_b255.wav", "e01": "fec_b01.wav", "e02": "d3_b02.wav",
    "e03": "t2_b03.wav", "e12": "d3_b12.wav", "e13": "t2_b13.wav",
    "e23": "fec_b23.wav", "e0_32": "v_b0_32.wav", "e2_32": "p_2_32.wav",
    "e3_32": "p_3_32.wav", "e0_255": "v_b0_255.wav", "e1_255": "p_1_255.wav",
    "e2_255": "p_2_255.wav", "e3_255": "p_3_255.wav", "e32_255": "p_32_255.wav",
}
IDENTITIES = [("e0", "e1", "e01"), ("e0", "e2", "e02"), ("e0", "e3", "e03"),
              ("e1", "e2", "e12"), ("e1", "e3", "e13"), ("e2", "e3", "e23"),
              ("e0", "e32", "e0_32"), ("e2", "e32", "e2_32"),
              ("e3", "e32", "e3_32"), ("e0", "e255", "e0_255"),
              ("e1", "e255", "e1_255"), ("e2", "e255", "e2_255"),
              ("e3", "e255", "e3_255"), ("e32", "e255", "e32_255")]
RP = {"rp_1": [3, 11, 19, 27, 35, 43, 51, 59, 67],
      "rp_2": [1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41],
      "rp_3": [0, 7, 14, 21, 28, 35, 42, 49, 56, 63],
      "rp_4": [2, 6, 10, 22, 34, 46, 58, 66, 68, 69],
      "rp_5": [4, 8, 12, 16, 20, 24, 44, 52, 60, 64, 65],
      "rp_6": [15, 23, 31, 39, 47, 55, 61, 62, 63],
      "rp_7": [18, 26, 30, 38, 45, 50, 53, 57, 69],
      "rp_8": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]}


def load(name):
    """Read a capture if it exists and is complete. A capture still being
    written, or not yet started, is skipped rather than crashing the run."""
    path = os.path.join(S, name)
    if not os.path.exists(path):
        return None
    if os.path.getsize(path) < 1_000_000:
        return None
    try:
        return vc.symbols_of(path)
    except Exception:
        return None


def main():
    caps = {}
    for k, fn in PAIR_FILES.items():
        v = load(fn)
        if v is not None:
            caps[k] = v
    ids = [t for t in IDENTITIES if all(k in caps for k in t)]
    sets = vc.offset_sets(caps, ids)
    base, raw = vc.load_sweep(os.path.join(S, "fec_sweep"), 32)
    print(f"generator rows: {len(raw)}   pair identities: {len(ids)}")
    print(f"offset sets from identities alone: "
          f"{sorted(Counter(len(x) for x in sets).items())}")

    obs = []
    for k, bb in RP.items():
        t = load(f"{k}.wav")
        if t is not None:
            obs.append((k, bb, t))
    spec_path = os.path.join(S, "mb_spec.json")
    if os.path.exists(spec_path):
        spec = json.load(open(spec_path))
        for k, bb in spec.items():
            t = load(f"{k}.wav")
            if t is not None:
                obs.append((k, bb, t))
    print(f"multi-bit captures usable: {len(obs)}")
    if len(obs) < 6:
        print("too few to fit and hold out")
        return 1

    held = obs[-4:]
    fit = obs[:-4]
    narrowed = vc.constrain_with_multibit(sets, base, raw,
                                          [(b, t) for _, b, t in fit])
    print(f"after fitting on {len(fit)}: "
          f"{sorted(Counter(len(x) for x in narrowed).items())}")
    amb = sum(1 for x in narrowed if len(x) > 2)
    print(f"positions still ambiguous beyond the irreducible {{r, r+8}}: {amb}/407")

    r = vc.choose_offsets(narrowed)
    rows = {i: ((v - r) % 16) ^ ((base - r) % 16) for i, v in raw.items()}
    codec = vc.Codec(r, base, rows)
    np.save(os.path.join(S, "r_final.npy"), r)

    print(f"\nHELD-OUT ({len(held)} captures the fit never saw):")
    print(f"{'capture':10} {'bits':>5} {'encode':>10} {'decode':>9} {'residual':>9}")
    for k, bb, t in held:
        enc = int((codec.encode(bb) == t).sum())
        got, res = codec.decode(t)
        ok = "CORRECT" if got == sorted(bb) else f"wrong({len(got)})"
        print(f"{k:10} {len(bb):>5} {str(enc)+'/407':>10} {ok:>9} {res:>9}")

    print(f"\nFITTED captures (sanity — should be exact):")
    for k, bb, t in fit[:5]:
        enc = int((codec.encode(bb) == t).sum())
        got, res = codec.decode(t)
        print(f"  {k:10} encode {enc}/407  decode "
              f"{'CORRECT' if got == sorted(bb) else 'wrong'}  residual {res}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
