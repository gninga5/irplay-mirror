/*
 * jni_bridge.cpp
 * Scopo: unico punto di contatto tra il motore C++ e Kotlin (classe com.irplay.tv.core.NativeBridge).
 *
 * Kotlin -> C++: startServer, stopServer, stopSession, ed25519PublicKey,
 *                updateMediaPlayback, stopMedia (AirPlay video).
 * C++ -> Kotlin: JavaSink implementa SessionSink chiamando i metodi statici di NativeBridge.
 *
 * Trabocchetti JNI gestiti qui:
 *  - I thread nativi (video, audio, RTSP) non sono noti alla JVM: vanno agganciati con
 *    AttachCurrentThread e sganciati prima di terminare, altrimenti ART va in abort.
 *    Lo sgancio è affidato a un oggetto thread_local, distrutto all'uscita del thread.
 *  - FindClass da un thread nativo usa il class loader di sistema e non trova le classi
 *    dell'app: classe e method ID si risolvono una volta in JNI_OnLoad.
 *  - NewStringUTF accetta solo "modified UTF-8": il nome del dispositivo (che può contenere
 *    emoji, caratteri a 4 byte) passa come byte[] UTF-8 e viene decodificato in Kotlin.
 *  - Un'eccezione Kotlin lasciata in sospeso farebbe fallire la chiamata JNI successiva:
 *    dopo ogni callback si controlla e si azzera.
 */
#define LOG_TAG "iRPlay_JNI"
#include <jni.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sodium.h>

#include "airplay_server.h"
#include "log.h"

namespace {

    JavaVM* gVm = nullptr;
    jclass gBridge = nullptr;   // global ref
    jmethodID gOnSessionStarted, gOnSessionEnded, gOnVideoStarted, gOnVideoStopped, gOnVideoConfig,
            gOnVideoFrame, gOnAudioStarted, gOnAudioStopped, gOnAudioFrame, gOnVolume, gOnRemoteControl,
            gOnPinRequested, gOnPinDismissed, gOnClientTrusted,
            gOnAudioFlush, gOnMetadata, gOnArtwork, gOnProgress,
            gOnMediaPlay, gOnMediaRate, gOnMediaScrub, gOnMediaStop;
    jclass gByteArrayClass = nullptr;   // global ref a byte[], per gli array di playlist

    std::mutex gServerMutex;                       // start e stop possono arrivare da thread Kotlin diversi
    std::unique_ptr<irplay::AirPlayServer> gServer;

    struct ThreadDetacher {
        bool attached = false;
        ~ThreadDetacher() { if (attached && gVm) gVm->DetachCurrentThread(); }
    };

