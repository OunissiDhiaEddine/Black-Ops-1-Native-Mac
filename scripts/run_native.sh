#!/usr/bin/env bash
# Runs the native arm64 build of the game in a window (Metal renderer, no Wine).
#   scripts/run_native.sh [seconds]        default: until the game exits or Ctrl-C
# Log: artifacts/boot/native_run.log. Useful environment:
#   BO1_KEYS="35000:0x20"           scripted key presses (ms after start : Windows virtual-key code)
#   BO1_CMDS="60000|devmap cuba"    scripted console commands
#   BO1_D3D_DUMP=dir                PNG of every 20th frame
#   BO1_VERBOSE=1                   shim and game debug output in the log
set -uo pipefail
cd "$(dirname "$0")/.."
scripts/build_boot.sh 2>&1 | grep -E "error|Undefined|ld:" && exit 1
mkdir -p artifacts/boot/home
# a leftover "did not quit properly" marker would bring up the safe-mode question
rm -f "artifacts/boot/home/AppData/Local/Activision/CoD/__BlackOps"
echo "log: artifacts/boot/native_run.log"
BO1_WINDOW=1 BO1_VERBOSE="${BO1_VERBOSE:-1}" artifacts/boot/bo1_boot --seconds "${1:-0}" > artifacts/boot/native_run.log 2>&1
echo "exit code $?"
tail -5 artifacts/boot/native_run.log | cut -c1-200
