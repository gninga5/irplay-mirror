/*
 * media_streams.cpp
 *
 * Header dei pacchetti video (128 byte, osservato da UxPlay/RPiPlay):
 *   [0..3]  lunghezza del payload, little-endian
 *   [4]     tipo: 0 = frame cifrato, 1 = SPS/PPS in chiaro (avcC), 2 = heartbeat, 5 = report
 *   [6]     opzione: 0x56/0x5e con tipo 1 = il client sospende il video (schermo bloccato)
 *   [8..15] timestamp NTP del frame, little-endian (secondi << 32 | frazione)
 *
 * Pacchetti audio: RTP (RFC 3550), header di 12 byte, payload type 96.
 * Richieste di timing: 32 byte, [0]=0x80 [1]=0xd2, timestamp di invio in [24..31].
 */
#define LOG_TAG "iRPlay_Stream"
#include "media_streams.h"

#include <cerrno>
#include <ctime>
#include <poll.h>
#include <unistd.h>
#include <vector>

#include <mbedtls/platform_util.h>

#include "log.h"
#include "nal_parser.h"
#include "net_util.h"
#include "sps_rewriter.h"
#include "third_party/alac/ALACBitUtilities.h"
#include "third_party/alac/ALACDecoder.h"

#ifdef __ANDROID__
#include <sys/resource.h>
#endif

namespace irplay {
    namespace {
        // Un IDR a 1080p pesa raramente più di 1 MB: 4 MB lasciano margine senza permettere a un
        // pacchetto falso di far allocare centinaia di MB alla TV.
        constexpr uint32_t kMaxVideoPayload = 4 * 1024 * 1024;
        constexpr int64_t kAcceptTimeoutMs = 30000;
        constexpr int kTimingIntervalMs = 3000;
        constexpr uint64_t kNtpEpochOffset = 2208988800ULL;   // secondi dal 1900 al 1970

        uint32_t le32(const uint8_t* p) {
            return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
        }
        uint64_t le64(const uint8_t* p) { return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32); }
        uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

