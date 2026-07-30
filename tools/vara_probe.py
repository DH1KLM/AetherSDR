#!/usr/bin/env python3
"""Drive a VARA modem over its published TCP host interface.

Three jobs, all on the bench:

  * `listen`  — sit on the link and print every command-channel message and
                every payload byte, timestamped. Whatever application drives
                the far-end modem, its payload arrives here in the clear:
                VARA delivers the ARQ payload de-framed and error-corrected,
                so this is a transparent view of the layer above.
  * `send`    — hand the modem an exact payload. Known-plaintext injection for
                the waveform work: send all-zeros, then a single set bit, and
                diff the recorded audio.
  * `command` — issue raw modem commands and show the replies.

No Qt dependency, standard library only, so it runs anywhere the bench does.

TRANSMIT WARNING. `connect`, `cq` and any `send` will key the transmitter that
the modem is driving. Nothing here transmits on its own — every transmission
follows an explicit argument you passed. Keep the bench on a dummy load.

VARA host-interface reference: docs/vara-cleanroom-design.md §3.5.
Wire vocabulary mirrors src/core/VaraProtocol.{h,cpp}.
"""

import argparse
import socket
import sys
import threading
import time

DEFAULT_HOST = "127.0.0.1"
DEFAULT_COMMAND_PORT = 8300

# The modem terminates each command-channel message with a bare CR. Not CRLF.
TERMINATOR = b"\r"

_start = time.monotonic()
_print_lock = threading.Lock()


def stamp() -> str:
    return f"{time.monotonic() - _start:9.3f}"


def emit(channel: str, text: str) -> None:
    with _print_lock:
        print(f"[{stamp()}] {channel:<7} {text}", flush=True)


def hexdump(data: bytes, width: int = 16) -> str:
    """Offset / hex / ASCII, so payload structure is visible at a glance."""
    lines = []
    for offset in range(0, len(data), width):
        chunk = data[offset : offset + width]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"    {offset:08x}  {hexpart:<{width * 3}} |{asciipart}|")
    return "\n".join(lines)


class VaraProbe:
    def __init__(self, host: str, command_port: int, data_port: int | None):
        self.host = host
        self.command_port = command_port
        # The modem's own convention: data port is command port + 1.
        self.data_port = data_port if data_port else command_port + 1
        self.command_sock: socket.socket | None = None
        self.data_sock: socket.socket | None = None
        self.running = True
        self.raw_data_log = None

    def connect(self) -> None:
        self.command_sock = socket.create_connection((self.host, self.command_port), timeout=10)
        emit("SYS", f"command channel connected to {self.host}:{self.command_port}")
        self.data_sock = socket.create_connection((self.host, self.data_port), timeout=10)
        emit("SYS", f"data channel connected to {self.host}:{self.data_port}")

    def close(self) -> None:
        self.running = False
        for sock in (self.command_sock, self.data_sock):
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass
        if self.raw_data_log:
            self.raw_data_log.close()

    def send_command(self, text: str) -> None:
        if not self.command_sock:
            return
        emit("TX-CMD", text)
        self.command_sock.sendall(text.encode("latin-1") + TERMINATOR)

    def send_data(self, payload: bytes) -> None:
        if not self.data_sock:
            return
        emit("TX-DATA", f"{len(payload)} bytes")
        print(hexdump(payload[:256]), flush=True)
        if len(payload) > 256:
            print(f"    ... {len(payload) - 256} more bytes", flush=True)
        self.data_sock.sendall(payload)

    def _read_commands(self) -> None:
        """Frame on CR, carrying any partial message to the next read."""
        buffer = b""
        while self.running:
            try:
                chunk = self.command_sock.recv(4096)
            except OSError:
                break
            if not chunk:
                emit("SYS", "command channel closed by modem")
                break
            buffer += chunk
            while TERMINATOR in buffer:
                frame, buffer = buffer.split(TERMINATOR, 1)
                if frame:
                    emit("RX-CMD", frame.decode("latin-1", errors="replace"))

    def _read_data(self) -> None:
        while self.running:
            try:
                chunk = self.data_sock.recv(4096)
            except OSError:
                break
            if not chunk:
                emit("SYS", "data channel closed by modem")
                break
            emit("RX-DATA", f"{len(chunk)} bytes")
            print(hexdump(chunk), flush=True)
            if self.raw_data_log:
                self.raw_data_log.write(chunk)
                self.raw_data_log.flush()

    def start_readers(self) -> list[threading.Thread]:
        threads = [
            threading.Thread(target=self._read_commands, daemon=True),
            threading.Thread(target=self._read_data, daemon=True),
        ]
        for thread in threads:
            thread.start()
        return threads


