#!/bin/bash
# Build build/android/pes6port.apk: the native library with the NDK, then a
# NativeActivity APK packaged by hand (aapt2 + zipalign + apksigner; no Gradle).
# The only Java is runtime/android/java (javac + d8 -> classes.dex). Needs the
# Android SDK build-tools, a platform, the NDK and a JDK.
#   tools/android_build.sh            build
#   tools/android_build.sh install    build, install on the connected device and push the game data
#                                     (several devices: ANDROID_SERIAL=<id> tools/android_build.sh install)
#   tools/android_build.sh mods [install]
#                                     the APK carries the product's game version, mods/$PRODUCT
#                                     (runtime/common/product.h, docs/29), the replacement music in
#                                     mods/music (docs/11 §8) and the menu background, never the
#                                     game data (pushed as above): the APK that can be shared
#                                     PRODUCT= (empty): the multi-version port, every mods/<id> and
#                                     the general commentary in mods/commentary (docs/24)
#   tools/android_build.sh bundle [install]
#                                     the same, but the APK carries the game data (assets/EBOOT.elf,
#                                     assets/pes6.iso stored uncompressed, ~1.3 GB) and the game
#                                     versions in mods/ (docs/17): nothing to push.
#                                     For the user's own device only -- never distribute it.
#   tools/android_build.sh iso [install]
#                                     like mods, as its own app (com.pes6port.iso,
#                                     build/android/pes6port-iso.apk, beside the others): the
#                                     player picks his ISO in the menu and the app decrypts its
#                                     EBOOT (docs/35). Carries pspdecrypt: GPL-3.
#   VERSION=1.0 tools/android_build.sh iso release
#                                     the APK published on GitHub Releases (docs/35 §Publicar):
#                                     signed with the release key, not the debug one, so updates
#                                     install over it -- $PES6_KEYSTORE (default
#                                     ~/.android/pes6-release.jks, password in <keystore>.pass),
#                                     never in the repo. Out: build/android/PES6-<VERSION>.apk.
set -euo pipefail
BUNDLE=0; INSTALL=0; MODS=0; ISO=0; RELEASE=0
for a in "$@"; do case "$a" in bundle) BUNDLE=1; MODS=1;; mods) MODS=1;; iso) ISO=1; MODS=1;; install) INSTALL=1;; release) RELEASE=1;; esac; done
if [ "$RELEASE" = 1 ]; then
    [ "$ISO" = 1 ] || { echo "release: only the iso APK is published"; exit 1; }
    [[ "${VERSION:-}" =~ ^([0-9]+)\.([0-9]+)(\.([0-9]+))?$ ]] || { echo "release: VERSION=<major>.<minor>[.<patch>]"; exit 1; }
    VERSION_CODE=$(( BASH_REMATCH[1] * 10000 + BASH_REMATCH[2] * 100 + ${BASH_REMATCH[4]:-0} ))
    RKS="${PES6_KEYSTORE:-$HOME/.android/pes6-release.jks}"
    [ -f "$RKS" ] && [ -f "$RKS.pass" ] || { echo "release: no key at $RKS (+ $RKS.pass)"; exit 1; }
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK="${ANDROID_NDK_HOME:-$( (ls -d "$SDK"/ndk/*/ /opt/homebrew/share/android-commandlinetools/ndk/*/ 2>/dev/null || true) | sort | tail -1)}"
NDK="${NDK%/}"
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
PLATFORM="$(ls -d "$SDK"/platforms/android-3[4-9]* | sort -V | head -1)"
ADB="$SDK/platform-tools/adb"
OUT="$ROOT/build/android"
PKG=com.pes6port
APK=pes6port.apk
[ "$ISO" = 1 ] && { PKG=com.pes6port.iso; APK=pes6port-iso.apk; }
PRODUCT=${PRODUCT-pech2026}       # the one version the app plays (product.h); empty for all
[ -d "$NDK" ] || { echo "Android NDK not found (set ANDROID_NDK_HOME)"; exit 1; }
echo "NDK: $NDK"; echo "build-tools: $BT"; echo "platform: $PLATFORM"
# The EBOOT the generated code comes from: a decrypted ISO must give exactly it (docs/35).
eboot_defs() {
    [ -f "$ROOT/build/EBOOT.elf" ] || { echo "-DPES6_EBOOT_SIZE=0 -DPES6_EBOOT_FNV=0"; return; }
    python3 - "$ROOT/build/EBOOT.elf" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
h = 1469598103934665603
for b in d:
    h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
print('-DPES6_EBOOT_SIZE=%d -DPES6_EBOOT_FNV=0x%016xull' % (len(d), h))
PY
}

