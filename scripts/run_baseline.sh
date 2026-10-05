#!/usr/bin/env bash
# T-011: repeatable launch of the SP exe under Wine (reference baseline only — never shipped).
#   scripts/run_baseline.sh [--trace] [--net] [-- <extra game args>]
#     --trace   put the d3d9 proxy logger (tools/d3d9_proxy) in front of Wine's d3d9
#     --net     allow outbound network (default: denied with sandbox-exec; SP does not need it)
#     --capture N   with --trace: dump the back buffer every N frames -> artifacts/baseline/frames-<stamp>/
#     --seconds S   stop the game after S seconds (wineserver -k); default: run until it exits
#     --wine NAME   which unpacked build under artifacts/wine to use: devel (default) | staging | stable
#                   (each build gets its own prefix: artifacts/wineprefix[-NAME])
#     --dxvk        use DXVK (artifacts/dxvk/*/x32/d3d9.dll, D3D9 -> Vulkan -> MoltenVK) instead of wined3d
#     --keys SPEC   with --trace: scripted key presses, "ms:vk,ms:vk" (ms after device creation,
#                   vk = Windows virtual-key code, e.g. 0x20 space, 0x0D enter, 0x1B esc, 0x28 down)
#     --shots S     screenshot the game WINDOW (not the screen) every S seconds ->
#                   artifacts/baseline/shots-<stamp>/ (needs Screen Recording permission)
# Layout (all under artifacts/, gitignored):
#   artifacts/game        writable APFS clone of reference/ (created here if missing)
#   artifacts/wine        unpacked Wine build (see docs/DECISIONS.md ADR-007)
#   artifacts/wineprefix  WINEPREFIX; drive Z: (whole filesystem) is removed, G: = artifacts/game
#   artifacts/baseline    logs + census output
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
TRACE=0; NET=0; CAPTURE=0; SECONDS_LIMIT=0; WINE_NAME=devel; SHOTS=0; DXVK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --trace) TRACE=1 ;;
    --net) NET=1 ;;
    --capture) CAPTURE="$2"; shift ;;
    --seconds) SECONDS_LIMIT="$2"; shift ;;
    --wine) WINE_NAME="$2"; shift ;;
    --shots) SHOTS="$2"; shift ;;
    --keys) export BO1_KEYS="$2"; shift ;;
    --dxvk) DXVK=1 ;;
    --) shift; break ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
  shift
done

WINE_BIN="${WINE_BIN:-$(ls -d "$ROOT"/artifacts/wine/*.app/Contents/Resources/wine/bin 2>/dev/null | grep -i "wine $WINE_NAME" | head -1)}"
[ -n "$WINE_BIN" ] && [ -x "$WINE_BIN/wine" ] || { echo "Wine not found under artifacts/wine (ADR-007). Set WINE_BIN."; exit 1; }
arch -x86_64 /usr/bin/true 2>/dev/null || { echo "Rosetta 2 is not installed (softwareupdate --install-rosetta)."; exit 1; }

GAME="$ROOT/artifacts/game"
if [ ! -f "$GAME/BlackOps.exe" ]; then
  mkdir -p "$GAME"
  cp -c -R "$ROOT/reference/Call of Duty Black Ops/" "$GAME/"   # APFS clone: no extra disk
  chmod -R u+w "$GAME"
  rm -rf "$GAME/Redist" "$GAME/~Common Redist" "$GAME/Soundtrack"
fi

if [ "$WINE_NAME" = devel ]; then export WINEPREFIX="$ROOT/artifacts/wineprefix"
else export WINEPREFIX="$ROOT/artifacts/wineprefix-$WINE_NAME"; fi
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
rm -f "$GAME/dxvk_d3d9.dll" "$GAME"/*_d3d9.log
if [ "$DXVK" = 1 ]; then
  DXVK_DLL=$(ls "$ROOT"/artifacts/dxvk/*/x32/d3d9.dll 2>/dev/null | head -1)
  [ -n "$DXVK_DLL" ] || { echo "DXVK x32/d3d9.dll not found under artifacts/dxvk"; exit 1; }
  export DXVK_LOG_PATH='G:\' DXVK_LOG_LEVEL=info
  if [ "$TRACE" = 1 ]; then          # proxy in front, DXVK behind it under another file name
    cp "$DXVK_DLL" "$GAME/dxvk_d3d9.dll"; export BO1_REAL_D3D9='G:\dxvk_d3d9.dll'
  fi
