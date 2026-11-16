# Test host di iRPlay

- `run_tests.sh`: test Rust; motore nativo per macOS con ASan/UBSan contro `server_test.cpp`
  (iPhone simulato: pairing, FairPlay, SETUP, video, audio, watchdog, TEARDOWN, input ostili);
  `parser_diff.cpp` (parser Rust contro le versioni C++ di `reference/`), `crypto_diff.cpp`
  (contro libsodium e Mbed TLS, solo qui), `srp_stress.cpp` (contro `srp_client.py`), verifica
  con VideoToolbox di H.264 e HEVC. Con `IRPLAY_TEST_DEVICE=<ip>:5555` esegue i test Rust
  anche sulla TV collegata con adb.
- `reference/`: i parser C++ che l'app usava prima di Rust, tenuti solo come metro di paragone.
- `third_party/mbedtls`: submodule git, solo per i test (`git submodule update --init`).
- `remote_client.cpp`: lo stesso iPhone simulato, ma contro l'app installata nell'emulatore TV.
  Richiede gli stream generati da `gen_h264.swift` (video H.264 AVCC con VideoToolbox) e
  `gen_eld.swift` (audio AAC-ELD 480 campioni con AudioToolbox).
  - video: `adb forward tcp:7000 tcp:7000`, poi `ADB=$(which adb) ./remote_client 10`
  - watchdog: `ADB=... ./remote_client 3 vanish`
  - audio: `adb emu redir add tcp:7100:7000`, poi `ADB=... ./remote_client 5 audio`
    (il redirect dell'emulatore serve perché adb forward non inoltra UDP).
  - musica: `adb emu redir add tcp:7100:7000`, poi `ADB=... ./remote_client 14 music`
    (ALAC, metadati di un brano noto, avanzamento, poi pausa come fa iOS: FLUSH e TEARDOWN dell'audio)
