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

# Componente Rust: test Rust, poi la libreria statica per il Mac da collegare ai test C++.
RUSTDIR="$ROOT/app/src/main/rust"
CARGO=$(command -v cargo || true)
for c in "$HOME/.cargo/bin/cargo" /opt/homebrew/opt/rustup/bin/cargo; do
  [ -z "$CARGO" ] && [ -x "$c" ] && CARGO="$c"
done
[ -n "$CARGO" ] || { echo "cargo non trovato: installare Rust (rustup)" >&2; exit 1; }
export PATH="$(dirname "$CARGO"):$PATH"   # i proxy di rustup cercano rustc nel PATH
(cd "$RUSTDIR" && CARGO_TARGET_DIR="$OUT/rust-target" "$CARGO" test --release --locked -q 2>&1 | grep -E "test result|FAILED|panicked" ) || true
(cd "$RUSTDIR" && CARGO_TARGET_DIR="$OUT/rust-target" "$CARGO" build --release --locked -q)
RUSTLIB="$OUT/rust-target/release/libirplay_core.a"

SRC="$C/alac_decoder.cpp $C/now_playing.cpp $C/srp_server.cpp $C/sps_rewriter.cpp $C/airplay_server.cpp $C/airplay_media.cpp $C/hls_playlist.cpp $C/rtsp_message.cpp $C/airplay_info.cpp $C/airplay_crypto.cpp $C/media_streams.cpp $C/nal_parser.cpp $C/net_util.cpp $C/plist_reader.cpp $C/plist_writer.cpp"
clang++ -std=c++20 -Wall -Wextra -Wformat=2 -Werror -Wno-missing-field-initializers \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  -I"$C" -I"$P" -I"$M/include" -I"$SODIUM/include" -I"$HERE/alac_enc" \
  "$HERE/server_test.cpp" $SRC "$OUT"/pf_*.o "$OUT"/alacenc_*.o "$RUSTLIB" "$OUT/mbed/library/libmbedcrypto.a" "$SODIUM/lib/libsodium.a" \
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

# HEVC (rust/src/hevc.rs): flussi generati con VideoToolbox, con e senza riordino e in 4K.
# I parametri riscritti devono dare immagini identiche agli originali, e fotogrammi chiave e
# B-slice riconosciuti devono coincidere con quanto dichiara l'encoder.
HB="$OUT/hevc"; mkdir -p "$HB"
swiftc -O "$HERE/gen_hevc.swift" -o "$HB/gen_hevc" 2>/dev/null
swiftc -O "$HERE/vt_hevc_check.swift" -o "$HB/vt_hevc_check" 2>/dev/null
(cd "$RUSTDIR" && CARGO_TARGET_DIR="$OUT/rust-target" "$CARGO" build --release --locked -q --example hevc_check)
for spec in "lowlat 1920 1080 0" "reorder 1920 1080 1" "uhd 3840 2160 0"; do
  set -- $spec
  "$HB/gen_hevc" "$HB/$1" "$2" "$3" "$4" >/dev/null
  "$OUT/rust-target/release/examples/hevc_check" "$HB/$1"
  "$HB/vt_hevc_check" "$HB/$1"
done

# Facoltativo: i test Rust anche su una TV collegata con adb, compilati per la sua CPU
# (IRPLAY_TEST_DEVICE=192.168.1.10:5555). Verifica su ARM vero anche l'intercettazione
# dei panic e il fuzzing.
if [ -n "${IRPLAY_TEST_DEVICE:-}" ]; then
  NDKBIN="${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin"
  case "$(adb -s "$IRPLAY_TEST_DEVICE" shell getprop ro.product.cpu.abi | tr -d '\r')" in
    arm64-v8a) TGT=aarch64-linux-android; LNK="$NDKBIN/aarch64-linux-android28-clang"; VAR=CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER ;;
    *)         TGT=armv7-linux-androideabi; LNK="$NDKBIN/armv7a-linux-androideabi28-clang"; VAR=CARGO_TARGET_ARMV7_LINUX_ANDROIDEABI_LINKER ;;
  esac
  BIN=$(cd "$RUSTDIR" && env "$VAR=$LNK" CARGO_TARGET_DIR="$OUT/rust-android" "$CARGO" test --release --locked --target "$TGT" --no-run --message-format=json 2>/dev/null \
        | python3 -c 'import json,sys; print([j["executable"] for j in map(json.loads,sys.stdin) if j.get("executable") and j["target"]["name"]=="irplay_core"][0])')
  adb -s "$IRPLAY_TEST_DEVICE" push "$BIN" /data/local/tmp/irplay_core_test >/dev/null
  echo "test Rust su $IRPLAY_TEST_DEVICE ($TGT):"
  adb -s "$IRPLAY_TEST_DEVICE" shell "chmod 755 /data/local/tmp/irplay_core_test && /data/local/tmp/irplay_core_test -q; rm -f /data/local/tmp/irplay_core_test" | grep -E "test result|FAILED|panicked"
fi
