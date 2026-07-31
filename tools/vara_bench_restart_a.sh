#!/bin/bash
# Restart bench instance A only, leaving instance B alone.
#
# A died once during a 40-capture scan — process still alive, 8300/8301 gone,
# which is the same failure the bench script warns about for a KISS port clash.
# An unattended campaign of a thousand captures will meet it again, so the
# capture harness calls this and carries on rather than losing the run.
#
# Deliberately does NOT touch instance B: another agent or a live session may be
# using it, and bringing the whole bench down to fix half of it is not worth it.
set -u

PREFIX="$HOME/.local/share/bottles/bottles/VARA-HF"
WINE="$HOME/.local/share/bottles/runners/soda-9.0-1/bin/wine"

# Identify A by the program directory it runs from, never by a bare pattern —
# a pgrep -f that matches this script's own command line is an exit-144 self-kill.
for pid in $(pgrep -x VARA.exe 2>/dev/null); do
    if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -qx "PULSE_SINK=varaA"; then
        kill "$pid" 2>/dev/null
        sleep 3
        kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
    fi
done
sleep 2

cd "$PREFIX/drive_c/VARA" || exit 1
DISPLAY="${BENCH_DISPLAY:-:99}" WINEDEBUG=-all WINEPREFIX="$PREFIX" \
    PULSE_SINK=varaA PULSE_SOURCE=varaB.monitor \
    setsid nohup "$WINE" VARA.exe >/dev/null 2>&1 &

for _ in $(seq 1 30); do
    sleep 2
    ss -ltn 2>/dev/null | grep -q ':8300 ' && { echo "instance A back on 8300"; exit 0; }
done
echo "instance A did NOT come back on 8300" >&2
exit 1
