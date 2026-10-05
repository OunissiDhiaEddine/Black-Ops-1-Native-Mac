#!/usr/bin/env bash
# T-011: repeatable launch of the SP exe under Wine (reference baseline only — never shipped).
#   scripts/run_baseline.sh [--trace] [--net] [-- <extra game args>]
#     --trace   put the d3d9 proxy logger (tools/d3d9_proxy) in front of Wine's d3d9
#     --net     allow outbound network (default: denied with sandbox-exec; SP does not need it)
# Layout (all under artifacts/, gitignored):
#   artifacts/game        writable APFS clone of reference/ (created here if missing)
#   artifacts/wine        unpacked Wine build (see docs/DECISIONS.md ADR-007)
#   artifacts/wineprefix  WINEPREFIX; drive Z: (whole filesystem) is removed, G: = artifacts/game
#   artifacts/baseline    logs + census output
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
TRACE=0; NET=0
while [ $# -gt 0 ]; do
  case "$1" in
    --trace) TRACE=1 ;;
    --net) NET=1 ;;
    --) shift; break ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
  shift
done

WINE_BIN="${WINE_BIN:-$(ls -d "$ROOT"/artifacts/wine/*.app/Contents/Resources/wine/bin 2>/dev/null | head -1)}"
[ -n "$WINE_BIN" ] && [ -x "$WINE_BIN/wine" ] || { echo "Wine not found under artifacts/wine (ADR-007). Set WINE_BIN."; exit 1; }
arch -x86_64 /usr/bin/true 2>/dev/null || { echo "Rosetta 2 is not installed (softwareupdate --install-rosetta)."; exit 1; }

GAME="$ROOT/artifacts/game"
if [ ! -f "$GAME/BlackOps.exe" ]; then
  mkdir -p "$GAME"
  cp -c -R "$ROOT/reference/Call of Duty Black Ops/" "$GAME/"   # APFS clone: no extra disk
  chmod -R u+w "$GAME"
  rm -rf "$GAME/Redist" "$GAME/~Common Redist" "$GAME/Soundtrack"
fi

export WINEPREFIX="$ROOT/artifacts/wineprefix"
export WINEDEBUG="${WINEDEBUG:--all,+loaddll}"
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
OUT="$ROOT/artifacts/baseline"; mkdir -p "$OUT"

if [ ! -d "$WINEPREFIX/drive_c" ]; then
  "$WINE_BIN/wineboot" --init >"$OUT/wineboot.log" 2>&1
  "$WINE_BIN/wineserver" -w
fi
# Containment: the prefix must not see the whole disk.
rm -f "$WINEPREFIX/dosdevices/z:"
ln -sfn "$GAME" "$WINEPREFIX/dosdevices/g:"

if [ "$TRACE" = 1 ]; then
  [ -f "$ROOT/artifacts/d3d9_proxy/d3d9.dll" ] || bash "$ROOT/tools/d3d9_proxy/build.sh"
  cp "$ROOT/artifacts/d3d9_proxy/d3d9.dll" "$GAME/d3d9.dll"
  export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;d3d9=n,b"
  export BO1_D3D9_LOG='G:\d3d9_census.txt'
else
  rm -f "$GAME/d3d9.dll"
fi

STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$OUT/run-$STAMP.log"
{
  echo "date: $(date)"; echo "wine: $("$WINE_BIN/wine" --version 2>&1)"; echo "trace: $TRACE net: $NET"
  echo "machine: $(sysctl -n machdep.cpu.brand_string) / macOS $(sw_vers -productVersion)"
  echo "args: +set r_fullscreen 0 $*"
} >"$LOG"

cd "$GAME"
CMD=("$WINE_BIN/wine" 'G:\BlackOps.exe' +set r_fullscreen 0 "$@")
if [ "$NET" = 1 ]; then
  "${CMD[@]}" >>"$LOG" 2>&1 || echo "exit code $?" >>"$LOG"
else
  sandbox-exec -p '(version 1)(allow default)(deny network-outbound (remote ip))' "${CMD[@]}" >>"$LOG" 2>&1 \
    || echo "exit code $?" >>"$LOG"
fi
"$WINE_BIN/wineserver" -w || true
[ -f "$GAME/d3d9_census.txt" ] && cp "$GAME/d3d9_census.txt" "$OUT/d3d9_census-$STAMP.txt"
echo "log: $LOG"
tail -20 "$LOG"
