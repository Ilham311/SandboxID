package de.robv.android.xposed;

import android.content.SharedPreferences;

/**
 * Compiler stub. Only the methods the module calls are present; the real class
 * in the framework is a drop-in for these.
 */
public final class XSharedPreferences implements SharedPreferences {

    private final android.content.SharedPreferences mDelegate;

    public XSharedPreferences(String packageName, String prefFileName) {
        mDelegate = null;
    }

    public boolean canRead() { return getFile().canRead(); }

    public java.io.File getFile() { return new java.io.File("/dev/null"); }

    @Override public java.util.Map<String, ?> getAll() { return null; }
    @Override public String getString(String key, String defValue) { return defValue; }
    @Override public java.util.Set<String> getStringSet(String key,
            java.util.Set<String> defValues) { return defValues; }
    @Override public int getInt(String key, int defValue) { return defValue; }
    @Override public long getLong(String key, long defValue) { return defValue; }
    @Override public float getFloat(String key, float defValue) { return defValue; }
    @Override public boolean getBoolean(String key, boolean defValue) { return defValue; }
    @Override public boolean contains(String key) { return false; }
    @Override public Editor edit() { return null; }
}
