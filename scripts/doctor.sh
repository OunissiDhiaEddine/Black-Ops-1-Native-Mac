#!/usr/bin/env bash
# Reports which tools are available. Paste the output into LOG.md.
cd "$(dirname "$0")/.."
for t in git cmake ninja clang python3 java xcodebuild xcrun brew ffmpeg wine wine64 7z i686-w64-mingw32-gcc; do
  if command -v "$t" >/dev/null 2>&1; then echo "OK   $t -> $(command -v $t)"; else echo "MISS $t"; fi
done
# Python tooling lives in ./.venv (scripts/setup_venv.sh); fall back to system python3.
PY=python3; [ -x .venv/bin/python ] && PY=.venv/bin/python
echo "python: $PY ($($PY --version 2>&1))"
$PY - <<'PY'
import importlib
for m in ["pefile","capstone","lief","unicorn","frida"]:
    try: importlib.import_module(m); print("OK   py:",m)
    except Exception: print("MISS py:",m)
PY
[ -d "/Applications/CrossOver.app" ] && echo "OK   CrossOver.app" || echo "MISS CrossOver.app"
G=$(ls -d /Applications/[Gg]hidra* ~/ghidra* /opt/homebrew/Caskroom/ghidra 2>/dev/null)
[ -n "$G" ] && echo "$G" | sed 's/^/OK   ghidra: /' || echo "MISS ghidra"
echo "chip: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) ($(uname -m))"
echo "ram:  $(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
echo "os:   $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
echo "xcode: $(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
echo "disk free: $(df -h . | awk 'NR==2{print $4}')"
