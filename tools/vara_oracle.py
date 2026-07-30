#!/usr/bin/env python3
"""End-to-end proof: synthesise a DATA frame for a payload never transmitted,
splice it into a real session, and see whether a real VARA decodes it.

Every result so far is self-referential — the model predicts captures, and the
captures are what built the model. This is the test that is not: a genuine VARA
modem, which knows nothing about any of this, is asked to decode a waveform that
AetherSDR generated for a payload the modem has never seen.

METHOD. A recorded session is replayed into the listening instance's input sink.
The payload-bearing DATA frame (burst 6) is replaced with one synthesised from
the codec. Everything else — the connect handshake, the control frames, the
session frame — is the original recording, so the receiver sees a well-formed
session.

CONTROL FIRST. Trial 1 replays the recording UNMODIFIED. If the receiver does not
decode the original all-zero payload from that, the replay path itself is broken
and no conclusion can be drawn from trial 2. Running the experiment without this
control would make a negative result uninterpretable.

BENCH ROUTING (tools/vara_bench_up.sh):
    instance A  8300/8301   out -> sink varaA,  in <- varaB.monitor
    instance B  8310/8311   out -> sink varaB,  in <- varaA.monitor
so feeding instance B means playing into sink varaA.
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time
import wave

import numpy as np

S = "/tmp/claude-1000/-home-jeremy-AetherSDR/dba67c7c-7678-4762-97b3-32d25143fd29/scratchpad"
sys.path.insert(0, os.path.join(S, "wt-varac", "tools"))
import vara_fec_capture as cap          # noqa: E402
import vara_synth as vs                 # noqa: E402
import vara_codec as vc                 # noqa: E402

CR = b"\r"
B_PORT = 8310
DST = "KK7GWY-8"
BASE = os.path.join(S, "probe_32.wav")
PAYLOAD_BITS = [3, 17, 42]              # bits set in the target 32-byte payload


class Listener:
    def __init__(self, port, attempts=20):
        # The host interface is single-client and briefly refuses connections
        # after a session tears down, so retry rather than treating the first
        # refusal as fatal — that aborted an earlier run between trials.
        last = None
        for i in range(attempts):
            try:
                self.cmd = socket.create_connection(("127.0.0.1", port), timeout=10)
                break
            except OSError as e:
                last = e
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


def splice(base_wav, out_wav, symbols, taper):
    x = cap.load_wav(base_wav)
    a, b = cap.split_bursts(x)[6]
    assert (b - a) == 407 * 512, f"burst 6 is {b-a} samples"
    frame = vs.synthesise(symbols, taper).astype(np.int64)
    y = x.astype(np.int64).copy()
    y[a:b] = frame
    y = np.clip(y, -32768, 32767).astype("<i2")
    with wave.open(out_wav, "wb") as h:
        h.setnchannels(1)
        h.setsampwidth(2)
        h.setframerate(48000)
        h.writeframes(y.tobytes())
    return int((x[a:b] != frame).sum())


def trial(name, wav, expect_hex):
    print(f"\n=== {name} ===", flush=True)
    L = Listener(B_PORT)
    try:
        L.send("VERSION"); time.sleep(0.4)
        L.send(f"MYCALL {DST}"); time.sleep(0.4)
        L.send("BW2300"); time.sleep(0.4)
        L.send("COMPRESSION OFF"); time.sleep(0.4)
        L.send("LISTEN ON"); time.sleep(1.0)
        p = subprocess.Popen(["paplay", "--device=varaA", wav],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        p.wait()
        time.sleep(25)
        got = b"".join(L.rx)
        print(f"  bytes delivered by the modem : {len(got)}")
        if got:
            print(f"  first 48 bytes hex : {got[:48].hex()}")
        print(f"  expected payload   : {expect_hex[:48]}...")
        ok = got.startswith(bytes.fromhex(expect_hex)) if got else False
        print(f"  MATCHES EXPECTED   : {ok}")
        interesting = [m for m in L.log
                       if not m.startswith(("BUFFER", "PTT", "IAMALIVE"))]
        print(f"  modem messages     : {interesting[:12]}")
        return ok, got
    finally:
        L.close()


def main():
    base, rows_raw = vc.load_sweep(os.path.join(S, "fec_sweep"), 32)
    if base is None:
        print("no sweep baseline", file=sys.stderr)
        return 2
    names = {"z": "probe_64.wav", "e0": "fec_c1.wav", "e1": "fec_c2.wav",
             "e2": "fec_b2.wav", "e3": "fec_b3.wav", "e32": "v_b32.wav",
             "e255": "v_b255.wav", "e01": "fec_b01.wav", "e02": "d3_b02.wav",
             "e03": "t2_b03.wav", "e12": "d3_b12.wav", "e13": "t2_b13.wav",
             "e23": "fec_b23.wav", "e0_32": "v_b0_32.wav",
             "e2_32": "p_2_32.wav", "e3_32": "p_3_32.wav",
             "e0_255": "v_b0_255.wav", "e1_255": "p_1_255.wav",
             "e2_255": "p_2_255.wav", "e3_255": "p_3_255.wav",
             "e32_255": "p_32_255.wav"}
    caps = {k: v for k, v in
            ((k, vc.symbols_of(os.path.join(S, f))) for k, f in names.items())
            if v is not None}
    ids = [t for t in [("e0", "e1", "e01"), ("e0", "e2", "e02"),
                       ("e0", "e3", "e03"), ("e1", "e2", "e12"),
                       ("e1", "e3", "e13"), ("e2", "e3", "e23"),
                       ("e0", "e32", "e0_32"), ("e2", "e32", "e2_32"),
                       ("e3", "e32", "e3_32"), ("e0", "e255", "e0_255"),
                       ("e1", "e255", "e1_255"), ("e2", "e255", "e2_255"),
                       ("e3", "e255", "e3_255"), ("e32", "e255", "e32_255")]
           if all(k in caps for k in t)]
    r = vc.choose_offsets(vc.offset_sets(caps, ids))
    rows = {i: ((v - r) % 16) ^ ((base - r) % 16) for i, v in rows_raw.items()}
    missing = [b for b in PAYLOAD_BITS if b not in rows]
    if missing:
        print(f"no generator row yet for payload bits {missing}", file=sys.stderr)
        return 2
    codec = vc.Codec(r, base, rows)
    syms = codec.encode(PAYLOAD_BITS)

    payload = bytearray(32)
    for b in PAYLOAD_BITS:
        payload[b // 8] |= 1 << (7 - (b % 8))
    print(f"target payload : {payload.hex()}")
    print(f"synthesised {len(syms)} symbols; differs from the all-zero frame at "
          f"{int((syms != base).sum())}/407 positions")

    taper, used = vs.measure_taper(
        [os.path.join(S, f) for f in
         ("probe_64.wav", "fec_c1.wav", "fec_b01.wav", "fec_b2.wav",
          "v_b255.wav", "probe_32.wav")])
    out = os.path.join(S, "oracle_spliced.wav")
    nd = splice(BASE, out, syms, taper)
    print(f"spliced into {os.path.basename(BASE)}: {nd} samples changed")

    ok1, _ = trial("CONTROL — replay the ORIGINAL recording unmodified",
                   BASE, "00" * 32)
    if not ok1:
        print("\nThe control did not decode. The replay path is not working, so")
        print("trial 2 cannot be interpreted. Stopping.")
        return 1
    time.sleep(30)
    ok2, _ = trial("TEST — replay with a SYNTHESISED payload frame",
                   out, payload.hex())
    print("\n" + "=" * 66)
    print(f"control (original replays correctly) : {ok1}")
    print(f"synthesised frame decoded correctly  : {ok2}")
    if ok1 and ok2:
        print("\nA real VARA modem decoded a waveform AetherSDR generated for a")
        print("payload it had never transmitted. The level-4 DATA encode path is")
        print("proven end to end.")
    print("=" * 66)
    return 0


if __name__ == "__main__":
    sys.exit(main())
