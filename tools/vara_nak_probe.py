#!/usr/bin/env python3
"""Find out which acknowledgement frame means what, by breaking a frame on purpose.

§3.35 showed the answering station replies with 11-control-symbol frames and that
only three distinct contents occur. What none of the clean sessions can show is
which is an acknowledgement and which, if any, is a negative one — every clean
session succeeds, so every reply is presumably positive.

METHOD. Replay a recorded session into the listening instance twice: once
unmodified, and once with the payload DATA frame destroyed. The receiver decodes
everything else identically, so any reply that appears ONLY in the damaged run is
a response to a failed frame. The control run is what makes that inference
possible; without it a reply seen in the damaged run could simply be one the clean
session also produces.

The damage is applied to the payload frame only (burst 6), leaving the connect
handshake and every control frame byte-identical, so the receiver reaches the same
state before the failure.

    tools/vara_nak_probe.py --session dual_a.wav --out-dir /tmp/nak
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap  # noqa: E402

CR = b"\r"
B_PORT = 8310
DST = "KK7GWY-8"
CTRL_SYM = 2048
BINS = np.arange(29, 99)


def ctrl_symbols(seg):
    return [int(np.argmax(np.abs(np.fft.rfft(seg[i * CTRL_SYM:(i + 1) * CTRL_SYM]))[BINS]))
            for i in range(len(seg) // CTRL_SYM)]


class Listener:
    def __init__(self, port, attempts=25):
        last = None
        for _ in range(attempts):
            try:
                self.cmd = socket.create_connection(("127.0.0.1", port), timeout=10)
                break
            except OSError as exc:
                last = exc
                time.sleep(3)
        else:
            raise last
        self.data = socket.create_connection(("127.0.0.1", port + 1), timeout=10)
        self.cmd.settimeout(None)
        self.data.settimeout(None)
        self.log, self.rx, self.running = [], [], True
        threading.Thread(target=self._rc, daemon=True).start()
        threading.Thread(target=self._rd, daemon=True).start()

    def send(self, t):
        self.cmd.sendall(t.encode("latin-1") + CR)

    def _rc(self):
        buf = b""
        while self.running:
            try:
                c = self.cmd.recv(4096)
            except OSError:
                return
            if not c:
                return
            buf += c
            while CR in buf:
                fr, buf = buf.split(CR, 1)
                m = fr.decode("latin-1", "replace")
                if fr and m != "IAMALIVE":
                    self.log.append(m)

    def _rd(self):
        while self.running:
            try:
                c = self.data.recv(65536)
            except OSError:
                return
            if not c:
                return
            self.rx.append(c)

    def close(self):
        self.running = False
        for s in (self.cmd, self.data):
            try:
                s.close()
            except OSError:
                pass


def write_wav(path, x):
    with wave.open(path, "wb") as h:
        h.setnchannels(1)
        h.setsampwidth(2)
        h.setframerate(48000)
        h.writeframes(np.clip(x, -32768, 32767).astype("<i2").tobytes())


def damage(src, dst, burst=6, seed=1):
    """Replace the payload frame with noise of matching amplitude.

    Noise rather than silence: silence would look like a missing transmission and
    might be handled by a timeout path, whereas noise looks like a frame that
    arrived and failed to decode, which is the case of interest.
    """
    x = cap.load_wav(src)
    bursts = cap.split_bursts(x)
    if len(bursts) <= burst:
        raise SystemExit(f"session has only {len(bursts)} bursts")
    a, b = bursts[burst]
    y = x.astype(np.int64).copy()
    rng = np.random.default_rng(seed)
    amp = int(np.abs(x[a:b]).mean() * 1.5)
    y[a:b] = rng.integers(-amp, amp, b - a)
    write_wav(dst, y)
    return (b - a) // 512


def run(label, wav, out_b, hold=45):
    print(f"\n=== {label} ===", flush=True)
    rec = cap.Recorder(out_b, "varaB.monitor")
    time.sleep(1.5)
    L = Listener(B_PORT)
    try:
        L.send("VERSION"); time.sleep(0.4)
        L.send(f"MYCALL {DST}"); time.sleep(0.4)
        L.send("BW2300"); time.sleep(0.4)
        L.send("COMPRESSION OFF"); time.sleep(0.4)
        L.send("LISTEN ON"); time.sleep(1.0)
        subprocess.run(["paplay", "--device=varaA", wav],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(hold)
        got = b"".join(L.rx)
        interesting = [m for m in L.log if not m.startswith(("BUFFER", "PTT"))]
        print(f"  payload delivered : {len(got)} bytes")
        print(f"  modem messages    : {interesting[:14]}")
    finally:
        L.close()
        time.sleep(1)
        rec.stop()
    x = cap.load_wav(out_b)
    frames = []
    for a, b in cap.split_bursts(x):
        if (b - a) % CTRL_SYM:
            continue
        seg = x[a:b].astype(float) / 32768.0
        frames.append((( b - a) // CTRL_SYM, tuple(ctrl_symbols(seg))))
    print(f"  replies from the answering station: {len(frames)}")
    for n, sym in frames:
        print(f"    {n:2d} symbols: {list(sym)}")
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--burst", type=int, default=6)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    bad = os.path.join(a.out_dir, "damaged.wav")
    n = damage(a.session, bad, a.burst)
    print(f"payload frame at burst {a.burst} ({n} symbols) replaced with noise")

    clean = run("CONTROL — session replayed unmodified", a.session,
                os.path.join(a.out_dir, "reply_clean.wav"))
    time.sleep(20)
    dmg = run("TEST — payload frame destroyed", bad,
              os.path.join(a.out_dir, "reply_damaged.wav"))

    cs, ds = {f for f in clean}, {f for f in dmg}
    print("\n" + "=" * 66)
    print("Replies seen ONLY when the payload frame was destroyed:")
    only = ds - cs
    for n, sym in sorted(only):
        print(f"  {n:2d} symbols: {list(sym)}")
    if not only:
        print("  (none — the receiver replied identically either way)")
    print("\nReplies seen ONLY in the clean run:")
    for n, sym in sorted(cs - ds):
        print(f"  {n:2d} symbols: {list(sym)}")
    print("=" * 66)
    return 0


if __name__ == "__main__":
    sys.exit(main())
