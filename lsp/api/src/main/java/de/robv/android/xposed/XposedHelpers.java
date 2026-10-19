package de.robv.android.xposed;

import java.lang.reflect.Field;

/**
 * Compiler stub. At runtime LSPosed supplies the real implementation; nothing
 * here is ever loaded on a device.
 */
public final class XposedHelpers {

    public static XC_MethodHook.Unhook findAndHookMethod(
            Class<?> clazz, String methodName, Object... parameterTypesAndCallback) {
        throw new RuntimeException("stub");
    }

    public static XC_MethodHook.Unhook findAndHookMethod(
            String className, ClassLoader classLoader, String methodName,
            Object... parameterTypesAndCallback) {
        throw new RuntimeException("stub");
    }

    private static Field field(Class<?> clazz, String name) throws NoSuchFieldException {
        for (Class<?> c = clazz; c != null; c = c.getSuperclass()) {
            try {
                Field f = c.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException ignored) { }
        }
        throw new NoSuchFieldException(name + " in " + clazz);
    }

    public static void setStaticIntField(Class<?> clazz, String fieldName, int value) {
        try { field(clazz, fieldName).setInt(null, value); }
        catch (Exception e) { throw new RuntimeException("stub", e); }
    }

    public static void setStaticLongField(Class<?> clazz, String fieldName, long value) {
        try { field(clazz, fieldName).setLong(null, value); }
        catch (Exception e) { throw new RuntimeException("stub", e); }
    }

    public static void setStaticObjectField(Class<?> clazz, String fieldName, Object value) {
        try { field(clazz, fieldName).set(null, value); }
        catch (Exception e) { throw new RuntimeException("stub", e); }
    }
}
