<p align="center">
  <img src="docs/banner.svg" alt="iRPlay Mirror" width="480">
</p>

<p align="center">
  Duplica lo schermo di iPhone, iPad o Mac su una TV Android. Senza Apple TV.
</p>

<p align="center">
  <a href="https://github.com/gninga5/irplay-mirror/releases/latest">Scarica</a> ·
  <a href="#compilarla-da-sé">Compilala</a> ·
  <a href="README.md">English</a>
</p>

---

Tutto è partito da un desiderio semplice: vedere l'iPhone sulla TV del salotto senza comprare
una Apple TV. I ricevitori che ho provato su Google TV erano lenti, pieni di pubblicità, o
tutte e due le cose. Così alla fine me ne sono scritto uno.

iRPlay Mirror gira sulla TV e compare in **Centro di Controllo → Duplica schermo** su qualsiasi
iPhone, iPad o Mac collegato alla stessa rete. Lo tocchi e lo schermo, audio compreso, è sulla TV.

<p align="center">
  <img src="docs/home.png" alt="La home dell'app sulla TV" width="720">
</p>

## Cosa sa fare

La cosa principale è la duplicazione dello schermo, e ci ho passato parecchio tempo perché
sembrasse immediata. Sulla mia Sony (dentro c'è un MediaTek MT5895) un fotogramma passa dalla
rete allo schermo in circa 10–20 ms, fino a 60 fotogrammi al secondo. Per smettere premi
INDIETRO sul telecomando; se il telefono esce dalla portata del Wi-Fi, la TV se ne accorge in
pochi secondi e torna da sola alla sua schermata iniziale.

Fa anche da altoparlante AirPlay. Scegli la TV da Apple Music, Spotify o da un'app di podcast e
compare una schermata "In riproduzione" con la copertina, la barra di avanzamento e, quando si
trova, il testo che scorre a tempo con la canzone. AirPlay i testi non li manda, quindi l'app li
cerca online: prima su [lrcmux.dev](https://lrcmux.dev), poi su [LRCLIB](https://lrclib.net),
poi su [lyrics.ovh](https://lyrics.ovh). Dalla TV escono solo titolo, artista, album e durata
del brano, se la TV è offline non parte nessuna richiesta, e dalla home si può spegnere.
Su quella schermata funziona anche il telecomando della TV: brano precedente, play/pausa e brano
successivo sono pulsanti che raggiungi con le frecce, e sulla barra di avanzamento ◀ ▶ saltano
di 10 secondi. Con Apple Music il salto non è immediato, perché da un ricevitore come questo il
telefono accetta solo l'avanti veloce: un salto di 30 secondi richiede una decina di secondi.

Funziona anche il pulsante AirPlay dentro le app. In YouTube, Safari e in molte altre app,
scegliendo la TV lo schermo non viene duplicato: il telefono passa il video e la TV lo riproduce
da sola, alla qualità piena, mentre il telefono diventa un telecomando per pausa, salti e stop.
Nel frattempo puoi bloccarlo o usare altre app.

Poi ci sono alcune cose che volevo per me:

- **Un PIN la prima volta.** Un iPhone o un Mac nuovo deve digitare le quattro cifre che compaiono
  sulla TV prima di potersi collegare; dopo, viene ricordato. Si può chiedere ogni volta, oppure
  disattivare.
- **Un nome per ogni TV.** Di default usa il nome che hai già dato alla TV in Android, così
  "Cucina" e "Camera" non si confondono. Se la TV ha già AirPlay integrato (le Sony recenti ce
  l'hanno), compare come "Cucina (iRPlay)" per distinguere le due voci.
- **Si apre da sola** quando inizi a duplicare lo schermo, senza doverla avviare prima.
- L'interfaccia è in italiano o in inglese, a seconda della lingua della TV.

Cosa non fa ancora: la duplicazione in HEVC/4K, i video protetti (Apple TV+, film comprati su
iTunes: quelli si vedono solo su una Apple TV) e l'audio su più stanze.

## Installarla

Scarica l'APK dall'[ultima release](https://github.com/gninga5/irplay-mirror/releases/latest)
e installalo a mano. Con adb ci vuole un minuto:

1. Sulla TV vai in Impostazioni → Sistema → Informazioni e premi OK sette volte su *Build del
   sistema operativo Android TV*. Si sbloccano le opzioni sviluppatore, dove attivi il debug USB
   (e il debug di rete, se la tua TV lo ha).
2. Da un computer sulla stessa rete esegui `adb connect <ip-della-tv>:5555` e accetta la richiesta
   che compare sulla TV.
3. `adb install iRPlay-Mirror-1.3.2.apk`

Serve Android TV o Google TV 9 o successivi su processore ARM, cioè praticamente qualsiasi TV
o chiavetta in commercio. Dovrebbero andare anche le chiavette Amazon Fire TV con Fire OS 7 o 8
(l'APK si installa con l'app *Downloader* o con adb); le più recenti con Vega OS non sono
Android e non possono installarla. Su una Fire TV vera non ho ancora potuto provarla.

Un consiglio: mentre duplichi lo schermo metti la TV in modalità immagine *Gioco*. Gran parte del
ritardo che resta viene dall'elaborazione dell'immagine della TV stessa, e la modalità Gioco la
spegne.

## Come funziona

La TV parla lo stesso protocollo di una Apple TV. Rete e crittografia sono in C++
(`app/src/main/cpp`), la parte Android è in Kotlin (`app/src/main/java`).

```
Kotlin   MainActivity (SurfaceView, telecomando)   AirPlayService (servizio in primo piano, mDNS)
           CodecManager (MediaCodec)                  AudioRenderer (AudioTrack)
              ▲ JNI
C++      server RTSP · pairing (Ed25519/X25519) · PIN (SRP-6a) · handshake FairPlay
         video AES-CTR / audio AES-CBC · H.264 AVCC → Annex-B · riscrittura SPS · ALAC · watchdog 3 s
```

Alcune cose mi hanno fatto perdere un po' di tempo, quindi le lascio qui: magari a qualcun
altro risparmiano una serata.

- iOS non dice mai se il suo flusso H.264 usa B-frame, così i decoder MediaTek tengono un buffer
  di riordino "per sicurezza" e trattengono fino a 18 fotogrammi. Riscrivere l'SPS dichiarando
  zero fotogrammi da riordinare (lo stesso trucco di Moonlight) ha portato la latenza da qualche
  centinaio di millisecondi a circa 11 ms. Se un giorno arrivassero B-frame, l'app torna all'SPS
  originale.
- Dopo il primo collegamento iOS salta `pair-setup` e passa direttamente a `pair-verify`.
- Con YouTube il telefono non manda un link che la TV possa aprire, ma `mlhls://localhost/master.m3u8`:
  le playlist stanno sul telefono. La TV le chiede una alla volta su una connessione HTTP
  "rovesciata", espande gli indirizzi abbreviati dei segmenti di YouTube e le passa a ExoPlayer
  direttamente dalla memoria, senza aprire altre porte sulla TV.
- Il bit `0x80` nel campo `flags` dell'annuncio mDNS significa "password richiesta". Se lo metti
  per sbaglio, iOS 27 chiede un PIN e poi rinuncia.
- In pausa iOS chiude il flusso audio e lo riapre alla ripresa, mentre la sessione resta aperta.
  Se lo interpreti come "la musica è finita", lo schermo continua a saltare avanti e indietro.
- iOS manda l'audio con circa 1,8 s di anticipo e, una volta al secondo, dice quale campione
  dovrebbe suonare in quel momento. Suonare i pacchetti appena arrivano funziona, ma la TV finisce
  1,8 s avanti rispetto al telefono, e a ogni pausa e ripresa la barra salta. I pacchetti devono
  aspettare il loro momento, come su una Apple TV.
- In pausa iOS continua a mandare pacchetti per qualche secondo, ed è silenzio puro. Solo il
  suono vero conta come "la musica è ripartita".
- I comandi alla musica dalla TV passano per DACP: il telefono manda `DACP-ID` e `Active-Remote`
  nelle intestazioni RTSP e annuncia `iTunes_Ctrl_<id>` via mDNS. L'ID è un numero a 64 bit e a
  volte da una delle due parti perde lo zero iniziale, quindi va confrontato come numero.
  Apple Music risponde 400 a `dacp.playingtime`, ma accetta `beginff`/`playresume`.
- Il decoder ALAC di riferimento di Apple si fida del numero di campioni scritto in ogni
  pacchetto, e su ARM legge la propria configurazione con i byte al contrario. Li avevo
  corretti entrambi, poi ho scoperto che ha vulnerabilità note mai sistemate (il repository è
  fermo dal 2012): ora l'app usa il decoder ALAC di FFmpeg, compilato da solo con
  `tools/build_ffmpeg.sh`. L'encoder di Apple resta in `tools/hosttest` per generare i
  pacchetti dei test.

La catena con cui vengono ricavate le chiavi è descritta in cima a `airplay_crypto.h`.

## Compilarla da sé

```
git clone --recursive https://github.com/gninga5/irplay-mirror.git
cd irplay-mirror/app/src/main/cpp/third_party/mbedtls
python3 -m pip install -r scripts/basic.requirements.txt
make generated_files
cd -
./gradlew :app:assembleRelease
```

Il passaggio di mbedTLS va fatto una volta sola; la compilazione con Gradle non ha bisogno di
Python. libsodium è già pronta come AAR in `app/libs`, compilata dai sorgenti ufficiali con
`dist-build/android-aar.sh`.

Per firmare con una chiave tua, metti `IRPLAY_STORE_FILE`, `IRPLAY_STORE_PASSWORD`,
`IRPLAY_KEY_ALIAS` e `IRPLAY_KEY_PASSWORD` in `~/.gradle/gradle.properties`. Senza, la versione
release viene firmata con la chiave di debug.

### Test

`tools/hosttest/run_tests.sh` compila il motore nativo per macOS con AddressSanitizer e UBSan e
lo mette alla prova contro un iPhone simulato: pairing e PIN, FairPlay, video e audio cifrati,
ALAC senza perdita andata e ritorno, il watchdog, richieste malformate e qualche centinaio di
migliaia di plist, SPS e pacchetti ALAC alterati apposta. C'è anche un client che parla con
l'app nell'emulatore della TV, descritto in `tools/hosttest/README.md`.

## Riconoscimenti e licenza

iRPlay Mirror è distribuito con licenza **GPL-3.0** (vedi `LICENSE`).

Si regge sulle spalle di [UxPlay](https://github.com/FDH2/UxPlay) e RPiPlay: senza il loro lavoro
per capire il protocollo, niente di tutto questo sarebbe stato possibile. La parte FairPlay usa
*playfair* (GPL-3) e le tabelle di risposta di Juho Vähä-Herttua (LGPL-2.1+). La crittografia
viene da [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0) e
[libsodium](https://libsodium.org) (ISC), l'audio Apple Lossless dal decoder
ALAC di [FFmpeg](https://ffmpeg.org) (LGPL-2.1+) e le icone dalle Material Icons
di Google (Apache-2.0). Gli avvisi completi sono nell'app, alla voce *Licenze*.

iRPlay Mirror è un progetto indipendente, non affiliato ad Apple Inc. né da essa approvato.
AirPlay, iPhone, iPad e Mac sono marchi di Apple Inc.
