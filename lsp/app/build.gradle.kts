// SandboxID TT — LSPosed module variant (standalone APK, no root required)
//
// DESIGN CHANGE (2026-10-06): this variant is now SELF-CONTAINED. The earlier
// revision read the persona from a path the *Zygisk module's* root side had to
// publish — which made it a dependent add-on, not a standalone module. That is
// not what was asked for.
//
// It now owns the whole persona lifecycle itself:
//   1. this app's own UI (MainActivity) generates and rotates the persona,
//   2. written to this app's normal SharedPreferences,
//   3. read from the *target* process via XSharedPreferences, which LSPosed
//      redirects to /data/misc/<uuid>/prefs/id.sandboxid.tt/ (chowned to this
//      app's UID, mode rwx--x--x — see LSPosed ConfigManager.getPrefsPath).
//
// No root, no Zygisk module, no /data/adb, no world-readable /data/local/tmp.
//
// BUILD NOTE: this module requires the Android SDK + Gradle. It is NOT
// buildable on the development device used to author it (no Gradle, aapt2, d8,
// or apksigner there), so it has never been compiled or installed. Build it on
// a machine with the Android toolchain:
//
//     cd lsp && ./gradlew assembleRelease
//
// Then sign the APK and install it as a normal app; LSPosed picks it up via the
// xposedmodule meta-data and assets/xposed_init.

plugins {
    id("com.android.application")
}

android {
    namespace = "id.sandboxid.tt"
    compileSdk = 34

    defaultConfig {
        applicationId = "id.sandboxid.tt"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
        }
    }
}

dependencies {
    // The LSPosed/Xposed API is provided by the framework at runtime; the stub
    // here only satisfies the compiler.
    compileOnly("de.robv.android.xposed:api:82")
}
