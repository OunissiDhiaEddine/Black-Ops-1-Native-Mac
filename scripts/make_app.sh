#!/usr/bin/env bash
# Assemble the personal Mac app and its DMG (P8 / T-077). For the owner's own use only: the app's binary is the
# recompiled game (CLAUDE.md rule 1) — never publish the app or the DMG.
#   scripts/make_app.sh [--dmg]      -> artifacts/app/Black Ops.app   (and artifacts/app/BlackOps-Mac.dmg)
# The app holds: bo1_boot, the Homebrew libraries it links (made relative to the bundle), the shader cache as it is
# now (artifacts/shader_cache), the program image the code was lifted from, and a launcher. It does NOT hold the
# game's data files (main/, zone/): the launcher looks for the owner's copy in the usual places or asks for its
# folder, and builds a private game folder of links to it under
# ~/Library/Application Support/Black Ops Mac (saves and settings live there; the install is never written to).
set -euo pipefail
cd "$(dirname "$0")/.."
APP="artifacts/app/Black Ops.app"
C="$APP/Contents"
[ -x artifacts/boot/bo1_boot ] || { echo "build first: scripts/build_boot.sh"; exit 1; }
rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources"
cp artifacts/boot/bo1_boot "$C/MacOS/bo1_boot"
# the program image the lifted code belongs to (data sections are read from it at start); with it in the bundle the
# app does not depend on which executable the user's copy of the game has
cp "reference/Call of Duty Black Ops/BlackOps.exe" "$C/Resources/program.image"

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
alert() { osascript -e "display alert \"Black Ops\" message \"$1\" as critical" >/dev/null 2>&1; }
is_game() { [ -f "$1/main/iw_00.iwd" ] && [ -f "$1/zone/Common/code_post_gfx.ff" ]; }
# The game's own files are not in the app. They are looked for in the usual places first; otherwise the user is
# told what is needed and picks the folder. A private folder of links is built from it: the install is only read,
# saves and settings are written under SUPPORT.
if ! is_game "$GAME"; then
  SRC=""
  for d in "$HOME/Games/Call of Duty Black Ops" "$HOME/Call of Duty Black Ops" "/Applications/Call of Duty Black Ops" \
           "$SUPPORT/Call of Duty Black Ops" "$HOME/Library/Application Support/Steam/steamapps/common/Call of Duty Black Ops"; do
    is_game "$d" && { SRC="$d"; break; }
  done
  while [ -z "$SRC" ]; do
    osascript -e 'display dialog "Black Ops needs the files of your own copy of Call of Duty: Black Ops (PC version).\n\n1. Put the game folder anywhere on this Mac — for example in a folder named Games in your home folder.\n2. Click Choose Folder and select it: the folder that contains the folders \"main\" and \"zone\".\n\nThe app only reads that folder. Saves and settings are kept separately." buttons {"Quit", "Choose Folder…"} default button 2 with title "Black Ops" with icon note' 2>/dev/null | grep -q "Choose" || exit 0
    PICK="$(osascript -e 'POSIX path of (choose folder with prompt "Select the Call of Duty: Black Ops folder (it contains main and zone)")' 2>/dev/null)" || continue
    PICK="${PICK%/}"
    if is_game "$PICK"; then SRC="$PICK"
    else alert "That folder does not look like the game: main/iw_00.iwd and zone/Common/code_post_gfx.ff were not found in it."; fi
  done
  mkdir -p "$SUPPORT/keep"; [ -d "$GAME/players" ] && [ ! -L "$GAME/players" ] && mv "$GAME/players" "$SUPPORT/keep/players"
  rm -rf "$GAME"; mkdir -p "$GAME"
  for f in "$SRC"/*; do
    b="$(basename "$f")"
    [ "$b" = players ] || ln -s "$f" "$GAME/$b"
  done
  if [ -d "$SUPPORT/keep/players" ]; then mv "$SUPPORT/keep/players" "$GAME/players"; else mkdir -p "$GAME/players"; fi
  rmdir "$SUPPORT/keep" 2>/dev/null
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
# The program image the app was translated from travels with the app (Resources/program.image), so the
# executable in the game folder is not used and may be any release of the same game version.
"$HERE/MacOS/bo1_boot" --exe "$HERE/Resources/program.image" --seconds 0 > "$SUPPORT/log.txt" 2>&1
code=$?
if [ $code -ne 0 ] && ! grep -aq "FATAL GAME ERROR" "$SUPPORT/log.txt"; then
  alert "The game stopped unexpectedly (code $code). The log is in ~/Library/Application Support/Black Ops Mac/log.txt"
fi
exit $code
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
  cat > "$STAGE/READ ME FIRST.txt" <<'TXT'
Black Ops for Mac — personal build

1. Drag "Black Ops" onto "Applications".
2. Have your own copy of Call of Duty: Black Ops (PC version) somewhere on this Mac, as a normal folder —
   for example  ~/Games/Call of Duty Black Ops . It is the folder that contains "main" and "zone".
3. Start Black Ops. The first time, macOS may refuse: right-click the app, choose Open, then Open.
   If the game folder is not in one of the usual places, the app asks you to choose it. It only reads that folder.

Saves, settings and the log are in  ~/Library/Application Support/Black Ops Mac .
To point the app at another game folder, delete the "game" folder there and start the app again.

This build is for its owner only. It contains program code translated from the game: do not pass it on.
TXT
  hdiutil create -volname "Black Ops" -srcfolder "$STAGE" -ov -format UDZO "$DMG" | tail -1
  rm -rf "$STAGE"
  echo "$DMG: $(du -sh "$DMG" | cut -f1)"
fi
