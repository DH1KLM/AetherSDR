#!/usr/bin/env python3
"""Export the recovered VARA level-4 model as a portable file.

Everything learned about the DATA waveform lives in Python analysis scripts and
in a pile of capture files. A C++ implementation cannot consume those. This
writes a single self-contained model that an implementation reads directly, and
enumerating it is itself useful: anything the exporter cannot fill in is a gap in
the understanding, not merely a gap in the code.

WHAT AN IMPLEMENTATION NEEDS, AND WHERE IT COMES FROM

  modulation  tone s -> sin(2*pi*(9+s)*n/512), amplitude 12509.706905, 407
              symbols per frame, final symbol tapered. Measured; resynthesis
              matches real captures to within 1 LSB (§3.29 area).
  offsets r   407 values. The transmitted tone is (d + r) mod 16. Fixed,
              payload-independent, and NOT a simple sequence — no parametric law
              in the position index fits better than chance — so it is shipped as
              a table. Determined only modulo 8, which is harmless.
  baseline d0 the coded symbol for an all-zero payload, per payload length. This
              is where payload LENGTH enters: rows are length-independent but the
              baseline is not.
  rows        one 407-symbol vector per payload bit. The payload block is 512
              bits; rows are the same at any payload length.
  pilots      payload-independent positions, for synchronisation and channel
              estimation on receive.

WHAT IS DELIBERATELY ABSENT, so nobody mistakes this for a complete modem:
control-frame generation, the ARQ state machine, soft-decision decoding, and
levels 5-11. See the clean-room note for what is and is not known about each.

    tools/vara_model_export.py --scratch DIR --out vara_model.json
    tools/vara_model_export.py --scratch DIR --verify vara_model.json
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_codec as vc  # noqa: E402
import vara_synth as vs  # noqa: E402

FRAME_SYMBOLS = 407
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


def load(scratch, name):
    """Read a capture only if it is present and complete."""
    p = os.path.join(scratch, name)
    if not os.path.exists(p) or os.path.getsize(p) < 1_000_000:
        return None
    try:
        return vc.symbols_of(p)
    except Exception:
        return None


def multibit_observations(scratch):
    obs = []
    for k, bb in RP.items():
        t = load(scratch, f"{k}.wav")
        if t is not None:
            obs.append((k, bb, t))
    spec = os.path.join(scratch, "mb_spec.json")
    if os.path.exists(spec):
        for k, bb in json.load(open(spec)).items():
            t = load(scratch, f"{k}.wav")
            if t is not None:
                obs.append((k, bb, t))
    return obs


def build(scratch):
    caps = {}
    for k, fn in PAIR_FILES.items():
        v = load(scratch, fn)
        if v is not None:
            caps[k] = v
    ids = [t for t in IDENTITIES if all(k in caps for k in t)]
    sets = vc.offset_sets(caps, ids)

    sweepdir = os.path.join(scratch, "fec_sweep")
    base32, raw32 = vc.load_sweep(sweepdir, 32)
    base64, raw64 = vc.load_sweep(sweepdir, 64)
    if base32 is None:
        raise SystemExit("no 32-byte sweep baseline")

    obs = multibit_observations(scratch)
    # Screen before fitting. One capture containing overlapping sessions is
    # enough to poison every offset, and such a capture still looks valid because
    # its burst 6 is a real 407-symbol frame — just from the wrong session.
    if obs:
        bad = vc.inconsistent_observations(
            sets, base32, raw32, [(k, b, t) for k, b, t in obs])
        if bad:
            print("  rejecting inconsistent captures (positions with no valid "
                  "offset):")
            for label, n in bad:
                print(f"    {label}: {n}/407")
        drop = {label for label, _ in bad}
        obs = [o for o in obs if o[0] not in drop]
        print(f"  fitting offsets on {len(obs)} consistent captures")
        sets = vc.constrain_with_multibit(sets, base32, raw32,
                                          [(b, t) for _, b, t in obs])
    r = vc.choose_offsets(sets)

    rows = {}
    for i, v in raw32.items():
        rows[i] = (((v - r) % 16) ^ ((base32 - r) % 16)).tolist()
    if base64 is not None:
        for i, v in raw64.items():
            rows.setdefault(i, (((v - r) % 16)
                                ^ ((base64 - r) % 16)).tolist())

    baselines = {"32": ((base32 - r) % 16).tolist()}
    if base64 is not None:
        baselines["64"] = ((base64 - r) % 16).tolist()

    # Payload-independent positions, measured rather than assumed.
    M = np.array([(v != base32) for v in raw32.values()])
    pilots = np.flatnonzero(M.sum(0) == 0).tolist() if len(M) else []

    taper, used = vs.measure_taper(
        [os.path.join(scratch, f) for f in
         ("probe_64.wav", "probe_32.wav", "fec_c1.wav", "fec_b01.wav",
          "fec_b2.wav", "v_b255.wav") if os.path.exists(os.path.join(scratch, f))])

    return {
        "format": "aethersdr.vara.level4.model",
        "version": 1,
        "modulation": {
            "sample_rate": 48000,
            "symbol_samples": 512,
            "tones": 16,
            "first_dft_bin": 9,
            "amplitude": vs.AMPLITUDE,
            "frame_symbols": FRAME_SYMBOLS,
            "final_symbol_taper": [round(float(x), 8) for x in taper[-32:]],
            "taper_start_sample": vs.TAPER_START,
            "taper_sources": used,
        },
        "scrambler": {
            "modulus": 16,
            "note": "transmitted tone = (coded symbol + offset) mod 16",
            "offsets": r.tolist(),
            "offset_candidates_remaining":
                [len(x) for x in sets],
        },
        "code": {
            "payload_block_bits": 512,
            "baselines_by_payload_bytes": baselines,
            "rows_present": sorted(rows),
            "rows": {str(k): v for k, v in sorted(rows.items())},
        },
        "pilots": {
            "payload_independent_positions": pilots,
            "note": "measured across every captured payload bit at 32 bytes",
        },
        "not_included": [
            "control-frame generation",
            "ARQ state machine",
            "soft-decision decoding for noisy channels",
            "levels 5-11 (unreachable without a paid licence)",
        ],
    }


def verify(scratch, model, holdout=6):
    """Round-trip the model against captures, with an honest split.

    Two traps this deliberately avoids. Testing every multi-bit capture against a
    model whose offsets were fitted using those same captures is circular and
    proves nothing, so a holdout is refitted from scratch without them. And every
    multi-bit capture here is a 32-BYTE payload, so scoring them against the
    64-byte baseline is meaningless — an earlier version did exactly that and
    reported a flat 0/28 that looked like a real failure.
    """
    r_full = np.array(model["scrambler"]["offsets"], dtype=int)
    rows = {int(k): np.array(v, dtype=int)
            for k, v in model["code"]["rows"].items()}
    obs = multibit_observations(scratch)
    usable = [(k, bb, t) for k, bb, t in obs if all(b in rows for b in bb)]
    base32c, raw32c = vc.load_sweep(os.path.join(scratch, "fec_sweep"), 32)
    caps0 = {}
    for k, fn in PAIR_FILES.items():
        v = load(scratch, fn)
        if v is not None:
            caps0[k] = v
    ids0 = [t for t in IDENTITIES if all(k in caps0 for k in t)]
    sets0 = vc.offset_sets(caps0, ids0)
    raw32c = {k: v for k, v in raw32c.items() if k < 256}
    drop = {lab for lab, _ in vc.inconsistent_observations(
        sets0, base32c, raw32c, [(k, b, t) for k, b, t in usable])}
    if drop:
        print(f"  excluding inconsistent captures from verification: {sorted(drop)}")
    usable = [o for o in usable if o[0] not in drop]
    print(f"  multi-bit captures usable: {len(usable)} (all 32-byte payloads)")

    base_key = "32"
    if base_key not in model["code"]["baselines_by_payload_bytes"]:
        print("  no 32-byte baseline; cannot verify")
        return False
    d0 = np.array(model["code"]["baselines_by_payload_bytes"][base_key], dtype=int)

    fitted = sum(int((vc.Codec(r_full, (d0 + r_full) % 16, rows).encode(bb) == t).sum())
                 == FRAME_SYMBOLS for _, bb, t in usable)
    print(f"  FITTED (circular, offsets were fitted on these): "
          f"{fitted}/{len(usable)} encode exactly")

    if len(usable) > holdout + 4:
        caps = {}
        for k, fn in PAIR_FILES.items():
            v = load(scratch, fn)
            if v is not None:
                caps[k] = v
        ids = [t for t in IDENTITIES if all(k in caps for k in t)]
        sets = vc.offset_sets(caps, ids)
        base32, raw32 = vc.load_sweep(os.path.join(scratch, "fec_sweep"), 32)
        fit_obs = usable[:-holdout]
        held = usable[-holdout:]
        sets = vc.constrain_with_multibit(sets, base32, raw32,
                                          [(b, t) for _, b, t in fit_obs])
        r2 = vc.choose_offsets(sets)
        rows2 = {i: ((v - r2) % 16) ^ ((base32 - r2) % 16)
                 for i, v in raw32.items()}
        c2 = vc.Codec(r2, base32, rows2)
        good = 0
        for k, bb, t in held:
            n = int((c2.encode(bb) == t).sum())
            got, res = c2.decode(t)
            exact = n == FRAME_SYMBOLS
            good += exact
            print(f"    held-out {k:8} {len(bb):2d} bits: encode {n}/407  "
                  f"decode {'CORRECT' if got == sorted(bb) else 'wrong'}  "
                  f"residual {res}")
        print(f"  HELD-OUT (refitted without them): {good}/{len(held)} encode exactly")

    amb = sum(1 for c in model["scrambler"]["offset_candidates_remaining"] if c > 2)
    print(f"  offsets still ambiguous beyond the irreducible {{r, r+8}}: {amb}/407")
    print(f"  generator rows present: {len(rows)}/512  "
          f"(the second sweep is still filling 256-511)")
    print(f"  pilots: {len(model['pilots']['payload_independent_positions'])}")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--out")
    ap.add_argument("--verify", metavar="MODEL")
    a = ap.parse_args()
    if a.verify:
        model = json.load(open(a.verify))
        print(f"verifying {a.verify}")
        return 0 if verify(a.scratch, model) else 1
    model = build(a.scratch)
    out = a.out or os.path.join(a.scratch, "vara_model.json")
    with open(out, "w") as h:
        json.dump(model, h)
    print(f"wrote {out} ({os.path.getsize(out)} bytes)")
    print(f"  generator rows : {len(model['code']['rows'])}/512")
    print(f"  baselines      : {sorted(model['code']['baselines_by_payload_bytes'])}")
    print(f"  pilots         : {len(model['pilots']['payload_independent_positions'])}")
    verify(a.scratch, model)
    return 0


if __name__ == "__main__":
    sys.exit(main())
