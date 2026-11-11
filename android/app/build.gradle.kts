plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }
val outputRoot = providers.gradleProperty("outputDir").orNull ?: "../build/android"
layout.buildDirectory.set(rootProject.file(outputRoot).resolve("app"))
android {
    namespace = "dev.jollymaam.app"
    compileSdk = 35
    ndkVersion = "27.2.12479018"
    defaultConfig {
        applicationId = "dev.jollymaam.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild { cmake { arguments += listOf("-DANDROID_STL=none", "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON") } }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1"; buildStagingDirectory = rootProject.file("../build/android-native") } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { buildConfig = true }
    // One debug key for every build and machine (the default one is made per
    // user and was lost between builds here): a new APK installs over an old.
    signingConfigs.getByName("debug") {
        storeFile = rootProject.file("debug.keystore"); storePassword = "android"
        keyAlias = "androiddebugkey"; keyPassword = "android"
    }
    buildTypes { release { isMinifyEnabled = true; proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro") } }
    // A developer utility with an English UI; settings text also comes from firmware.
    lint { warningsAsErrors = true; disable += setOf("HardcodedText", "SetTextI18n") }
    // The page, web/, as assets/web: MainActivity serves it to the WebView.
    sourceSets.getByName("main").assets.srcDir(layout.buildDirectory.dir("generated/web-assets"))
}
// The firmware package the page offers (make firmware): always in the APK.
val firmwareZip = rootProject.file(providers.gradleProperty("firmwareZip").orNull ?: "../build/firmware.zip")
val webAssets = tasks.register<Sync>("webAssets") {
    doFirst { if (!firmwareZip.isFile) throw GradleException("$firmwareZip is missing: run make firmware (or pass -PfirmwareZip=)") }
    from(rootProject.file("../web")) { include("*.html", "*.js", "*.css") }
    from(firmwareZip) { rename { "firmware.zip" } }
    into(layout.buildDirectory.dir("generated/web-assets/web"))
}
tasks.named("preBuild") { dependsOn(webAssets) }
dependencies {
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    testImplementation("junit:junit:4.13.2")
}
