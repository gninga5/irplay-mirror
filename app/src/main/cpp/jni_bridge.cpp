#include <jni.h>
#include <cstdio>
#include <android/log.h>
#include <sodium.h>
#include <mbedtls/version.h>
extern "C" {
#include "playfair.h"   // header C senza extern "C": va racchiuso qui
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

}
