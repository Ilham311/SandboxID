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

    /**
     * Hooks an already-resolved method. Used by {@code HookUtil.hookByArity},
     * which finds overloads by name + arity so a renamed parameter type does
     * not cost the hook.
     */
    public static XC_MethodHook.Unhook hookMethod(
            java.lang.reflect.Member method, XC_MethodHook callback) {
        throw new RuntimeException("stub");
    }
}
