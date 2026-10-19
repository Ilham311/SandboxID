package de.robv.android.xposed.callbacks;

/**
 * Compiler stub. The framework fills this in before calling
 * {@code IXposedHookLoadPackage.handleLoadPackage}; nothing here is ever
 * class-loaded on a device.
 */
public final class XC_LoadPackage {

    private XC_LoadPackage() { }

    /** The argument passed to a module's load-package entry point. */
    public static final class LoadPackageParam {
        /** Package name of the process being loaded. */
        public String packageName;
        /** Class loader of that process; pass it to XposedHelpers lookups. */
        public ClassLoader classLoader;
        /** True when the process is the app's own, not a secondary one. */
        public boolean isFirstApplication;
        /** Process name, e.g. "com.example" or "com.example:push". */
        public String processName;
        /** UID of the process. */
        public int uid;
    }
}
