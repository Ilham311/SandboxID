// Root settings for the LSPosed variant.
//
// Only :app. This tree is self-contained: it reads nothing from the rest of
// the repo (PropMap.java is hand-maintained here, not generated from
// native/include/prop_defs.hpp). See lsp/README.md.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

// The version of the Android Gradle Plugin. Without this block, a bare
// `id("com.android.application")` in app/build.gradle.kts cannot be resolved
// ("Plugin was not found in any of the following sources") and the whole
// configuration fails before any task runs. Keep it in step with the AGP the
// app is tested against.
plugins {
    id("com.android.application") version "8.2.2"
}

rootProject.name = "sandboxid-tt-lsp"
include(":app")
