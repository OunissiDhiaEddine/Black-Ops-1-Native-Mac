#!/usr/bin/env bash
# T-047 (ADR-014): check that the real Steam client can serve the game.
# Needs (user actions): Steam for macOS running and logged in to the account that owns Black Ops, and
# Valve's Steamworks SDK unpacked under artifacts/steamworks/ (the script looks for libsteam_api.dylib there).
set -euo pipefail
cd "$(dirname "$0")/.."
DYLIB="$(find artifacts/steamworks -name libsteam_api.dylib 2>/dev/null | head -1)"
[ -n "$DYLIB" ] || { echo "no libsteam_api.dylib under artifacts/steamworks/ (see docs/DECISIONS.md ADR-014)"; exit 2; }
mkdir -p artifacts/steamworks/spike
clang -O1 -Wall tests/steam_spike/main.c -o artifacts/steamworks/spike/steam_spike
echo 42700 > artifacts/steamworks/spike/steam_appid.txt     # tells the Steam client which app this process is
cd artifacts/steamworks/spike
DYLD_LIBRARY_PATH="$(dirname "../../../$DYLIB")" ./steam_spike "../../../$DYLIB" | tee ../spike_result.txt
