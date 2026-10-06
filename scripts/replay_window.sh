#!/usr/bin/env bash
# Show a recorded scenario rendered by the Metal backend in a live window (P5/T-050).
#   scripts/replay_window.sh <menu|cuba|vorkuta|khe_sanh> [first_frame] [fps]
# Frames before first_frame are replayed for state only. Close the window or press Escape to stop.
set -euo pipefail
cd "$(dirname "$0")/.."
NAME="${1:-menu}"; FIRST="${2:-0}"; FPS="${3:-60}"
TRACE="artifacts/traces/$NAME.bin"
[ -f "$TRACE" ] || { echo "no trace $TRACE (scripts/capture_golden.sh records one)"; exit 2; }
[ -x build/replay ] || scripts/build.sh replay
exec build/replay "$TRACE" --window --fps "$FPS" --frames "$FIRST-1000000"
