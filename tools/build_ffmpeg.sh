#!/bin/bash
# Compila da FFmpeg solo il decoder ALAC (Apple Lossless), come librerie statiche.
#
#   tools/build_ffmpeg.sh android   -> app/src/main/cpp/third_party/ffmpeg/{include,lib/<abi>}
#   tools/build_ffmpeg.sh host DIR  -> DIR/{include,lib} per il Mac (test in tools/hosttest)
#
# Perché FFmpeg e non il decoder di riferimento di Apple: quello è fermo dal 2012, ha
# vulnerabilità note mai corrette (CVE-2021-0674, CVE-2021-0675, CVE-2021-30351) ed è stato
# abbandonato anche dai progetti che lo usavano. Il decoder di FFmpeg è mantenuto e passa
# di continuo per il fuzzing di OSS-Fuzz.
#
# Licenza: libavcodec/libavutil sono LGPL 2.1 o successiva; senza --enable-gpl e senza
# librerie esterne il risultato resta LGPL, compatibile con la GPL-3 dell'app.
#
# Il sorgente è verificato con SHA-256 (stesso contenuto del tag n9.0.2 del mirror GitHub).
# Requisiti: curl, make, Android NDK (per "android").
set -euo pipefail

VERSION=9.0.2
SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
NDK_VERSION=28.2.13676358
API=29

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${FFMPEG_WORK:-$ROOT/build/ffmpeg}
mkdir -p "$WORK"

fetch() {
  local tarball="$WORK/ffmpeg-$VERSION.tar.xz"
  if [ ! -f "$tarball" ]; then
    curl -sSfL "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" -o "$tarball.part"
    mv "$tarball.part" "$tarball"
  fi
  echo "$SHA256  $tarball" | shasum -a 256 -c - >/dev/null || { echo "SHA-256 di FFmpeg non corrisponde" >&2; exit 1; }
  rm -rf "$WORK/src"
  mkdir -p "$WORK/src"
  tar -xJf "$tarball" -C "$WORK/src" --strip-components=1
}

# Solo ciò che serve: libavcodec con il decoder ALAC, libavutil. Niente asm (il decoder è
# leggero, e così la build è uguale su ogni architettura), niente rete, niente librerie esterne.
COMMON=(
  --disable-everything --enable-decoder=alac
  --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter
  --disable-swscale --disable-swresample --disable-network --disable-autodetect
  --disable-asm --disable-debug --enable-static --disable-shared --enable-pic
)

build() {   # build <dir di installazione> <argomenti extra di configure...>
  local prefix=$1; shift
  local b="$WORK/build-$(basename "$prefix")-$RANDOM"
  mkdir -p "$b"
  (cd "$b" && "$WORK/src/configure" --prefix="$prefix" "${COMMON[@]}" "$@" >/dev/null && make -j8 >/dev/null && make install >/dev/null)
  rm -rf "$b"
}

case "${1:-}" in
  android)
    SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
    TC="$SDK/ndk/$NDK_VERSION/toolchains/llvm/prebuilt/darwin-x86_64/bin"
    OUT="$ROOT/app/src/main/cpp/third_party/ffmpeg"
    fetch
    rm -rf "$OUT/lib" "$OUT/include"
    for abi in arm64-v8a armeabi-v7a; do
      case $abi in
        arm64-v8a)   arch=aarch64; cpu=armv8-a;  cc="$TC/aarch64-linux-android$API-clang" ;;
        armeabi-v7a) arch=arm;     cpu=armv7-a;  cc="$TC/armv7a-linux-androideabi$API-clang" ;;
      esac
      build "$WORK/install-$abi" --enable-cross-compile --target-os=android --arch=$arch --cpu=$cpu \
        --cc="$cc" --ar="$TC/llvm-ar" --nm="$TC/llvm-nm" --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" \
        --extra-cflags="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2"
      mkdir -p "$OUT/lib/$abi"
      cp "$WORK/install-$abi/lib/libavcodec.a" "$WORK/install-$abi/lib/libavutil.a" "$OUT/lib/$abi/"
    done
    cp -R "$WORK/install-arm64-v8a/include" "$OUT/include"
    cp "$WORK/src/COPYING.LGPLv2.1" "$OUT/COPYING.LGPLv2.1"
    echo "$VERSION" > "$OUT/VERSION"
    ;;
  host)
    [ -n "${2:-}" ] || { echo "uso: $0 host DIR" >&2; exit 1; }
    fetch
    # AddressSanitizer anche dentro FFmpeg: il fuzzing dei test deve vedere ogni accesso fuori limite.
    build "$2" --extra-cflags="-O1 -g -fsanitize=address -fno-omit-frame-pointer" --extra-ldflags="-fsanitize=address"
    ;;
  *)
    echo "uso: $0 android | host DIR" >&2; exit 1 ;;
esac
