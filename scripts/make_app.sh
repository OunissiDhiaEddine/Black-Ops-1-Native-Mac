#!/usr/bin/env bash
# Assemble the personal Mac app and its DMG (P8 / T-077). For the owner's own use only: the app's binary is the
# recompiled game (CLAUDE.md rule 1) — never publish the app or the DMG.
#   scripts/make_app.sh [--dmg]      -> artifacts/app/Black Ops.app   (and artifacts/app/BlackOps-Mac.dmg)
# The app holds: bo1_boot, the Homebrew libraries it links (made relative to the bundle), the shader cache as it is
# now (artifacts/shader_cache), and a launcher. It does NOT hold the game's files: on first start the launcher asks
# for the folder of the owner's installed copy and builds a private game folder of links to it under
# ~/Library/Application Support/Black Ops Mac (saves and settings live there; the install is never written to).
set -euo pipefail
cd "$(dirname "$0")/.."
APP="artifacts/app/Black Ops.app"
C="$APP/Contents"
[ -x artifacts/boot/bo1_boot ] || { echo "build first: scripts/build_boot.sh"; exit 1; }
rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources"
cp artifacts/boot/bo1_boot "$C/MacOS/bo1_boot"

# every non-system library, followed through its own dependencies; install names rewritten to the bundle
.venv/bin/python - "$C" <<'PY'
import os, shutil, subprocess, sys
C = sys.argv[1]
def deps(p):
    out = subprocess.run(["otool", "-L", p], capture_output=True, text=True, check=True).stdout.split("\n")[1:]
    return [l.strip().split(" (")[0] for l in out if l.strip()]
def external(d): return not d.startswith(("/usr/lib", "/System", "@"))
done, work = {}, [f"{C}/MacOS/bo1_boot"]
while work:
    p = work.pop()
    for d in deps(p):
        if not external(d): continue
        real = os.path.realpath(d)
        name = os.path.basename(d)
        if name not in done:
            dst = f"{C}/Frameworks/{name}"
            shutil.copyfile(real, dst)
            os.chmod(dst, 0o755)
            done[name] = dst
            work.append(dst)
        subprocess.run(["install_name_tool", "-change", d, f"@executable_path/../Frameworks/{name}", p], check=True, capture_output=True)
for name, dst in done.items():
    subprocess.run(["install_name_tool", "-id", f"@executable_path/../Frameworks/{name}", dst], check=True, capture_output=True)
left = [d for p in [f"{C}/MacOS/bo1_boot", *done.values()] for d in deps(p) if external(d)]
if left: sys.exit("still outside the bundle: " + ", ".join(sorted(set(left))))
print(f"bundled {len(done)} libraries")
PY

if [ -d artifacts/shader_cache ]; then
  mkdir -p "$C/Resources/shader_cache"
  find artifacts/shader_cache/ -maxdepth 1 -name '*.metallib' -exec cp {} "$C/Resources/shader_cache/" \;
  echo "shader cache: $(ls "$C/Resources/shader_cache" | wc -l | tr -d ' ') libraries"
fi

cat > "$C/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Black Ops</string>
  <key>CFBundleDisplayName</key><string>Black Ops</string>
  <key>CFBundleIdentifier</key><string>local.didou.blackops-mac</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
PLIST

cat > "$C/MacOS/launch" <<'SH'
#!/bin/bash
# Launcher of the personal Black Ops app (scripts/make_app.sh). Everything it writes is under SUPPORT.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SUPPORT="$HOME/Library/Application Support/Black Ops Mac"
GAME="$SUPPORT/game"
mkdir -p "$SUPPORT/home"
say_and_quit() { osascript -e "display alert \"Black Ops\" message \"$1\" as critical" >/dev/null 2>&1; exit 1; }
if [ ! -e "$GAME/BlackOps.exe" ]; then
  SRC="$(osascript -e 'POSIX path of (choose folder with prompt "Choose the folder of your installed Call of Duty: Black Ops (the one that contains BlackOps.exe)")' 2>/dev/null)" || exit 0
  SRC="${SRC%/}"
  [ -e "$SRC/BlackOps.exe" ] && [ -d "$SRC/main" ] && [ -d "$SRC/zone" ] || say_and_quit "That folder has no BlackOps.exe with its main and zone folders."
  # a private game folder of links: the install is only read, saves and settings are written here
  rm -rf "$GAME"; mkdir -p "$GAME/players"
  for f in "$SRC"/*; do
    b="$(basename "$f")"
    [ "$b" = players ] || ln -s "$f" "$GAME/$b"
  done
fi
# compiled shaders: seed the cache once from the bundle; shaders met for the first time are compiled by the game
if [ ! -d "$SUPPORT/shader_cache" ] && [ -d "$HERE/Resources/shader_cache" ]; then
  cp -R "$HERE/Resources/shader_cache" "$SUPPORT/shader_cache"
fi
mkdir -p "$SUPPORT/shader_cache"
# a leftover "did not quit properly" marker would bring up the game's safe-mode question
rm -f "$SUPPORT/home/AppData/Local/Activision/CoD/__BlackOps"
[ -f "$SUPPORT/log.txt" ] && mv -f "$SUPPORT/log.txt" "$SUPPORT/log.previous.txt"
cd "$SUPPORT" || exit 1
export BO1_WINDOW=1 BO1_GAME_DIR="$GAME" BO1_HOME_DIR="$SUPPORT/home" BO1_SHADER_CACHE="$SUPPORT/shader_cache" BO1_VERBOSE="${BO1_VERBOSE:-1}"
exec "$HERE/MacOS/bo1_boot" --exe "$GAME/BlackOps.exe" --seconds 0 > "$SUPPORT/log.txt" 2>&1
SH
chmod +x "$C/MacOS/launch"
codesign --force --deep -s - "$APP" 2>&1 | tail -1
codesign --verify --deep "$APP" && echo "signed (ad hoc): $APP, $(du -sh "$APP" | cut -f1)"

if [ "${1:-}" = "--dmg" ]; then
  DMG=artifacts/app/BlackOps-Mac.dmg
  rm -f "$DMG"
  STAGE="$(mktemp -d)"
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -volname "Black Ops" -srcfolder "$STAGE" -ov -format UDZO "$DMG" | tail -1
  rm -rf "$STAGE"
  echo "$DMG: $(du -sh "$DMG" | cut -f1)"
fi
