#!/usr/bin/env bash
# Loads campaign maps one after the other in the native build and reports how far each one gets (T-061).
#   scripts/map_sweep.sh [seconds per map] [map ...]      default: 120 s, every campaign map in story order
#   scripts/map_sweep.sh 0                                only rebuild the table from the logs that exist
# Each map runs in its own process, headless and silent, without movies: title screen, key at 35 s,
# `devmap <map>` at 50 s. Per map: did the level start (CL_InitCGame), was its start save written, how many
# frames were presented, did it stop (Com_ERROR / trap / host signal). Two frames near the end are kept.
# Output: artifacts/boot/sweep/<map>.log, <map>.png, <map>_earlier.png and the table artifacts/boot/sweep/summary.txt,
# which always lists every map that has a log (story order), whichever call produced it.
set -uo pipefail
cd "$(dirname "$0")/.."
SECS="${1:-120}"
shift || true
ALL=(cuba vorkuta pentagon flashpoint khe_sanh hue_city kowloon fullahead creek_1 river wmd_sr71 wmd pow rebirth int_escape underwaterbase outro)
MAPS=("$@")
[ ${#MAPS[@]} -eq 0 ] && MAPS=("${ALL[@]}")
[ "$SECS" = 0 ] && MAPS=()
OUT=artifacts/boot/sweep
mkdir -p "$OUT" artifacts/boot/home
[ -x artifacts/boot/bo1_boot ] || { echo "no artifacts/boot/bo1_boot: run scripts/build_boot.sh"; exit 1; }
for m in ${MAPS[@]+"${MAPS[@]}"}; do
  rm -rf "$OUT/frames"; mkdir -p "$OUT/frames"
  rm -f "artifacts/boot/home/AppData/Local/Activision/CoD/__BlackOps"
  echo "running $m for $SECS s"
  SDL_AUDIO_DRIVER=dummy BO1_VERBOSE=1 BO1_BINK=off BO1_KEYS="35000:0x20" BO1_CMDS="50000|devmap $m" BO1_D3D_DUMP="$OUT/frames" \
    artifacts/boot/bo1_boot --seconds "$SECS" > "$OUT/$m.log" 2>&1
  # two frames are kept: the last one and one about 10 s of play earlier (a single frame can be a fade)
  last=$(ls "$OUT/frames" 2>/dev/null | tail -1)
  earlier=$(ls "$OUT/frames" 2>/dev/null | tail -30 | head -1)
  [ -n "$last" ] && cp "$OUT/frames/$last" "$OUT/$m.png"
  [ -n "$earlier" ] && cp "$OUT/frames/$earlier" "$OUT/${m}_earlier.png"
done
rm -rf "$OUT/frames"

printf '%-15s %-8s %-10s %-7s %-8s %-9s %s\n' map started start-save frames objects lock-MiB stop | tee "$OUT/summary.txt"
for m in "${ALL[@]}"; do
  [ -f "$OUT/$m.log" ] || continue
  # the part of the log after the map command, as a file: `grep -q` on a pipe would end the pipe early (pipefail)
  n=$(grep -a -n "scripted command: devmap" "$OUT/$m.log" | head -1 | cut -d: -f1)
  tail -n +"${n:-1}" "$OUT/$m.log" > "$OUT/after.tmp"
  started=no; grep -a -q "CL_InitCGame:" "$OUT/after.tmp" && started=yes
  save=no; grep -a -q "players.save.$m.svg" "$OUT/after.tmp" && save=yes
  line=$(grep -a "frame [0-9]* presented; objects" "$OUT/$m.log" | tail -1)
  frames=$(echo "$line" | sed -n 's/.*frame \([0-9]*\) presented.*/\1/p')
  objects=$(echo "$line" | sed -n 's/.*objects: \([0-9]*\) alive.*/\1/p')
  lock=$(echo "$line" | sed -n 's/.* \([0-9.]*\) MiB lock memory.*/\1/p')
  stop=$(grep -a -m1 "guest stopped\|host signal\|Com_ERROR" "$OUT/$m.log" | cut -c1-90)
  grep -a -q "time limit reached" "$OUT/$m.log" || stop="${stop:-ended early (exit before the time limit)}"
  printf '%-15s %-8s %-10s %-7s %-8s %-9s %s\n' "$m" "$started" "$save" "${frames:-0}" "${objects:--}" "${lock:--}" "${stop:--}" | tee -a "$OUT/summary.txt"
done
rm -f "$OUT/after.tmp"
