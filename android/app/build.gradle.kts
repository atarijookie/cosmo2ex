plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.cosmosex.floppy"
    compileSdk = 35
    buildToolsVersion = "35.0.0"

    defaultConfig {
        applicationId = "com.cosmosex.floppy"
        minSdk = 26
        targetSdk = 35
        versionCode = 1 // stays: a phone refuses only a lower one, and this way any build can replace any other
        versionName = System.getenv("VERSION")?.takeIf { it.isNotEmpty() } ?: "dev"
    }

    signingConfigs {
        // the key is public on purpose, see android/build.sh
        create("release") {
            storeFile = file("../ce_app.keystore")
            storePassword = "cosmosex"
            keyAlias = "ce_app"
            keyPassword = "cosmosex"
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("release")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    lint {
        checkReleaseBuilds = false
    }
}
