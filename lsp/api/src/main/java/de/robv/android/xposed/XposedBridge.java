package de.robv.android.xposed;

import java.util.Set;

/**
 * Compiler stub. At runtime LSPosed supplies the real implementation.
 */
public final class XposedBridge {

    /** True only inside a process the framework has injected. */
    public static boolean isZygoteHooked() { return false; }

    /** LSPosed routes this to logcat with the module's tag. */
    public static void log(String text) { }

    public static void log(Throwable t) { }

    /**
     * Hooks every overload of {@code methodName} on {@code hookClass}.
     *
     * <p>Signature matches the real API:
     * {@code Set<XC_MethodHook.Unhook> hookAllMethods(Class<?>, String,
     * XC_MethodHook)}. Needed where the exact signature cannot be named at
     * compile time because it mentions an obfuscated type that differs per
     * build — the hook then discriminates on the argument shape instead.
     */
    public static Set<XC_MethodHook.Unhook> hookAllMethods(
            Class<?> hookClass, String methodName, XC_MethodHook callback) {
        throw new RuntimeException("stub");
    }
}