    JNIEnv* currentEnv() {
        JNIEnv* env = nullptr;
        if (gVm == nullptr) return nullptr;
        if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
        thread_local ThreadDetacher detacher;
        JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("iRPlay-native"), nullptr};
        if (gVm->AttachCurrentThread(&env, &args) != JNI_OK) return nullptr;
        detacher.attached = true;
        return env;
    }

    void clearException(JNIEnv* env) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();   // niente ExceptionDescribe: lo stack trace finirebbe nel log
            LOGW("Eccezione in una callback Kotlin, ignorata");
        }
    }

    jbyteArray toJava(JNIEnv* env, const uint8_t* data, size_t len) {
        if (len > 0x7FFFFFFF) return nullptr;
        jbyteArray a = env->NewByteArray(static_cast<jsize>(len));
        if (a != nullptr && len > 0) {
            env->SetByteArrayRegion(a, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
        }
        return a;
    }

    class JavaSink final : public irplay::SessionSink {
    public:
        void onSessionStarted(const std::string& name) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            jbyteArray n = toJava(env, reinterpret_cast<const uint8_t*>(name.data()), name.size());
            if (n == nullptr) { clearException(env); return; }
            env->CallStaticVoidMethod(gBridge, gOnSessionStarted, n);
            env->DeleteLocalRef(n);
            clearException(env);
        }
        void onSessionEnded(irplay::EndReason reason) override {
            callVoid(gOnSessionEnded, static_cast<jint>(reason));
        }
        void onVideoStarted() override { callVoid(gOnVideoStarted); }
        void onVideoStopped() override { callVoid(gOnVideoStopped); }

        void onVideoConfig(const uint8_t* sps, size_t spsLen, const uint8_t* pps, size_t ppsLen) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            jbyteArray s = toJava(env, sps, spsLen);
            jbyteArray p = toJava(env, pps, ppsLen);
            if (s && p) env->CallStaticVoidMethod(gBridge, gOnVideoConfig, s, p);
            if (s) env->DeleteLocalRef(s);
            if (p) env->DeleteLocalRef(p);
            clearException(env);
        }

        void onVideoFrame(const uint8_t* data, size_t len, int64_t ptsUs, bool keyframe) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            jbyteArray a = toJava(env, data, len);
            if (a == nullptr) { clearException(env); return; }   // OOM: si perde un frame, non la sessione
            env->CallStaticVoidMethod(gBridge, gOnVideoFrame, a, static_cast<jlong>(ptsUs),
                                      static_cast<jboolean>(keyframe));
            env->DeleteLocalRef(a);   // il thread video non torna mai a Java: senza questo i local ref si accumulano
            clearException(env);
        }

        void onAudioStarted(int codecType) override { callVoid(gOnAudioStarted, static_cast<jint>(codecType)); }
        void onAudioStopped() override { callVoid(gOnAudioStopped); }

        void onAudioFrame(const uint8_t* data, size_t len, int64_t dueMs) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            jbyteArray a = toJava(env, data, len);
            if (a == nullptr) { clearException(env); return; }
            env->CallStaticVoidMethod(gBridge, gOnAudioFrame, a, static_cast<jlong>(dueMs));
            env->DeleteLocalRef(a);
            clearException(env);
        }

        void onVolume(float db) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            env->CallStaticVoidMethod(gBridge, gOnVolume, static_cast<jfloat>(db));
            clearException(env);
        }

        void onRemoteControl(const std::string& dacpId, const std::string& activeRemote,
                             const std::string& peerAddr) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            // Tutti ASCII già controllati (esadecimale, cifre, indirizzo IP): modified UTF-8 coincide.
            jstring a = env->NewStringUTF(dacpId.c_str());
            jstring b = env->NewStringUTF(activeRemote.c_str());
            jstring c = env->NewStringUTF(peerAddr.c_str());
            if (a && b && c) env->CallStaticVoidMethod(gBridge, gOnRemoteControl, a, b, c);
            for (jstring x : {a, b, c}) if (x) env->DeleteLocalRef(x);
            clearException(env);
        }

        void onPinRequested(const std::string& pin) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            jstring s = env->NewStringUTF(pin.c_str());   // solo cifre ASCII: modified UTF-8 coincide
            if (s == nullptr) { clearException(env); return; }
            env->CallStaticVoidMethod(gBridge, gOnPinRequested, s);
            env->DeleteLocalRef(s);
            clearException(env);
        }

        void onPinDismissed() override { callVoid(gOnPinDismissed); }

        void onAudioFlush() override { callVoid(gOnAudioFlush); }

        // Stringhe come byte UTF-8: titoli con emoji non sono "modified UTF-8" (vedi intestazione).
        void onMetadata(const std::string& title, const std::string& artist, const std::string& album) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            auto bytes = [&](const std::string& s) { return toJava(env, reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
            jbyteArray t = bytes(title), ar = bytes(artist), al = bytes(album);
            if (t && ar && al) env->CallStaticVoidMethod(gBridge, gOnMetadata, t, ar, al);
            for (jbyteArray a : {t, ar, al}) if (a) env->DeleteLocalRef(a);
            clearException(env);
        }

        void onArtwork(const uint8_t* data, size_t len) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            jbyteArray a = toJava(env, data, len);
            if (a == nullptr) { clearException(env); return; }
            env->CallStaticVoidMethod(gBridge, gOnArtwork, a);
            env->DeleteLocalRef(a);
            clearException(env);
        }

        void onProgress(int64_t positionMs, int64_t durationMs, int64_t atMs) override {
            callVoid(gOnProgress, static_cast<jlong>(positionMs), static_cast<jlong>(durationMs), static_cast<jlong>(atMs));
        }

        void onClientTrusted(const uint8_t publicKey[32]) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            jbyteArray a = toJava(env, publicKey, 32);
            if (a == nullptr) { clearException(env); return; }
            env->CallStaticVoidMethod(gBridge, gOnClientTrusted, a);
            env->DeleteLocalRef(a);
            clearException(env);
        }

        void onMediaPlay(const std::string& url, double startSec, double startFraction,
                         const std::vector<std::pair<std::string, std::string>>& playlists) override {
            JNIEnv* env = currentEnv();
            if (!env) return;
            // Testi come byte UTF-8 per lo stesso motivo dei metadati: gli URL sono ASCII, ma le
            // playlist possono contenere nomi di tracce in qualsiasi lingua.
            auto bytes = [&](const std::string& s) { return toJava(env, reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
            const auto n = static_cast<jsize>(playlists.size());
            jbyteArray u = bytes(url);
            jobjectArray keys = env->NewObjectArray(n, gByteArrayClass, nullptr);
            jobjectArray bodies = env->NewObjectArray(n, gByteArrayClass, nullptr);
            bool ok = u && keys && bodies;
            for (jsize i = 0; ok && i < n; ++i) {
                jbyteArray k = bytes(playlists[static_cast<size_t>(i)].first);
                jbyteArray b = bytes(playlists[static_cast<size_t>(i)].second);
                ok = k && b;
                if (ok) {
                    env->SetObjectArrayElement(keys, i, k);
                    env->SetObjectArrayElement(bodies, i, b);
                }
                if (k) env->DeleteLocalRef(k);   // fino a 49 coppie: i local ref vanno liberati nel ciclo
                if (b) env->DeleteLocalRef(b);
            }
            if (ok) {
                env->CallStaticVoidMethod(gBridge, gOnMediaPlay, u, static_cast<jdouble>(startSec),
                                          static_cast<jdouble>(startFraction), keys, bodies);
            }
            for (jobject o : {static_cast<jobject>(u), static_cast<jobject>(keys), static_cast<jobject>(bodies)}) {
                if (o) env->DeleteLocalRef(o);
            }
            clearException(env);
        }
        void onMediaRate(float rate) override { callVoid(gOnMediaRate, static_cast<jfloat>(rate)); }
        void onMediaScrub(double positionSec) override { callVoid(gOnMediaScrub, static_cast<jdouble>(positionSec)); }
        void onMediaStop() override { callVoid(gOnMediaStop); }

    private:
        template <typename... Args>
        void callVoid(jmethodID m, Args... args) {
            JNIEnv* env = currentEnv();
            if (!env) return;
            env->CallStaticVoidMethod(gBridge, m, args...);
            clearException(env);
        }
    };

    // Copia una jstring con un limite di lunghezza. Il contenuto (solo ASCII, formati
    // MAC/UUID) viene validato da buildInfoPlists: qui si evitano solo copie enormi.
    bool copyString(JNIEnv* env, jstring s, jsize maxLen, std::string& out) {
        if (s == nullptr) return false;
        const jsize len = env->GetStringLength(s);
        if (len <= 0 || len > maxLen) return false;
        const char* chars = env->GetStringUTFChars(s, nullptr);
        if (chars == nullptr) return false;        // OutOfMemoryError già in sospeso nella JVM
        out.assign(chars);
        env->ReleaseStringUTFChars(s, chars);
        return true;
    }

    bool copyBytes(JNIEnv* env, jbyteArray a, jsize maxLen, std::vector<uint8_t>& out) {
        if (a == nullptr) return false;
        const jsize len = env->GetArrayLength(a);
        if (len <= 0 || len > maxLen) return false;
        out.resize(static_cast<size_t>(len));
        env->GetByteArrayRegion(a, 0, len, reinterpret_cast<jbyte*>(out.data()));
        return true;
    }

    bool resolve(JNIEnv* env, jmethodID& out, const char* name, const char* sig) {
        out = env->GetStaticMethodID(gBridge, name, sig);
        if (out == nullptr) {
            env->ExceptionClear();
            LOGE("Metodo Kotlin mancante: %s%s (regole R8?)", name, sig);
            return false;
        }
        return true;
    }

} // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass local = env->FindClass("com/irplay/tv/core/NativeBridge");
    if (local == nullptr) { env->ExceptionClear(); return JNI_ERR; }
    gBridge = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    jclass bytes = env->FindClass("[B");
    if (bytes == nullptr) { env->ExceptionClear(); return JNI_ERR; }
    gByteArrayClass = static_cast<jclass>(env->NewGlobalRef(bytes));
    env->DeleteLocalRef(bytes);

    const bool ok = resolve(env, gOnSessionStarted, "onSessionStarted", "([B)V")
                    && resolve(env, gOnSessionEnded, "onSessionEnded", "(I)V")
                    && resolve(env, gOnVideoStarted, "onVideoStarted", "()V")
                    && resolve(env, gOnVideoStopped, "onVideoStopped", "()V")
                    && resolve(env, gOnVideoConfig, "onVideoConfig", "([B[B)V")
                    && resolve(env, gOnVideoFrame, "onVideoFrame", "([BJZ)V")
                    && resolve(env, gOnAudioStarted, "onAudioStarted", "(I)V")
                    && resolve(env, gOnAudioStopped, "onAudioStopped", "()V")
                    && resolve(env, gOnAudioFrame, "onAudioFrame", "([BJ)V")
                    && resolve(env, gOnVolume, "onVolume", "(F)V")
                    && resolve(env, gOnRemoteControl, "onRemoteControl", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")
                    && resolve(env, gOnPinRequested, "onPinRequested", "(Ljava/lang/String;)V")
                    && resolve(env, gOnPinDismissed, "onPinDismissed", "()V")
                    && resolve(env, gOnClientTrusted, "onClientTrusted", "([B)V")
                    && resolve(env, gOnAudioFlush, "onAudioFlush", "()V")
                    && resolve(env, gOnMetadata, "onMetadata", "([B[B[B)V")
                    && resolve(env, gOnArtwork, "onArtwork", "([B)V")
                    && resolve(env, gOnProgress, "onProgress", "(JJJ)V")
                    && resolve(env, gOnMediaPlay, "onMediaPlay", "([BDD[[B[[B)V")
                    && resolve(env, gOnMediaRate, "onMediaRate", "(F)V")
                    && resolve(env, gOnMediaScrub, "onMediaScrub", "(D)V")
                    && resolve(env, gOnMediaStop, "onMediaStop", "()V");
    return ok ? JNI_VERSION_1_6 : JNI_ERR;
}

