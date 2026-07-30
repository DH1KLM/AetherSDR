#!/usr/bin/env python3
"""Sweep single-bit payloads to build VARA's generator matrix — the FEC campaign.

WHAT THIS IS FOR. §3.23 of the clean-room note established that the two
least-significant bits of every DATA-frame tone index are an exactly GF(2)-linear
function of the payload (0 violations of the affine identity across two
independent triples), while the two most-significant bits are not. That gives
814 linear bits per 407-symbol frame, and on those bits

    row_i = Encode(e_i) XOR Encode(0)

recovers a row of the generator matrix. Collecting every row gives the whole
linear map, which is what the interleaver and polynomials can then be read out
of.

WHY 32-BYTE PAYLOADS. A 32-byte payload produces the same burst structure as a
64-byte one — a 164-symbol connect frame, a payload-independent 407-symbol
session frame at burst 1, then the payload frame at burst 6 — so it needs 256
captures instead of 512. A 16-byte payload does NOT: its bursts come out
misaligned and it emits a 468-symbol frame, so 32 bytes is the floor.

    tools/vara_fec_sweep.py --length 32 --bits 0-255      # the campaign
    tools/vara_fec_sweep.py --length 32 --analyse         # build G, no capture

RUNTIME. Each capture is a full ARQ session: roughly three minutes, and they
CANNOT overlap because the VARA host interface is single-client (§3.23). 256
captures is therefore ~13 hours. Run it detached.

RESUMABLE. Every capture writes its own symbols file and is skipped if that file
already exists, so an interrupted sweep resumes where it stopped. WAV files are
deleted after the symbols are extracted unless --keep-wav is given: 256 captures
would otherwise be about 1.3 GB.
"""

import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap

PAYLOAD_BURST = 6      # burst index of the payload-bearing frame
FRAME_SYMBOLS = 407


def extract_payload_frame(wav_path):
    """Return the payload frame's symbols, or None with a reason.

    Deliberately strict. A capture that does not yield a clean 407-symbol frame
    at the expected burst index is discarded rather than guessed at — a lenient
    filter is how the "all-zero payload emits no payload frame" error in §3.22
    happened, and how the CW identifier got misread as a DATA frame.
    """
    x = cap.load_wav(wav_path)
    bursts = cap.split_bursts(x)
    if len(bursts) <= PAYLOAD_BURST:
        return None, f"only {len(bursts)} bursts"
    a, b = bursts[PAYLOAD_BURST]
    length = b - a
    if length % cap.SYM:
        return None, f"burst {PAYLOAD_BURST} is not a whole number of symbols"
    if length // cap.SYM != FRAME_SYMBOLS:
        return None, f"burst {PAYLOAD_BURST} has {length // cap.SYM} symbols"
    seg = x[a:b].astype(np.float64) / 32768.0
    spec = np.abs(np.fft.rfft(seg[:2048]))
    # The CW identifier is a 750 Hz continuous sub-tone plus OOK; bin 32 of a
    # 2048-point transform is 750 Hz. A DATA frame has no energy there.
    if spec[32] ** 2 > 0.15 * (spec[36:100] ** 2).sum():
        return None, "burst is the CW identifier, not a DATA frame"
    syms, conf = cap.demod(seg)
    return dict(symbols=syms.astype(int).tolist(),
                median_conf=float(np.median(conf)),
                min_conf=float(np.min(conf))), None


