#!/usr/bin/env bash
# T-035: replay every recorded scenario in Metal and compare with the reference frames recorded with it.
#   scripts/replay_check.sh [threshold]      (SSIM, default 0.95)
# Needs: scripts/build.sh, artifacts/traces/<name>.bin and tests/golden/<name>/*.png. Both are recordings of the
# original game's Direct3D 9 calls and frames (docs/TRACE_FORMAT.md); they hold game data and are not in git. Writes artifacts/replay/<name>/ (frames), <name>_diff/ and
# re/replay_check.json (committed summary).
set -uo pipefail
cd "$(dirname "$0")/.."
THRESH="${1:-0.95}"
bash scripts/build.sh replay >/dev/null || exit 2
mkdir -p artifacts/replay
status=0
for dir in tests/golden/*/; do
  name=$(basename "$dir")
  trace="artifacts/traces/$name.bin"
  [ -f "$trace" ] && [ -f "$dir/metrics.json" ] || continue
  first=$(.venv/bin/python -c "import json;print(json.load(open('$dir/metrics.json'))['window_frames'][0])")
  rm -rf "artifacts/replay/$name" "artifacts/replay/${name}_diff"
  ./build/replay "$trace" --backend metal --frames "$first-$((first + 42))" --dump "artifacts/replay/$name" \
    >"artifacts/replay/$name.log" 2>&1
  echo "== $name: $(grep 'metal backend' "artifacts/replay/$name.log")"
  grep -E '^ +[0-9]+  |^  unrecorded' "artifacts/replay/$name.log" | head -8
  .venv/bin/python tools/compare_frames.py "$dir" "artifacts/replay/$name" --diff "artifacts/replay/${name}_diff" \
    --threshold "$THRESH" --json "artifacts/replay/$name.json" || status=1
done
.venv/bin/python - "$THRESH" <<'PY'
import glob, json, os, sys
out = {"threshold": float(sys.argv[1]), "scenarios": {}}
for p in sorted(glob.glob("artifacts/replay/*.json")):
    out["scenarios"][os.path.basename(p)[:-5]] = json.load(open(p))["frames"]
json.dump(out, open("re/replay_check.json", "w"), indent=1)
PY
exit $status
