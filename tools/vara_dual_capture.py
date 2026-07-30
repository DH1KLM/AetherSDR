#!/usr/bin/env python3
"""Capture BOTH sides of a VARA session simultaneously.

Every capture so far recorded only `varaA.monitor`, which is instance A's
transmitted audio. That is one half of the conversation: A's connect frame, its
DATA frames and its control frames. Instance B's replies — the ACKs and whatever
else drives the ARQ state machine — go out on `varaB` and were never recorded, so
they appear in A's capture only as silence between bursts.

The session grammar recovered from one-sided captures is therefore the
INITIATOR's script, not the protocol. Recovering the state machine needs both
directions on a common timebase, which is what this does: two recorders started
together, so burst timestamps are directly comparable and the interleaving of A
and B is visible.

    tools/vara_dual_capture.py --out-a a.wav --out-b b.wav --bits 32,3,17,42
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-a", required=True, help="instance A's transmitted audio")
    ap.add_argument("--out-b", required=True, help="instance B's transmitted audio")
    ap.add_argument("--bits", help="payload as LEN,bit,bit,... (32-byte default)")
    ap.add_argument("--zeros", type=int)
    a = ap.parse_args()

    if a.zeros is not None:
        payload = b"\x00" * a.zeros
    elif a.bits:
        parts = [int(v) for v in a.bits.split(",")]
        ln, bits = parts[0], parts[1:]
        buf = bytearray(ln)
        for b in bits:
            buf[b // 8] |= 1 << (7 - (b % 8))
        payload = bytes(buf)
    else:
        ap.error("need --bits or --zeros")

    # Only side B is recorded here. run_once already starts its own recorder on
    # varaA.monitor writing to the path it is given, and pointing a second
    # recorder at the same file is precisely what produced the corrupted
    # multi-session captures earlier — two parecords appending to one file.
    rb = cap.Recorder(a.out_b, "varaB.monitor")
    time.sleep(2)
    try:
        info, err = cap.run_once(payload, a.out_a)
    finally:
        rb.stop()
    if err:
        print(f"FAILED: {err}", file=sys.stderr)
        return 2
    for label, path in (("A", a.out_a), ("B", a.out_b)):
        try:
            x = cap.load_wav(path)
            bursts = cap.split_bursts(x)
            print(f"  side {label}: {len(x)/48000:.1f}s, {len(bursts)} bursts, "
                  f"sizes {[ (b-aa)//512 for aa,b in bursts ][:14]}")
        except Exception as exc:
            print(f"  side {label}: unreadable ({exc})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
