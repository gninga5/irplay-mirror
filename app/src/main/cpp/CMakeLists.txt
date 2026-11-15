# ==============================================================================
# iRPlay – CMakeLists.txt
# Scopo: compila libirplay_native.so (motore RTSP / crypto / parser NAL) e collega:
#   - mbedTLS   (Apache-2.0): AES-128-CTR (video mirroring) e AES-128-CBC (audio)
#   - libsodium (ISC, Prefab AAR): Ed25519 / X25519 / SHA-512 per pair-setup/verify
#   - playfair  (GPL-3): decifratura della chiave FairPlay ricevuta in fp-setup
#   - FFmpeg    (LGPL-2.1+): solo il decoder ALAC, per l'audio "solo musica"
# ==============================================================================
cmake_minimum_required(VERSION 3.22.1)
project("irplay_native" C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
# Le librerie statiche vengono incluse in una .so: devono essere compilate come PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# --- mbedTLS: compiliamo solo la libreria crypto -------------------------------
# GEN_FILES OFF: i sorgenti generati esistono già (make generated_files), quindi
#   la build Gradle non dipende da Python o Jinja2.
# FATAL_WARNINGS OFF: i clang NDK più recenti aggiungono warning sconosciuti a
#   mbedTLS che bloccherebbero la build senza alcun impatto sulla sicurezza.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY}/mbedtls EXCLUDE_FROM_ALL)

# --- libsodium: pacchetto Prefab dall'AAR in app/libs ----------------------------
find_package(sodium REQUIRED CONFIG)

# --- playfair: target nostro al posto del CMakeLists originale -----------------
# Il file originale usa include_directories() globale, che "sporcherebbe" tutti i
# target. Qui gli include valgono solo per chi linka esplicitamente playfair.
add_library(playfair STATIC
        ${THIRD_PARTY}/playfair/playfair.c
        ${THIRD_PARTY}/playfair/omg_hax.c
        ${THIRD_PARTY}/playfair/hand_garble.c
        ${THIRD_PARTY}/playfair/modified_md5.c
        ${THIRD_PARTY}/playfair/sap_hash.c)
target_include_directories(playfair PUBLIC ${THIRD_PARTY}/playfair)
target_compile_options(playfair PRIVATE -w)   # codice upstream: i warning vanno visti solo sul nostro codice

# --- ALAC: decoder Apple Lossless di FFmpeg (LGPL-2.1+), librerie statiche precompilate ------
# Generate da tools/build_ffmpeg.sh (FFmpeg con il solo decoder ALAC, sorgente verificato
# con SHA-256). Per rigenerarle: tools/build_ffmpeg.sh android.
set(FFMPEG ${THIRD_PARTY}/ffmpeg)
add_library(ffmpeg_avcodec STATIC IMPORTED)
set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION ${FFMPEG}/lib/${ANDROID_ABI}/libavcodec.a
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG}/include)
add_library(ffmpeg_avutil STATIC IMPORTED)
set_target_properties(ffmpeg_avutil PROPERTIES IMPORTED_LOCATION ${FFMPEG}/lib/${ANDROID_ABI}/libavutil.a)

# --- Libreria iRPlay ---------------------------------------------------------------
add_library(${CMAKE_PROJECT_NAME} SHARED
        jni_bridge.cpp
        alac_decoder.cpp
        airplay_server.cpp
        airplay_media.cpp
        hls_playlist.cpp
        rtsp_message.cpp
        airplay_info.cpp
        airplay_crypto.cpp
        media_streams.cpp
        nal_parser.cpp
        net_util.cpp
        plist_reader.cpp
        plist_writer.cpp
        sps_rewriter.cpp
        srp_server.cpp
        now_playing.cpp)

# Hardening: questo codice legge pacchetti da una rete non fidata (la LAN).
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wformat=2 -Werror=format-security
        -fstack-protector-strong)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        -Wl,-z,relro -Wl,-z,now
        -Wl,-z,max-page-size=16384)   # pagine da 16 KB: necessarie sui dispositivi Android 15+

find_library(log-lib log)
target_link_libraries(${CMAKE_PROJECT_NAME}
        sodium::sodium-static
        mbedcrypto
        playfair
        ffmpeg_avcodec
        ffmpeg_avutil     # dopo avcodec: l'ordine conta per il linker con le librerie statiche
        ${log-lib}
        android)