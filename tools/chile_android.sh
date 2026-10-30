#!/bin/bash
# Put the "Apertura 2007" game version (docs/16, docs/17) on a phone over adb: push the mod
# (mods/apertura2007: version.txt, textures/, save/) to <files>/mods/. The app's launcher then
# offers it, and its first run copies the template save into <files>/versions/apertura2007/ --
# the phone's own (original) saves are never touched.
#   ANDROID_SERIAL=<id> tools/chile_android.sh
# Without adb, `tools/android_build.sh bundle` carries the same mod inside the APK.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADB="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
MOD="$ROOT/mods/apertura2007"
REMOTE=/sdcard/Android/data/com.pes6port/files/mods/apertura2007
[ -f "$MOD/version.txt" ] || { echo "no mod: run tools/chile_mod.sh first"; exit 1; }
"$ADB" shell am force-stop com.pes6port
"$ADB" shell mkdir -p "$REMOTE/save/ULES004760013000"
"$ADB" push "$MOD/version.txt" "$REMOTE/"
"$ADB" push "$MOD/textures" "$REMOTE/"
for f in DATA.BIN PARAM.BIN; do
    "$ADB" push "$MOD/save/ULES004760013000/$f" "$REMOTE/save/ULES004760013000/"
done
echo "done: choose \"Apertura 2007\" in the launcher when the app starts."
