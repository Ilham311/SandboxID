package de.robv.android.xposed;

/**
 * Compiler stub. Only the members the module calls are present; the real class
 * in the framework is a drop-in for them.
 *
 * <p>Deliberately <b>not</b> declared as {@code implements SharedPreferences}.
 * This module is a plain {@code java-library}, so {@code android.content} is not
 * on its compile classpath — and pulling in an Android library plugin just to
 * inherit that one interface would add AGP to a module that only exists to give
 * {@code :app} a compile-time symbol. The module never uses an
 * {@code XSharedPreferences} through the interface (it calls {@link #getFile()}
 * and {@link #getString(String, String)} directly), so the interface buys
 * nothing here.
 */
public final class XSharedPreferences {

    public XSharedPreferences(String packageName, String prefFileName) { }

    public boolean canRead() { return false; }

    public java.io.File getFile() { return new java.io.File("/dev/null"); }

    public java.util.Map<String, ?> getAll() { return null; }

    public String getString(String key, String defValue) { return defValue; }

    public java.util.Set<String> getStringSet(String key,
            java.util.Set<String> defValues) { return defValues; }

    public int getInt(String key, int defValue) { return defValue; }

    public long getLong(String key, long defValue) { return defValue; }

    public float getFloat(String key, float defValue) { return defValue; }

    public boolean getBoolean(String key, boolean defValue) { return defValue; }

    public boolean contains(String key) { return false; }

    public void reload() { }

    public void makeWorldReadable() { }
}
