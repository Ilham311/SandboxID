package de.robv.android.xposed;

/**
 * Compiler stub. At runtime LSPosed supplies the real implementation.
 */
public final class XposedBridge {

    /** True only inside a process the framework has injected. */
    public static boolean isZygoteHooked() { return false; }

    /** LSPosed routes this to logcat with the module's tag. */
    public static void log(String text) { }

    public static void log(Throwable t) { }
}
