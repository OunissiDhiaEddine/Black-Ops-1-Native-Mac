#!/usr/bin/env bash
# Loads campaign maps one after the other in the native build and reports how far each one gets (T-061).
#   scripts/map_sweep.sh [seconds per map] [map ...]      default: 120 s, every campaign map in story order
# Each map runs in its own process, headless and silent, without movies: title screen, key at 35 s,
# `devmap <map>` at 50 s. Per map: did the level start (CL_InitCGame), was its start save written, how many
# frames were presented, did it stop (Com_ERROR / trap / host signal). One frame near the end is kept.
# Output: artifacts/boot/sweep/<map>.log, <map>.png, and the table artifacts/boot/sweep/summary.txt.
set -uo pipefail
cd "$(dirname "$0")/.."
SECS="${1:-120}"
shift || true
MAPS=("$@")
[ ${#MAPS[@]} -eq 0 ] && MAPS=(cuba vorkuta pentagon flashpoint khe_sanh hue_city kowloon fullahead creek_1 river wmd_sr71 wmd pow rebirth int_escape underwaterbase outro)
OUT=artifacts/boot/sweep
mkdir -p "$OUT" artifacts/boot/home
[ -x artifacts/boot/bo1_boot ] || { echo "no artifacts/boot/bo1_boot: run scripts/build_boot.sh"; exit 1; }
printf '%-15s %-8s %-10s %-7s %s\n' map started start-save frames stop | tee "$OUT/summary.txt"
for m in "${MAPS[@]}"; do
  rm -rf "$OUT/frames"; mkdir -p "$OUT/frames"
  rm -f "artifacts/boot/home/AppData/Local/Activision/CoD/__BlackOps"
  SDL_AUDIO_DRIVER=dummy BO1_VERBOSE=1 BO1_BINK=off BO1_KEYS="35000:0x20" BO1_CMDS="50000|devmap $m" BO1_D3D_DUMP="$OUT/frames" \
    artifacts/boot/bo1_boot --seconds "$SECS" > "$OUT/$m.log" 2>&1
  n=$(grep -a -n "scripted command: devmap" "$OUT/$m.log" | head -1 | cut -d: -f1)
  after() { tail -n +"${n:-1}" "$OUT/$m.log"; }
  started=no; after | grep -a -q "CL_InitCGame" && started=yes
  save=no; after | grep -a -q "players.save.$m.svg" && save=yes
  frames=$(grep -a -o "frame [0-9]* presented" "$OUT/$m.log" | tail -1 | awk '{print $2}')
  stop=$(grep -a -m1 "guest stopped\|host signal\|Com_ERROR" "$OUT/$m.log" | cut -c1-90)
  grep -a -q "time limit reached" "$OUT/$m.log" || stop="${stop:-ended early (exit before the time limit)}"
  last=$(ls "$OUT/frames" 2>/dev/null | tail -1)
  [ -n "$last" ] && cp "$OUT/frames/$last" "$OUT/$m.png"
  printf '%-15s %-8s %-10s %-7s %s\n' "$m" "$started" "$save" "${frames:-0}" "${stop:--}" | tee -a "$OUT/summary.txt"
done
rm -rf "$OUT/frames"
