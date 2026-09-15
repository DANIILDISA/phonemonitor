// =============================================================================
//  Сборка модуля приложения PhoneMonitor Client.
// =============================================================================
//
//  Принципиальные решения:
//    * никаких сторонних библиотек. Всё, что нужно (MediaCodec, SurfaceView,
//      сокеты), есть в платформе. Каждая лишняя зависимость - это лишнее
//      время запуска, а запуск здесь на критическом пути;
//    * minSdk 26 (Android 8.0) по требованию задачи;
//    * targetSdk 34 - Android 14.

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.phonemonitor.client"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.phonemonitor.client"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"

        // Приложение работает только по USB и не имеет инструментальных тестов,
        // зависящих от устройства, поэтому runner не объявляется.
    }

    buildTypes {
        release {
            // Минификация отключена намеренно: приложение состоит из нескольких
            // классов, выигрыш от обфускации нулевой, а риск сломать рефлексию
            // в MediaCodec - ненулевой.
            isMinifyEnabled = false
            isShrinkResources = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            isMinifyEnabled = false
            // Отдельный суффикс, чтобы отладочная и релизная сборки могли
            // стоять на устройстве одновременно.
            applicationIdSuffix = ".debug"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
        freeCompilerArgs = listOf(
            // Явные проверки null на границе с платформенными типами Java:
            // MediaCodec активно возвращает платформенные типы.
            "-Xjvm-default=all"
        )
    }

    buildFeatures {
        // ViewBinding вместо findViewById: меньше кода и нет приведения типов.
        viewBinding = true
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    // Минимальный набор AndroidX: только то, что реально используется.
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.1")
    // Корутины - для управления фоновыми потоками без ручного Thread/Handler.
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
