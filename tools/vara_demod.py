#!/usr/bin/env python3
"""VARA HF OFDM demodulator front end — analysis instrument, not a decoder.

Turns a captured 48 kHz recording into the 52 x N grid of complex subcarrier
values that every downstream question is asked against: which carriers are
occupied, where the pilots sit, what the constellations look like per level,
and eventually what the coded bits are.

GEOMETRY — all of it derived from EA5HVK's published VARA HF Modem
Specification Rev 2.0.0 (docs/varac-cleanroom-design.md §3.1), not from
inspecting any binary:

    sample rate            48000 Hz          (spec 3.3)
    OFDM symbol            24 ms             (spec 5.1)
      cyclic prefix        2.6 ms  ->  125 samples, taken as 128
      FFT block            21.33 ms -> 1024 samples exactly at 48 kHz
    carrier spacing        1/21.33ms = 46.875 Hz  ==  48000/1024, exact
    carriers               52                (spec 5.1)
    occupied bandwidth     52 * 46.875 = 2437.5 Hz  ~= the stated 2.4 kHz
    DATA frame             196 symbols       (spec 5.1)
    sync pilots            208 = 4 per carrier, in 4 rows
    data carriers          9984 = 52 * (196 - 4)
    levels 9-11            + 1020 EQ pilots, payload 8964 = 9984 - 1020

The 1024/128 split is the load-bearing derivation: 1024 + 128 = 1152 samples
= exactly 24 ms at 48 kHz, and 48000/1024 = 46.875 Hz reproduces the stated
carrier spacing to the digit. If the CP correlation below peaks at 1152, the
whole geometry is confirmed against a real signal.

UNEXPLAINED, and worth measuring: 196 symbols x 24 ms = 4704 ms, but the spec
calls a DATA frame 5225 ms. The ~521 ms difference (about 21.7 symbol times)
is not accounted for anywhere in the document and is presumably preamble or
acquisition. This tool reports observed burst lengths so that gap can be
characterised rather than guessed at.

USAGE
    tools/vara_demod.py capture.wav                  # analyse every burst
    tools/vara_demod.py capture.wav --burst 0 --grid grid0.npy
    tools/vara_demod.py capture.wav --json report.json
"""

import argparse
import json
import struct
import sys
import wave

import numpy as np

FS = 48000
N_FFT = 1024              # 21.33 ms at 48 kHz
N_CP = 128                # 2.6 ms at 48 kHz (125 rounded to a power of two)
N_SYM = N_FFT + N_CP      # 1152 samples == 24 ms
CARRIERS = 52
SPACING = FS / N_FFT      # 46.875 Hz
FRAME_SYMBOLS = 196
SPEC_FRAME_MS = 5225.0


