plugins {
    id("com.android.application") version "8.7.3" apply false
    id("org.jetbrains.kotlin.android") version "2.0.21" apply false
}

layout.buildDirectory.set(file(providers.gradleProperty("outputDir").orNull ?: "../build/android").resolve("root"))
