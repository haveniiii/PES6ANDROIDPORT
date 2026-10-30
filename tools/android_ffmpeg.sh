#!/bin/bash
# Build the only pieces of FFmpeg the port uses -- the H.264, ATRAC3 and
# ATRAC3plus decoders plus libswscale -- as static arm64 libraries for Android
# (LGPL configuration, nothing else enabled). Output: vendor/ffmpeg-android/
# {include,lib}; runtime/android/CMakeLists.txt picks it up when present.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER=7.1.1
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK="${ANDROID_NDK_HOME:-$( (ls -d "$SDK"/ndk/*/ /opt/homebrew/share/android-commandlinetools/ndk/*/ 2>/dev/null || true) | sort | tail -1)}"
NDK="${NDK%/}"
TC="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/ | head -1)"; TC="${TC%/}"
SRC="$ROOT/vendor/ffmpeg-$VER"
OUT="$ROOT/vendor/ffmpeg-android"
mkdir -p "$ROOT/vendor"
if [ ! -d "$SRC" ]; then
    curl -sSL "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz" -o "$ROOT/vendor/ffmpeg-$VER.tar.xz"
    tar -xf "$ROOT/vendor/ffmpeg-$VER.tar.xz" -C "$ROOT/vendor"
fi
cd "$SRC"
./configure --prefix="$OUT" --target-os=android --arch=aarch64 --enable-cross-compile \
    --cc="$TC/bin/aarch64-linux-android26-clang" --cxx="$TC/bin/aarch64-linux-android26-clang++" \
    --nm="$TC/bin/llvm-nm" --ar="$TC/bin/llvm-ar" --ranlib="$TC/bin/llvm-ranlib" --strip="$TC/bin/llvm-strip" \
    --sysroot="$TC/sysroot" \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-debug \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
    --enable-swscale --enable-decoder=h264 --enable-decoder=atrac3 --enable-decoder=atrac3p \
    --enable-parser=h264 --enable-static --disable-shared --enable-pic
make -j"$(sysctl -n hw.ncpu)"
make install
ls -lh "$OUT/lib/"*.a