JNIEXPORT jint JNICALL
Java_com_irplay_tv_core_NativeBridge_startServer(
        JNIEnv* env, jclass, jint preferredPort, jbyteArray name, jstring deviceId, jstring pairingId,
        jbyteArray publicKey, jlong features, jstring model, jstring sourceVersion, jbyteArray txtAirPlay,
        jbyteArray seed, jint maxFps, jint pinMode, jbyteArray trustedKeys) {
    if (preferredPort < 0 || preferredPort > 65535) return -1;
    if (seed == nullptr || env->GetArrayLength(seed) != 32) return -1;

    // Parametri piatti invece di un oggetto Kotlin letto con GetFieldID: R8 rinomina i
    // campi nella build release e la lettura fallirebbe solo lì, senza errori in compilazione.
    // Il nome arriva come byte UTF-8 standard: GetStringUTFChars produrrebbe "modified UTF-8"
    // (emoji come coppie di surrogati da 3 byte), che iOS non riconosce.
    std::vector<uint8_t> nameBytes;
    irplay::ReceiverInfo info;
    if (!copyBytes(env, name, 63, nameBytes)
        || !copyString(env, deviceId, 17, info.deviceId)
        || !copyString(env, pairingId, 36, info.pairingId)
        || !copyBytes(env, publicKey, 32, info.publicKey)
        || !copyString(env, model, 32, info.model)
        || !copyString(env, sourceVersion, 16, info.sourceVersion)
        || !copyBytes(env, txtAirPlay, 1300, info.txtAirPlay)) {
        return -1;
    }
    info.name.assign(nameBytes.begin(), nameBytes.end());   // validato in buildInfoPlists
    info.features = static_cast<uint64_t>(features);
    info.maxFps = static_cast<int>(maxFps);   // validato in buildInfoPlists

    // Controllo di accesso: modalità PIN e chiavi dei dispositivi già associati (32 byte l'una,
    // concatenate). Un array di lunghezza non multipla di 32 è un errore del chiamante.
    irplay::AccessConfig access;
    if (pinMode < 0 || pinMode > 2) return -1;
    access.pin = static_cast<irplay::PinMode>(pinMode);
    if (trustedKeys != nullptr) {
        const jsize n = env->GetArrayLength(trustedKeys);
        if (n % 32 != 0 || n > 64 * 32) return -1;
        access.trusted.resize(static_cast<size_t>(n / 32));
        for (jsize i = 0; i < n / 32; ++i) {
            env->GetByteArrayRegion(trustedKeys, i * 32, 32, reinterpret_cast<jbyte*>(access.trusted[i].data()));
        }
    }

    unsigned char s[32];
    env->GetByteArrayRegion(seed, 0, 32, reinterpret_cast<jbyte*>(s));

    std::lock_guard<std::mutex> lock(gServerMutex);
    if (gServer) gServer->stop();
    gServer = std::make_unique<irplay::AirPlayServer>(std::make_shared<JavaSink>());
    const int port = gServer->start(static_cast<uint16_t>(preferredPort), info, s, access);
    sodium_memzero(s, sizeof s);
    if (port < 0) gServer.reset();
    return port;
}

