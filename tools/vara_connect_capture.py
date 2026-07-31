#!/usr/bin/env python3
"""Capture a connect frame per destination callsign, as fast as the modem allows.

WHY THIS EXISTS. The earlier probe captured connect frames as a side effect of
running a whole ARQ session: link up, send a payload, drain, disconnect. That
costs two to three minutes per callsign, which was fine for a handful of
differential trials and is hopeless now. Establishing what the connect frame
encodes needs hundreds of them.

Nothing about a session is needed. A connect frame is transmitted the instant
CONNECT is issued, before anything answers — so there is no reason to wait for a
link, no reason for a responder to be listening, and no reason to send data. Open
one modem, arm the recorder, issue CONNECT, capture the first burst, ABORT. That
is about ten seconds.

WHAT IS DELIBERATELY NOT DONE. No link is established and no payload is sent, so
this never keys a real radio: the bench instances render to a PipeWire sink, and
ABORT stops the retry train immediately. A capture that yields no clean frame is
retried once and then reported as a failure rather than silently returning
whatever the extractor found — a silently wrong frame poisons every downstream
statistic, which has already happened once here.

    tools/vara_connect_capture.py --out DIR --calls A,B,C
    tools/vara_connect_capture.py --out DIR --calls-file list.txt
"""

import argparse
import atexit
import os
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_capture as cap          # noqa: E402
import vara_callsign_analyse as A       # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# The recorder currently running, so it can be stopped if this process is killed.
#
# Pausing a campaign with a plain kill terminates Python without running the
# `finally` that stops parecord, orphaning it. One such orphan ran 31 minutes and
# wrote 181 MB into a slot that should hold 13 seconds — and because the file
# existed and still contained a preamble, a resumed run would have SKIPPED that
# callsign and kept the suspect capture. A silently wrong frame is worse than a
# missing one, so the recorder is torn down on the way out.
_live_recorder = None


def _stop_live_recorder(*_):
    global _live_recorder
    rec, _live_recorder = _live_recorder, None
    if rec is not None:
        try:
            rec.stop()
        except Exception:
            pass


atexit.register(_stop_live_recorder)
for _sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(_sig, lambda s, f: (_stop_live_recorder(), sys.exit(128 + s)))

SRC = "KK7GWY-9"
PORT = 8300
SINK = "varaA.monitor"

# Long enough that the WAV clears the extractor's one-megabyte floor, which
# exists to reject truncated captures: 48 kHz mono s16 needs 10.4 s to reach it,
# and a connect frame plus its first retry is well inside that.
LISTEN = 13.0

# A capture is `LISTEN` seconds of 48 kHz mono s16, so about 1.3 MB. Anything
# several times that did not come from this tool's own recording window.
MAX_WAV_BYTES = 8_000_000


def open_modem(port, restart=True, tries=3):
    """Connect to the modem, restarting instance A if it has fallen over.

    Instance A died once mid-scan — the process stayed alive but 8300/8301
    disappeared — and a campaign of a thousand captures will meet that again.
    Losing eight hours of unattended bench time to a dead socket is not
    acceptable, so reopen rather than abort.
    """
    for attempt in range(tries):
        try:
            return cap.Modem(port, "A")
        except OSError as exc:
            if not restart or attempt == tries - 1:
                raise
            print(f"    modem on {port} is gone ({exc}); restarting instance A",
                  flush=True)
            subprocess.run(["bash", os.path.join(HERE, "vara_bench_restart_a.sh")],
                           timeout=180)
            time.sleep(5)
    raise RuntimeError("unreachable")


def wait_idle(m, timeout=12.0):
    """Block until the modem has finished winding down from a previous attempt.

    VARA answers WRONG to a CONNECT issued while an ABORT is still settling, and
    it keeps that state across host connections — so a fresh socket does not
    clear it. Back-to-back captures failed for exactly this reason, and the
    failure looked like callsign rejection: AAAAAA drew a WRONG here despite
    already being in the ground truth. Wait for the modem to say it is done.
    """
    end = time.time() + timeout
    seen = len(m.log)
    while time.time() < end:
        tail = m.log[seen:]
        if any(l in ("BUSY OFF", "DISCONNECTED") for l in tail):
            time.sleep(0.6)
            return True
        if not m.linked and time.time() - (end - timeout) > 2.5 and not tail:
            return True                 # already quiet; nothing to wind down
        time.sleep(0.2)
    return False


