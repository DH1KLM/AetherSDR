#!/usr/bin/env python3
"""Prove the Phase 2 capability: read an ARQ payload as plaintext.

Instance B connects to instance A and sends a known payload. A's data socket
should deliver exactly those bytes. That proves the transport end to end: any
application layer riding inside a VARA ARQ link is carried byte-transparently,
so a payload written at one end arrives unaltered at the other.

Unlike the earlier harness, this one reports reader-thread failures instead of
letting them die silently, which is what previously hid a successful transfer.
"""
import socket
import sys
import threading
import time

CR = b"\r"
start = time.monotonic()
lock = threading.Lock()
received = bytearray()
events = {"A": [], "B": []}


def emit(who, text):
    with lock:
        print(f"[{time.monotonic() - start:7.2f}] {who}  {text}", flush=True)


class Modem:
    def __init__(self, name, port):
        self.name = name
        self.cmd = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.data = socket.create_connection(("127.0.0.1", port + 1), timeout=10)
        # create_connection's timeout also applies to recv(). A VARA link is
        # mostly silence punctuated by 5.2 s frames, so a read timeout kills the
        # reader long before anything arrives — which is exactly what hid a
        # successful 35-byte transfer earlier. Block indefinitely instead.
        self.cmd.settimeout(None)
        self.data.settimeout(None)
        self.running = True
        for target in (self._read_cmd, self._read_data):
            threading.Thread(target=self._guard(target), daemon=True).start()

    def _guard(self, fn):
        def wrapper():
            try:
                fn()
            except Exception as exc:  # never die silently again
                emit(self.name, f"!! reader thread failed: {exc!r}")
        return wrapper

    def send(self, text):
        emit(self.name, f"-> {text}")
        self.cmd.sendall(text.encode("latin-1") + CR)

    def _read_cmd(self):
        buf = b""
        while self.running:
            chunk = self.cmd.recv(4096)
            if not chunk:
                emit(self.name, "cmd channel closed")
                return
            buf += chunk
            while CR in buf:
                frame, buf = buf.split(CR, 1)
                if frame:
                    msg = frame.decode("latin-1", "replace")
                    events[self.name].append(msg)
                    if msg != "IAMALIVE":
                        emit(self.name, f"<- {msg}")

    def _read_data(self):
        while self.running:
            chunk = self.data.recv(4096)
            if not chunk:
                emit(self.name, "data channel closed")
                return
            with lock:
                received.extend(chunk)
            emit(self.name, f"<== PLAINTEXT {len(chunk)} bytes: {chunk!r}")

    def close(self):
        self.running = False
        for s in (self.cmd, self.data):
            try:
                s.close()
            except OSError:
                pass


PAYLOAD = b"AETHERSDR PHASE2 PLAINTEXT READ 0123456789\r\n"


def main():
    a = Modem("A", 8300)   # receives
    b = Modem("B", 8310)   # sends
    time.sleep(1)

    a.send("MYCALL KK7GWY")
    b.send("MYCALL KK7GWY-1")
    time.sleep(1)
    for m in (a, b):
        m.send("BW2300")
    time.sleep(1)
    a.send("LISTEN ON")
    time.sleep(2)
    b.send("CONNECT KK7GWY-1 KK7GWY")

    deadline = time.time() + 120
    while time.time() < deadline:
        if any(m.startswith("CONNECTED") for m in events["A"] + events["B"]):
            break
        time.sleep(1)
    else:
        emit("**", "no link")
        a.close(); b.close()
        return 1

    emit("**", "linked; settling")
    time.sleep(10)
    emit("**", f"sending {len(PAYLOAD)} bytes on B's data channel")
    b.data.sendall(PAYLOAD)

    # 175 bps with 5.2 s ARQ frames: allow generous time.
    deadline = time.time() + 150
    while time.time() < deadline:
        with lock:
            if PAYLOAD in bytes(received):
                break
        time.sleep(2)

    with lock:
        got = bytes(received)

    ok = PAYLOAD in got
    emit("**", f"received {len(got)} bytes on A's data channel")
    emit("**", f"PAYLOAD MATCH: {ok}")
    if got:
        emit("**", f"content: {got!r}")

    b.send("DISCONNECT")
    time.sleep(6)
    a.close(); b.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
