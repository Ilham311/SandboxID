package id.sandboxid.tt;

import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;

/**
 * Lookup helpers for obfuscated classes.
 *
 * <p><b>Why this exists.</b> Every interesting class in TikTok 47.1.4 is named
 * {@code X.02zD}, {@code X.088c}, {@code X.08Dg} — one letter of meaning per
 * release. {@code XposedHelpers.findAndHookMethod(String, ClassLoader, String,
 * Object...)} needs the <em>exact</em> parameter classes, which means naming
 * {@code X.085A} and {@code LX/02z2} in source; a rename then costs the whole
 * hook. Resolving the method by <b>name + arity</b> through reflection survives
 * renames of the parameter types, and degrading to "not found" (logged) is
 * exactly the failure mode the module wants.
 *
 * <p>Also holds the two field operations the hooks need on classes that are not
 * otherwise reachable.
 */
final class HookUtil {

    private static final String TAG = "SandboxID-TT";

    private HookUtil() {}

    /**
     * Hooks every method of {@code className} named {@code methodName} that
     * takes exactly {@code arity} parameters.
     *
     * @return how many overloads were hooked (0 = class or method missing)
     */
    static int hookByArity(ClassLoader cl, String className, String methodName,
                           int arity, XC_MethodHook cb) {
        Class<?> c = load(cl, className);
        if (c == null) return 0;
        Method[] methods;
        try {
            // getDeclaredMethods() resolves every parameter and return type of
            // the class. If any of them is missing from this process (TikTok
            // ships optional integrations — e.g. the Apple SDK classes — that
            // are not on every build), it throws NoClassDefFoundError. That
            // must not escape: it would abort handleLoadPackage and take every
            // later hook down with it.
            methods = c.getDeclaredMethods();
        } catch (Throwable t) {
            Log.w(TAG, "cannot enumerate " + className + " methods: " + t);
            return 0;
        }
        int n = 0;
        for (Method m : methods) {
            if (!m.getName().equals(methodName)) continue;
            if (m.getParameterTypes().length != arity) continue;
            try {
                m.setAccessible(true);
                XposedBridge.hookMethod(m, cb);
                n++;
            } catch (Throwable t) {
                Log.w(TAG, "hook failed: " + className + "." + methodName, t);
            }
        }
        if (n == 0) {
            Log.w(TAG, "no method " + className + "." + methodName
                    + "/" + arity + " — TikTok may have renamed it");
        }
        return n;
    }

    /** Hooks a single method by name + arity, including inherited ones. */
    static int hookAny(ClassLoader cl, String className, String methodName,
                       int arity, XC_MethodHook cb) {
        Class<?> c = load(cl, className);
        if (c == null) return 0;
        int n = 0;
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            Method[] methods;
            try {
                methods = k.getDeclaredMethods();
            } catch (Throwable t) {
                Log.w(TAG, "cannot enumerate " + k.getName() + " methods: " + t);
                continue;
            }
            for (Method m : methods) {
                if (!m.getName().equals(methodName)) continue;
                if (m.getParameterTypes().length != arity) continue;
                try {
                    m.setAccessible(true);
                    XposedBridge.hookMethod(m, cb);
                    n++;
                } catch (Throwable t) {
                    Log.w(TAG, "hook failed: " + k.getName() + "." + methodName, t);
                }
            }
        }
        return n;
    }

    /** Loads a class without initialising it; null if absent. */
    static Class<?> load(ClassLoader cl, String name) {
        try {
            return Class.forName(name, false, cl);
        } catch (Throwable t) {
            return null;
        }
    }

    /**
     * Sets a static field to null if it exists. Used to drop TikTok's caches
     * (the android_id memo in {@code X.02yq}) before the app has read them.
     */
    static void clearStatic(ClassLoader cl, String className, String field) {
        Class<?> c = load(cl, className);
        if (c == null) return;
        try {
            Field f = c.getDeclaredField(field);
            f.setAccessible(true);
            f.set(null, null);
            Log.d(TAG, "cleared " + className + "." + field);
        } catch (Throwable t) {
            Log.w(TAG, "cannot clear " + className + "." + field + ": " + t);
        }
    }

    /**
     * Sets an instance field to null, so a provider re-derives instead of
     * returning a value it memoised before the hook was installed.
     */
    static void clearInstanceField(Object target, String field) {
        setInstanceField(target, field, null);
    }

    /**
     * Writes an instance field, searching superclasses.
     *
     * <p>Used to keep a provider's <b>cache field</b> in step with what the
     * method hooks return: the field is read directly by some call sites
     * ({@code iget-object}), so overriding only the accessor would leave those
     * reads on the old value.
     */
    static void setInstanceField(Object target, String field, Object value) {
        if (target == null) return;
        try {
            Field f = null;
            for (Class<?> k = target.getClass(); k != null; k = k.getSuperclass()) {
                try {
                    f = k.getDeclaredField(field);
                    break;
                } catch (NoSuchFieldException ignored) {
                }
            }
            if (f == null) return;
            f.setAccessible(true);
            f.set(target, value);
        } catch (Throwable ignored) {
        }
    }

    /** {@code id.get(key)}, or null. */
    static String get(Map<String, String> id, String key) {
        return id == null ? null : id.get(key);
    }

    /** True when the value is a usable identifier (non-null, non-empty). */
    static boolean has(String s) {
        return s != null && !s.isEmpty();
    }
}
