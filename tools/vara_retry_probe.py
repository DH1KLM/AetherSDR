#!/usr/bin/env python3
"""Corrupt a frame in flight during a LIVE session and watch the recovery.

§3.36 isolated the answering station's negative acknowledgement by replaying a
recording with the payload frame destroyed. A recording cannot retransmit, so the
exchange simply stopped there and the INITIATOR's reaction was never observed.

This runs a live session and injects noise into the forward path while it is in
progress. The noise is played into sink `varaA`, which the answering station
listens to, so it mixes with the initiator's transmission and corrupts what the
answering station hears. The initiator itself is unaffected and is free to react,
which is the whole point.

Both directions are recorded, so the retransmission, the acknowledgements around
it, and the timing are all visible.

WHY NOT CORRUPT THE RETURN PATH. Damaging the answering station's replies would
also force a retry, but through a different mechanism — a lost acknowledgement
rather than a failed frame — and the two are worth separating. This tool does the
forward path; --direction reverse does the other.

    tools/vara_retry_probe.py --out-dir DIR --at 8 --for 7
"""

import argparse
import os
import subprocess
import sys
import threading
import time
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap  # noqa: E402


def make_noise(path, seconds, amplitude=9000, seed=3):
    """Band-limited noise across the modem's occupied band.

    Shaped to 800-2400 Hz rather than white: out-of-band energy would be filtered
    out by the receiver anyway and would only make the injection louder than it
    needs to be.
    """
    n = int(48000 * seconds)
    rng = np.random.default_rng(seed)
    x = rng.normal(0, 1, n)
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1 / 48000)
    spec[(f < 800) | (f > 2400)] = 0
    y = np.fft.irfft(spec, n)
    y = y / (np.abs(y).max() + 1e-9) * amplitude
    with wave.open(path, "wb") as h:
        h.setnchannels(1)
        h.setsampwidth(2)
        h.setframerate(48000)
        h.writeframes(y.astype("<i2").tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--at", type=float, default=8.0,
                    help="seconds after the link forms before injecting")
    ap.add_argument("--for", dest="dur", type=float, default=7.0,
                    help="seconds of noise")
    ap.add_argument("--direction", choices=["forward", "reverse"],
                    default="forward",
                    help="forward corrupts what the answering station hears")
    ap.add_argument("--amplitude", type=int, default=9000)
    ap.add_argument("--bits", default="32,3,17,42")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)

    parts = [int(v) for v in a.bits.split(",")]
    ln, bits = parts[0], parts[1:]
    buf = bytearray(ln)
    for b in bits:
        buf[b // 8] |= 1 << (7 - (b % 8))
    payload = bytes(buf)

    noise = os.path.join(a.out_dir, "noise.wav")
    make_noise(noise, a.dur, a.amplitude)
    sink = "varaA" if a.direction == "forward" else "varaB"
    print(f"injecting {a.dur:.0f}s of 800-2400 Hz noise into {sink} "
          f"{a.at:.0f}s after the link forms (amplitude {a.amplitude})")

    # Drive the session inline rather than calling run_once. The host interface
    # is single-client, so opening our own connection to the answering station
    # while run_once also connects to it is refused — that is exactly what
    # happened on the first attempt. Owning both modems here lets us read the
    # answering station's DATA socket, which is the only direct evidence of
    # whether the payload actually got through. Inferring that from the reply
    # pattern was what let a too-quiet injection look like "no effect".
    import socket

    out_a = os.path.join(a.out_dir, "retry_a.wav")
    out_b = os.path.join(a.out_dir, "retry_b.wav")
    rec_a = cap.Recorder(out_a, "varaA.monitor")
    rec_b = cap.Recorder(out_b, "varaB.monitor")
    time.sleep(2)
    A = cap.Modem(8300, "A")
    B = cap.Modem(8310, "B")
    brx = []

    def drain():
        while True:
            try:
                c = B.data.recv(65536)
            except OSError:
                return
            if not c:
                return
            brx.append(c)
    threading.Thread(target=drain, daemon=True).start()

    fired = {"t": None}

    def inject():
        while not injector_ready.is_set():
            time.sleep(0.2)
        time.sleep(a.at)
        fired["t"] = time.time()
        subprocess.run(["paplay", f"--device={sink}", noise],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    injector_ready = threading.Event()
    threading.Thread(target=inject, daemon=True).start()

    err = None
    try:
        for m, call in ((B, "KK7GWY-8"), (A, "KK7GWY-9")):
            m.send("VERSION"); time.sleep(0.3)
            m.send(f"MYCALL {call}"); time.sleep(0.3)
            m.send("BW2300"); time.sleep(0.3)
            m.send("COMPRESSION OFF"); time.sleep(0.3)
        B.send("LISTEN ON"); time.sleep(0.6)
        A.send("CONNECT KK7GWY-9 KK7GWY-8")
        dl = time.time() + 120
        while time.time() < dl and not A.linked:
            time.sleep(0.4)
        if not A.linked:
            err = "no ARQ link"
        else:
            injector_ready.set()          # inject relative to the LINK, not startup
            time.sleep(3)
            A.data.sendall(payload)
            end = time.time() + 120
            drained = None
            while time.time() < end:
                if drained is None and any(r == "BUFFER 0" for r in A.buffer_reports[-3:]):
                    drained = time.time()
                if drained and time.time() - drained > 25:
                    break
                time.sleep(1)
            A.send("DISCONNECT"); time.sleep(8)
    finally:
        alog = list(A.log)
        A.close(); B.close()
        time.sleep(1)
        rec_a.stop(); rec_b.stop()
    if err:
        print(f"session failed: {err}")
    else:
        print(f"session completed; modem log tail: {alog[-6:]}")
    delivered = b"".join(brx)
    print(f"  payload delivered to the answering station: {len(delivered)} bytes"
          + (f" — {delivered[:16].hex()}" if delivered else "  <<< corruption took effect"))

    for label, path in (("A", out_a), ("B", out_b)):
        try:
            x = cap.load_wav(path)
            bursts = cap.split_bursts(x)
            sizes = [(b - aa) // 512 for aa, b in bursts]
            print(f"  side {label}: {len(x)/48000:.1f}s, {len(bursts)} bursts, {sizes}")
        except Exception as exc:
            print(f"  side {label}: unreadable ({exc})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
