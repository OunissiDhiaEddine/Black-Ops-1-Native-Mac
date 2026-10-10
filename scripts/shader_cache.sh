#!/usr/bin/env bash
# Fills the shader cache (P7): compiles every shader source the game left in the cache directory into a Metal
# library with Apple's offline compiler, so that the game loads it instead of compiling at the shader's first use.
#   scripts/shader_cache.sh [status]        JOBS=n (default 3) compilers at a time, at low priority
# The game (and the trace replay) leave <key>.metal for every shader they had to compile themselves, and the game
# also for every shader a level creates when it loads; this script turns each into <key>.metallib and removes the
# source. Directory: $BO1_SHADER_CACHE, default artifacts/shader_cache. Safe to interrupt and to run while playing
# (libraries appear by rename). A source the compiler rejects is kept as <key>.failed with the message.
set -uo pipefail
cd "$(dirname "$0")/.."
DIR="${BO1_SHADER_CACHE:-artifacts/shader_cache}"
mkdir -p "$DIR"
count() { find "$DIR/" -maxdepth 1 -name "*.$1" | wc -l | tr -d ' '; }
if [ "${1:-}" = status ]; then
  echo "$DIR: $(count metallib) libraries, $(count metal) sources waiting, $(count failed) failed, $(du -sh "$DIR/" | cut -f1)"
  exit 0
fi
todo=$(count metal)
[ "$todo" = 0 ] && { echo "shader cache: nothing to compile ($(count metallib) libraries)"; exit 0; }
xcrun -sdk macosx -f metal >/dev/null 2>&1 || { echo "Apple's Metal compiler is not installed (xcodebuild -downloadComponent MetalToolchain)"; exit 1; }
find "$DIR/" -maxdepth 1 -name "*.tmp*" -mmin +10 -delete
echo "shader cache: compiling $todo shaders ($(count metallib) libraries so far)"
start=$(date +%s)
export DIR
find "$DIR/" -maxdepth 1 -name "*.metal" -print0 | nice -n 15 xargs -0 -n 1 -P "${JOBS:-3}" sh -c '
  src="$1"; key=$(basename "$src" .metal); out="$DIR/$key.metallib"
  if xcrun -sdk macosx metal -w -o "$out.tmp$$" "$src" 2> "$DIR/$key.err"; then
    mv "$out.tmp$$" "$out" && rm -f "$src" "$DIR/$key.err"
  else
    rm -f "$out.tmp$$"; mv "$src" "$DIR/$key.failed"; cat "$DIR/$key.err" >> "$DIR/$key.failed"; rm -f "$DIR/$key.err"
  fi' sh
echo "shader cache: $(count metallib) libraries, $(count failed) failed, $(( $(date +%s) - start )) s"
