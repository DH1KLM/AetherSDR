#!/usr/bin/env python3
"""Capture VARA DATA frames for controlled payloads — the FEC differential rig.

WHAT THIS IS FOR. VARA's forward error correction is the one layer of the low-rate
waveform still unrecovered. The modulation is solved exactly (16-ary orthogonal
CPFSK, 93.75 baud, DFT bins 9-24, 512-sample symbols, 407 symbols per DATA
frame — bit-exact resynthesis), so a recording can be turned into the exact
symbol sequence VARA transmitted. What is missing is the map from those symbols
back to payload bytes: the turbo generator polynomials, the interleaver, the
puncturing and any scrambler.

THE ATTACK. A turbo encoder is built entirely from GF(2)-linear parts — the RSC
encoders are LFSRs, the interleaver is a permutation, puncturing is a projection,
trellis termination is linear in the input, and CRC is linear. So the whole
encode chain is one binary generator matrix G, and:

    capture c0 = Encode(all-zero payload)
    capture ci = Encode(payload with only bit i set)
    row i of G  =  ci XOR c0

DATA frames carry 16 tones = exactly 4 bits per symbol, a clean power of two, so
symbol differences map onto bit differences without a base conversion. (The
35-value alphabet that obstructs this lives in CONTROL frames, not DATA frames —
see §3.21 of the clean-room note; conflating the two once cost a wrong decision.)

WHY IT IS SOUND HERE. The encoder is proven deterministic sample-exact across
sessions (§3.14): the identical payload produced byte-identical audio in two
independent runs, 0 differing samples of 208384. Without that, the subtraction
would be meaningless.

WHAT THIS TOOL DOES, AND ONLY THIS. For each requested payload it establishes an
ARQ link between the two bench instances, sends that payload, captures the
transmitter's audio, and stores the demodulated symbol sequence. It does not
attempt any algebra — that is deliberate. Get the captures right first.

    tools/vara_fec_capture.py --probe-sizes        # payload size -> frame count
    tools/vara_fec_capture.py --zeros 64 --out c0.json
    tools/vara_fec_capture.py --onebit 64,0 --out c1.json
"""

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time

import numpy as np

FS = 48000
SYM = 512
TONES = 16
SPACING = 93.75
CENTER = 1546.875
CR = b"\r"

SRC = "KK7GWY-9"
DST = "KK7GWY-8"
BW = 2300           # DATA-frame work uses the wide mode


