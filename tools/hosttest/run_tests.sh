#!/bin/bash
# Test del motore nativo sul Mac (senza Android): simula un iPhone contro AirPlayServer.
# Copre pairing, FairPlay, SETUP, video AES-CTR, audio AES-CBC, watchdog, TEARDOWN,
# input malformati e fuzzing del parser plist. Compilato con ASan + UBSan.
# Requisiti: Xcode Command Line Tools, Homebrew libsodium (brew install libsodium),
# CMake e Ninja (quelli dell'Android SDK vanno bene).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
C=$ROOT/app/src/main/cpp; P=$C/third_party/playfair; M=$C/third_party/mbedtls
OUT=$HERE/build; mkdir -p "$OUT"
SODIUM=$(brew --prefix libsodium)
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
CMAKE_BIN=$(ls -d "$SDK"/cmake/*/bin 2>/dev/null | tail -1)
export PATH="$CMAKE_BIN:$PATH"

if [ ! -f "$OUT/mbed/library/libmbedcrypto.a" ]; then
  cmake -S "$M" -B "$OUT/mbed" -G Ninja -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES=OFF \
        -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C "$OUT/mbed" mbedcrypto >/dev/null
fi

# playfair: solo ASan. Contiene shift su int con segno formalmente indefiniti ma innocui:
# UBSan resta attivo sul nostro codice.
for f in playfair omg_hax hand_garble modified_md5 sap_hash; do
  clang -c -w -fsanitize=address -g -O1 -I"$P" "$P/$f.c" -o "$OUT/pf_$f.o"
done
# Encoder ALAC di Apple: solo per generare nei test pacchetti come quelli di iOS.
rm -f "$OUT"/alac_*.o
for f in ag_enc ag_dec dp_enc matrix_enc ALACBitUtilities EndianPortable; do
  clang -c -w -fsanitize=address -g -O1 -I"$HERE/alac_enc" "$HERE/alac_enc/$f.c" -o "$OUT/alacenc_$f.o"
done
clang++ -std=c++17 -c -w -fsanitize=address -g -O1 -I"$HERE/alac_enc" "$HERE/alac_enc/ALACEncoder.cpp" -o "$OUT/alacenc_ALACEncoder.o"

# Decoder ALAC dell'app: FFmpeg compilato per il Mac con lo stesso script dell'app.
FF="$OUT/ffmpeg"
[ -f "$FF/lib/libavcodec.a" ] || FFMPEG_WORK="$ROOT/build/ffmpeg" "$ROOT/tools/build_ffmpeg.sh" host "$FF"
SRC="$C/alac_decoder.cpp $C/now_playing.cpp $C/srp_server.cpp $C/sps_rewriter.cpp $C/airplay_server.cpp $C/airplay_media.cpp $C/hls_playlist.cpp $C/rtsp_message.cpp $C/airplay_info.cpp $C/airplay_crypto.cpp $C/media_streams.cpp $C/nal_parser.cpp $C/net_util.cpp $C/plist_reader.cpp $C/plist_writer.cpp"
clang++ -std=c++20 -Wall -Wextra -Wformat=2 -Werror -Wno-missing-field-initializers \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  -I"$C" -I"$P" -I"$M/include" -I"$SODIUM/include" -I"$FF/include" -I"$HERE/alac_enc" \
  "$HERE/server_test.cpp" $SRC "$OUT"/pf_*.o "$OUT"/alacenc_*.o "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" "$OUT/mbed/library/libmbedcrypto.a" "$SODIUM/lib/libsodium.a" \
  -o "$OUT/server_test"

cd "$OUT"
cp "$HERE/srp_client.py" .
plutil -convert binary1 -o utf16.bplist "$HERE/utf16.json"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./server_test 2>&1 | grep -v "^I/"

# Riscrittura SPS: unit test + fuzz, poi verifica indipendente con il decoder di VideoToolbox.
cd "$OUT"
swiftc -O "$HERE/gen_h264.swift" -o gen_h264 2>/dev/null && ./gen_h264 >/dev/null
clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$C" "$HERE/sps_test.cpp" "$C/sps_rewriter.cpp" "$C/nal_parser.cpp" -o sps_test
./sps_test
swiftc -O "$HERE/vt_check.swift" -o vt_check 2>/dev/null && ./vt_check
