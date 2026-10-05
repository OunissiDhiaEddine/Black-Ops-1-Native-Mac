#!/usr/bin/env bash
# Builds the native (arm64) targets with CMake + Ninja into ./build.
#   scripts/build.sh [target...]
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}" >/dev/null
cmake --build build -- "$@"
