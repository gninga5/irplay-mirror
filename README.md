<p align="center">
  <img src="docs/banner.svg" alt="iRPlay Mirror" width="480">
</p>

<p align="center">
  Mirror your iPhone, iPad or Mac to an Android TV. No Apple TV needed.
</p>

<p align="center">
  <a href="https://github.com/gninga5/irplay-mirror/releases/latest">Download</a> ·
  <a href="#building-it-yourself">Build it</a> ·
  <a href="README.it.md">Italiano</a>
</p>

---

It started with a simple wish: put my iPhone on the living room TV without buying an Apple TV.
The receivers I tried on Google TV were either laggy, full of ads, or both, so I ended up
writing my own.

iRPlay Mirror runs on the TV and shows up in **Control Center → Screen Mirroring** on any
iPhone, iPad or Mac on the same network. Tap it and your screen, sound included, is on the TV.

<p align="center">
  <img src="docs/home.png" alt="Home screen on the TV" width="720">
</p>

## What it can do

The main thing is screen mirroring, and I spent a lot of time making it feel immediate. On my
Sony (a MediaTek MT5895 inside) a frame goes from the network to the panel in about 10–20 ms,
at up to 60 fps. Press BACK on the remote to stop; if the phone wanders out of Wi-Fi range,
the TV notices within a few seconds and goes back to its home screen on its own.

It also works as an AirPlay speaker. Pick the TV from Apple Music, Spotify or a podcast app
and you get a "Now playing" screen with the artwork, the progress bar and, when they can be
found, the lyrics scrolling in time with the song. AirPlay doesn't send lyrics, so the app
looks them up online: first [lrcmux.dev](https://lrcmux.dev), then [LRCLIB](https://lrclib.net),
then [lyrics.ovh](https://lyrics.ovh). Only the song title, artist, album and length leave the
TV, nothing is sent when it's offline, and you can switch it off from the home screen.

A few other things I wanted for myself:

- **A PIN the first time.** A new iPhone or Mac has to type the four digits shown on the TV
  before it can connect. After that it's remembered. You can ask for it every time, or turn it off.
- **A name for each TV.** By default it uses the name you already gave the TV in Android, so
  "Kitchen" and "Bedroom" don't get mixed up. If your TV has AirPlay built in (newer Sonys do),
  it shows up as "Kitchen (iRPlay)" so you can tell the two apart.
- **It can open itself** when you start mirroring, so you don't have to launch it first.
- The interface is in English or Italian, depending on the TV.

What it doesn't do yet: HEVC/4K, AirPlay "video" (when an app like YouTube hands the TV a link
instead of mirroring the screen) and multi-room audio.

## Installing

Download the APK from the [latest release](https://github.com/gninga5/irplay-mirror/releases/latest)
and sideload it. With adb it takes a minute:

1. On the TV, go to Settings → System → About and press OK seven times on *Android TV OS build*.
   That unlocks the developer options, where you turn on USB debugging (and network debugging,
   if your TV has it).
2. From a computer on the same network, run `adb connect <tv-ip>:5555` and accept the prompt on the TV.
3. `adb install iRPlay-Mirror-1.2.0.apk`

You need Android TV or Google TV 10 or newer on an ARM chip, which is pretty much every TV and
streaming stick out there.

One tip: switch the TV to its *Game* picture mode while you mirror. Most of the delay that's
left comes from the TV's own image processing, and Game mode turns it off.

## How it works

The TV speaks the same protocol an Apple TV does. Networking and crypto live in C++
(`app/src/main/cpp`), the Android side is Kotlin (`app/src/main/java`).

```
Kotlin   MainActivity (SurfaceView, remote)   AirPlayService (foreground service, mDNS)
           CodecManager (MediaCodec)             AudioRenderer (AudioTrack)
              ▲ JNI
C++      RTSP server · pairing (Ed25519/X25519) · PIN (SRP-6a) · FairPlay handshake
         AES-CTR video / AES-CBC audio · H.264 AVCC → Annex-B · SPS rewrite · ALAC · 3 s watchdog
```

Some things took me a while to figure out, so I'll leave them here in case they save someone
else an evening:

- iOS never says whether its H.264 stream uses B-frames, so MediaTek decoders keep a reorder
  buffer "just in case" and hold back up to 18 frames. Rewriting the SPS to declare zero
  reorder frames (the same trick Moonlight uses) took latency from a few hundred milliseconds
  down to about 11 ms. If B-frames ever show up, the app goes back to the original SPS.
- After the first connection, iOS skips `pair-setup` and goes straight to `pair-verify`.
- The `0x80` bit in the mDNS `flags` record means "password required". Set it by mistake and
  iOS 27 asks for a PIN, then gives up.
- In pause, iOS closes the audio stream and reopens it when you resume, while the session stays
  up. If you treat that as "the music stopped", the screen keeps flipping back and forth.
- Apple's reference ALAC decoder trusts the sample count written in each packet, and it reads
  its own config byte-swapped on ARM. Both are fixed here, see `third_party/alac/PATCHES.md`.

The key derivation chain is described at the top of `airplay_crypto.h`.

## Building it yourself

```
git clone --recursive https://github.com/gninga5/irplay-mirror.git
cd irplay-mirror/app/src/main/cpp/third_party/mbedtls
python3 -m pip install -r scripts/basic.requirements.txt
make generated_files
cd -
./gradlew :app:assembleRelease
```

The mbedTLS step is needed only once; the Gradle build itself doesn't need Python. libsodium
comes as a prebuilt AAR in `app/libs`, built from the official sources with
`dist-build/android-aar.sh`.

To sign with your own key, put `IRPLAY_STORE_FILE`, `IRPLAY_STORE_PASSWORD`,
`IRPLAY_KEY_ALIAS` and `IRPLAY_KEY_PASSWORD` in `~/.gradle/gradle.properties`. Without them the
release build falls back to the debug key.

### Tests

`tools/hosttest/run_tests.sh` builds the native engine for macOS with AddressSanitizer and
UBSan and runs it against a fake iPhone: pairing and PIN, FairPlay, encrypted video and audio,
lossless ALAC round trips, the watchdog, malformed requests, and a few hundred thousand fuzzed
plists, SPS and ALAC packets. There's also a client that talks to the app running in the TV
emulator, described in `tools/hosttest/README.md`.

## Credits and license

iRPlay Mirror is released under the **GPL-3.0** (see `LICENSE`).

It stands on the shoulders of [UxPlay](https://github.com/FDH2/UxPlay) and RPiPlay, whose work
on understanding the protocol made all of this possible. The FairPlay part uses *playfair*
(GPL-3) and reply tables by Juho Vähä-Herttua (LGPL-2.1+). Crypto comes from
[Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0) and
[libsodium](https://libsodium.org) (ISC), Apple Lossless audio from Apple's
[ALAC](https://github.com/macosforge/alac) decoder (Apache-2.0), and the icons from Google's
Material Icons (Apache-2.0). The full notices are in the app, under *Licenses*.

iRPlay Mirror is an independent project and isn't affiliated with or endorsed by Apple Inc.
AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc.
