#!/usr/bin/env python3
"""Does the connect frame carry the callsigns?

This decides whether a native implementation is possible at all with what is
currently known. Control frames are payload-independent and drawn from a small
vocabulary, so a modem could in principle REPLAY captured ones. But if the
connect frame encodes the source and destination callsigns, replaying someone
else's frame would announce the wrong station, and a native modem must be able
to GENERATE it — which needs the 70-ary control-frame coding, still unrecovered.

METHOD. Run sessions that differ ONLY in the callsigns, and compare the connect
frames symbol by symbol. Everything else is held constant: same payload, same
bandwidth, same bench, same modem instances. A control pair with identical
callsigns establishes what "no change" looks like, since the connect frame was
already observed to vary between sessions for other reasons.

    tools/vara_callsign_probe.py --data DIR
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap  # noqa: E402

CTRL = 2048
BINS = np.arange(29, 99)


def ctrl_symbols(seg):
    return [int(np.argmax(np.abs(np.fft.rfft(seg[i * CTRL:(i + 1) * CTRL]))[BINS]))
            for i in range(len(seg) // CTRL)]


def session(wav, src, dst, payload=b"\x00" * 32, hold=60.0):
    """One ARQ session with the given callsigns, recording the initiator."""
    rec = cap.Recorder(wav, "varaA.monitor")
    time.sleep(2)
    a = cap.Modem(8300, "A")
    b = cap.Modem(8310, "B")
    try:
        for m, call in ((b, dst), (a, src)):
            m.send("VERSION"); time.sleep(0.3)
            m.send(f"MYCALL {call}"); time.sleep(0.3)
            m.send("BW2300"); time.sleep(0.3)
            m.send("COMPRESSION OFF"); time.sleep(0.3)
        b.send("LISTEN ON"); time.sleep(0.6)
        a.send(f"CONNECT {src} {dst}")
        dl = time.time() + 90
        while time.time() < dl and not a.linked:
            time.sleep(0.4)
        if not a.linked:
            return None
        time.sleep(3)
        a.data.sendall(payload)
        end = time.time() + hold
        drained = None
        while time.time() < end:
            if drained is None and any(r == "BUFFER 0" for r in a.buffer_reports[-3:]):
                drained = time.time()
            if drained and time.time() - drained > 15:
                break
            time.sleep(1)
        a.send("DISCONNECT"); time.sleep(6)
        return True
    finally:
        a.close(); b.close()
        time.sleep(1)
        rec.stop()


def connect_frame(wav):
    """The 41-control-symbol connect frame: a 9-symbol acquisition preamble
    followed by an ordinary 32-symbol frame."""
    x = cap.load_wav(wav)
    for a, b in cap.split_bursts(x):
        n = b - a
        if n % CTRL or n // CTRL != 41:
            continue
        return ctrl_symbols(x[a:b].astype(float) / 32768.0)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    a = ap.parse_args()
    os.makedirs(a.data, exist_ok=True)

    trials = [
        ("base_a", "KK7GWY-9", "KK7GWY-8"),
        ("base_b", "KK7GWY-9", "KK7GWY-8"),   # control: identical callsigns
        ("altsrc", "KK7GWY-1", "KK7GWY-8"),   # source differs only
        ("altdst", "KK7GWY-9", "KK7GWY-2"),   # destination differs only
        ("both",   "W1AW",     "VK2XYZ"),     # both differ, different lengths
    ]
    frames = {}
    for name, src, dst in trials:
        wav = os.path.join(a.data, f"call_{name}.wav")
        print(f"=== {name}: {src} -> {dst} ===", flush=True)
        if not os.path.exists(wav) or os.path.getsize(wav) < 1_000_000:
            if not session(wav, src, dst):
                print("  no link; skipping")
                continue
            time.sleep(6)
        f = connect_frame(wav)
        if f is None:
            print("  no 41-symbol connect frame in the capture")
            continue
        frames[name] = f
        print(f"  connect frame: {f[:12]} ...")

    print("\n" + "=" * 70)
    print("Does the connect frame change with the callsigns?")
    print("=" * 70)
    if "base_a" not in frames:
        print("no baseline captured")
        return 1
    base = frames["base_a"]
    for name in ("base_b", "altsrc", "altdst", "both"):
        if name not in frames:
            continue
        f = frames[name]
        diff = sum(1 for i in range(min(len(base), len(f))) if base[i] != f[i])
        pre = sum(1 for i in range(9) if base[i] != f[i])
        print(f"  {name:8} vs base_a: {diff:2d}/41 symbols differ "
              f"({pre} of them in the 9-symbol preamble)")
    print("\nThe control pair (base_b) shows how much a connect frame varies when")
    print("NOTHING changes. Any callsign trial differing by materially more than")
    print("that is carrying the callsign; a trial differing by the same amount is")
    print("not evidence of anything.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
