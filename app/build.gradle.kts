plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.irplay.tv"

    // SDK di compilazione standard per Google TV moderna
    compileSdk = 37

    defaultConfig {
        applicationId = "com.irplay.tv"
        // Min SDK 29: garantisce supporto completo da Android 10 in poi (inclusa la TCL 43P639)
        minSdk = 28
        targetSdk = 34
        versionCode = 8
        versionName = "1.3.4"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // Configurazione delle opzioni di compilazione C++ NDK
        externalNativeBuild {
            cmake {
                // Forziamo standard C++20, ottimizzazioni di velocità -O3 e gestione eccezioni
                cppFlags += listOf("-std=c++20", "-O3", "-fexceptions", "-frtti")
            }
        }

        // Filtro architetture: compiliamo ESCLUSIVAMENTE per processori ARM TV reali.
        // Escludiamo x86 e x86_64, dimezzando il peso dell'APK ed evitando sprechi di risorse.
        ndk {
            abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a"))
        }
    }

    // Firma delle release pubblicate. Chiave e password NON sono nel repository: stanno in
    // ~/.gradle/gradle.properties (IRPLAY_STORE_FILE, IRPLAY_STORE_PASSWORD, IRPLAY_KEY_ALIAS,
    // IRPLAY_KEY_PASSWORD). Senza, la release è firmata con la chiave di debug (solo per test).
    val releaseStore = providers.gradleProperty("IRPLAY_STORE_FILE").orNull?.let { file(it) }
    signingConfigs {
        if (releaseStore != null && releaseStore.exists()) {
            create("release") {
                storeFile = releaseStore
                storePassword = providers.gradleProperty("IRPLAY_STORE_PASSWORD").get()
                keyAlias = providers.gradleProperty("IRPLAY_KEY_ALIAS").get()
                keyPassword = providers.gradleProperty("IRPLAY_KEY_PASSWORD").get()
                storeType = "pkcs12"
            }
        }
    }

    buildTypes {
        release {
            // Ottimizzazione e offuscamento con R8 per performance stabili sulla TV
            isMinifyEnabled = true
            isShrinkResources = true
            // Le regole del progetto stanno in src/main/keepRules/rules.keep (lette da AGP in automatico).
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
        debug {
            isMinifyEnabled = false
            isDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Solo inglese (predefinito) e italiano: senza filtro, AppCompat e Material porterebbero
    // decine di traduzioni dei propri testi, e l'app risulterebbe in parte in altre lingue.
    androidResources {
        localeFilters += listOf("en", "it")
    }

    buildFeatures {
        viewBinding = true
        prefab = true          // espone libsodium dall'AAR a CMake
    }
}

dependencies {
    // Dipendenze base UI e KTX
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)

    // Lettore dell'AirPlay video: ExoPlayer con HLS e i controlli di Media3 (Apache-2.0).
    implementation("androidx.media3:media3-exoplayer:1.11.1")
    implementation("androidx.media3:media3-exoplayer-hls:1.11.1")
    implementation("androidx.media3:media3-ui:1.11.1")

    // Coroutine per la gestione asincrona dei socket di rete senza bloccare la UI della TV
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")

    // Test dependencies
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)

    implementation(files("libs/libsodium-1.0.22.0.aar"))
}