JNIEXPORT void JNICALL
Java_com_irplay_tv_core_NativeBridge_stopServer(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gServerMutex);
    if (gServer) {
        gServer->stop();
        gServer.reset();
    }
}

JNIEXPORT void JNICALL
Java_com_irplay_tv_core_NativeBridge_stopSession(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gServerMutex);
    if (gServer) gServer->stopActiveSession();
}

// Stato di ExoPlayer: posizione e durata in secondi, rate 0/1, state = AirPlayServer::MediaState.
// Valori fuori intervallo vengono scartati: arrivano da Kotlin ma finiscono in risposte a iOS.
JNIEXPORT void JNICALL
Java_com_irplay_tv_core_NativeBridge_updateMediaPlayback(JNIEnv*, jclass, jdouble position, jdouble duration,
                                                         jfloat rate, jint state) {
    if (state < 0 || state > 4) return;
    std::lock_guard<std::mutex> lock(gServerMutex);
    if (gServer) {
        gServer->updateMediaPlayback(position, duration, rate, static_cast<irplay::AirPlayServer::MediaState>(state));
    }
}

JNIEXPORT void JNICALL
Java_com_irplay_tv_core_NativeBridge_stopMedia(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gServerMutex);
    if (gServer) gServer->stopMedia();
}

// Calcola la chiave pubblica Ed25519 dal seed (RFC 8032), senza conservare il segreto.
// Seed e chiave privata espansa vengono azzerati con sodium_memzero, che il
// compilatore non può eliminare in ottimizzazione (al contrario di memset su un
// buffer che non viene più letto).
JNIEXPORT jbyteArray JNICALL
Java_com_irplay_tv_core_NativeBridge_ed25519PublicKey(JNIEnv* env, jclass, jbyteArray seed) {
    if (seed == nullptr || env->GetArrayLength(seed) != crypto_sign_SEEDBYTES) return nullptr;
    if (sodium_init() < 0) return nullptr;

    unsigned char s[crypto_sign_SEEDBYTES];
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    env->GetByteArrayRegion(seed, 0, crypto_sign_SEEDBYTES, reinterpret_cast<jbyte*>(s));

    const int rc = crypto_sign_seed_keypair(pk, sk, s);
    sodium_memzero(s, sizeof s);
    sodium_memzero(sk, sizeof sk);
    if (rc != 0) return nullptr;

    jbyteArray out = env->NewByteArray(crypto_sign_PUBLICKEYBYTES);
    if (out != nullptr) {
        env->SetByteArrayRegion(out, 0, crypto_sign_PUBLICKEYBYTES, reinterpret_cast<const jbyte*>(pk));
    }
    return out;
}

} // extern "C"
