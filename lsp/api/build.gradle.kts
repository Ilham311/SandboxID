// The compile-time-only Xposed API.
//
// dl.xposed.info is dead — DNS does not resolve, and api.xposed.info serves a
// GitHub Pages 404 page, so `de.robv.android.xposed:api:82` (still listed on
// mvnrepository as if it were fetchable) cannot be resolved from anywhere.
// Rather than depend on a third-party mirror for a build that has to reproduce,
// the handful of classes and methods the module uses are stubbed here. They are
// compileOnly against :app, so nothing from this module reaches the APK: LSPosed
// supplies the real implementations inside the target process and these stubs
// are never class-loaded on a device. The class list mirrors
// org.lsposed.lspd, whose API surface is the same one Xposed exposes to modules.

plugins {
    id("java-library")
}

java {
    sourceCompatibility = JavaVersion.VERSION_1_8
    targetCompatibility = JavaVersion.VERSION_1_8
}
