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

rootProject.name = "sandboxid-tt-lsp"
include(":app")
