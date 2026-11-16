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
The TV remote works on that screen too: previous track, play/pause and next track are buttons
you can reach with the arrows, and on the progress bar ◀ ▶ skip 10 seconds. With Apple Music
the skip isn't instant, because the phone only accepts fast-forward from a receiver like this
one: a 30-second jump takes about ten seconds.

The AirPlay button inside apps works too. In YouTube, Safari and many other apps, picking the
TV doesn't mirror the screen: the phone hands over the video and the TV plays it by itself, at
full quality, while the phone becomes a remote for pause, seek and stop. You can lock the
phone or use other apps in the meantime.

A few other things I wanted for myself:

- **A PIN the first time.** A new iPhone or Mac has to type the four digits shown on the TV
  before it can connect. After that it's remembered. You can ask for it every time, or turn it off.
- **A name for each TV.** By default it uses the name you already gave the TV in Android, so
  "Kitchen" and "Bedroom" don't get mixed up. If your TV has AirPlay built in (newer Sonys do),
  it shows up as "Kitchen (iRPlay)" so you can tell the two apart.
- **It can open itself** when you start mirroring, so you don't have to launch it first.
- **Video quality, up to 4K.** Standard is H.264, with the lowest delay. High (HEVC, 1080p) gives
  a cleaner picture for the same bandwidth, and Maximum (HEVC, 4K) is the sharpest, especially
  with iPad and Mac. The TV only offers what its hardware decoder can handle.
- The interface is in English or Italian, depending on the TV.

What it doesn't do: copy-protected videos (Apple TV+, films bought on iTunes: those only play on
an Apple TV) and multi-room audio, which needs PTP on UDP ports 319/320, out of reach for an app
on Android without root.

## Installing

