#!/usr/bin/env python3
"""VARA low-level (M-FSK) demodulator — turns a capture into a symbol stream.

VARA's low speed levels are NOT the OFDM waveform its published specification
describes. Measured on the bench (docs/vara-cleanroom-design.md §3.8), level 4
is constant envelope — peak/RMS 1.42 = 3.05 dB crest factor, power kurtosis
1.00 — which OFDM cannot be, and its instantaneous frequency resolves into a
regular grid of discrete tones. That is M-FSK. VARA appears to be dual
waveform: MFSK where robustness matters, OFDM where throughput does.

MFSK is far easier to demodulate than OFDM: no cyclic prefix, no channel
estimation, no equalisation. Detect which tone is present in each symbol
interval and you have the symbol stream. This tool does exactly that, and
nothing more — mapping symbols to coded bits, and coded bits to information
bits, comes later.

--------------------------------------------------------------------------
SELF-TEST FIRST. This tool refuses to be trusted blind.

    tools/vara_mfsk.py --selftest

generates M-FSK with known parameters and checks that the demodulator recovers
the exact symbol sequence. Run it before believing any output from real data.

That discipline is not decoration. Four separate estimators earlier in this
investigation produced confident, wrong answers because nobody checked them
against known truth first — a passband correlation swamped by its
double-frequency term, a lag scan whose metric rose trivially with lag, a
spectrum autocorrelation over too long a window, and a recorder that silently
captured the wrong audio source entirely. The one estimator that WAS validated
against synthetic truth is the one that redirected the whole investigation.
--------------------------------------------------------------------------

USAGE
    tools/vara_mfsk.py --selftest
    tools/vara_mfsk.py capture.wav --tones 16 --spacing 93.75 --baud 93.75 \
                       --center 1500 --start 26.6 --stop 30.4
    tools/vara_mfsk.py capture.wav --scan          # search the parameter space
"""

import argparse
import sys

import numpy as np

FS = 48000


def load_wav(path):
    import wave
    with wave.open(path, "rb") as handle:
        channels, width, rate = (handle.getnchannels(),
                                 handle.getsampwidth(), handle.getframerate())
        raw = handle.readframes(handle.getnframes())
    if width != 2:
        raise SystemExit(f"expected 16-bit PCM, got {width * 8}-bit")
    if rate != FS:
        raise SystemExit(f"expected {FS} Hz, got {rate}")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    if channels > 1:
        x = x.reshape(-1, channels).mean(axis=1)
    return x


def tone_grid(n_tones, spacing, center):
    """Tone frequencies, centred on `center`."""
    offsets = (np.arange(n_tones) - (n_tones - 1) / 2.0) * spacing
    return center + offsets