def build_payload(args) -> bytes:
    """Payloads for known-plaintext differential analysis.

    --zeros N            N zero bytes: the reference capture.
    --one-bit N,BIT      N zero bytes with a single bit set, so the diff
                         against the reference isolates that bit's effect on
                         the encoder.
    --text STR / --file  ordinary payloads.
    """
    if args.zeros is not None:
        return b"\x00" * args.zeros
    if args.one_bit is not None:
        length_str, _, bit_str = args.one_bit.partition(",")
        length = int(length_str)
        bit_index = int(bit_str)
        if bit_index >= length * 8:
            raise SystemExit(f"bit {bit_index} is outside a {length}-byte payload")
        payload = bytearray(length)
        # Bit 0 is the most significant bit of byte 0 — a stable convention so
        # successive sweeps are comparable.
        payload[bit_index // 8] = 1 << (7 - (bit_index % 8))
        return bytes(payload)
    if args.file:
        with open(args.file, "rb") as handle:
            return handle.read()
    if args.text is not None:
        return args.text.encode("utf-8")
    raise SystemExit("send needs one of --zeros, --one-bit, --text or --file")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Drive a VARA modem over its TCP host interface.",
        epilog="Transmitting verbs key a radio. Keep the bench on a dummy load.",
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_COMMAND_PORT,
                        help="command port (data port defaults to this + 1)")
    parser.add_argument("--data-port", type=int, default=None)
    parser.add_argument("--mycall", help="register callsign(s) before doing anything else")
    parser.add_argument("--bandwidth", type=int, choices=[500, 2300, 2750])
    parser.add_argument("--log-data", metavar="PATH",
                        help="append raw received payload bytes to PATH")

    sub = parser.add_subparsers(dest="mode", required=True)

    listen_cmd = sub.add_parser("listen", help="observe a link (receive-only)")
    listen_cmd.add_argument("--cq", action="store_true", help="LISTEN CQ instead of LISTEN ON")
    listen_cmd.add_argument("--seconds", type=float, default=0.0,
                            help="stop after N seconds (0 = until interrupted)")

    send_cmd = sub.add_parser("send", help="connect to a peer and send a payload")
    send_cmd.add_argument("--to", required=True, help="destination callsign")
    send_cmd.add_argument("--zeros", type=int, help="send N zero bytes")
    send_cmd.add_argument("--one-bit", metavar="LEN,BIT",
                          help="LEN zero bytes with a single bit set")
    send_cmd.add_argument("--text")
    send_cmd.add_argument("--file")
    send_cmd.add_argument("--hold", type=float, default=30.0,
                          help="seconds to stay linked after sending")

    raw_cmd = sub.add_parser("command", help="issue raw modem commands")
    raw_cmd.add_argument("commands", nargs="+", help='e.g. VERSION "LISTEN ON"')
    raw_cmd.add_argument("--wait", type=float, default=3.0,
                         help="seconds to collect replies afterwards")

    args = parser.parse_args()

    probe = VaraProbe(args.host, args.port, args.data_port)
    try:
        probe.connect()
    except OSError as exc:
        print(f"cannot reach the modem: {exc}", file=sys.stderr)
        return 1

    if args.log_data:
        probe.raw_data_log = open(args.log_data, "ab")
        emit("SYS", f"logging received payload to {args.log_data}")

    probe.start_readers()

    # VERSION first: it proves the channel works and records which modem build
    # produced everything that follows, which matters for the provenance log.
    probe.send_command("VERSION")
    time.sleep(0.5)

    if args.mycall:
        probe.send_command(f"MYCALL {args.mycall}")
        time.sleep(0.3)
    if args.bandwidth:
        # No space: the command is BW2300, not BW 2300.
        probe.send_command(f"BW{args.bandwidth}")
        time.sleep(0.3)

    try:
        if args.mode == "listen":
            probe.send_command("LISTEN CQ" if args.cq else "LISTEN ON")
            emit("SYS", "listening (receive-only). Ctrl-C to stop.")
            if args.seconds > 0:
                time.sleep(args.seconds)
            else:
                while True:
                    time.sleep(0.5)

        elif args.mode == "send":
            payload = build_payload(args)
            if not args.mycall:
                print("send needs --mycall so the modem has a source callsign",
                      file=sys.stderr)
                return 1
            source = args.mycall.split()[0]
            emit("SYS", f"keying: CONNECT {source} -> {args.to}")
            probe.send_command(f"CONNECT {source} {args.to}")
            # Wait for the link before pushing payload; the modem buffers
            # anyway, but this keeps the transcript readable.
            time.sleep(5.0)
            probe.send_data(payload)
            time.sleep(args.hold)
            probe.send_command("DISCONNECT")
            time.sleep(2.0)

        elif args.mode == "command":
            for command in args.commands:
                probe.send_command(command)
                time.sleep(0.3)
            time.sleep(args.wait)

    except KeyboardInterrupt:
        emit("SYS", "interrupted")
    finally:
        emit("SYS", "closing")
        probe.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