fi
if [ "$TRACE" = 1 ]; then
  [ -f "$ROOT/artifacts/d3d9_proxy/d3d9.dll" ] || bash "$ROOT/tools/d3d9_proxy/build.sh"
  cp "$ROOT/artifacts/d3d9_proxy/d3d9.dll" "$GAME/d3d9.dll"
  export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;d3d9=n,b"
  export BO1_D3D9_LOG='G:\d3d9_census.txt'
  export BO1_IAT_HOOKS="${BO1_IAT_HOOKS:-0}" BO1_MSGBOX_AUTO="${BO1_MSGBOX_AUTO:-1}"
  rm -f "$GAME/d3d9_census.txt"
  if [ "$CAPTURE" != 0 ]; then
    rm -rf "$GAME/capture"; mkdir -p "$GAME/capture"
    export BO1_CAPTURE_EVERY="$CAPTURE" BO1_CAPTURE_DIR='G:\capture'
  fi
elif [ "$DXVK" = 1 ]; then
  cp "$DXVK_DLL" "$GAME/d3d9.dll"; export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;d3d9=n,b"
else
  rm -f "$GAME/d3d9.dll"
fi

LOG="$OUT/run-$STAMP.log"
{
  echo "dxvk: $DXVK ${DXVK_DLL:-}"
  echo "date: $(date)"; echo "wine: $("$WINE_BIN/wine" --version 2>&1) ($WINE_NAME) prefix: $WINEPREFIX"; echo "trace: $TRACE net: $NET"
  echo "machine: $(sysctl -n machdep.cpu.brand_string) / macOS $(sw_vers -productVersion)"
  echo "args: +set r_fullscreen 0 +set logfile 2 $*"
  [ -f "$CFG" ] && grep -E "^seta (r_fullscreen|r_mode|r_aaSamples|r_vsync|com_maxfps) " "$CFG"
} >"$LOG"

TIMER=""; SHOOTER=""
cleanup() {   # every step tolerant of failure: set -e must not abort the trap half-way
  if [ -n "$TIMER" ]; then pkill -P "$TIMER" 2>/dev/null || true; kill "$TIMER" 2>/dev/null || true; fi
  if [ -n "$SHOOTER" ]; then pkill -P "$SHOOTER" 2>/dev/null || true; kill "$SHOOTER" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM
if [ "$SHOTS" != 0 ]; then
  [ -x "$ROOT/artifacts/bin/winlist" ] || { mkdir -p "$ROOT/artifacts/bin"; xcrun swiftc -O "$ROOT/tools/macos/winlist.swift" -o "$ROOT/artifacts/bin/winlist"; }
  mkdir -p "$OUT/shots-$STAMP"
  ( n=0; t0=$(date +%s)
    while sleep "$SHOTS"; do
      id=$("$ROOT/artifacts/bin/winlist" wine | awk -F'\t' '$3 ~ /Call of Duty/ {print $1; exit}')
      [ -n "$id" ] || continue
      n=$((n+1)); screencapture -x -o -l "$id" "$OUT/shots-$STAMP/shot_$(printf %03d $n)_t$(( $(date +%s) - t0 ))s.png" 2>/dev/null
    done ) &
  SHOOTER=$!
fi

cd "$GAME"
CMD=("$WINE_BIN/wine" 'G:\BlackOps.exe' +set r_fullscreen 0 +set logfile 2 "$@")
if [ "$SECONDS_LIMIT" != 0 ]; then
  ( sleep "$SECONDS_LIMIT"; "$WINE_BIN/wineserver" -k ) &
  TIMER=$!   # reaped by the EXIT trap: a leftover timer would kill the NEXT run's wineserver
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
for f in "$GAME"/players/*.log "$GAME"/main/*.log "$GAME"/*_d3d9.log; do [ -f "$f" ] && cp "$f" "$OUT/$(basename "${f%.log}")-$STAMP.log"; done
echo "log: $LOG"
tail -20 "$LOG"
