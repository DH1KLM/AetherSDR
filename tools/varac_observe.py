#!/usr/bin/env python3
"""Observe VarAC's application protocol in the clear.

THE IDEA. VarAC speaks its own undocumented protocol *inside* a VARA ARQ link.
VARA hands the far end that payload decrypted and de-framed, so pointing VarAC
at one modem and this at another makes VarAC's protocol directly observable —
no decompiling, no disassembly, nothing but bytes on a socket we are entitled
to read. That is the clean-room route (Constitution Principle IV); see
docs/varac-cleanroom-design.md.

BENCH TOPOLOGY (tools/vara_bench_up.sh brings it up):

    VarAC  ->  VARA instance B (8310/8311)  ->  sink varaB
                                                   |
                                          varaB.monitor
                                                   v
    this   <-  VARA instance A (8300/8301)  <- captures

Run this, then drive VarAC normally: call CQ, send a chat line, park a VMail,
transfer a file. Every byte VarAC frames appears here, timestamped, annotated
with the modem events around it so each exchange can be attributed to the UI
action that caused it.

USAGE
    tools/varac_observe.py                       # listen on 8300, log to stdout
    tools/varac_observe.py --port 8300 --out session.jsonl --raw session.bin
    tools/varac_observe.py --mycall KK7GWY

WHAT TO CAPTURE, one feature per session so exchanges stay attributable:
    connect/disconnect handshake, CQ, beacon, broadcast, direct chat message,
    VMail send/park/forward, file transfer, alert tags, QSY negotiation,
    pathfinder, digipeater chaining, slot negotiation.

NOTE ON TRANSMIT. This tool only ever sends LISTEN and MYCALL, both
receive-side. It never issues CONNECT, CQFRAME or TUNE, and never writes to the
data channel, so it cannot key anything (Principle VI).
"""

import argparse
import json
import socket
import sys
import threading
import time

CR = b"\r"

# Modem messages that mark a session boundary or explain what the payload is
# doing. Logged inline so a byte run can be tied to the exchange around it.
INTERESTING = (
    "CONNECTED", "DISCONNECTED", "PENDING", "CANCELPENDING", "CQFRAME",
    "BITRATE", "SN ", "LINK", "ENCRYPTION", "BUFFER", "MISSING SOUNDCARD",
    "REGISTERED", "WRONG",
)


class Observer:
    def __init__(self, host, port, out_path, raw_path):
        self.start = time.monotonic()
        self.lock = threading.Lock()
        self.cmd = socket.create_connection((host, port), timeout=10)
        self.data = socket.create_connection((host, port + 1), timeout=10)
        # create_connection's timeout also governs recv(). A VARA link is
        # mostly silence between 5.2 s frames, so a read timeout would kill
        # these readers long before anything arrives. Block instead.
        self.cmd.settimeout(None)
        self.data.settimeout(None)
        self.running = True
        self.out = open(out_path, "a") if out_path else None
        self.raw = open(raw_path, "ab") if raw_path else None
        self.total = 0

    def log(self, kind, **fields):
        stamp = time.monotonic() - self.start
        with self.lock:
            if kind == "payload":
                data = fields["data"]
                print(f"[{stamp:9.3f}] PAYLOAD {len(data)} bytes", flush=True)
                print(hexdump(data), flush=True)
            else:
                print(f"[{stamp:9.3f}] {kind.upper():8} {fields.get('text', '')}",
                      flush=True)
            if self.out:
                rec = {"t": round(stamp, 3), "kind": kind}
                for key, value in fields.items():
                    rec[key] = value.decode("latin-1") if isinstance(value, bytes) else value
                self.out.write(json.dumps(rec) + "\n")
                self.out.flush()

    def send(self, text):
        self.log("send", text=text)
        self.cmd.sendall(text.encode("latin-1") + CR)

    def read_commands(self):
        buf = b""
        while self.running:
            try:
                chunk = self.cmd.recv(4096)
            except OSError as exc:
                self.log("error", text=f"command channel: {exc!r}")
                return
            if not chunk:
                self.log("error", text="command channel closed by modem")
                return
            buf += chunk
            while CR in buf:
                frame, buf = buf.split(CR, 1)
                if not frame:
                    continue
                msg = frame.decode("latin-1", "replace")
                if msg == "IAMALIVE":
                    continue  # keepalive noise
                if any(msg.startswith(p) for p in INTERESTING):
                    self.log("modem", text=msg)
                else:
                    self.log("modem-minor", text=msg)

    def read_payload(self):
        while self.running:
            try:
                chunk = self.data.recv(65536)
            except OSError as exc:
                self.log("error", text=f"data channel: {exc!r}")
                return
            if not chunk:
                self.log("error", text="data channel closed by modem")
                return
            self.total += len(chunk)
            if self.raw:
                self.raw.write(chunk)
                self.raw.flush()
            self.log("payload", data=chunk, size=len(chunk),
                     hex=chunk.hex(), text=safe_ascii(chunk))

    def close(self):
        self.running = False
        for handle in (self.cmd, self.data):
            try:
                handle.close()
            except OSError:
                pass
        for handle in (self.out, self.raw):
            if handle:
                handle.close()


def safe_ascii(data):
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data)


def hexdump(data, width=16):
    lines = []
    for offset in range(0, len(data), width):
        chunk = data[offset:offset + width]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        lines.append(f"    {offset:08x}  {hexpart:<{width * 3}} |{safe_ascii(chunk)}|")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Observe VarAC's application protocol through a VARA link.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8300,
                        help="command port of the OBSERVING modem (not VarAC's)")
    parser.add_argument("--mycall", help="register a callsign so VarAC can address us")
    parser.add_argument("--out", help="append structured events as JSON lines")
    parser.add_argument("--raw", help="append raw payload bytes to this file")
    parser.add_argument("--cq", action="store_true",
                        help="LISTEN CQ instead of LISTEN ON")
    args = parser.parse_args()

    try:
        obs = Observer(args.host, args.port, args.out, args.raw)
    except OSError as exc:
        print(f"cannot reach the modem on {args.host}:{args.port} — {exc}",
              file=sys.stderr)
        print("is the bench up? tools/vara_bench_up.sh", file=sys.stderr)
        return 1

    threading.Thread(target=obs.read_commands, daemon=True).start()
    threading.Thread(target=obs.read_payload, daemon=True).start()

    obs.send("VERSION")
    time.sleep(0.5)
    if args.mycall:
        obs.send(f"MYCALL {args.mycall}")
        time.sleep(0.3)
    obs.send("LISTEN CQ" if args.cq else "LISTEN ON")

    print("\n--- observing. Drive VarAC now; Ctrl-C to stop. ---\n", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        print(f"\n--- captured {obs.total} payload bytes ---", flush=True)
        obs.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
