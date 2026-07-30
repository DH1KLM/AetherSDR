#!/bin/bash
# Bring up the two-VARA bench: two modem instances cross-connected over
# PipeWire null sinks, each with its own TCP host interface.
#
#   instance A  (wine-cachyos prefix)  TCP 8300/8301
#       audio out -> sink varaA        audio in <- varaB.monitor
#   instance B  (Bottles soda bottle)  TCP 8310/8311
#       audio out -> sink varaB        audio in <- varaA.monitor
#
# So A hears B and B hears A, with no radio and no RF involved.
#
# THE THING THAT COST HOURS: wine exposes exactly one generic audio device pair
# ("PulseAudio Input" / "PulseAudio Output"), NOT one entry per PipeWire node.
# So the two instances cannot be pointed at different sinks from inside VARA's
# own dialog. PULSE_SINK / PULSE_SOURCE per process is what separates them.
#
# Also: VARA reports "MISSING SOUNDCARD" when VARA.ini [Soundcard] device names
# are EMPTY, which is indistinguishable from having no devices at all. Both
# ini files must name the devices or the modem never opens audio.
set -u

SCRATCH="$(cd "$(dirname "$0")" && pwd)"
export DISPLAY="${BENCH_DISPLAY:-:99}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}"
export WINEDEBUG=-all
export MANGOHUD=0

# BOTH instances run from the SAME Bottles prefix, in separate program folders
# (drive_c/VARA and drive_c/VARA2) — the multi-instance pattern from §3.2 of the
# clean-room note. This matters: an earlier version of this script launched
# instance A from a standalone wine-cachyos prefix, a configuration that never
# worked end to end. Only the soda runner has carried a full ARQ session here.
PREFIX="$HOME/.local/share/bottles/bottles/VARA-HF"
WINE="$HOME/.local/share/bottles/runners/soda-9.0-1/bin/wine"
A_DIR=VARA
B_DIR=VARA2

log() { echo "[bench] $*"; }

# Headless display so nothing lands on the operator's real desktop.
pgrep -f "Xvfb $DISPLAY" >/dev/null || {
    setsid nohup Xvfb "$DISPLAY" -screen 0 1280x800x24 >/dev/null 2>&1 &
    sleep 3
}

# Idempotent null sinks.
for s in varaA varaB; do
    pactl list short sinks 2>/dev/null | grep -q "	$s	" || {
        pactl load-module module-null-sink sink_name="$s" object.linger=1 \
            media.class=Audio/Sink sink_properties=device.description="$s" >/dev/null
        log "created null sink $s"
    }
done

# CRITICAL: each instance needs its OWN KISS port. Both VARA.ini files ship
# with "KISS Port=8100", so the second instance loses the bind, and the
# resulting instance is unstable — it was observed dying outright after a
# host application triggered a modem restart, taking 8310/8311 with it and making every
# subsequent connect attempt fail with an unexplained "no link". Instance A
# keeps 8100; instance B must use 8110. Any host application driving instance B
# must be configured for the matching KISS port (8110).
for cfg_port in "VARA:8100" "VARA2:8110"; do
    d="${cfg_port%%:*}"; kp="${cfg_port##*:}"
    ini="$PREFIX/drive_c/$d/VARA.ini"
    [ -f "$ini" ] && sed -i "s/^KISS Port=.*/KISS Port=$kp/" "$ini" &&         log "$d KISS port set to $kp"
done

log "starting instance A (8300) -> sink varaA, listening on varaB.monitor"
cd "$PREFIX/drive_c/$A_DIR" || exit 1
WINEPREFIX="$PREFIX" PULSE_SINK=varaA PULSE_SOURCE=varaB.monitor \
    setsid nohup "$WINE" VARA.exe >/dev/null 2>&1 &

sleep 8

log "starting instance B (8310) -> sink varaB, listening on varaA.monitor"
cd "$PREFIX/drive_c/$B_DIR" || exit 1
WINEPREFIX="$PREFIX" PULSE_SINK=varaB PULSE_SOURCE=varaA.monitor \
    setsid nohup "$WINE" VARA.exe >/dev/null 2>&1 &

sleep 14

log "listening ports:"
ss -ltn 2>/dev/null | grep -E ':(830[01]|831[01])' | awk '{print "   ", $4}'
log "pulse clients named VARA:"
pactl list clients 2>/dev/null | grep -c 'application.name = "VARA HF Modem"' \
    | xargs -I{} echo "    {} VARA modem client(s) connected to the sound server"
