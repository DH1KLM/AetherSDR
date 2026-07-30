#!/usr/bin/env python3
"""Connect TO a real VarAC instance and capture its application protocol.

WHY THIS DIRECTION. Driving VarAC's GUI headlessly to make it dial us proved
unreliable: bare Xvfb has no window manager, so keyboard focus cannot be held
on a VB6 text field. Initiating from our side sidesteps the GUI entirely — and
it is the better experiment anyway, because as the caller we observe VarAC's
*responses*, which is where its protocol reveals itself.

TOPOLOGY (tools/vara_bench_up.sh, then VarAC on instance B):

    us  -> VARA instance A (8300/8301) -> sink varaA
                                            |
                                     varaA.monitor
                                            v
    VarAC <- VARA instance B (8310/8311) <- captures

We issue CONNECT from instance A to VarAC's callsign. VarAC's modem answers the
ARQ handshake, then VarAC itself sends whatever application-layer greeting its
protocol specifies. Every byte on our data socket is VarAC talking.

Nothing here transmits without being asked: one CONNECT, then listen.
"""

import argparse
import json
import socket
import sys
import threading
import time

CR = b"\r"


def ascii_of(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)


def hexdump(data, width=16):
    out = []
    for off in range(0, len(data), width):
        ch = data[off:off + width]
        out.append(f"    {off:08x}  {' '.join(f'{c:02x}' for c in ch):<{width*3}} |{ascii_of(ch)}|")
    return "\n".join(out)


class Probe:
    def __init__(self, host, port, out=None, raw=None):
        self.t0 = time.monotonic()
        self.lock = threading.Lock()
        self.cmd = socket.create_connection((host, port), timeout=10)
        self.data = socket.create_connection((host, port + 1), timeout=10)
        # create_connection's timeout also governs recv(); a VARA link is mostly
        # silence between 5.2 s frames, so block instead of timing out.
        self.cmd.settimeout(None)
        self.data.settimeout(None)
        self.out = open(out, "a") if out else None
        self.raw = open(raw, "ab") if raw else None
        self.total = 0
        self.linked = False
        self.running = True

    def log(self, kind, text="", data=None):
        t = time.monotonic() - self.t0
        with self.lock:
            if data is not None:
                print(f"[{t:8.2f}] {kind:9} {len(data)} bytes", flush=True)
                print(hexdump(data), flush=True)
            else:
                print(f"[{t:8.2f}] {kind:9} {text}", flush=True)
            if self.out:
                rec = {"t": round(t, 2), "kind": kind, "text": text}
                if data is not None:
                    rec["hex"] = data.hex()
                    rec["ascii"] = ascii_of(data)
                self.out.write(json.dumps(rec) + "\n")
                self.out.flush()

    def send(self, s):
        self.log("TX-CMD", s)
        self.cmd.sendall(s.encode("latin-1") + CR)

    def read_cmd(self):
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
                if not fr:
                    continue
                m = fr.decode("latin-1", "replace")
                if m == "IAMALIVE":
                    continue
                if m.startswith("CONNECTED"):
                    self.linked = True
                if m == "DISCONNECTED":
                    self.linked = False
                self.log("RX-CMD", m)

    def read_data(self):
        while self.running:
            try:
                c = self.data.recv(65536)
            except OSError:
                return
            if not c:
                return
            self.total += len(c)
            if self.raw:
                self.raw.write(c)
                self.raw.flush()
            self.log("VARAC-TX", data=c)

    def close(self):
        self.running = False
        for s in (self.cmd, self.data):
            try:
                s.close()
            except OSError:
                pass
        for h in (self.out, self.raw):
            if h:
                h.close()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=8300, help="OUR modem's command port")
    p.add_argument("--mycall", default="KK7GWY-9")
    p.add_argument("--target", default="KK7GWY", help="VarAC's callsign")
    p.add_argument("--bw", type=int, default=500, choices=[500, 2300, 2750],
                   help="MUST match VarAC's modem bandwidth. VarAC's own log "
                        "records 'Setting Bandwith to 500Hz' on startup; a "
                        "mismatch makes an ARQ link impossible and looks like "
                        "an unexplained connect failure.")
    p.add_argument("--hold", type=float, default=120.0,
                   help="seconds to stay linked, listening")
    p.add_argument("--say", help="send this text as a payload once linked")
    p.add_argument("--out"), p.add_argument("--raw")
    a = p.parse_args()

    try:
        pr = Probe("127.0.0.1", a.port, a.out, a.raw)
    except OSError as e:
        print(f"cannot reach our modem on {a.port}: {e}", file=sys.stderr)
        return 1

    threading.Thread(target=pr.read_cmd, daemon=True).start()
    threading.Thread(target=pr.read_data, daemon=True).start()

    pr.send("VERSION"); time.sleep(0.5)
    pr.send(f"MYCALL {a.mycall}"); time.sleep(0.4)
    pr.send(f"BW{a.bw}"); time.sleep(0.4)
    pr.send("COMPRESSION OFF"); time.sleep(0.4)   # keep bytes as VarAC framed them
    pr.log("SYS", f"dialling VarAC at {a.target}")
    pr.send(f"CONNECT {a.mycall} {a.target}")

    deadline = time.time() + 150
    while time.time() < deadline and not pr.linked:
        time.sleep(1)
    if not pr.linked:
        pr.log("SYS", "no ARQ link established")
        pr.close()
        return 2

    pr.log("SYS", "LINKED — listening for VarAC's application protocol")
    if a.say:
        time.sleep(20)
        pr.log("SYS", f"sending our own payload: {a.say!r}")
        pr.data.sendall(a.say.encode() + b"\r")

    end = time.time() + a.hold
    while time.time() < end:
        time.sleep(2)

    pr.send("DISCONNECT"); time.sleep(8)
    pr.log("SYS", f"captured {pr.total} payload bytes from VarAC")
    pr.close()
    return 0 if pr.total else 3


if __name__ == "__main__":
    sys.exit(main())
