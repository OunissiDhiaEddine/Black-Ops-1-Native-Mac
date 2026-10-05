#!/usr/bin/env bash
# T-006: one-command, reproducible Ghidra headless analysis + export of BlackOps.exe.
#   scripts/ghidra_analyze.sh            import + auto-analyse + export   (slow: tens of minutes)
#   scripts/ghidra_analyze.sh --export   re-run only the export script on the existing project
# Outputs: re/ghidra_projects/bo1 (project, gitignored), re/ghidra_export/*.csv (gitignored),
#          re/ghidra_summary.json (committed), artifacts/ghidra/analyze.log
set -euo pipefail
cd "$(dirname "$0")/.."
GHIDRA_HOME="${GHIDRA_HOME:-/opt/homebrew/opt/ghidra/libexec}"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
[ -x "$HEADLESS" ] || { echo "analyzeHeadless not found under $GHIDRA_HOME (brew install ghidra)"; exit 1; }
# Homebrew's ghidra depends on openjdk@21; prefer it over whatever `java` is on PATH.
[ -d /opt/homebrew/opt/openjdk@21 ] && export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
EXE="reference/Call of Duty Black Ops/BlackOps.exe"
PROJ_DIR=re/ghidra_projects
EXPORT_DIR=re/ghidra_export
mkdir -p "$PROJ_DIR" "$EXPORT_DIR" artifacts/ghidra
# 8 GB machine: cap the JVM heap so analysis doesn't push the system into swap.
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-3G}"
if [ "${1:-}" = "--export" ]; then
  "$HEADLESS" "$PROJ_DIR" bo1 -process BlackOps.exe -noanalysis -readOnly \
    -scriptPath tools/ghidra -postScript ExportProgramFacts.java "$EXPORT_DIR" \
    -log artifacts/ghidra/export.log
else
  "$HEADLESS" "$PROJ_DIR" bo1 -import "$EXE" -overwrite \
    -scriptPath tools/ghidra -postScript ExportProgramFacts.java "$EXPORT_DIR" \
    -log artifacts/ghidra/analyze.log
fi
cp "$EXPORT_DIR/summary.json" re/ghidra_summary.json
cat re/ghidra_summary.json
