#!/usr/bin/env python3
"""Determinism probe — is VARA's encoder a pure function of the payload?

THE QUESTION THIS GATES. Recovering the FEC by differential analysis assumes
Encode() is deterministic: send the all-zeros payload, send a single-bit
payload, XOR the codewords, and each difference is a row of the generator
matrix. That works only if the same payload always produces the same waveform.
If a sequence number, session key or seeded scrambler varies between frames,
the subtraction is meaningless and the whole approach has to change.

VARA's own specification says ACK codes "are different for each session, to
avoid false commands when 2 stations are using in the same dial" — so SOMETHING
is session-seeded. Whether that seeding reaches the DATA path is exactly what
this measures.

METHOD. Send an identical payload in two separate sessions, capture the
transmitter's audio each time, isolate the DATA frames, demodulate to symbols
with the known-exact parameters, and compare.

  identical symbol streams  -> deterministic; differential attack is tractable
                               linear algebra over GF(2)
  differing symbol streams  -> session-seeded; report as a hard blocker

BOUNDARIES. Bursts are separated by EXACT digital zeros (70% of a capture), so
frames are split on zero runs rather than an energy threshold. An energy
detector previously over-ran every boundary by ~478 samples and manufactured a
phantom "lead-in ramp" that does not exist. Do not reintroduce one.

USAGE
    tools/vara_determinism.py --selftest
    tools/vara_determinism.py a.wav b.wav
"""

import argparse
import sys
import wave

import numpy as np

FS = 48000
SYM = 512                 # DATA symbol length, samples
TONES = 16
SPACING = 93.75           # = 48000/512
CENTER = 1546.875         # = 16.5 * 93.75, i.e. DFT bins 9..24
MIN_ZERO_RUN = 2048       # inter-burst silence is far longer than this


def load_wav(path):
    with wave.open(path, "rb") as h:
        ch, w, rate = h.getnchannels(), h.getsampwidth(), h.getframerate()
        raw = h.readframes(h.getnframes())
    if w != 2 or rate != FS:
        raise SystemExit(f"{path}: need 16-bit/{FS} Hz, got {w * 8}-bit/{rate}")
    x = np.frombuffer(raw, dtype="<i2").astype(np.int32)
    if ch > 1:
        x = x.reshape(-1, ch)[:, 0]
    return x


def split_bursts(x, min_zero=MIN_ZERO_RUN):
    """Split on runs of exact digital zeros. No threshold, no uncertainty."""
    nz = x != 0
    if not nz.any():
        return []
    idx = np.flatnonzero(nz)
    breaks = np.flatnonzero(np.diff(idx) > min_zero)
    starts = np.concatenate([[idx[0]], idx[breaks + 1]])
    ends = np.concatenate([idx[breaks], [idx[-1]]])

    out = []
    for a, b in zip(starts, ends):
        a, b = int(a), int(b) + 1
        # Every symbol is A*sin(2*pi*k*n/512), so sample 0 of a burst is an
        # EXACT zero and the first-nonzero index lands one or more samples
        # late. Snap the start back onto the 512-sample grid rather than
        # inheriting that offset — it is what made an earlier energy detector
        # mis-report every boundary.
        for back in range(0, 8):
            if (b - (a - back)) % SYM == 0 and a - back >= 0:
                a = a - back
                break
        out.append((a, b))
    return out


def classify(length):
    """DATA frames use a 512-sample symbol; control frames use 2048."""
    if length % SYM:
        return "ragged"
    n = length // SYM
    if n >= 200:
        return "DATA"
    return "control"


def demod_symbols(seg):
    """Non-coherent 16-FSK detection on the known tone grid."""
    freqs = CENTER + (np.arange(TONES) - (TONES - 1) / 2.0) * SPACING
    n = len(seg) // SYM
    out = np.zeros(n, dtype=int)
    conf = np.zeros(n)
    t = np.arange(SYM) / FS
    ref = np.exp(-2j * np.pi * np.outer(freqs, t))
    for i in range(n):
        mags = np.abs(ref @ seg[i * SYM:(i + 1) * SYM])
        order = np.argsort(mags)[::-1]
        out[i] = order[0]
        conf[i] = mags[order[0]] / max(mags[order[1]], 1e-12)
    return out, conf


