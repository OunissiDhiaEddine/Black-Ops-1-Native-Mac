#!/usr/bin/env bash
# Builds the 32-bit d3d9.dll proxy logger -> artifacts/d3d9_proxy/d3d9.dll
# Needs: brew install mingw-w64
set -euo pipefail
cd "$(dirname "$0")"
OUT=../../artifacts/d3d9_proxy
mkdir -p "$OUT"
CXX=i686-w64-mingw32-g++
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
  -shared -static -static-libgcc -static-libstdc++ \
  -Wl,--enable-stdcall-fixup -Wl,--subsystem,windows \
  -o "$OUT/d3d9.dll" proxy.cpp thunks.S d3d9.def
i686-w64-mingw32-objdump -p "$OUT/d3d9.dll" | grep -A6 "Ordinal/Name Pointer" || true
ls -l "$OUT/d3d9.dll"
