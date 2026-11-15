/*
 * log.h
 * Scopo: log uniforme per il codice nativo. Su Android va in Logcat; su host (test
 * eseguiti sul Mac durante lo sviluppo) va su stderr, così lo stesso codice compila
 * in entrambi gli ambienti.
 * Regola di progetto: nel log non finiscono mai chiavi, corpi delle richieste o il
 * nome del dispositivo del client.
 * Ogni file .cpp definisce LOG_TAG prima di includere questo header.
 */
#pragma once

#ifdef __ANDROID__
#include <android/log.h>
#define IRPLAY_LOG(prio, ...) __android_log_print(prio, LOG_TAG, __VA_ARGS__)
#define IRPLAY_PRIO_I ANDROID_LOG_INFO
#define IRPLAY_PRIO_W ANDROID_LOG_WARN
#define IRPLAY_PRIO_E ANDROID_LOG_ERROR
#else
#include <cstdio>
#define IRPLAY_LOG(prio, ...) \
    (std::fprintf(stderr, "%c/%s: ", prio, LOG_TAG), std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#define IRPLAY_PRIO_I 'I'
#define IRPLAY_PRIO_W 'W'
#define IRPLAY_PRIO_E 'E'
#endif

#define LOGI(...) IRPLAY_LOG(IRPLAY_PRIO_I, __VA_ARGS__)
#define LOGW(...) IRPLAY_LOG(IRPLAY_PRIO_W, __VA_ARGS__)
#define LOGE(...) IRPLAY_LOG(IRPLAY_PRIO_E, __VA_ARGS__)
