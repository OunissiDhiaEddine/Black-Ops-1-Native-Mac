#!/usr/bin/env bash
# Runs every golden scenario in sequence (about 3 minutes each). See scripts/capture_golden.sh.
set -uo pipefail
cd "$(dirname "$0")/.."
#            name      map       first_frame  seconds
SCENARIOS=( "menu      -         1700         80"
            "cuba      cuba      5200         180"
            "vorkuta   vorkuta   5200         180"
            "khe_sanh  khe_sanh  5200         180" )
for sc in "${SCENARIOS[@]}"; do
  read -r name map first secs <<<"$sc"
  echo "=== $name ($(date +%H:%M:%S))"
  bash scripts/capture_golden.sh "$name" "$map" "$first" "$secs" >"artifacts/baseline/golden-$name.summary" 2>&1
  tail -3 "artifacts/baseline/golden-$name.summary"
done
echo "ALL DONE $(date +%H:%M:%S)"
