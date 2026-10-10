#!/usr/bin/env bash
# Runs the native arm64 build of the game in a window (Metal renderer, no Wine).
#   scripts/run_native.sh [seconds]        default: until the game exits or Ctrl-C
# Log: artifacts/boot/native_run.log. Useful environment:
#   BO1_KEYS="45000:0x20"           scripted key presses (ms after start : Windows virtual-key code)
#   BO1_CMDS="60000|devmap cuba"    scripted console commands
#   BO1_D3D_DUMP=dir                PNG of every 20th frame
#   BO1_VERBOSE=1                   shim and game debug output in the log
#   BO1_AUDIO=off                   no sound · BO1_BINK=off  no movies
set -uo pipefail
cd "$(dirname "$0")/.."
scripts/build_boot.sh 2>&1 | grep -E "error|Undefined|ld:" && exit 1
mkdir -p artifacts/boot/home
# a leftover "did not quit properly" marker would bring up the safe-mode question
rm -f "artifacts/boot/home/AppData/Local/Activision/CoD/__BlackOps"
echo "log: artifacts/boot/native_run.log"
# The game runs with the shader cache being filled next to it (T-069): what a level creates when it loads is compiled
# while you play, BO1_CACHE_JOBS at a time (default 2, at low priority; 0 = only after the game exits), so that most
# shaders are in the cache before their first use. Measured in the heaviest scene: the frame rate holds, single
# frames get up to ~5 ms longer while it compiles.
[ "${BO1_HEADLESS:-0}" = 1 ] || export BO1_WINDOW=1   # BO1_HEADLESS=1: no window (unattended checks of this script)
BO1_VERBOSE="${BO1_VERBOSE:-1}" artifacts/boot/bo1_boot --seconds "${1:-0}" > artifacts/boot/native_run.log 2>&1 &
game=$!
if [ "${BO1_CACHE_JOBS:-2}" != 0 ]; then
  ( while kill -0 "$game" 2>/dev/null; do JOBS="${BO1_CACHE_JOBS:-2}" scripts/shader_cache.sh >/dev/null 2>&1; sleep 5; done ) &
  filler=$!
fi
wait "$game"
echo "exit code $?"
tail -5 artifacts/boot/native_run.log | cut -c1-200
# What is still waiting is compiled now (a level's first load leaves ~5,500 shaders; Ctrl-C is fine, it continues next time).
[ -n "${filler:-}" ] && wait "$filler"
scripts/shader_cache.sh
