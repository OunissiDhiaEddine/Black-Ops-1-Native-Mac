#!/usr/bin/env bash
# T-014: capture one golden scenario from the Wine baseline: frames, full trace window, frame times.
#   scripts/capture_golden.sh <name> <map|-> <first_frame> [seconds]
#     name         scenario id, e.g. menu, cuba, vorkuta
#     map          SP map to load with `devmap` 10 s after "Press Any Key" ("-" = stay in the main menu)
#     first_frame  first of 61 frames recorded with all uploads (and captured as PNG every 20)
# Results:
#   tests/golden/<name>/frame_NNNNNN.png   back buffers inside the window  (gitignored: game imagery)
#   tests/golden/<name>/metrics.json       frame-time statistics + provenance (committed)
#   artifacts/traces/<name>.bin            the trace (gitignored)
set -euo pipefail
cd "$(dirname "$0")/.."
NAME="$1"; MAP="$2"; FIRST="$3"; SECS="${4:-180}"
LAST=$((FIRST + 60))
CMDS=()
[ "$MAP" != "-" ] && CMDS=(--cmds "42000|devmap $MAP")
# The main-menu camera follows the mouse: pin the cursor to the window centre so the view is repeatable.
[ "$MAP" = "-" ] && CMDS=(--cursor 400,300)
WINEDEBUG="${WINEDEBUG:--all,warn+debugstr}" scripts/run_baseline.sh --wine cx --net --trace --record \
  --record-frames "$FIRST-$LAST" --record-max-mb 9000 --capture 20 --seconds "$SECS" \
  --keys "32000:0x20" ${CMDS[@]+"${CMDS[@]}"} >"artifacts/baseline/golden-$NAME.out" 2>&1 || true
STAMP=$(ls -t artifacts/baseline/d3d9_census-*.txt | head -1 | sed -E 's/.*census-(.*)\.txt/\1/')
DEST="tests/golden/$NAME"; mkdir -p "$DEST"; rm -f "$DEST"/frame_*.png
for f in artifacts/baseline/frames-"$STAMP"/frame_*.png; do
  n=$((10#$(basename "$f" .png | sed 's/frame_//')))
  if [ "$n" -ge "$FIRST" ] && [ "$n" -le "$LAST" ]; then cp "$f" "$DEST/"; fi
done
[ -f "artifacts/traces/trace-$STAMP.bin" ] && mv "artifacts/traces/trace-$STAMP.bin" "artifacts/traces/$NAME.bin"
.venv/bin/python - "$NAME" "$MAP" "$FIRST" "$LAST" "$STAMP" <<'PY'
import csv, json, statistics, subprocess, sys
name, mp, first, last, stamp = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
rows = list(csv.DictReader(open(f"artifacts/baseline/frametimes-{stamp}.csv")))
t = {int(r["frame"]): int(r["us"]) for r in rows}
def stats(a, b):
    d = [(t[i] - t[i - 1]) / 1000 for i in range(max(a, 2), b + 1) if i in t and i - 1 in t]
    if not d: return None
    d.sort()
    return {"frames": len(d), "mean_ms": round(statistics.mean(d), 2), "median_ms": round(d[len(d) // 2], 2),
            "p95_ms": round(d[int(len(d) * 0.95)], 2), "max_ms": round(d[-1], 2),
            "fps_mean": round(1000 / statistics.mean(d), 1)}
census = open(f"artifacts/baseline/d3d9_census-{stamp}.txt", errors="replace").read()
m = {"scenario": name, "map": None if mp == "-" else mp, "run_stamp": stamp,
     "window_frames": [first, last], "frames_presented": max(t) if t else 0,
     "frame_time_window": stats(first, last), "frame_time_last_600": stats(max(t) - 600, max(t)) if t else None,
     "note": "Baseline = WineCX 24.0.7 + wined3d under Rosetta 2, 800x600 windowed, com_maxfps 60, with the "
             "tracing proxy active. Frame times measure the BASELINE under tracing, not the game's potential.",
     "git": subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip(),
     "trace_stopped_early": "trace stopped" in census}
json.dump(m, open(f"tests/golden/{name}/metrics.json", "w"), indent=1)
print(json.dumps(m, indent=1))
PY
ls "$DEST"
