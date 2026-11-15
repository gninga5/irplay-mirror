# Test host di iRPlay

- `run_tests.sh`: compila il motore nativo per macOS con ASan/UBSan ed esegue `server_test.cpp`
  (iPhone simulato: pairing, FairPlay, SETUP, video, audio, watchdog, TEARDOWN, input ostili,
  300.000 plist mutati).
- `remote_client.cpp`: lo stesso iPhone simulato, ma contro l'app installata nell'emulatore TV.
  Richiede gli stream generati da `gen_h264.swift` (video H.264 AVCC con VideoToolbox) e
  `gen_eld.swift` (audio AAC-ELD 480 campioni con AudioToolbox).
  - video: `adb forward tcp:7000 tcp:7000`, poi `ADB=$(which adb) ./remote_client 10`
  - watchdog: `ADB=... ./remote_client 3 vanish`
  - audio: `adb emu redir add tcp:7100:7000`, poi `ADB=... ./remote_client 5 audio`
    (il redirect dell'emulatore serve perché adb forward non inoltra UDP).
