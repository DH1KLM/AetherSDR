#!/bin/bash
# Install VarAC into the bench bottle, alongside the two VARA modems.
#
# VarAC is distributed behind a registration form (name / callsign / email);
# the download link arrives by email. Obtain the archive yourself, then:
#
#     ./setup-varac.sh /path/to/VarAC_vXX.zip
#     ./setup-varac.sh /path/to/VarAC_setup.exe
#
# VarAC is a C# application needing .NET Framework 4.x. The bottle already
# carries wine-mono (2743 DLLs under drive_c/windows/mono, plus a populated
# Microsoft.NET/Framework/v4.0.30319), so no extra runtime install should be
# needed. If VarAC refuses to start, `winetricks -q dotnet48` is the fallback —
# but try wine-mono first, since dotnet48 and wine-mono conflict.
#
# VarAC drives modem instance B on TCP 8310 so that instance A on 8300 stays
# free for tools/varac_observe.py to watch from the other end of the link.
set -u

ARCHIVE="${1:-}"
[ -n "$ARCHIVE" ] && [ -f "$ARCHIVE" ] || {
    echo "usage: $0 /path/to/VarAC.(zip|exe)"
    echo
    echo "VarAC must be downloaded manually — the vendor gates it behind a"
    echo "registration form at https://www.varac-hamradio.com/download and"
    echo "emails the link. This script does not attempt to bypass that."
    exit 1
}

RUNNER="$HOME/.local/share/bottles/runners/soda-9.0-1"
export WINEPREFIX="$HOME/.local/share/bottles/bottles/VARA-HF"
export WINE="$RUNNER/bin/wine"
export WINEDEBUG=-all
export DISPLAY="${VARAC_DISPLAY:-:99}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}"

TARGET="$WINEPREFIX/drive_c/VarAC"
LOG="$(cd "$(dirname "$0")" && pwd)/varac-setup.log"
: > "$LOG"
log() { echo "[varac] $*" | tee -a "$LOG"; }

[ -x "$WINE" ] || { echo "missing runner $WINE"; exit 1; }

pgrep -f "Xvfb $DISPLAY" >/dev/null || {
    setsid nohup Xvfb "$DISPLAY" -screen 0 1280x800x24 >/dev/null 2>&1 &
    sleep 3
}

mkdir -p "$TARGET"

case "$ARCHIVE" in
    *.zip|*.ZIP)
        log "extracting $ARCHIVE -> C:\\VarAC"
        unzip -o -q "$ARCHIVE" -d "$TARGET" || { log "unzip failed"; exit 1; }
        ;;
    *.exe|*.EXE)
        log "running installer (Inno Setup silent flags; a timeout here is normal)"
        cd "$(dirname "$ARCHIVE")" || exit 1
        timeout 180 "$WINE" "$ARCHIVE" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART \
            >>"$LOG" 2>&1
        pkill -f "VarAC.*setup" 2>/dev/null
        ;;
    *)
        log "unrecognised archive type: $ARCHIVE"; exit 1 ;;
esac

EXE="$(find "$WINEPREFIX/drive_c" -iname 'VarAC.exe' 2>/dev/null | head -1)"
[ -n "$EXE" ] || { log "FAILED: VarAC.exe not found after install"; exit 1; }
log "found $EXE"

VARAC_DIR="$(dirname "$EXE")"
INI="$VARAC_DIR/VarAC.ini"

# Point VarAC at modem instance B (8310) and enable Linux-compatible mode, which
# the manual says disables narration and the speller under WINE. Only touch keys
# that already exist — VarAC writes a full ini on first run, so if this is a
# fresh extract the file may not exist yet and these are applied after first run.
if [ -f "$INI" ]; then
    sed -i 's/^VARAHFPort=.*/VARAHFPort=8310/' "$INI" 2>/dev/null
    sed -i 's/^LinuxCompatibleMode=.*/LinuxCompatibleMode=True/' "$INI" 2>/dev/null
    log "pointed VarAC at VARA instance B on 8310"
else
    log "no VarAC.ini yet (written on first run) — set VARA port to 8310 in the UI"
fi

log "launching VarAC headless on $DISPLAY"
cd "$VARAC_DIR" || exit 1
setsid nohup "$WINE" VarAC.exe >>"$LOG" 2>&1 &
sleep 25

if pgrep -f "VarAC\.exe" >/dev/null; then
    log "SUCCESS: VarAC is running"
    log "screenshot it with:  DISPLAY=$DISPLAY import -window root varac.png"
    log "then observe from the far end:  tools/varac_observe.py --port 8300 --mycall <call>"
else
    log "VarAC did not stay running — see $LOG"
    log "if it is a .NET failure, try: winetricks -q dotnet48"
fi
