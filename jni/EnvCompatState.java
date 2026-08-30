package androidx.core.os;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class EnvCompatState {

    public boolean isStatic;
    public int     keyArgIndex;
    public String  keyMatch;
    public int     retType;
    public String  sval;
    public byte[]  bval;
    public Method  backup;

    public EnvCompatState() {}

    public Object handle(Object[] args) {

        if (retType == 9) return watchClassLoad(args);
        try {
            if (keyArgIndex < 0 || matches(args)) {
                Object v = spoofValue(args);
                if (v != null) return v;
            }
        } catch (Throwable ignored) {

        }
        return invokeOriginal(args);
    }

    private boolean matches(Object[] args) {
        return args != null && keyArgIndex >= 0 && args.length > keyArgIndex
                && keyMatch != null && keyMatch.equals(String.valueOf(args[keyArgIndex]));
    }

    private Object spoofValue(Object[] args) {
        switch (retType) {
            case 1:
                return (bval != null && bval.length > 0) ? bval.clone() : null;
            case 2:
                return (sval == null) ? null : Integer.valueOf(Integer.parseInt(sval));
            case 3:
                return (sval == null) ? null : Long.valueOf(Long.parseLong(sval));
            case 4:
                return (sval == null) ? null : Boolean.valueOf(Boolean.parseBoolean(sval));
            case 6:
                return new java.util.ArrayList<Object>();
            case 7:
                return buildGservicesCursor(args);
            default:
                return sval;
        }
    }

    private Object buildGservicesCursor(Object[] args) {
        if (sval == null || args == null || args.length <= 4) return null;
        Object uri = args[1];
        if (uri == null) return null;
        if (!String.valueOf(uri).startsWith("content://com.google.android.gsf.gservices"))
            return null;
        if (!(args[4] instanceof String[])) return null;
        String[] selArgs = (String[]) args[4];
        if (selArgs.length != 1 || !"android_id".equals(selArgs[0])) return null;
        try {
            Class<?> mc = Class.forName("android.database.MatrixCursor");
            Object cursor = mc.getConstructor(String[].class)
                              .newInstance((Object) new String[]{"key", "value"});
            mc.getMethod("addRow", Object[].class)
              .invoke(cursor, (Object) new Object[]{"android_id", sval});
            return cursor;
        } catch (Throwable t) {
            return null;
        }
    }

    private Object watchClassLoad(Object[] args) {
        Object result = invokeOriginal(args);
        try {
            if (result != null && args != null && args.length > 1
                    && args[1] instanceof String) {
                String name = (String) args[1];
                if (name.startsWith("com.google.android.gms."))
                    onClassLoaded(name, result);
            }
        } catch (Throwable ignored) {

        }
        return result;
    }

    public static native void onClassLoaded(String name, Object cls);

    private Object invokeOriginal(Object[] args) {
        try {
            if (backup == null) return null;
            if (isStatic) {
                return backup.invoke(null, args);
            }
            Object thiz = (args != null && args.length > 0) ? args[0] : null;
            int n = (args == null) ? 0 : args.length - 1;
            if (n < 0) n = 0;
            Object[] rest = new Object[n];
            if (n > 0) System.arraycopy(args, 1, rest, 0, n);
            return backup.invoke(thiz, rest);
        } catch (InvocationTargetException e) {
            Throwable c = e.getCause();
            throw sneakyThrow(c != null ? c : e);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException sneakyThrow(Throwable t) throws T {
        throw (T) t;
    }
}
