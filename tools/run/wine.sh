#!/usr/bin/env bash
# Runs a Windows program (build/mkw.exe, a probe) under Wine in a prefix of its own, build/win/wineprefix, made on
# first use. Why: plain `wine` updates ~/.wine when its version changed and asks to install Wine Mono in a dialog on
# the desktop (05/11/2026); here Mono and Gecko are switched off (no dialog) and the user's prefix is never touched.
# A crash shows no "program error" dialog either (it left the program waiting for a click, 05/11/2026): winedbg writes
# the backtrace to stderr and the program ends.
# The DLLs of build/win/deps/bin are found through WINEPATH. Output and exit code are the program's; Wine's own messages
# are off unless WINEDEBUG is set.
# Usage: tools/run/wine.sh PROGRAM.exe [ARGS...]
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
export WINEPREFIX=$root/build/win/wineprefix
export WINEDLLOVERRIDES="mscoree=;mshtml=${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export WINEDEBUG=${WINEDEBUG:--all}
export WINEPATH="$root/build/win/deps/bin${WINEPATH:+;$WINEPATH}"   # SDL3.dll, libopus-0.dll (tools/build/windeps.py)
if [ ! -f "$WINEPREFIX/.mkw-ready" ]; then
    wineboot -i >/dev/null 2>&1
    wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
    touch "$WINEPREFIX/.mkw-ready"
fi
exec wine "$@"
