#!/usr/bin/env bash
# Run once from the project root: ./scripts/bootstrap.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p reference artifacts re tools src/{runtime,platform,rhi,metal,shaders,lifted,native} \
         tests/{golden,diff,replay} docs scripts
for d in re tools src/runtime src/platform src/rhi src/metal src/shaders src/lifted src/native \
         tests/golden tests/diff tests/replay; do touch "$d/.gitkeep"; done
[ -d .git ] || git init -q
git add -A && git commit -qm "P0/scaffold: initial project structure" || true
echo "Bootstrapped. Next: put your BO1 install in ./reference/ and tell the agent: 'Read AGENTS.md and start.'"
