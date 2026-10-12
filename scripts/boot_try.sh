#!/usr/bin/env bash
# Build the boot runtime and run it once; print where it stopped (T-045 iteration loop).
#   scripts/boot_try.sh [seconds] [lines]
set -uo pipefail
cd "$(dirname "$0")/.."
scripts/build_boot.sh 2>&1 | grep -E "error|Undefined|ld:" && exit 1
mkdir -p artifacts/boot/home
# a leftover "did not quit properly" marker would bring up the safe-mode prompt
rm -f "artifacts/boot/home/AppData/Local/Activision/CoD/__BlackOps"
# unattended runs stay silent: SDL's dummy audio device runs the whole mix without making a sound
SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-dummy}" BO1_VERBOSE="${BO1_VERBOSE:-1}" artifacts/boot/bo1_boot --seconds "${1:-30}" > artifacts/boot/last_run.log 2>&1
echo "exit code $?"
grep -v "no shim yet\|steam_api.dll imports\|GetProcAddress(kernel32.dll, \"\(Encode\|Decode\|Fls\)" artifacts/boot/last_run.log | tail -"${2:-16}" | cut -c1-220