cmake -S "$ROOT/runtime/android" -B "$ROOT/runtime/android/build" \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
      -DPES6_PRODUCT_VERSION="$PRODUCT" $(eboot_defs) > "$OUT.cmake.log" 2>&1 \
  || { mkdir -p "$OUT"; tail -20 "$OUT.cmake.log"; exit 1; }
cmake --build "$ROOT/runtime/android/build" -j

# the work files go; the APKs already built stay (bundle, mods and iso live side by side)
mkdir -p "$OUT"; find "$OUT" -mindepth 1 -maxdepth 1 ! -name '*.apk' -exec rm -rf {} +
mkdir -p "$OUT/pkg/lib/arm64-v8a"
"$NDK/toolchains/llvm/prebuilt/"*/bin/llvm-strip -o "$OUT/pkg/lib/arm64-v8a/libpes6.so" \
    "$ROOT/runtime/android/build/libpes6.so"
# Launcher icon (tools/app_icon.py): from the user's logo in mods/app_icon/ when there is
# one (not versioned), else a plain one.
ICON_SRC=$(ls "$ROOT"/mods/app_icon/icon.* 2>/dev/null | head -1 || true)
python3 "$ROOT/tools/app_icon.py" ${ICON_SRC:+"$ICON_SRC"} "$OUT/res"
cp -R "$ROOT/runtime/android/res/." "$OUT/res/"     # the theme (values*/styles.xml): screen cutout
"$BT/aapt2" compile --dir "$OUT/res" -o "$OUT/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM/android.jar" \
    --manifest "$ROOT/runtime/android/AndroidManifest.xml" "$OUT/res.zip" \
    $( [ "$ISO" = 1 ] && echo --rename-manifest-package "$PKG" ) \
    $( [ "$RELEASE" = 1 ] && echo --version-name "$VERSION" --version-code "$VERSION_CODE" --replace-version )
(cd "$OUT/pkg" && zip -q -r ../base.apk lib)
# Java helpers (Wi-Fi Direct): classes.dex at the APK root.
mkdir -p "$OUT/classes" "$OUT/dex"
javac --release 8 -cp "$PLATFORM/android.jar" -d "$OUT/classes" $(find "$ROOT/runtime/android/java" -name '*.java')
"$BT/d8" --release --min-api 26 --lib "$PLATFORM/android.jar" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
(cd "$OUT/dex" && zip -q ../base.apk classes.dex)
if [ "$BUNDLE" = 1 ]; then
    # -0: stored, so the host reads the ISO in place (AAsset_openFileDescriptor64).
    mkdir -p "$OUT/pkg/assets"
    ln -f "$ROOT/build/EBOOT.elf" "$OUT/pkg/assets/EBOOT.elf"
    ln -f "$ROOT/data/pes6.iso" "$OUT/pkg/assets/pes6.iso"
    (cd "$OUT/pkg" && zip -q -0 ../base.apk assets/EBOOT.elf assets/pes6.iso)