        int64_t ntpToUs(uint64_t ntp) {
            const uint64_t sec = ntp >> 32, frac = ntp & 0xFFFFFFFFu;
            return static_cast<int64_t>(sec * 1000000u + ((frac * 1000000u) >> 32));
        }
    } // namespace

    int64_t monotonicMs() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    // =============================================================== MirrorStream

    MirrorStream::MirrorStream(SessionSink& sink, const sockaddr_in6& peer, const uint8_t sessionKey[16],
                               uint64_t streamConnectionId, std::atomic<int64_t>& activity)
            : sink_(sink), peer_(peer), activity_(activity) {
        uint8_t key[16], iv[16];
        crypto::deriveMirrorKey(sessionKey, streamConnectionId, key, iv);
        aes_ = std::make_unique<crypto::AesCtr>(key, iv);
        mbedtls_platform_zeroize(key, sizeof key);
        mbedtls_platform_zeroize(iv, sizeof iv);
    }

    MirrorStream::~MirrorStream() { stop(); }

    int MirrorStream::start() {
        uint16_t port = 0;
        listenFd_ = net::tcpListen(port, 1);
        if (listenFd_ < 0) return -1;
        running_ = true;
        thread_ = std::thread(&MirrorStream::run, this);
        return port;
    }

    void MirrorStream::stop() {
        if (!running_.exchange(false) && !thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (connFd_ >= 0) ::shutdown(connFd_, SHUT_RDWR);   // sblocca recv() nel thread
        }
        if (thread_.joinable()) thread_.join();
        if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    }

    bool MirrorStream::readExact(int fd, uint8_t* buf, size_t len) {
        while (len > 0) {
            const ssize_t n = ::recv(fd, buf, len, 0);
            if (n == 0) return false;
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            buf += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

    void MirrorStream::run() {
        int fd = -1;
        const int64_t deadline = monotonicMs() + kAcceptTimeoutMs;
        while (running_ && monotonicMs() < deadline) {
            pollfd p{listenFd_, POLLIN, 0};
            if (::poll(&p, 1, 500) <= 0) continue;
            sockaddr_storage ss{};
            const int c = net::acceptConn(listenFd_, ss);
            if (c < 0) continue;
            if (!net::sameHost(peer_, ss)) {
                LOGW("Connessione video da un host diverso dal client RTSP: rifiutata");
                ::close(c);
                continue;
            }
            fd = c;
            break;
        }
        if (fd < 0) {
            if (running_) LOGW("Il client non ha aperto il canale video entro %lld s", (long long) (kAcceptTimeoutMs / 1000));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) { ::close(fd); return; }
            connFd_ = fd;
        }
        // Il listener non serve più: un secondo client non può agganciarsi allo stesso flusso.
        ::close(listenFd_);
        listenFd_ = -1;

        LOGI("Canale video connesso");
#ifdef __ANDROID__
        // Stessa priorità del thread di composizione di Android (URGENT_DISPLAY): il frame
        // deve passare dal socket al decoder senza attendere thread meno importanti.
        ::setpriority(PRIO_PROCESS, 0, -8);
#endif
        connected_ = true;
        activity_ = monotonicMs();
        sink_.onVideoStarted();

        uint8_t hdr[128];
        std::vector<uint8_t> payload, sps, pps, frame, spsOriginal;
        bool prependConfig = false;
        bool lowLatencySps = true;   // si spegne se iOS usa B-frame (vedi sps_rewriter.h)
        unsigned badFrames = 0;

        while (running_) {
            if (!readExact(fd, hdr, sizeof hdr)) break;
            const uint32_t size = le32(hdr);
            if (size > kMaxVideoPayload) {
                LOGW("Pacchetto video di %u byte oltre il limite: chiudo il canale", size);
                break;
            }
            payload.resize(size);
            if (size > 0 && !readExact(fd, payload.data(), size)) break;
            activity_ = monotonicMs();

            switch (hdr[4]) {
                case 0x00: {
                    if (size == 0) break;
                    // Va decifrato ogni pacchetto, anche quelli poi scartati: il keystream CTR è
                    // continuo e saltarne uno renderebbe illeggibili tutti i successivi.
                    aes_->apply(payload.data(), size);
                    bool keyframe = false, bSlice = false;
                    if (!nal::avccToAnnexB(payload.data(), size, keyframe, lowLatencySps ? &bSlice : nullptr)) {
                        if (badFrames++ % 100 == 0) LOGW("Frame video non valido scartato (%u in totale)", badFrames);
                        break;
                    }
                    if (bSlice) {
                        LOGW("Il client usa B-frame: torno all'SPS originale");
                        lowLatencySps = false;
                        if (!spsOriginal.empty() && sps != spsOriginal) {
                            sps = spsOriginal;
                            sink_.onVideoConfig(sps.data(), sps.size(), pps.data(), pps.size());
                            prependConfig = true;
                        }
                    }
                    const int64_t pts = ntpToUs(le64(hdr + 8));
                    if (prependConfig) {
                        // SPS/PPS anche in banda davanti al primo frame dopo il cambio di formato:
                        // alcuni decoder MediaTek ignorano csd-0/csd-1 dopo un flush.
                        frame.clear();
                        frame.insert(frame.end(), sps.begin(), sps.end());
                        frame.insert(frame.end(), pps.begin(), pps.end());
                        frame.insert(frame.end(), payload.begin(), payload.end());
                        sink_.onVideoFrame(frame.data(), frame.size(), pts, keyframe);
                        prependConfig = false;
                    } else {
                        sink_.onVideoFrame(payload.data(), size, pts, keyframe);
                    }
                    break;
                }
                case 0x01: {
                    if (hdr[6] == 0x56 || hdr[6] == 0x5E) break;   // client in pausa: nessun formato nuovo
                    std::vector<uint8_t> newSps, newPps;
                    if (!nal::parseAvcC(payload.data(), size, newSps, newPps)) {
                        LOGW("Formato video non supportato (solo H.264)");
                        break;
                    }
                    spsOriginal = newSps;
                    std::vector<uint8_t> patched;
                    if (lowLatencySps && nal::patchSpsLowLatency(newSps, patched)) {
                        LOGI("SPS riscritto per bassa latenza (%zu -> %zu byte)", newSps.size(), patched.size());
                        sps.swap(patched);
                    } else {
                        sps.swap(newSps);
                    }
                    pps.swap(newPps);
                    sink_.onVideoConfig(sps.data(), sps.size(), pps.data(), pps.size());
                    prependConfig = true;
                    break;
                }
                default:
                    break;   // heartbeat e report: contano solo come attività per il watchdog
            }
        }

        connected_ = false;
        LOGI("Canale video chiuso");
        sink_.onVideoStopped();
        std::lock_guard<std::mutex> lock(mutex_);
        ::close(connFd_);
        connFd_ = -1;
    }

    // =============================================================== ALAC

    /*
     * Decoder Apple Lossless (sorgente ufficiale Apple, Apache-2.0, in third_party/alac).
     * AirPlay usa sempre la stessa configurazione, che iOS non invia nel SETUP (era nel
     * campo "fmtp" dell'ANNOUNCE di AirPlay 1): 352 campioni per pacchetto, 16 bit, stereo,
     * 44,1 kHz -> "96 352 0 16 40 10 14 2 255 0 0 44100".
     * Il pacchetto arriva dalla rete. Il decoder Apple ha due punti deboli (vedi
     * third_party/alac/PATCHES.md): non controllava il numero di campioni dichiarato dal pacchetto
     * (corretto nel sorgente) e il suo lettore di bit non controlla la fine del buffer. Per il
     * secondo si decodifica da una copia con kPadding byte di zeri in coda, più del massimo che
     * può leggere con al più kFrame campioni, verso un buffer di uscita molto più grande del
     * necessario; si accettano solo esiti con al massimo kFrame campioni.
     */
    class AlacDecoder {
    public:
        static constexpr uint32_t kFrame = 352;
        static constexpr uint32_t kChannels = 2;
        static constexpr size_t kPadding = 16 * 1024;

        bool init() {
            uint8_t cookie[24] = {
                    0x00, 0x00, 0x01, 0x60,   // frameLength 352
                    0x00,                     // compatibleVersion
                    0x10,                     // bitDepth 16
                    0x28, 0x0A, 0x0E,         // pb 40, mb 10, kb 14
                    0x02,                     // numChannels
                    0x00, 0xFF,               // maxRun 255
                    0x00, 0x00, 0x00, 0x00,   // maxFrameBytes
                    0x00, 0x00, 0x00, 0x00,   // avgBitRate
                    0x00, 0x00, 0xAC, 0x44,   // sampleRate 44100
            };
            return dec_.Init(cookie, sizeof cookie) == 0;
        }

        // Restituisce false se il pacchetto non è decodificabile; out = PCM 16 bit little-endian interleaved.
        bool decode(const uint8_t* in, size_t len, std::vector<uint8_t>& out) {
            if (len == 0 || len > 8192) return false;
            in_.assign(in, in + len);
            in_.resize(len + kPadding, 0);
            scratch_.assign(4096 * kChannels * 4, 0);
            BitBuffer bits;
            BitBufferInit(&bits, in_.data(), static_cast<uint32_t>(len));
            uint32_t samples = 0;
            if (dec_.Decode(&bits, scratch_.data(), kFrame, kChannels, &samples) != 0) return false;
            if (samples == 0 || samples > kFrame) return false;
            out.assign(scratch_.begin(), scratch_.begin() + samples * kChannels * 2);
            return true;
        }

    private:
        ALACDecoder dec_;
        std::vector<uint8_t> in_, scratch_;
    };

    // =============================================================== AudioStream

    namespace {
        constexpr int64_t kGapWaitMs = 150;    // attesa di un pacchetto mancante prima di saltarlo
        constexpr uint16_t kMaxResend = 64;
    }

    AudioStream::AudioStream(SessionSink& sink, const sockaddr_in6& peer, const uint8_t sessionKey[16],
                             const uint8_t iv[16], int codecType, uint16_t remoteControlPort,
                             std::atomic<int64_t>& activity)
            : sink_(sink), peer_(peer), remoteControl_(peer), decryptor_(sessionKey, iv), codecType_(codecType),
              music_(codecType != kCodecAacEld), activity_(activity) {
        remoteControl_.sin6_port = htons(remoteControlPort);
    }

    AudioStream::~AudioStream() { stop(); }

    bool AudioStream::start(uint16_t& dataPort, uint16_t& controlPort) {
        if (codecType_ == kCodecAlac) {
            alac_ = std::make_unique<AlacDecoder>();
            if (!alac_->init()) { LOGE("Decoder ALAC non inizializzato"); return false; }
        }
        dataPort = 0;
        controlPort = 0;
        dataFd_ = net::udpBind(dataPort, "audio");
        controlFd_ = net::udpBind(controlPort, "controllo audio");
        if (dataFd_ < 0 || controlFd_ < 0) {
            if (dataFd_ >= 0) ::close(dataFd_);
            if (controlFd_ >= 0) ::close(controlFd_);
            dataFd_ = controlFd_ = -1;
            return false;
        }
        running_ = true;
        // ALAC e PCM vengono decodificati qui: a Kotlin arriva PCM 16 bit (codice kCodecPcm).
        const bool pcmOut = codecType_ == kCodecAlac || codecType_ == kCodecPcm;
        sink_.onAudioStarted(pcmOut ? kCodecPcm : codecType_);
        thread_ = std::thread(&AudioStream::run, this);
        return true;
    }

    void AudioStream::stop() {
        if (!running_.exchange(false) && !thread_.joinable()) return;
        if (thread_.joinable()) thread_.join();   // il thread esce entro un ciclo di poll (250 ms)
        if (dataFd_ >= 0) ::close(dataFd_);
        if (controlFd_ >= 0) ::close(controlFd_);
        dataFd_ = controlFd_ = -1;
        sink_.onAudioStopped();
    }

    void AudioStream::requestFlush(int nextSeq) { flushSeq_ = nextSeq < 0 ? -1 : (nextSeq & 0xFFFF); }

    void AudioStream::deliver(const uint8_t* payload, size_t len) {
        if (codecType_ == kCodecAlac) {
            if (alac_->decode(payload, len, pcm_)) sink_.onAudioFrame(pcm_.data(), pcm_.size());
        } else if (codecType_ == kCodecPcm) {
            // L16 di RTP è big-endian; AudioTrack vuole little-endian.
            pcm_.assign(payload, payload + (len & ~size_t{1}));
            for (size_t i = 0; i + 1 < pcm_.size(); i += 2) std::swap(pcm_[i], pcm_[i + 1]);
            sink_.onAudioFrame(pcm_.data(), pcm_.size());
        } else {
            sink_.onAudioFrame(payload, len);   // AAC: lo decodifica MediaCodec in Kotlin
        }
    }

    void AudioStream::resetBuffer() {
        for (auto& s : slots_) { s.used = false; s.data.clear(); }
        buffered_ = 0;
        gapSinceMs_ = 0;
    }

    void AudioStream::requestResend(uint16_t first, uint16_t count) {
        if (remoteControl_.sin6_port == 0 || count == 0) return;
        // Richiesta di ritrasmissione AirPlay: 0x80 0xD5, contatore, primo mancante, quanti.
        const uint16_t c = resendCounter_++;
        const uint8_t req[8] = {0x80, 0xD5, static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c),
                                static_cast<uint8_t>(first >> 8), static_cast<uint8_t>(first),
                                static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count)};
        ::sendto(controlFd_, req, sizeof req, net::kSendFlags,
                 reinterpret_cast<const sockaddr*>(&remoteControl_), sizeof remoteControl_);
    }

    void AudioStream::insert(uint16_t seq, const uint8_t* payload, size_t len) {
        if (!haveSeq_) { haveSeq_ = true; expected_ = seq; }
        const auto delta = static_cast<int16_t>(seq - expected_);
        if (delta < 0) {
            if (delta > -1000) return;          // duplicato o arrivato dopo essere stato saltato
            resetBuffer();                      // salto all'indietro grande: il flusso è ripartito
            expected_ = seq;
        } else if (delta >= static_cast<int>(kSlots)) {
            resetBuffer();                      // troppo avanti: si riparte da qui
            expected_ = seq;
        }
        Slot& slot = slots_[seq % kSlots];
        if (slot.used) return;                  // stesso pacchetto ricevuto due volte
        slot.used = true;
        slot.seq = seq;
        slot.data.assign(payload, payload + len);
        ++buffered_;
    }

    void AudioStream::drain(int64_t now) {
        for (;;) {
            Slot& slot = slots_[expected_ % kSlots];
            if (slot.used && slot.seq == expected_) {
                deliver(slot.data.data(), slot.data.size());
                slot.used = false;
                slot.data.clear();
                --buffered_;
                ++expected_;
                gapSinceMs_ = 0;
                continue;
            }
            if (buffered_ == 0) return;
            // Manca il pacchetto atteso ma ne sono arrivati di successivi.
            uint16_t next = expected_;
            for (size_t i = 1; i < kSlots; ++i) {
                const auto s = static_cast<uint16_t>(expected_ + i);
                if (slots_[s % kSlots].used && slots_[s % kSlots].seq == s) { next = s; break; }
            }
            if (next == expected_) { resetBuffer(); return; }   // stato incoerente: si svuota
            if (gapSinceMs_ == 0) {
                gapSinceMs_ = now;
                const auto missing = static_cast<uint16_t>(next - expected_);
                requestResend(expected_, missing < kMaxResend ? missing : kMaxResend);
                return;
            }
            if (now - gapSinceMs_ < kGapWaitMs) return;
            expected_ = next;                   // non è arrivato in tempo: si salta (un breve silenzio)
            gapSinceMs_ = 0;
        }
    }

    void AudioStream::onRtp(uint8_t* buf, size_t len) {
        if (len <= 12 || (buf[0] & 0xC0) != 0x80 || (buf[1] & 0x7F) != 0x60) return;
        const uint16_t seq = be16(buf + 2);
        activity_ = monotonicMs();
        uint8_t* payload = buf + 12;
        const size_t n = len - 12;
        decryptor_.decrypt(payload, n);

        if (music_) {
            insert(seq, payload, n);
            drain(monotonicMs());
            return;
        }
        // Duplicazione schermo: AAC-ELD a bassa latenza, nessun riordino. Pacchetti duplicati o
        // in ritardo producono solo un click: si scartano. Un salto all'indietro grande è un riavvio.
        if (haveSeq_) {
            const auto delta = static_cast<int16_t>(seq - lastSeq_);
            if (delta <= 0 && delta > -1000) return;
        }
        haveSeq_ = true;
        lastSeq_ = seq;
        deliver(payload, n);
    }

    void AudioStream::run() {
        uint8_t buf[2048];
        while (running_) {
            const int pending = flushSeq_.exchange(-2);
            if (pending != -2) {
                resetBuffer();
                if (pending >= 0) { expected_ = static_cast<uint16_t>(pending); haveSeq_ = true; }
                else haveSeq_ = false;
                sink_.onAudioFlush();
            }
            pollfd p[2] = {{dataFd_, POLLIN, 0}, {controlFd_, POLLIN, 0}};
            // Con pacchetti in attesa si controlla più spesso, per saltare in tempo quelli persi.
            const int timeout = buffered_ > 0 ? 20 : 250;
            if (::poll(p, 2, timeout) > 0) {
                for (int i = 0; i < 2; ++i) {
                    if (!(p[i].revents & POLLIN)) continue;
                    sockaddr_storage from{};
                    socklen_t fl = sizeof from;
                    const ssize_t n = ::recvfrom(p[i].fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                    if (n <= 0 || !net::sameHost(peer_, from)) continue;
                    if (i == 0) {
                        onRtp(buf, static_cast<size_t>(n));
                    } else if (n > 4 + 12 && (buf[1] & 0x7F) == 0x56 && music_) {
                        // Ritrasmissione: 4 byte di intestazione e poi il pacchetto RTP originale.
                        onRtp(buf + 4, static_cast<size_t>(n) - 4);
                    }
                    // 0x54 (sincronizzazione): non serve, l'audio è riprodotto appena pronto.
                }
            }
            if (music_ && buffered_ > 0) drain(monotonicMs());
        }
    }

    // =============================================================== TimingClient

    TimingClient::TimingClient(const sockaddr_in6& peer, uint16_t remotePort) : peer_(peer) {
        peer_.sin6_port = htons(remotePort);
    }

    TimingClient::~TimingClient() { stop(); }

    int TimingClient::start() {
        uint16_t port = 0;
        fd_ = net::udpBind(port, "timing");
        if (fd_ < 0) return -1;
        running_ = true;
        thread_ = std::thread(&TimingClient::run, this);
        return port;
    }

    void TimingClient::stop() {
        if (!running_.exchange(false) && !thread_.joinable()) return;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    void TimingClient::run() {
        int64_t nextSend = 0;
        uint8_t buf[128];
        while (running_) {
            const int64_t now = monotonicMs();
            if (now >= nextSend && peer_.sin6_port != 0) {
                uint8_t req[32] = {0x80, 0xD2, 0x00, 0x07};
                timespec ts{};
                clock_gettime(CLOCK_REALTIME, &ts);
                const uint64_t sec = static_cast<uint64_t>(ts.tv_sec) + kNtpEpochOffset;
                const uint64_t frac = (static_cast<uint64_t>(ts.tv_nsec) << 32) / 1000000000u;
                for (int i = 0; i < 4; ++i) {
                    req[24 + i] = static_cast<uint8_t>(sec >> (24 - 8 * i));
                    req[28 + i] = static_cast<uint8_t>(frac >> (24 - 8 * i));
                }
                ::sendto(fd_, req, sizeof req, net::kSendFlags, reinterpret_cast<const sockaddr*>(&peer_), sizeof peer_);
                nextSend = now + kTimingIntervalMs;
            }
            pollfd p{fd_, POLLIN, 0};
            // Le risposte vengono solo scaricate: audio e video sono riprodotti appena arrivano,
            // quindi l'offset dell'orologio del mittente non serve (vedi README, sincronizzazione A/V).
            if (::poll(&p, 1, 500) > 0) (void) ::recv(fd_, buf, sizeof buf, 0);
        }
    }

    // =============================================================== Session

    Session::Session(SessionSink& sink, const sockaddr_in6& peer, const uint8_t sessionKey[16], const uint8_t iv[16])
            : sink_(sink), peer_(peer), activity_(monotonicMs()) {
        std::copy(sessionKey, sessionKey + 16, key_);
        std::copy(iv, iv + 16, iv_);
    }

    Session::~Session() {
        stopAudio();
        stopMirror();
        if (timing_) timing_->stop();
        mbedtls_platform_zeroize(key_, sizeof key_);
        mbedtls_platform_zeroize(iv_, sizeof iv_);
    }

    int Session::startTiming(uint16_t remotePort) {
        timing_ = std::make_unique<TimingClient>(peer_, remotePort);
        return timing_->start();
    }

    int Session::startMirror(uint64_t streamConnectionId) {
        stopMirror();
        mirror_ = std::make_unique<MirrorStream>(sink_, peer_, key_, streamConnectionId, activity_);
        const int port = mirror_->start();
        if (port < 0) mirror_.reset();
        return port;
    }

    bool Session::startAudio(int codecType, uint16_t remoteControlPort, uint16_t& dataPort, uint16_t& controlPort) {
        stopAudio();
        audio_ = std::make_unique<AudioStream>(sink_, peer_, key_, iv_, codecType, remoteControlPort, activity_);
        if (!audio_->start(dataPort, controlPort)) {
            audio_.reset();
            return false;
        }
        return true;
    }

    void Session::stopMirror() {
        if (mirror_) {
            mirror_->stop();
            mirror_.reset();
        }
    }

    void Session::flushAudio(int nextSeq) {
        if (audio_) audio_->requestFlush(nextSeq);
    }

    void Session::stopAudio() {
        if (audio_) {
            audio_->stop();
            audio_.reset();
        }
    }

} // namespace irplay
