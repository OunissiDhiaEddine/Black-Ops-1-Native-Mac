#!/usr/bin/env bash
# Frame statistics and CPU profiles of two heavy scenes, headless and silent (P6).
#   scripts/perf_scene.sh <tag>            -> artifacts/boot/<tag>.log, <tag>_sample1.txt, <tag>_sample2.txt
#   BIN=/path/to/other/bo1_boot scripts/perf_scene.sh <tag>     (compare another build on the same saves)
# Needs the checkpoint saves `cuba-1` (street fight, ~31,000 Direct3D calls per frame) and `cuba-2`
# (~15,000) in artifacts/game/players/save; 225 s. Frames while a profile is being taken (10 s from 110 s and
# from 200 s) are slowed by the profiler: read the windows next to them. A first run in a new folder compiles
# every shader it meets (stalls of seconds, once): run it twice.
# Read the profiles with tools/sample_tree.py <file> "MetalRenderer::draw_indexed" 3.
set -uo pipefail
cd "$(dirname "$0")/.."
tag="${1:?usage: scripts/perf_scene.sh <tag>}"
mkdir -p artifacts/boot/home
rm -f "artifacts/boot/home/AppData/Local/Activision/CoD/__BlackOps"
SDL_AUDIO_DRIVER=dummy BO1_BINK=off BO1_FRAME_STATS=300 BO1_KEYS="35000:0x20" BO1_CMDS="50000|loadgame cuba-1;135000|loadgame cuba-2" \
  "${BIN:-artifacts/boot/bo1_boot}" --seconds 225 > "artifacts/boot/$tag.log" 2>&1 &
pid=$!
sleep 110; sample "$pid" 10 -file "artifacts/boot/${tag}_sample1.txt" >/dev/null 2>&1
sleep 80;  sample "$pid" 10 -file "artifacts/boot/${tag}_sample2.txt" >/dev/null 2>&1
wait "$pid"
grep -a "frame stats: frame\|scripted command\|time limit\|guest stopped\|Com_ERROR\|host signal" "artifacts/boot/$tag.log" | cut -c13-230