def data_frames(x, label):
    frames = []
    for a, b in split_bursts(x):
        kind = classify(b - a)
        if kind != "DATA":
            continue
        syms, conf = demod_symbols(x[a:b].astype(np.float64) / 32768.0)
        frames.append(dict(start=a, length=b - a, n=len(syms),
                           symbols=syms, conf=float(np.median(conf))))
    print(f"{label}: {len(frames)} DATA frame(s)")
    for i, f in enumerate(frames):
        print(f"   [{i}] start={f['start']} len={f['length']} "
              f"({f['length'] // SYM} symbols) median-conf={f['conf']:.0f}")
    return frames


def compare(fa, fb):
    n = min(len(fa["symbols"]), len(fb["symbols"]))
    a, b = fa["symbols"][:n], fb["symbols"][:n]
    same = int(np.sum(a == b))
    return n, same, float(same) / n * 100.0


def selftest():
    """A deterministic source must read identical; a seeded one must not."""
    print("=== determinism probe self-test ===")
    rng = np.random.default_rng(3)
    freqs = CENTER + (np.arange(TONES) - (TONES - 1) / 2.0) * SPACING

    def build(symbols):
        out = []
        for s in symbols:
            t = np.arange(SYM) / FS
            out.append(np.sin(2 * np.pi * freqs[s] * t) * 12510)
        sig = np.concatenate(out).astype(np.int32)
        return np.concatenate([np.zeros(4096, np.int32), sig,
                               np.zeros(4096, np.int32)])

    truth = rng.integers(0, TONES, 407)
    ok = True

    # identical input -> identical symbols
    fa = data_frames(build(truth), "  synthetic A")
    fb = data_frames(build(truth), "  synthetic B")
    if not fa or not fb:
        print("  FAIL: burst splitter did not find the DATA frame")
        return 1
    n, same, pct = compare(fa[0], fb[0])
    print(f"  deterministic case : {same}/{n} ({pct:.1f}%) — expect 100.0%")
    ok &= pct == 100.0
    ok &= bool(np.array_equal(fa[0]["symbols"], truth))

    # seeded input -> must NOT read identical, or the probe cannot detect seeding
    seeded = truth.copy()
    seeded[::7] = (seeded[::7] + 1) % TONES
    fc = data_frames(build(seeded), "  synthetic C (seeded)")
    n2, same2, pct2 = compare(fa[0], fc[0])
    print(f"  seeded case        : {same2}/{n2} ({pct2:.1f}%) — expect <100%")
    ok &= pct2 < 100.0

    print(f"\n{'SELF-TEST PASSED' if ok else 'SELF-TEST FAILED'}")
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wavs", nargs="*")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args()

    if args.selftest:
        return selftest()
    if len(args.wavs) < 2:
        p.error("need two captures, or --selftest")

    sets = [data_frames(load_wav(w), w.split("/")[-1]) for w in args.wavs]
    if not all(sets):
        print("\nno DATA frames in one or more captures — cannot conclude")
        return 2

    print("\n=== cross-capture comparison ===")
    a, b = sets[0][0], sets[1][0]
    n, same, pct = compare(a, b)
    print(f"  frame lengths : {a['length']} vs {b['length']} samples "
          f"({'same' if a['length'] == b['length'] else 'DIFFER'})")
    print(f"  symbols equal : {same}/{n} = {pct:.2f}%")
    print(f"  first 24 A    : {a['symbols'][:24].tolist()}")
    print(f"  first 24 B    : {b['symbols'][:24].tolist()}")

    if pct == 100.0:
        print("\n  VERDICT: DETERMINISTIC across sessions.")
        print("  The differential attack is tractable linear algebra.")
    elif pct > 90.0:
        print(f"\n  VERDICT: MOSTLY identical ({pct:.2f}%).")
        print("  Suggests a small varying field (sequence number / session id)")
        print("  rather than a whole-frame scrambler. Locate the differing")
        print("  symbol positions — they bound where the varying field lives.")
        diff = np.flatnonzero(a["symbols"][:n] != b["symbols"][:n])
        print(f"  differing symbol indices: {diff.tolist()[:40]}")
    else:
        print(f"\n  VERDICT: NOT deterministic ({pct:.2f}% — chance is "
              f"{100.0 / TONES:.1f}%).")
        print("  Session-seeded scrambling is likely. HARD BLOCKER for the")
        print("  differential attack as designed; report rather than proceed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
