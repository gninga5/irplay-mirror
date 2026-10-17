plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.icast.tv"

    // SDK di compilazione standard per Google TV moderna
    compileSdk = 37

    defaultConfig {
        applicationId = "com.icast.tv"
        // Min SDK 29: garantisce supporto completo da Android 10 in poi (inclusa la TCL 43P639)
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"

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

    buildTypes {
        release {
            // Ottimizzazione e offuscamento con R8 per performance stabili sulla TV
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("debug")
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
    implementation(libs.androidx.constraintlayout)

    // Dipendenza fondamentale per Android TV: gestione telecomando D-Pad e layout Leanback
    implementation("androidx.leanback:leanback:1.2.0")

    // Coroutine per la gestione asincrona dei socket di rete senza bloccare la UI della TV
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")

    // Test dependencies
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)

    implementation(files("libs/libsodium-1.0.22.0.aar"))
}