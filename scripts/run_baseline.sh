#!/usr/bin/env bash
# T-011: repeatable launch of the SP exe under Wine (reference baseline only — never shipped).
#   scripts/run_baseline.sh [--trace] [--net] [-- <extra game args>]
#     --trace   put the d3d9 proxy logger (tools/d3d9_proxy) in front of Wine's d3d9
#     --net     allow outbound network (default: denied with sandbox-exec; SP does not need it)
#     --capture N   with --trace: dump the back buffer every N frames -> artifacts/baseline/frames-<stamp>/
#     --seconds S   stop the game after S seconds (wineserver -k); default: run until it exits
# Layout (all under artifacts/, gitignored):
#   artifacts/game        writable APFS clone of reference/ (created here if missing)
#   artifacts/wine        unpacked Wine build (see docs/DECISIONS.md ADR-007)
#   artifacts/wineprefix  WINEPREFIX; drive Z: (whole filesystem) is removed, G: = artifacts/game
#   artifacts/baseline    logs + census output
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
TRACE=0; NET=0; CAPTURE=0; SECONDS_LIMIT=0
while [ $# -gt 0 ]; do
  case "$1" in
    --trace) TRACE=1 ;;
    --net) NET=1 ;;
    --capture) CAPTURE="$2"; shift ;;
    --seconds) SECONDS_LIMIT="$2"; shift ;;
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

# The game leaves %LOCALAPPDATA%\Activision\CoD\__BlackOps behind when it is killed and then
# blocks on a "Run In Safe Mode?" message box at the next start. We always kill it, so clear it.
rm -f "$WINEPREFIX"/drive_c/users/*/AppData/Local/Activision/CoD/__BlackOps

# Deterministic video settings: config.cfg (written by the game on first run) overrides +set.
CFG="$GAME/players/config.cfg"
if [ -f "$CFG" ]; then
  setcfg() { sed -i '' -E "s|^seta $1 \".*\"|seta $1 \"$2\"|" "$CFG"; }
  setcfg r_fullscreen 0; setcfg r_mode 1280x720; setcfg r_aaSamples 1; setcfg r_vsync 0; setcfg com_maxfps 60
fi

STAMP=$(date +%Y%m%d-%H%M%S)
if [ "$TRACE" = 1 ]; then
  [ -f "$ROOT/artifacts/d3d9_proxy/d3d9.dll" ] || bash "$ROOT/tools/d3d9_proxy/build.sh"
  cp "$ROOT/artifacts/d3d9_proxy/d3d9.dll" "$GAME/d3d9.dll"
  export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;d3d9=n,b"
  export BO1_D3D9_LOG='G:\d3d9_census.txt'
  rm -f "$GAME/d3d9_census.txt"
  if [ "$CAPTURE" != 0 ]; then
    rm -rf "$GAME/capture"; mkdir -p "$GAME/capture"
    export BO1_CAPTURE_EVERY="$CAPTURE" BO1_CAPTURE_DIR='G:\capture'
  fi
else
  rm -f "$GAME/d3d9.dll"
fi

LOG="$OUT/run-$STAMP.log"
{
  echo "date: $(date)"; echo "wine: $("$WINE_BIN/wine" --version 2>&1)"; echo "trace: $TRACE net: $NET"
  echo "machine: $(sysctl -n machdep.cpu.brand_string) / macOS $(sw_vers -productVersion)"
  echo "args: +set r_fullscreen 0 +set logfile 2 $*"
  [ -f "$CFG" ] && grep -E "^seta (r_fullscreen|r_mode|r_aaSamples|r_vsync|com_maxfps) " "$CFG"
} >"$LOG"

cd "$GAME"
CMD=("$WINE_BIN/wine" 'G:\BlackOps.exe' +set r_fullscreen 0 +set logfile 2 "$@")
if [ "$SECONDS_LIMIT" != 0 ]; then
  ( sleep "$SECONDS_LIMIT"; "$WINE_BIN/wineserver" -k ) &
  TIMER=$!
  # a leftover timer would kill the NEXT run's wineserver: always reap it
  trap 'pkill -P $TIMER 2>/dev/null; kill $TIMER 2>/dev/null || true' EXIT
fi
if [ "$NET" = 1 ]; then
  "${CMD[@]}" >>"$LOG" 2>&1 || echo "exit code $?" >>"$LOG"
else
  sandbox-exec -p '(version 1)(allow default)(deny network-outbound (remote ip))' "${CMD[@]}" >>"$LOG" 2>&1 \
    || echo "exit code $?" >>"$LOG"
fi
"$WINE_BIN/wineserver" -w || true
[ -f "$GAME/d3d9_census.txt" ] && cp "$GAME/d3d9_census.txt" "$OUT/d3d9_census-$STAMP.txt"
if [ "$CAPTURE" != 0 ] && ls "$GAME/capture"/*.bmp >/dev/null 2>&1; then
  mkdir -p "$OUT/frames-$STAMP"
  for b in "$GAME/capture"/*.bmp; do sips -s format png "$b" --out "$OUT/frames-$STAMP/$(basename "${b%.bmp}").png" >/dev/null; done
  rm -rf "$GAME/capture"; echo "frames: $OUT/frames-$STAMP ($(ls "$OUT/frames-$STAMP" | wc -l | tr -d ' '))"
fi
for f in "$GAME"/players/*.log "$GAME"/main/*.log; do [ -f "$f" ] && cp "$f" "$OUT/$(basename "${f%.log}")-$STAMP.log"; done
echo "log: $LOG"
tail -20 "$LOG"
