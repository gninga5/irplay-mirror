# iRPlay Mirror

Ricevitore di duplicazione schermo per Android TV / Google TV: iPhone e iPad trasmettono
schermo e audio alla TV dal Centro di Controllo ("Duplica schermo"), senza Apple TV.

Codice sorgente: https://github.com/gninga5/irplay-mirror

Licenza: **GPL-3.0** (vedi `LICENSE` e la schermata "Licenze" dell'app).
iRPlay non è affiliato ad Apple Inc.

## Architettura

```
UI e ciclo di vita (Kotlin)        ui/MainActivity, core/AirPlayService, core/BootReceiver
  - SurfaceView, D-pad, IP           servizio in primo piano (mediaPlayback) + MulticastLock
  - mDNS con NsdManager              _airplay._tcp e _raop._tcp
        |  JNI (jni_bridge.cpp, core/NativeBridge.kt)
Motore di rete (C++20)             airplay_server, airplay_crypto, media_streams
  - RTSP/HTTP: /info, pair-setup, pair-verify, fp-setup, SETUP, RECORD, TEARDOWN...
  - Ed25519/X25519/SHA-512 (libsodium), AES-CTR/CBC (mbedTLS), FairPlay (playfair)
  - watchdog 3 s, una sessione alla volta
        |                                   |
Video: AVCC -> Annex-B (nal_parser)    Audio: RTP AAC-ELD (AES-CBC)
MediaCodec asincrono su SurfaceView    MediaCodec AAC + AudioTrack bassa latenza
(core/CodecManager)                    (audio/AudioRenderer), audio focus
```

Protocollo: AirPlay 1 con pairing "legacy" (feature bit 27), lo stesso usato da UxPlay e
RPiPlay. La catena delle chiavi è descritta in `app/src/main/cpp/airplay_crypto.h`.

## Compilare

1. Clonare con i submodule: `git clone --recursive https://github.com/gninga5/irplay-mirror.git` (oppure `git submodule update --init`).
2. Generare una volta i sorgenti di mbedTLS (serve Python 3 con `jinja2` e `jsonschema`):
   ```
   cd app/src/main/cpp/third_party/mbedtls
   python3 -m pip install -r scripts/basic.requirements.txt
   make generated_files
   ```
   La build Gradle usa `GEN_FILES=OFF` e non dipende da Python.
3. libsodium è in `app/libs/libsodium-1.0.22.0.aar` (Prefab), compilato con
   `dist-build/android-aar.sh` dai sorgenti ufficiali con firma minisign verificata.
4. `./gradlew :app:assembleRelease` → `app/build/outputs/apk/release/app-release.apk`
   (firmato con la chiave di debug: va bene per l'installazione manuale, non per il Play Store).

## Test

- `tools/hosttest/run_tests.sh`: motore nativo sul Mac con ASan/UBSan e iPhone simulato.
- `tools/hosttest/remote_client.cpp`: iPhone simulato contro l'app nell'emulatore TV
  (video H.264 reale, watchdog, audio AAC-ELD). Istruzioni in `tools/hosttest/README.md`.
- Emulatore: è dietro NAT e un iPhone vero non lo vede; con iPhone serve una TV reale.

## Installazione sulla TV (adb)

1. Sulla TV: Impostazioni → Sistema → Informazioni → premere 7 volte su "Build del sistema
   operativo Android TV" → nelle Opzioni sviluppatore attivare "Debug USB" (e "Debug ADB in
   rete" se presente).
2. Dal computer, sulla stessa rete: `adb connect <IP-della-TV>:5555`, accettare sulla TV.
3. `adb install -r iRPlay.apk`

## Limiti noti

- Solo H.264 (niente HEVC/4K) e solo duplicazione schermo: nessun AirPlay "video" (YouTube/
  app che inviano un URL) e nessun AirPlay 2 audio multiroom.
- Audio e video sono riprodotti appena arrivano, senza sincronizzazione NTP: la latenza
  resta bassa, ma su alcune TV l'audio può risultare leggermente sfasato.
- Apertura automatica dell'app all'avvio della duplicazione: solo se l'utente concede
  "Mostra sopra altre app" (pulsante "Apri automaticamente"); altrimenti si apre iRPlay a mano.
- Nessun PIN (pw=false): chiunque sulla stessa rete può trasmettere. Previsto per la Fase 6.

## Pubblicazione

- Il nome sullo Store non deve contenere "AirPlay" (marchio Apple).
- Play Console: dichiarare il servizio in primo piano `mediaPlayback` e il permesso
  `SYSTEM_ALERT_WINDOW`.
- GPL-3: pubblicare il sorgente corrispondente a ogni APK distribuito.