def capture_one(m, wav, dst, src=SRC, sink=SINK, listen=LISTEN):
    """Record the connect frame VARA transmits for `dst`. True if a frame is in
    the file — verified by extracting it, not by assuming."""
    global _live_recorder
    wait_idle(m)
    rec = cap.Recorder(wav, sink)
    _live_recorder = rec
    time.sleep(1.2)                     # let parecord actually open the stream
    mark = len(m.log)
    try:
        m.send(f"CONNECT {src} {dst}")
        time.sleep(listen)
        m.send("ABORT")
        wait_idle(m)
    finally:
        _stop_live_recorder()
    # A rejected CONNECT looks exactly like a quiet channel in the audio, so
    # report what the modem said rather than leaving it to be inferred.
    said = m.log[mark:]
    if "WRONG" in said:
        print(f"    modem refused CONNECT ({dst}): {said[:4]}", flush=True)
        return False
    return A.connect_body(wav) is not None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--calls")
    ap.add_argument("--calls-file")
    ap.add_argument("--src", default=SRC)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--sink", default=SINK)
    ap.add_argument("--listen", type=float, default=LISTEN)
    a = ap.parse_args()

    calls = []
    if a.calls:
        calls += [c.strip() for c in a.calls.split(",") if c.strip()]
    if a.calls_file:
        with open(a.calls_file) as fh:
            calls += [l.strip() for l in fh if l.strip() and not l.startswith("#")]
    if not calls:
        print("no callsigns given")
        return 2

    os.makedirs(a.out, exist_ok=True)
    ok = bad = skip = 0
    t0 = time.time()
    # One host connection for the whole run. Reconnecting per capture bought
    # nothing — VARA's state lives in the modem, not the socket — and cost a
    # second each.
    def configure(mm):
        mm.send("VERSION"); time.sleep(0.3)
        mm.send(f"MYCALL {a.src}"); time.sleep(0.3)
        mm.send("BW2300"); time.sleep(0.3)
        mm.send("COMPRESSION OFF"); time.sleep(0.3)

    m = open_modem(a.port)
    try:
        configure(m)

        for i, c in enumerate(calls, 1):
            wav = os.path.join(a.out, f"cs_{c}.wav")
            if os.path.exists(wav) and os.path.getsize(wav) > MAX_WAV_BYTES:
                # Far longer than this run records, so it is not this callsign's
                # capture in any trustworthy sense — an orphaned recorder that
                # outlived its campaign will have swept up every transmission
                # since. It still contains a preamble, so the extractor accepts
                # it and the skip below would keep it forever.
                print(f"[{i}/{len(calls)}] {c}: existing capture is "
                      f"{os.path.getsize(wav)/1e6:.0f} MB, far too long to trust "
                      f"— discarding and recapturing", flush=True)
                os.replace(wav, wav + ".suspect")
            if os.path.exists(wav) and A.connect_body(wav) is not None:
                skip += 1
                continue
            got = False
            for attempt in (1, 2):
                print(f"[{i}/{len(calls)}] {c}"
                      f"{' (retry)' if attempt == 2 else ''}", flush=True)
                try:
                    if capture_one(m, wav, c, a.src, a.sink, a.listen):
                        got = True
                        break
                except OSError as exc:
                    print(f"    host link lost ({exc}); recovering", flush=True)
                    try:
                        m.close()
                    except OSError:
                        pass
                    m = open_modem(a.port)
                    configure(m)
                try:
                    os.remove(wav)
                except OSError:
                    pass
            ok, bad = (ok + 1, bad) if got else (ok, bad + 1)
            if not got:
                print("    NO CLEAN CONNECT FRAME", flush=True)
            if i % 10 == 0 or i == len(calls):
                rate = (time.time() - t0) / max(i - skip, 1)
                print(f"    -- {ok} ok, {bad} failed, {skip} already present, "
                      f"{rate:.1f}s per capture", flush=True)
    finally:
        m.send("ABORT")
        time.sleep(1.0)
        m.close()
    print(f"\n{ok} captured, {bad} failed, {skip} already present")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
