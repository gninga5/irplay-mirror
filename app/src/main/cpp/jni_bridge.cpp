#include <jni.h>
#include <cstdio>
#include <android/log.h>
#include <sodium.h>
#include <mbedtls/version.h>
#include <memory>
#include <mutex>
#include "airplay_server.h"

namespace {
    std::mutex gServerMutex;                       // start e stop possono arrivare da thread Kotlin diversi
    std::unique_ptr<icast::AirPlayServer> gServer;
}

extern "C" {
#include "playfair.h"

JNIEXPORT jint JNICALL
Java_com_icast_tv_core_NativeBridge_startServer(JNIEnv*, jclass, jint preferredPort) {
    if (preferredPort < 0 || preferredPort > 65535) return -1;
    std::lock_guard<std::mutex> lock(gServerMutex);
    if (gServer) gServer->stop();
    gServer = std::make_unique<icast::AirPlayServer>();
    return gServer->start(static_cast<uint16_t>(preferredPort));
}

JNIEXPORT void JNICALL
Java_com_icast_tv_core_NativeBridge_stopServer(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gServerMutex);
    if (gServer) {
        gServer->stop();
        gServer.reset();
    }
}

}

#define TAG "IcAst_NativeBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_icast_tv_ui_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    LOGI("Ponte JNI inizializzato con successo.");
    return env->NewStringUTF("IcAst Core Nativo [C++20] Attivo e Funzionante");
}

JNIEXPORT jstring JNICALL
Java_com_icast_tv_MainActivity_stringFromJNI(JNIEnv* env, jobject /* this */) {
    // sodium_init() inizializza il generatore casuale e sceglie le implementazioni
    // ottimizzate per la CPU. Se fallisce, nessuna operazione crittografica è sicura.
    if (sodium_init() < 0) {
        LOGI("sodium_init fallita");
        return env->NewStringUTF("ERRORE: libsodium non inizializzata");
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "IcAst nativo OK | libsodium %s | %s",
                  sodium_version_string(), MBEDTLS_VERSION_STRING_FULL);
    return env->NewStringUTF(buf);
}


// Calcola la chiave pubblica Ed25519 dal seed (RFC 8032), senza conservare il segreto.
// Seed e chiave privata espansa vengono azzerati con sodium_memzero, che il
// compilatore non può eliminare in ottimizzazione (al contrario di memset su un
// buffer che non viene più letto).
JNIEXPORT jbyteArray JNICALL
Java_com_icast_tv_core_NativeBridge_ed25519PublicKey(JNIEnv* env, jclass, jbyteArray seed) {
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

}