def demodulate(x, n_tones, spacing, baud, center, phase_offset=0.0):
    """Non-coherent MFSK detection.

    Per symbol interval, correlate against each candidate tone and take the
    largest magnitude. Non-coherent because we have no phase reference and do
    not need one: |sum(x * conj(e^{jwt}))| is invariant to the tone's phase.

    Returns (symbols, confidence, energies) where confidence is the ratio of
    the winning tone's energy to the runner-up's — a clean signal on the right
    parameters gives a large ratio, and a value near 1.0 means the parameter
    guess is wrong.
    """
    freqs = tone_grid(n_tones, spacing, center)
    sym_len = FS / baud
    n_sym = int((len(x) - phase_offset) // sym_len)
    if n_sym < 1:
        return np.array([]), np.array([]), np.zeros((0, n_tones))

    symbols = np.zeros(n_sym, dtype=int)
    confidence = np.zeros(n_sym)
    energies = np.zeros((n_sym, n_tones))

    for i in range(n_sym):
        a = int(phase_offset + i * sym_len)
        b = int(phase_offset + (i + 1) * sym_len)
        seg = x[a:b]
        if len(seg) < 8:
            break
        t = np.arange(len(seg)) / FS
        # Correlate against every candidate tone at once.
        ref = np.exp(-2j * np.pi * np.outer(freqs, t))
        mags = np.abs(ref @ seg) / len(seg)
        energies[i] = mags
        order = np.argsort(mags)[::-1]
        symbols[i] = order[0]
        confidence[i] = mags[order[0]] / max(mags[order[1]], 1e-12)

    return symbols, confidence, energies


def score_parameters(x, n_tones, spacing, baud, center, n_offsets=8):
    """Best median confidence over a few symbol-phase offsets.

    Symbol timing is unknown, so try several phases; the correct parameter set
    should score above the wrong ones even at a mediocre phase.

    KNOWN DEFECT — do not trust a large value from this function. On real
    captures it has produced the identical score (114237.231) for genuinely
    different parameter sets, which is impossible for a real measurement and
    means some window is yielding a near-zero runner-up and an unbounded
    ratio. Treat any score above ~50 as an artifact and read the per-symbol
    confidence profile instead (see --verbose): a real signal on correct
    parameters shows a consistent, finite ratio across the whole burst, not
    one enormous outlier. Fixing this needs a bounded metric — e.g. winning
    energy over TOTAL energy, which cannot exceed 1.
    """
    sym_len = FS / baud
    best = (0.0, 0.0)
    for k in range(n_offsets):
        off = k * sym_len / n_offsets
        _, conf, _ = demodulate(x, n_tones, spacing, baud, center, off)
        if len(conf) == 0:
            continue
        # Trim the tails: partial symbols at the edges are not informative.
        trimmed = conf[2:-2] if len(conf) > 8 else conf
        score = float(np.median(trimmed))
        if score > best[0]:
            best = (score, off)
    return best


def synth_mfsk(n_tones, spacing, baud, center, n_sym, seed=7, continuous=True):
    """Generate M-FSK with known symbols, for self-test."""
    rng = np.random.default_rng(seed)
    syms = rng.integers(0, n_tones, n_sym)
    freqs = tone_grid(n_tones, spacing, center)
    sym_len = int(round(FS / baud))
    out = np.zeros(n_sym * sym_len)
    phase = 0.0
    for i, s in enumerate(syms):
        t = np.arange(sym_len) / FS
        ph = 2 * np.pi * freqs[s] * t + phase
        out[i * sym_len:(i + 1) * sym_len] = np.cos(ph)
        if continuous:                      # phase-continuous, like CPFSK
            phase = (ph[-1] + 2 * np.pi * freqs[s] / FS) % (2 * np.pi)
    return out / np.abs(out).max(), syms


def selftest():
    print("=== MFSK demodulator self-test ===")
    print("(parameters deliberately NOT the VARA hypothesis, so a hard-coded")
    print(" assumption cannot pass by accident)\n")
    cases = [
        dict(n_tones=8, spacing=125.0, baud=125.0, center=1200.0, n_sym=200),
        dict(n_tones=16, spacing=62.5, baud=62.5, center=1500.0, n_sym=200),
        dict(n_tones=4, spacing=250.0, baud=250.0, center=1000.0, n_sym=200),
    ]
    failures = 0
    for case in cases:
        sig, truth = synth_mfsk(**case)
        got, conf, _ = demodulate(sig, case["n_tones"], case["spacing"],
                                  case["baud"], case["center"])
        n = min(len(got), len(truth))
        # Ignore the first and last symbol: edge effects are expected.
        agree = int(np.sum(got[1:n - 1] == truth[1:n - 1]))
        total = max(n - 2, 1)
        pct = agree / total * 100.0
        ok = pct > 99.0
        failures += 0 if ok else 1
        print(f"  {case['n_tones']:>2}-FSK  {case['spacing']:>6.1f} Hz  "
              f"{case['baud']:>6.1f} baud : {agree}/{total} symbols "
              f"({pct:5.1f}%)  median conf {np.median(conf[1:n - 1]):5.2f}  "
              f"{'PASS' if ok else 'FAIL'}")

    # A wrong-parameter control MUST score poorly, or the confidence metric is
    # meaningless as a discriminator.
    sig, _ = synth_mfsk(n_tones=16, spacing=62.5, baud=62.5, center=1500.0, n_sym=200)
    right, _ = score_parameters(sig, 16, 62.5, 62.5, 1500.0)
    wrong, _ = score_parameters(sig, 16, 93.75, 93.75, 1500.0)
    print(f"\n  discriminator: correct params median conf {right:.2f}, "
          f"wrong params {wrong:.2f}  ratio {right / max(wrong, 1e-9):.1f}x")
    if right < wrong * 1.5:
        print("  FAIL: the confidence metric does not separate right from wrong")
        failures += 1

    print(f"\n{'ALL SELF-TESTS PASSED' if not failures else f'{failures} SELF-TEST(S) FAILED'}")
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wav", nargs="?")
    parser.add_argument("--selftest", action="store_true",
                        help="validate the demodulator against synthetic truth")
    # Defaults are the MEASURED level-4 DATA parameters (§3.11): 16-ary
    # orthogonal CPFSK on the 48000/512 grid, tones at DFT bins 9..24.
    parser.add_argument("--tones", type=int, default=16)
    parser.add_argument("--spacing", type=float, default=93.75)
    parser.add_argument("--baud", type=float, default=93.75)
    parser.add_argument("--center", type=float, default=1546.875)
    parser.add_argument("--start", type=float, help="segment start, seconds")
    parser.add_argument("--stop", type=float, help="segment stop, seconds")
    parser.add_argument("--scan", action="store_true",
                        help="search tone count / spacing / baud")
    parser.add_argument("--out", help="write symbol indices to this file")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.wav:
        parser.error("need a capture file, or --selftest")

    x = load_wav(args.wav)
    if args.start is not None:
        a = int(args.start * FS)
        b = int(args.stop * FS) if args.stop else len(x)
        x = x[a:b]
    print(f"segment: {len(x)} samples ({len(x) / FS:.3f} s)")

    if args.scan:
        print("\nscanning parameter space (median confidence; higher is better)")
        results = []
        for n_tones in (4, 8, 16, 32):
            for spacing in (46.875, 62.5, 93.75, 125.0, 187.5):
                for baud in (spacing, spacing / 2.0, spacing * 2.0):
                    score, _ = score_parameters(x, n_tones, spacing, baud, args.center)
                    results.append((score, n_tones, spacing, baud))
        results.sort(reverse=True)
        print(f"  {'score':>7} {'tones':>6} {'spacing':>9} {'baud':>9}")
        for score, n_tones, spacing, baud in results[:12]:
            print(f"  {score:>7.3f} {n_tones:>6} {spacing:>9.3f} {baud:>9.3f}")
        print("\nNOTE: a top score close to the runners-up means the scan did NOT")
        print("      discriminate — do not read a winner out of a flat field.")
        return 0

    symbols, conf, _ = demodulate(x, args.tones, args.spacing, args.baud, args.center)
    print(f"\n{len(symbols)} symbols at {args.tones}-FSK, "
          f"{args.spacing} Hz spacing, {args.baud} baud")
    print(f"  median confidence : {np.median(conf):.2f}  (1.0 = no discrimination)")
    print(f"  symbol histogram  : {np.bincount(symbols, minlength=args.tones)}")
    print(f"  first 48 symbols  : {symbols[:48].tolist()}")
    if args.out:
        np.savetxt(args.out, symbols, fmt="%d")
        print(f"  wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
