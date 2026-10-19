package de.robv.android.xposed;

import de.robv.android.xposed.callbacks.XC_LoadPackage.LoadPackageParam;

/**
 * Compiler stub. The framework discovers the real entry point through
 * {@code assets/xposed_init}, which names the implementing class.
 */
public interface IXposedHookLoadPackage {
    void handleLoadPackage(LoadPackageParam lpparam) throws Throwable;
}