def load_wav(path):
    with wave.open(path, "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        rate = handle.getframerate()
        frames = handle.readframes(handle.getnframes())
    if width != 2:
        raise SystemExit(f"expected 16-bit samples, got {width * 8}-bit")
    data = np.frombuffer(frames, dtype="<i2").astype(np.float64) / 32768.0
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    if rate != FS:
        raise SystemExit(f"expected {FS} Hz, got {rate} Hz — recapture at 48 kHz")
    return data


def find_bursts(x, floor_db=-45.0, min_ms=200.0):
    """Split the recording into transmissions on a smoothed energy envelope."""
    win = int(0.010 * FS)                      # 10 ms
    power = np.convolve(x * x, np.ones(win) / win, mode="same")
    peak = power.max()
    if peak <= 0:
        return []
    db = 10.0 * np.log10(np.maximum(power / peak, 1e-12))
    active = db > floor_db

    bursts = []
    start = None
    for i, flag in enumerate(active):
        if flag and start is None:
            start = i
        elif not flag and start is not None:
            if (i - start) / FS * 1000.0 >= min_ms:
                bursts.append((start, i))
            start = None
    if start is not None and (len(active) - start) / FS * 1000.0 >= min_ms:
        bursts.append((start, len(active)))
    return bursts


def to_baseband(seg, center_hz):
    """Real passband -> complex baseband.

    The CP estimator below correlates r(n) against r*(n+N_FFT). On a real
    signal that product carries a strong double-frequency term which buries
    the correlation peak; on the analytic signal downconverted to DC it does
    not. This is why the first pass produced scattered period estimates.
    """
    n = len(seg)
    S = np.fft.fft(seg)
    h = np.zeros(n)
    h[0] = 1
    if n % 2 == 0:
        h[n // 2] = 1
        h[1:n // 2] = 2
    else:
        h[1:(n + 1) // 2] = 2
    analytic = np.fft.ifft(S * h)
    t = np.arange(n) / FS
    return analytic * np.exp(-2j * np.pi * center_hz * t)


def cp_metric(bb, period):
    """van de Beek style CP correlation magnitude, averaged per candidate period.

    Returns the peak-to-mean ratio of the folded metric: sharp concentration
    means the candidate period is the true symbol length.
    """
    if len(bb) < N_FFT + N_CP * 3:
        return -1.0, 0
    a = bb[:-N_FFT]
    b = np.conj(bb[N_FFT:])
    prod = a * b
    kernel = np.ones(N_CP)
    corr = np.abs(np.convolve(prod, kernel, mode="valid"))
    usable = (len(corr) // period) * period
    if usable < period * 2:
        return -1.0, 0
    folded = corr[:usable].reshape(-1, period).mean(axis=0)
    mean = folded.mean()
    if mean <= 0:
        return -1.0, 0
    return float(folded.max() / mean), int(np.argmax(folded))


def estimate_symbol_period(seg, center_hz):
    """Scan candidate symbol periods and return the sharpest."""
    bb = to_baseband(seg, center_hz)
    best = (-1.0, None, 0)
    for period in range(N_SYM - 60, N_SYM + 61):
        score, offset = cp_metric(bb, period)
        if score > best[0]:
            best = (score, period, offset)
    return best  # (score, period, offset)


def band_edges(spectrum, db_down):
    """Bins where the smoothed spectrum is within db_down of its peak."""
    peak = spectrum.max()
    if peak <= 0:
        return None
    thresh = peak * (10.0 ** (-abs(db_down) / 20.0))
    idx = np.where(spectrum > thresh)[0]
    if not len(idx):
        return None
    return int(idx[0]), int(idx[-1])


def cp_correlation(seg):
    """Symbol timing from the cyclic prefix.

    The CP is a copy of the last N_CP samples of the FFT block, so the signal
    correlates with itself at lag N_FFT. Averaging that product over an N_CP
    window peaks once per symbol; the spacing between peaks IS the symbol
    length, which is how the 1152-sample geometry gets confirmed empirically
    rather than assumed.
    """
    if len(seg) < N_FFT + N_CP * 2:
        return None, None
    a = seg[:-N_FFT]
    b = seg[N_FFT:]
    prod = a * b
    kernel = np.ones(N_CP) / N_CP
    corr = np.convolve(prod, kernel, mode="valid")
    if len(corr) < N_SYM * 2:
        return corr, None

    # Fold the correlation modulo a trial symbol length and score how sharply
    # it concentrates. The true period gives the sharpest fold.
    best, best_score = None, -1.0
    for period in range(N_SYM - 24, N_SYM + 25):
        usable = (len(corr) // period) * period
        if usable < period * 2:
            continue
        folded = corr[:usable].reshape(-1, period).mean(axis=0)
        score = folded.max() - folded.mean()
        if score > best_score:
            best_score, best = score, period
    return corr, best


def occupied_bins(seg, offset):
    """Average magnitude spectrum over whole symbols, to locate the carriers."""
    mags = []
    pos = offset
    while pos + N_SYM <= len(seg):
        block = seg[pos + N_CP: pos + N_CP + N_FFT]
        mags.append(np.abs(np.fft.rfft(block * np.hanning(N_FFT))))
        pos += N_SYM
    if not mags:
        return None
    return np.mean(mags, axis=0)


def build_grid(seg, offset, first_bin):
    """Demodulate into a CARRIERS x nsym complex grid."""
    rows = []
    pos = offset
    while pos + N_SYM <= len(seg):
        block = seg[pos + N_CP: pos + N_CP + N_FFT]
        spec = np.fft.rfft(block)
        rows.append(spec[first_bin:first_bin + CARRIERS])
        pos += N_SYM
    if not rows:
        return np.zeros((CARRIERS, 0), dtype=complex)
    return np.array(rows).T          # carriers x symbols


def analyse_burst(x, span, index, verbose=True):
    start, end = span
    seg = x[start:end]
    ms = len(seg) / FS * 1000.0

    report = {
        "burst": index,
        "start_sample": int(start),
        "length_samples": int(len(seg)),
        "length_ms": round(ms, 1),
        "symbols_if_24ms": round(ms / 24.0, 2),
    }

    # Centre estimate from the spectrum, then baseband CP timing.
    rough = occupied_bins(seg, 0)
    center_hz = 1500.0
    if rough is not None:
        edges = band_edges(rough, 25.0)
        if edges:
            center_hz = (edges[0] + edges[1]) / 2.0 * SPACING
    report["center_hz_est"] = round(center_hz, 1)

    score, period, offset_bb = estimate_symbol_period(seg, center_hz)
    report["cp_sharpness"] = round(score, 3)
    report["cp_period_samples"] = int(period) if period else None
    if period:
        report["cp_period_ms"] = round(period / FS * 1000.0, 3)
        report["matches_1152"] = bool(abs(period - N_SYM) <= 2)
    corr = None

    # Symbol phase: the offset within one period where the CP correlation is
    # strongest marks a symbol boundary.
    offset = offset_bb if period else 0
    report["symbol_offset"] = int(offset)

    spectrum = occupied_bins(seg, offset)
    if spectrum is not None:
        # Report several thresholds — a single one hides whether the block has
        # 52 carriers with sloped edges or genuinely fewer.
        report["bandwidth_by_threshold"] = {}
        for db in (10, 20, 30, 40):
            edges = band_edges(spectrum, db)
            if edges:
                lo, hi = edges
                report["bandwidth_by_threshold"][f"-{db}dB"] = {
                    "bins": [lo, hi], "count": hi - lo + 1,
                    "hz": [round(lo * SPACING, 1), round(hi * SPACING, 1)],
                    "bw_hz": round((hi - lo + 1) * SPACING, 1),
                }
        e30 = band_edges(spectrum, 30.0)
        if e30:
            report["first_bin_guess"] = e30[0]
        report["carriers_expected"] = CARRIERS

    if verbose:
        print(f"\n=== burst {index} @ {start / FS:.3f}s  "
              f"{ms:.1f} ms ({ms / 24.0:.1f} symbol times) ===")
        if period:
            flag = "MATCHES SPEC" if report.get("matches_1152") else "!! unexpected"
            print(f"  CP period      : {period} samples "
                  f"({period / FS * 1000:.3f} ms)  sharpness={report.get('cp_sharpness')}"
                  f"  [expect 1152 / 24.000 ms]  {flag}")
        else:
            print("  CP period      : not resolved (burst too short?)")
        print(f"  centre est     : {report.get('center_hz_est')} Hz")
        for db, info in report.get("bandwidth_by_threshold", {}).items():
            mark = "  <== 52" if info["count"] == CARRIERS else ""
            print(f"  {db:>6} band   : bins {info['bins'][0]:>3}..{info['bins'][1]:<3}"
                  f" = {info['count']:>2} bins, {info['bw_hz']:>7.1f} Hz{mark}")
    return report, seg, offset


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wav")
    parser.add_argument("--burst", type=int, help="only analyse this burst index")
    parser.add_argument("--grid", help="save the demodulated grid as .npy")
    parser.add_argument("--json", help="write the full report as JSON")
    parser.add_argument("--floor", type=float, default=-45.0,
                        help="burst detector threshold in dB below peak")
    args = parser.parse_args()

    x = load_wav(args.wav)
    print(f"loaded {len(x)} samples ({len(x) / FS:.2f} s) at {FS} Hz")
    print(f"geometry: N_FFT={N_FFT} N_CP={N_CP} symbol={N_SYM} samples "
          f"({N_SYM / FS * 1000:.3f} ms), spacing={SPACING} Hz, "
          f"{CARRIERS} carriers = {CARRIERS * SPACING} Hz")

    bursts = find_bursts(x, floor_db=args.floor)
    print(f"\ndetected {len(bursts)} burst(s)")
    if not bursts:
        print("nothing above the threshold — try --floor -55")
        return 1

    reports = []
    for i, span in enumerate(bursts):
        if args.burst is not None and i != args.burst:
            continue
        rep, seg, offset = analyse_burst(x, span, i)
        reports.append(rep)
        if args.grid and args.burst is not None and rep.get("first_bin_guess") is not None:
            grid = build_grid(seg, offset, rep["first_bin_guess"])
            np.save(args.grid, grid)
            print(f"  saved grid {grid.shape} -> {args.grid}")

    # Burst-length histogram: DATA frames should cluster near the spec's
    # 5225 ms and ACK bursts near 842 ms. Anything else is worth a look.
    print("\n=== burst length summary ===")
    for rep in reports:
        ms = rep["length_ms"]
        if abs(ms - SPEC_FRAME_MS) < 400:
            kind = "DATA-frame-ish (spec 5225 ms)"
        elif abs(ms - 842.0) < 250:
            kind = "ACK-ish (spec 842 ms)"
        else:
            kind = "other"
        print(f"  burst {rep['burst']:>2}: {ms:>8.1f} ms   {kind}")

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(reports, handle, indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