Download the APK from the [latest release](https://github.com/gninga5/irplay-mirror/releases/latest)
and sideload it. With adb it takes a minute:

1. On the TV, go to Settings → System → About and press OK seven times on *Android TV OS build*.
   That unlocks the developer options, where you turn on USB debugging (and network debugging,
   if your TV has it).
2. From a computer on the same network, run `adb connect <tv-ip>:5555` and accept the prompt on the TV.
3. `adb install iRPlay-Mirror-1.7.0.apk`

You need Android TV or Google TV 9 or newer on an ARM chip, which is pretty much every TV and
streaming stick out there. Amazon Fire TV sticks with Fire OS 7 or 8 should work too (sideload
the APK with the *Downloader* app or adb); the newest ones running Vega OS aren't Android and
can't install it. I haven't been able to test a Fire TV myself yet.

One tip: switch the TV to its *Game* picture mode while you mirror. Most of the delay that's
left comes from the TV's own image processing, and Game mode turns it off.

## How it works

The TV speaks the same protocol an Apple TV does. Three layers: Kotlin for Android
(`app/src/main/java`), a C++ shell for sockets, threads and protocol flow (`app/src/main/cpp`),
and a Rust core for everything that reads bytes sent by the phone, plus all the cryptography
(`app/src/main/rust`). The C++ side only ever sees values Rust has already checked.

```
Kotlin   MainActivity (SurfaceView, remote)   AirPlayService (foreground service, mDNS)
           CodecManager (MediaCodec)             AudioRenderer (AudioTrack)
              ▲ JNI
C++      RTSP server · sessions · sockets and threads · FairPlay (playfair) · 3 s watchdog
              ▲ C ABI (rust_core.h, versioned; no panic crosses it)
Rust     plist · RTSP/HTTP · DMAP · HLS · H.264/HEVC (SPS/VPS rewrite) · ALAC (Symphonia)
         RTP reorder buffer · pairing, PIN (SRP-6a), keys · AES-CTR/CBC (ARMv8 instructions)
```

Some things took me a while to figure out, so I'll leave them here in case they save someone
else an evening:

- iOS never says whether its H.264 stream uses B-frames, so MediaTek decoders keep a reorder
  buffer "just in case" and hold back up to 18 frames. Rewriting the SPS to declare zero
  reorder frames (the same trick Moonlight uses) took latency from a few hundred milliseconds
  down to about 11 ms. If B-frames ever show up, the app goes back to the original SPS.
- After the first connection, iOS skips `pair-setup` and goes straight to `pair-verify`.
- With YouTube the phone doesn't send a link the TV can open, but `mlhls://localhost/master.m3u8`:
  the playlists live on the phone. The TV asks for them one by one over a reversed HTTP
  connection, expands YouTube's condensed segment URLs and feeds them to ExoPlayer straight
  from memory, so no extra port is opened on the TV.
- On the reversed connection iOS doesn't only answer `HTTP/1.1 200 OK`: every 15 seconds or so
  it sends an empty `EVENT/1.0 200 OK`. Treat that as an error and close the connection, and the
  TV can no longer ask for playlists. That's what broke changing the audio track in YouTube, which
  removes the video and inserts it again (`playlistRemove`, then `playlistInsert`).
- After that insert, the iPhone only follows the new video once the receiver sends a
  `currentItemChanged` event (`sessionID`, `type`, `uuid`, `itemCurrent`; no `category`). Without
  it the phone keeps a spinner and stops sending commands. UxPlay sends it *before* asking for the
  new playlists ([PR 579](https://github.com/FDH2/UxPlay/pull/579)); with iOS 27.2 that makes the
  phone ignore the playlist request, so here it goes out *after* the playlists have arrived.
  `/action` replies carry an `errorCode` plist, `/playback-info` names the current item's `uuid`
  and stays empty while the item isn't ready yet. Tested with iOS 27.2 and the YouTube app.
- The phone also reports the chosen tracks in `selectedMediaArray` (`soun` for audio, `sbtl`/`clcp`
  for subtitles, with a language tag), and the TV turns them into player preferences. YouTube
  itself never puts subtitles in what it casts: it switches captions off as soon as AirPlay
  starts, and the playlist has no subtitle tracks at all. Screen mirroring shows them.
- HEVC mirroring is feature bit 42 ("screen multi codec") plus a 3840×2160 display in `/info`;
  iOS then sends an `hvc1` configuration packet with VPS, SPS and PPS. The Rust module rewrites
  VPS and SPS to declare no frame reordering, like the H.264 trick, and falls back if B-slices
  show up. iOS 27.2 already sends them with no reordering, though: on a TCL with a Realtek chip
  the ~30 ms that 4K adds over H.264 is plain decoding time, not frames being held back.
- The `0x80` bit in the mDNS `flags` record means "password required". Set it by mistake and
  iOS 27 asks for a PIN, then gives up.
- In pause, iOS closes the audio stream and reopens it when you resume, while the session stays
  up. If you treat that as "the music stopped", the screen keeps flipping back and forth.
- iOS sends the audio about 1.8 s early and says, once a second, which sample should be playing
  right now. Playing packets as soon as they arrive works, but the TV ends up 1.8 s ahead of the
  phone, and every pause and resume makes the progress bar jump. The packets have to wait for
  their time, as on an Apple TV.
- While paused, iOS keeps sending packets for a few more seconds, and they're pure silence. Only
  real sound counts as "playing again".
- Controlling the music from the TV goes through DACP: the phone sends `DACP-ID` and
  `Active-Remote` in the RTSP headers and advertises `iTunes_Ctrl_<id>` over mDNS. The ID is a
  64-bit number and sometimes loses its leading zero on one side, so compare it as a number.
  Apple Music answers 400 to `dacp.playingtime`, but it does accept `beginff`/`playresume`.
- Apple's reference ALAC decoder trusts the sample count written in each packet, and it reads
  its own config byte-swapped on ARM. I patched both, then found out it has known CVEs that
  were never fixed upstream (the repository has been frozen since 2012). The app moved to
  FFmpeg's decoder, and now to [Symphonia](https://github.com/pdeljanov/Symphonia)'s, written
  in Rust with no `unsafe` code: over a million packets from Apple's reference encoder, eight
  kinds of signal, full and partial frames, it matches the original audio bit for bit, exactly
  like FFmpeg did. Apple's encoder is still in `tools/hosttest` to generate test packets.
- AES on 32-bit ARM: most Android TVs run a 32-bit system on an ARMv8 CPU that has AES
  instructions, but neither Mbed TLS (built for ARMv7) nor RustCrypto use them there. On the
  TCL the table-based AES of Mbed TLS did 25 MB/s, RustCrypto's constant-time one 19 MB/s; a
  small AESE/AESMC backend (`aes128.rs`, enabled only when the kernel reports HWCAP2_AES) does
  ~680 MB/s and is constant-time by construction. It's checked against the NIST vectors and
  against RustCrypto on the device itself.
- The Rust side catches its own panics at the C boundary (`guard` in `lib.rs`): a packet that
  trips a bug in a library costs that packet, not the app.

The key derivation chain is described at the top of `airplay_crypto.h`.

## Building it yourself

```
git clone https://github.com/gninga5/irplay-mirror.git
cd irplay-mirror
./gradlew :app:assembleRelease
```

You also need Rust: install `rustup` and the build picks the pinned compiler from
`app/src/main/rust/rust-toolchain.toml` (with the Android targets) by itself. Part of the
native engine is moving from C++ to Rust, starting with the code that reads data from the
network; that crate is built by Cargo and linked into the same native library. Already in
Rust: the parsers for binary plists, RTSP/HTTP messages, DMAP metadata and HLS playlists, all
the cryptography (pairing with and without PIN, session keys, audio and video decryption), the
H.264 and HEVC handling (including the SPS rewrite), the audio RTP reorder buffer and the ALAC
decoder. The old C++ parsers live on in `tools/hosttest/reference`
only as a yardstick: `parser_diff` feeds both millions of generated and corrupted inputs and
fails on the first difference.

The app links no C crypto library any more. The host tests still use Mbed TLS (git submodule in
`tools/hosttest/third_party`, fetch it with `git submodule update --init`) and Homebrew's
libsodium, but only to play the iPhone: an independent implementation to check ours against.

To sign with your own key, put `IRPLAY_STORE_FILE`, `IRPLAY_STORE_PASSWORD`,
`IRPLAY_KEY_ALIAS` and `IRPLAY_KEY_PASSWORD` in `~/.gradle/gradle.properties`. Without them the
release build falls back to the debug key.

### Tests

`tools/hosttest/run_tests.sh` runs, in order:
- the Rust unit tests (test vectors, fuzzing);
- the native engine built for macOS with AddressSanitizer and UBSan against a fake iPhone:
  pairing and PIN, FairPlay, encrypted video and audio, lossless ALAC round trips, the
  watchdog, malformed requests;
- `parser_diff`: every Rust parser against the C++ it replaced, on generated and corrupted
  input; `crypto_diff`: our crypto against libsodium and Mbed TLS; `srp_stress`: PIN pairings
  against an independent Python client;
- VideoToolbox decoding the rewritten H.264 and HEVC parameters;
- with `IRPLAY_TEST_DEVICE=<ip>:5555`, the Rust tests again on a real TV through adb (32-bit
  ARM there, hardware AES included).

There's also a client that talks to the app running in the TV emulator, described in
`tools/hosttest/README.md`.

## Credits and license

iRPlay Mirror is released under the **GPL-3.0** (see `LICENSE`).

It stands on the shoulders of [UxPlay](https://github.com/FDH2/UxPlay) and RPiPlay, whose work
on understanding the protocol made all of this possible. The FairPlay part uses *playfair*
(GPL-3) and reply tables by Juho Vähä-Herttua (LGPL-2.1+). Crypto comes from
[RustCrypto](https://github.com/RustCrypto) (MIT/Apache-2.0) and the
[dalek](https://github.com/dalek-cryptography) crates (BSD-3-Clause), Apple Lossless audio from
[Symphonia](https://github.com/pdeljanov/Symphonia)'s ALAC decoder (MPL-2.0), and the icons from Google's
Material Icons (Apache-2.0). The full notices are in the app, under *Licenses*.

iRPlay Mirror is an independent project and isn't affiliated with or endorsed by Apple Inc.
AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc.
