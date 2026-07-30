#!/usr/bin/env python3
"""Synthesise a VARA level-4 DATA frame from its symbol sequence.

This is the transmit half of the waveform work. Everything here was measured
from captures, and the module's own --verify mode re-proves it against real
recordings rather than asking to be trusted.

THE WAVEFORM. A DATA frame is 407 symbols of 512 samples at 48000 Hz. Symbol
value s (0..15) is a pure sine sitting exactly on DFT bin 9+s of a 512-point
transform, i.e. frequency (9+s) * 93.75 Hz, spanning 843.75 to 2250 Hz:

    x[n] = A * sin(2*pi*(9+s)*n/512),  n = 0..511

Because each symbol occupies a whole number of cycles in its 512-sample window,
the tones are exactly orthogonal, every symbol begins and ends at zero, and
there is no phase continuity between symbols to preserve. That is why
demodulation is unambiguous (winner-to-runner-up magnitude ratios of ~186000).

AMPLITUDE. A = 12509.7069, measured as the mean over 406 symbols with a standard
deviation of 0.072 — constant to within rounding.

THE FINAL SYMBOL IS TAPERED. Symbol 406, and only symbol 406, carries a
fade-out: full amplitude through sample 495, then a decay to zero over the last
16 samples. No closed form fits it well (a cos^2 over 17 samples is the best
tried, at RMS 0.012), so the envelope is measured rather than modelled. It is
identical in every capture examined, so it is a constant of the waveform. The
table is recovered from several captures at once: the final symbol's tone differs
between them, so samples where one capture's tone crosses zero are covered by
another.

HOW EXACT IS IT. Symbols 0 to 405 reconstruct with a maximum sample error of
**1 LSB**, and the residual takes only the values -1, 0 and +1 with mean -0.0002.
That is rounding and nothing else: the modem rounds the same ideal waveform and
disagrees with numpy's rint on about 3 % of samples. The amplitude is confirmed
independently by least squares over 406 symbols as 12509.706905.

With the least-squares taper this holds for the final symbol too: across 14
captures the worst single-sample error anywhere in a 208384-sample frame is 1.
An earlier median-of-ratios taper with a magnitude threshold gave 67, because
thresholding starved the samples nearest the end of the fade; that is recorded
because the failure was silent — the reconstruction still looked broadly right.
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap  # noqa: E402

FS = 48000
SYM = 512
BIN0 = 9                 # tone 0 sits on DFT bin 9
AMPLITUDE = 12509.7069
FRAME_SYMBOLS = 407
TAPER_START = 496        # first sample of the final symbol's fade-out


def tone(symbol, count=SYM):
    n = np.arange(count)
    return np.sin(2.0 * np.pi * (BIN0 + int(symbol)) * n / SYM)


def measure_taper(paths, burst=6):
    """Recover the final symbol's envelope from real captures, by least squares.

    A per-sample ratio last[i]/pure[i] is useless where that capture's tone is
    near a zero crossing, and thresholding those out starves the samples near the
    end of the taper — an earlier version did exactly that and made the worst
    reconstruction error seven times larger. Instead solve for each sample's
    envelope value across all captures at once,

        env[i] = sum_c last_c[i]*pure_c[i] / sum_c pure_c[i]^2

    which weights each capture by its own tone magnitude at that sample and needs
    no threshold, no interpolation and no gaps. Captures differ in their final
    tone, so between them every sample is well covered.
    """
    num = np.zeros(SYM)
    den = np.zeros(SYM)
    used = 0
    for path in paths:
        x = cap.load_wav(path)
        bursts = cap.split_bursts(x)
        if len(bursts) <= burst:
            continue
        a, b = bursts[burst]
        if (b - a) % SYM or (b - a) // SYM != FRAME_SYMBOLS:
            continue
        seg = x[a:b].astype(np.float64)
        syms, _ = cap.demod(seg / 32768.0)
        last = seg[(FRAME_SYMBOLS - 1) * SYM:]
        pure = AMPLITUDE * tone(syms[-1])
        num += last * pure
        den += pure * pure
        used += 1
    env = np.ones(SYM)
    ok = den > 0
    env[ok] = num[ok] / den[ok]
    env[:TAPER_START] = 1.0
    return env, used


def synthesise(symbols, taper=None):
    """Symbols -> int16 PCM. `taper` is the final-symbol envelope."""
    symbols = list(symbols)
    out = np.concatenate([AMPLITUDE * tone(s) for s in symbols])
    if taper is not None and len(symbols):
        out[(len(symbols) - 1) * SYM:] *= taper
    return np.rint(out).astype(np.int16)


def verify(paths, burst=6):
    """Round-trip real captures: demodulate, resynthesise, compare sample by
    sample. Anything short of exact is reported, not smoothed over."""
    taper, used = measure_taper(paths, burst)
    print(f"taper measured by least squares from {used} usable captures")
    print(f"  envelope[494:512] = {np.round(taper[494:], 4).tolist()}")
    print()
    worst = 0
    for path in paths:
        x = cap.load_wav(path)
        bursts = cap.split_bursts(x)
        if len(bursts) <= burst:
            print(f"  {os.path.basename(path):18} no burst {burst}")
            continue
        a, b = bursts[burst]
        if (b - a) % SYM or (b - a) // SYM != FRAME_SYMBOLS:
            print(f"  {os.path.basename(path):18} burst {burst} is not a "
                  f"{FRAME_SYMBOLS}-symbol frame")
            continue
        seg = x[a:b].astype(np.int64)
        syms, _ = cap.demod(seg.astype(np.float64) / 32768.0)
        re = synthesise(syms, taper).astype(np.int64)
        diff = np.abs(re - seg)
        exact = int((diff == 0).sum())
        worst = max(worst, int(diff.max()))
        print(f"  {os.path.basename(path):18} exact {exact:6d}/{len(seg)} "
              f"({exact / len(seg):.6f})  max|err| {int(diff.max())}")
    print(f"\nworst sample error across all captures: {worst}")
    print("A worst error of 1 is rounding: the modem rounds the same ideal")
    print("waveform and disagrees with rint on about 3 % of samples. Anything")
    print("larger points at the taper table, not the model.")
    return taper


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--verify", nargs="+", metavar="WAV",
                   help="round-trip these captures and report exactness")
    p.add_argument("--burst", type=int, default=6)
    a = p.parse_args()
    if a.verify:
        verify(a.verify, a.burst)
        return 0
    p.error("nothing to do; pass --verify")


if __name__ == "__main__":
    sys.exit(main())