fi
if [ "$MODS" = 1 ]; then
    # What goes in, as paths under mods/ (one per line), then linked into assets/mods/ with
    # assets/mods/index.txt ("<path> <size> <sha1>"), from which the app copies them into its
    # files directory on start.
    LIST="$OUT/mods_list.txt"; : > "$LIST"
    # Game versions (docs/17): every mods/<id>/ with a version.txt (only the product's, unless
    # PRODUCT is empty) -- its title, textures/, afs/, faces/, commentary/ and save/ (not the
    # .orig pristine copies, the .base copy of tools/roster_update.py, nor source images).
    for v in "$ROOT"/mods/*/version.txt; do
        [ -f "$v" ] || continue
        [ -z "$PRODUCT" ] || [ "$v" = "$ROOT/mods/$PRODUCT/version.txt" ] || continue
        id=$(basename "$(dirname "$v")")
        (cd "$ROOT/mods" && find "$id/version.txt" "$id/textures" "$id/afs" "$id/faces" "$id/commentary" "$id/save" -type f ! -name '*.orig' ! -name '*.base' 2>/dev/null) >> "$LIST"
    done
    # Textures for every version (mods/common/textures: the menu background, tools/menu_background.py).
    (cd "$ROOT/mods" && find common/textures -type f -name '*.pam' 2>/dev/null) >> "$LIST"
    # Replacement music (tools/music_replace.py, docs/11 §8): mods/music/track*.pcm, for every version.
    (cd "$ROOT/mods" && find music -maxdepth 1 -type f -name 'track*.pcm' 2>/dev/null) >> "$LIST"
    # Commentary (docs/24): mods/commentary/ = a voice bank and its catalogue (s_sound.afs +
    # s_sound.txt, cut down with tools/commentary/trim_bank.py). The product has its own
    # (mods/$PRODUCT/commentary).
    [ -z "$PRODUCT" ] && (cd "$ROOT/mods" && find commentary -maxdepth 1 -type f \( -name '*.afs' -o -name '*.txt' \) 2>/dev/null) >> "$LIST"
    # The original game's commentary (decision of the user, 6/11): the English PS2 bank
    # (Brackley and Brooking) in mods/commentary_en/ (e_sound.afs, cut down, + e_sound.txt); the
    # patch keeps Palma's. The bank is disc audio: a release build leaves it out unless
    # COMMENTARY_EN=1 (how the public APK gets it is still open).
    if [ "$RELEASE" = 0 ] || [ "${COMMENTARY_EN:-0}" = 1 ]; then
        (cd "$ROOT/mods" && find commentary_en -maxdepth 1 -type f \( -name '*.afs' -o -name '*.txt' \) 2>/dev/null) >> "$LIST"
    else
        echo "release: the English commentary bank left out (COMMENTARY_EN=1 puts it in)"
    fi
    python3 - "$ROOT/mods" "$OUT/pkg/assets/mods" "$LIST" <<'PY'
import hashlib, os, sys
src, dst, lst = sys.argv[1:]
os.makedirs(dst, exist_ok=True)
with open(os.path.join(dst, 'index.txt'), 'w') as ix:
    for rel in filter(None, open(lst).read().split('\n')):
        a, b = os.path.join(src, rel), os.path.join(dst, rel)
        os.makedirs(os.path.dirname(b), exist_ok=True)
        os.link(a, b)
        h = hashlib.sha1()
        with open(a, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        ix.write('%s %d %s\n' % (rel, os.path.getsize(a), h.hexdigest()))
PY
    [ -s "$OUT/pkg/assets/mods/index.txt" ] && (cd "$OUT/pkg" && zip -q -r ../base.apk assets/mods)
fi
"$BT/zipalign" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"; rm -f "$OUT/base.apk"   # disk: one copy at a time
KS="$ROOT/build/android-debug.keystore"
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug \
    -dname "CN=PES6PORT debug" -keyalg RSA -keysize 2048 -validity 10000 > /dev/null 2>&1
if [ "$RELEASE" = 1 ]; then
    APK="PES6-$VERSION.apk"
    "$BT/apksigner" sign --ks "$RKS" --ks-pass "file:$RKS.pass" \
        --out "$OUT/$APK" "$OUT/aligned.apk"
else
    "$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
        --out "$OUT/$APK" "$OUT/aligned.apk"
fi
rm -f "$OUT/base.apk" "$OUT/aligned.apk"; rm -rf "$OUT/pkg/assets"
ls -lh "$OUT/$APK"
# A firm rule (docs/29): an APK to share never carries the game. Only bundle may, for the user's own phone.
if [ "$BUNDLE" = 0 ] && unzip -l "$OUT/$APK" | grep -Eiq '\.(iso|cso|elf|prx|pbp)$|EBOOT'; then
    echo "error: $APK carries game data:"; unzip -l "$OUT/$APK" | grep -Ei '\.(iso|cso|elf|prx|pbp)$|EBOOT' | head
    rm -f "$OUT/$APK"; exit 1
fi

if [ "$INSTALL" = 1 ] && [ "$BUNDLE" = 1 ]; then
    "$ADB" install -r --no-incremental "$OUT/$APK"
    echo "installed (game data inside the APK)."
elif [ "$INSTALL" = 1 ] && [ "$ISO" = 1 ]; then
    "$ADB" install -r "$OUT/$APK"
    echo "installed: open it and choose the ISO in the menu."
elif [ "$INSTALL" = 1 ]; then
    "$ADB" install -r "$OUT/$APK"
    DIR=/sdcard/Android/data/$PKG/files
    "$ADB" shell mkdir -p "$DIR"
    # The game data is the user's own copy; it is pushed, never packaged.
    for f in "$ROOT/build/EBOOT.elf:EBOOT.elf" "$ROOT/data/pes6.iso:pes6.iso"; do
        src="${f%%:*}"; dst="$DIR/${f##*:}"
        have="$( ("$ADB" shell "stat -c %s $dst 2>/dev/null" || true) | tr -d '\r')"
        [ "$have" = "$(stat -f %z "$src")" ] || "$ADB" push "$src" "$dst"
    done
    echo "installed. start with: $ADB shell am start -n $PKG/android.app.NativeActivity"
fi
