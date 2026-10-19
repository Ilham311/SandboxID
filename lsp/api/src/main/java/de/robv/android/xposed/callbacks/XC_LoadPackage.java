package de.robv.android.xposed.callbacks;

import android.content.pm.ApplicationInfo;

/**
 * Compiler stub. LSPosed supplies the real class; only the fields the module
 * reads are declared here.
 */
public final class XC_LoadPackage {

    private XC_LoadPackage() {}

    public static final class LoadPackageParam {
        public String packageName;
        public ClassLoader classLoader;
        public ApplicationInfo appInfo;
        public boolean isFirstApplication;
    }
}
