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

// NOTE: the AGP version is declared in app/build.gradle.kts, not here. Putting
// `id("com.android.application")` in a settings plugins{} block makes AGP fail
// at configuration time with "Unexpected plugin type" — the plugin marker
// resolves to a type that cannot be applied from settings.
rootProject.name = "sandboxid-tt-lsp"
include(":app")