# ── capture ────────────────────────────────────────────────────────────────
class Recorder:
    """parecord on a sink monitor. pw-record is NOT used: its --target silently
    falls back to the default source, which once captured a microphone instead
    of the bench."""

    def __init__(self, path, source="varaA.monitor"):
        self.path = path
        self.proc = subprocess.Popen(
            ["parecord", f"--device={source}", "--rate=48000", "--channels=1",
             "--format=s16le", "--file-format=wav", path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ── modem ──────────────────────────────────────────────────────────────────
class Modem:
    def __init__(self, port, label):
        self.label = label
        self.cmd = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.data = socket.create_connection(("127.0.0.1", port + 1), timeout=10)
        # create_connection's timeout also governs recv(); a VARA link is mostly
        # silence between frames, so block rather than time out.
        self.cmd.settimeout(None)
        self.data.settimeout(None)
        self.linked = False
        self.buffer_reports = []
        self.log = []
        self.running = True
        threading.Thread(target=self._read, daemon=True).start()

    def send(self, text):
        self.cmd.sendall(text.encode("latin-1") + CR)

    def _read(self):
        buf = b""
        while self.running:
            try:
                ch = self.cmd.recv(4096)
            except OSError:
                return
            if not ch:
                return
            buf += ch
            while CR in buf:
                fr, buf = buf.split(CR, 1)
                if not fr:
                    continue
                m = fr.decode("latin-1", "replace")
                if m == "IAMALIVE":
                    continue
                self.log.append(m)
                if m.startswith("CONNECTED"):
                    self.linked = True
                elif m == "DISCONNECTED":
                    self.linked = False
                elif m.startswith("BUFFER"):
                    self.buffer_reports.append(m)

    def close(self):
        self.running = False
        for s in (self.cmd, self.data):
            try:
                s.close()
            except OSError:
                pass


# ── demodulation (mirrors tools/vara_determinism.py) ────────────────────────
def load_wav(path):
    import wave
    with wave.open(path, "rb") as h:
        ch, w, rate = h.getnchannels(), h.getsampwidth(), h.getframerate()
        raw = h.readframes(h.getnframes())
    if w != 2 or rate != FS:
        raise SystemExit(f"{path}: need 16-bit/{FS} Hz")
    x = np.frombuffer(raw, dtype="<i2").astype(np.int32)
    return x.reshape(-1, ch)[:, 0] if ch > 1 else x


def split_bursts(x, min_zero=2048):
    """Bursts are separated by EXACT digital zeros, so no threshold is involved.
    Starts are snapped onto the 512-sample grid: every symbol is
    A*sin(2*pi*k*n/512), so a burst's first sample is an exact zero and
    first-nonzero detection lands late."""
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
        for back in range(8):
            if a - back >= 0 and (b - (a - back)) % SYM == 0:
                a -= back
                break
        out.append((a, b))
    return out


def demod(seg):
    freqs = CENTER + (np.arange(TONES) - (TONES - 1) / 2.0) * SPACING
    t = np.arange(SYM) / FS
    ref = np.exp(-2j * np.pi * np.outer(freqs, t))
    n = len(seg) // SYM
    syms = np.zeros(n, dtype=int)
    conf = np.zeros(n)
    for i in range(n):
        mags = np.abs(ref @ seg[i * SYM:(i + 1) * SYM])
        order = np.argsort(mags)[::-1]
        syms[i] = order[0]
        conf[i] = mags[order[0]] / max(mags[order[1]], 1e-12)
    return syms, conf


def data_frames(path):
    x = load_wav(path)
    out = []
    for a, b in split_bursts(x):
        n = (b - a) // SYM
        if (b - a) % SYM or n < 200:
            continue                      # DATA frames are 407 symbols
        syms, conf = demod(x[a:b].astype(np.float64) / 32768.0)
        out.append(dict(start=a, symbols=syms.tolist(), n=len(syms),
                        median_conf=float(np.median(conf))))
    return out


# ── one experiment ─────────────────────────────────────────────────────────
def run_once(payload, wav_path, hold=150.0):
    """Link, send payload, capture the transmitter's audio, demodulate."""
    rec = Recorder(wav_path, "varaA.monitor")   # instance A transmits into varaA
    time.sleep(2)
    a = Modem(8300, "A")
    b = Modem(8310, "B")
    try:
        b.send("VERSION"); time.sleep(0.3)
        b.send(f"MYCALL {DST}"); time.sleep(0.3)
        b.send(f"BW{BW}"); time.sleep(0.3)
        b.send("COMPRESSION OFF"); time.sleep(0.3)
        b.send("LISTEN ON"); time.sleep(0.5)

        a.send("VERSION"); time.sleep(0.3)
        a.send(f"MYCALL {SRC}"); time.sleep(0.3)
        a.send(f"BW{BW}"); time.sleep(0.3)
        # Compression MUST be off or the payload is transformed before the FEC,
        # and the differential is against the wrong input.
        a.send("COMPRESSION OFF"); time.sleep(0.3)
        a.send(f"CONNECT {SRC} {DST}")

        dl = time.time() + 120
        while time.time() < dl and not a.linked:
            time.sleep(0.5)
        if not a.linked:
            return None, "no ARQ link"

        time.sleep(4)
        a.data.sendall(payload)

        # Wait for the TX buffer to drain, then let the last frame finish.
        drained_at = None
        end = time.time() + hold
        while time.time() < end:
            if any(r == "BUFFER 0" for r in a.buffer_reports[-3:]) and drained_at is None:
                drained_at = time.time()
            if drained_at and time.time() - drained_at > 20:
                break
            time.sleep(1)

        a.send("DISCONNECT")
        time.sleep(8)
        return dict(buffer_reports=a.buffer_reports[:], cmd_log=a.log[:]), None
    finally:
        a.close(); b.close()
        time.sleep(1)
        rec.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--zeros", type=int, help="payload of N zero bytes")
    p.add_argument("--onebit", metavar="N,BIT",
                   help="N zero bytes with a single bit set (bit 0 = MSB of byte 0)")
    p.add_argument("--bits", metavar="N,B1,B2,...",
                   help="N zero bytes with several bits set. The linearity test: "
                        "if the code is GF(2)-linear then "
                        "c(bit i) XOR c(bit j) XOR c(bits i+j) = Encode(0), which "
                        "both proves linearity and DERIVES the all-zero baseline "
                        "that cannot be captured (an all-zero payload emits no "
                        "payload frame). It also tests the tone->bit mapping: the "
                        "identity fails under the wrong mapping.")
    p.add_argument("--probe-sizes", action="store_true",
                   help="map payload size to DATA-frame count")
    p.add_argument("--out", help="write the demodulated result here as JSON")
    p.add_argument("--wav", help="keep the capture at this path")
    a = p.parse_args()

    scratch = os.path.dirname(os.path.abspath(a.out or "./fec.json"))

    if a.probe_sizes:
        # The differential needs payloads that produce exactly ONE DATA frame,
        # so alignment is unambiguous. Find the size range that does.
        print(f"{'bytes':>6} {'frames':>7} {'symbols':>9} {'conf':>9}")
        for n in (16, 32, 64, 96, 128, 192):
            wav = os.path.join(scratch, f"probe_{n}.wav")
            info, err = run_once(b"\x00" * n, wav, hold=180)
            if err:
                print(f"{n:>6} {'-':>7}  {err}")
                continue
            fr = data_frames(wav)
            print(f"{n:>6} {len(fr):>7} "
                  f"{','.join(str(f['n']) for f in fr) or '-':>9} "
                  f"{max((f['median_conf'] for f in fr), default=0):>9.0f}")
            time.sleep(6)
        return 0

    if a.zeros is not None:
        payload = b"\x00" * a.zeros
        tag = f"zeros{a.zeros}"
    elif a.bits:
        parts = [int(v) for v in a.bits.split(",")]
        ln, bits = parts[0], parts[1:]
        if not bits:
            raise SystemExit("--bits needs at least one bit index after the length")
        bad = [b for b in bits if b >= ln * 8]
        if bad:
            raise SystemExit(f"bit(s) {bad} outside a {ln}-byte payload")
        buf = bytearray(ln)
        for b in bits:
            buf[b // 8] |= 1 << (7 - (b % 8))
        payload = bytes(buf)
        tag = f"bits{'+'.join(str(b) for b in bits)}of{ln}"
    elif a.onebit:
        ln, _, bit = a.onebit.partition(",")
        ln, bit = int(ln), int(bit)
        if bit >= ln * 8:
            raise SystemExit(f"bit {bit} outside a {ln}-byte payload")
        buf = bytearray(ln)
        buf[bit // 8] = 1 << (7 - (bit % 8))
        payload, tag = bytes(buf), f"bit{bit}of{ln}"
    else:
        p.error("need --zeros, --onebit, --bits or --probe-sizes")

    wav = a.wav or os.path.join(scratch, f"fec_{tag}.wav")
    info, err = run_once(payload, wav)
    if err:
        print(f"FAILED: {err}", file=sys.stderr)
        return 2
    frames = data_frames(wav)
    print(f"{tag}: {len(frames)} DATA frame(s); "
          f"symbols={[f['n'] for f in frames]}; "
          f"conf={[round(f['median_conf']) for f in frames]}")
    result = dict(tag=tag, payload_hex=payload.hex(), wav=wav,
                  frames=frames, buffer_reports=info["buffer_reports"])
    if a.out:
        with open(a.out, "w") as h:
            json.dump(result, h)
        print(f"wrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