def capture_bit(length, bit, outdir, keep_wav, attempts=2):
    """Capture one single-bit payload. Returns the symbols dict or None."""
    stem = "zeros" if bit is None else f"bit{bit}"
    js = os.path.join(outdir, f"{stem}_of{length}.json")
    if os.path.exists(js):
        with open(js) as h:
            return json.load(h), "cached"

    payload = bytearray(length)
    if bit is not None:
        payload[bit // 8] = 1 << (7 - (bit % 8))

    for attempt in range(1, attempts + 1):
        wav = os.path.join(outdir, f"{stem}_of{length}.wav")
        info, err = cap.run_once(bytes(payload), wav)
        if err:
            print(f"  {stem}: link failed ({err}), attempt {attempt}/{attempts}",
                  flush=True)
            time.sleep(15)
            continue
        rec, why = extract_payload_frame(wav)
        if rec is None:
            print(f"  {stem}: unusable capture ({why}), attempt {attempt}/{attempts}",
                  flush=True)
            if not keep_wav and os.path.exists(wav):
                os.remove(wav)
            time.sleep(15)
            continue
        rec["bit"] = bit
        rec["length"] = length
        with open(js, "w") as h:
            json.dump(rec, h)
        if not keep_wav:
            os.remove(wav)
        return rec, "captured"
    return None, "failed"


def linear_bits(symbols):
    """The two LSBs of each tone index — the exactly-linear part (§3.23)."""
    s = np.asarray(symbols, dtype=int)
    return np.stack([(s >> 1) & 1, s & 1], axis=1).ravel().astype(np.uint8)


def analyse(length, outdir):
    base_path = os.path.join(outdir, f"zeros_of{length}.json")
    if not os.path.exists(base_path):
        print(f"no baseline capture at {base_path}", file=sys.stderr)
        return 1
    with open(base_path) as h:
        base = linear_bits(json.load(h)["symbols"])

    rows, have, missing = [], [], []
    for bit in range(length * 8):
        p = os.path.join(outdir, f"bit{bit}_of{length}.json")
        if not os.path.exists(p):
            missing.append(bit)
            continue
        with open(p) as h:
            rows.append(linear_bits(json.load(h)["symbols"]) ^ base)
            have.append(bit)

    print(f"baseline    : {len(base)} linear bits per frame")
    print(f"rows present: {len(have)} / {length * 8}")
    if missing:
        print(f"rows missing: {len(missing)} "
              f"(first few: {missing[:12]}{' ...' if len(missing) > 12 else ''})")
    if not rows:
        return 1

    G = np.array(rows, dtype=np.uint8)
    npz = os.path.join(outdir, f"generator_{length}.npz")
    np.savez_compressed(npz, G=G, bits=np.array(have), base=base)
    print(f"\nG is {G.shape[0]} x {G.shape[1]}, saved to {npz}")

    weights = G.sum(axis=1)
    print(f"row weights : min {weights.min()} max {weights.max()} "
          f"mean {weights.mean():.1f} ({100 * weights.mean() / G.shape[1]:.1f} %)")
    print(f"rank        : {gf2_rank(G)} (of {G.shape[0]} rows)")
    zero_cols = int((G.sum(axis=0) == 0).sum())
    print(f"columns no row touches: {zero_cols} / {G.shape[1]} — these are "
          f"payload-independent positions (headers, pilots or padding)")
    return 0


def gf2_rank(A):
    A = A.copy() % 2
    rows, cols = A.shape
    r = 0
    for c in range(cols):
        piv = None
        for i in range(r, rows):
            if A[i, c]:
                piv = i
                break
        if piv is None:
            continue
        A[[r, piv]] = A[[piv, r]]
        for i in range(rows):
            if i != r and A[i, c]:
                A[i] ^= A[r]
        r += 1
        if r == rows:
            break
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--length", type=int, default=32, help="payload length in bytes")
    p.add_argument("--bits", default="0-255",
                   help="bit range to sweep, e.g. 0-255 or 0-15")
    p.add_argument("--outdir", default=None)
    p.add_argument("--keep-wav", action="store_true",
                   help="keep captures; 256 of them is about 1.3 GB")
    p.add_argument("--analyse", action="store_true",
                   help="build the generator matrix from existing captures only")
    a = p.parse_args()

    outdir = a.outdir or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                      "..", "fec_sweep")
    outdir = os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)

    if a.analyse:
        return analyse(a.length, outdir)

    lo, _, hi = a.bits.partition("-")
    todo = list(range(int(lo), int(hi or lo) + 1))
    limit = a.length * 8
    if todo and todo[-1] >= limit:
        raise SystemExit(f"bit {todo[-1]} is outside a {a.length}-byte payload "
                         f"({limit} bits)")

    print(f"sweep: {a.length}-byte payloads, bits {todo[0]}-{todo[-1]} "
          f"({len(todo)} captures) into {outdir}")
    print("captures are serialised — the VARA host interface is single-client\n")

    rec, how = capture_bit(a.length, None, outdir, a.keep_wav)
    print(f"baseline Encode(0): {how}")
    if rec is None:
        print("cannot proceed without a baseline", file=sys.stderr)
        return 2

    ok = failed = 0
    t0 = time.time()
    for n, bit in enumerate(todo, 1):
        rec, how = capture_bit(a.length, bit, outdir, a.keep_wav)
        if rec is None:
            failed += 1
        else:
            ok += 1
        done = time.time() - t0
        rate = done / max(n, 1)
        print(f"[{n}/{len(todo)}] bit {bit}: {how}   "
              f"ok={ok} failed={failed}   "
              f"elapsed {done / 60:.0f}m, eta {rate * (len(todo) - n) / 60:.0f}m",
              flush=True)

    print(f"\nsweep finished: {ok} captured, {failed} failed")
    return analyse(a.length, outdir)


if __name__ == "__main__":
    sys.exit(main())
