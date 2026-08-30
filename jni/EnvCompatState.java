package androidx.core.os;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * L3 LSPlant callback. Disguised as an androidx support class so it does not
 * stand out in a heap/class dump. ONE instance is created per hooked method;
 * each instance carries its own spoof value and the reflected backup Method,
 * so a single class can dispatch for Settings.Secure, Build, TelephonyManager,
 * MediaDrm, WifiInfo and BluetoothAdapter without any shared state.
 *
 * LSPlant contract: the callback is the instance method {@link #handle(Object[])}.
 * For a hooked NON-static method, args[0] is the receiver ("this") and the real
 * parameters follow; for a static method, args holds only the real parameters.
 */
public final class EnvCompatState {

    // Populated from native (JNI) immediately after construction, before Hook().
    public boolean isStatic;    // was the hooked method static?
    public int     keyArgIndex; // absolute index into args[] to gate on, or -1 = always
    public String  keyMatch;    // required String.valueOf(args[keyArgIndex]), or null
    public int     retType;     // 0 String, 1 byte[], 2 int, 3 long, 4 boolean, 5 CharSequence, 6 empty List
    public String  sval;        // spoof value (also the source for int/long/boolean)
    public byte[]  bval;        // spoof bytes (retType == 1)
    public Method  backup;      // original method returned by lsplant::Hook

    public EnvCompatState() {}

    public Object handle(Object[] args) {
        // retType 9 is a pure observer (GMS class-load watch): it never substitutes
        // a value — it lets the real method run and then reports the loaded class.
        if (retType == 9) return watchClassLoad(args);
        try {
            if (keyArgIndex < 0 || matches(args)) {
                Object v = spoofValue(args);
                if (v != null) return v;   // null -> fall through to the real method
            }
        } catch (Throwable ignored) {
            // Any failure in the spoof path must never crash the app: fall through.
        }
        return invokeOriginal(args);
    }

    private boolean matches(Object[] args) {
        return args != null && keyArgIndex >= 0 && args.length > keyArgIndex
                && keyMatch != null && keyMatch.equals(String.valueOf(args[keyArgIndex]));
    }

    private Object spoofValue(Object[] args) {
        switch (retType) {
            case 1: // byte[]
                return (bval != null && bval.length > 0) ? bval.clone() : null;
            case 2: // int
                return (sval == null) ? null : Integer.valueOf(Integer.parseInt(sval));
            case 3: // long
                return (sval == null) ? null : Long.valueOf(Long.parseLong(sval));
            case 4: // boolean
                return (sval == null) ? null : Boolean.valueOf(Boolean.parseBoolean(sval));
            case 6: // empty java.util.List (e.g. WifiManager.getScanResults/getConfiguredNetworks)
                return new java.util.ArrayList<Object>();
            case 7: // GSF/Gservices android_id -> synthetic MatrixCursor (or null passthrough)
                return buildGservicesCursor(args);
            default: // 0 String / 5 CharSequence
                return sval;
        }
    }

    // GSF/Gservices android_id: return a one-row MatrixCursor {key,value} carrying
    // the spoofed id, but ONLY for the exact ContentResolver.query() the Gservices
    // reader issues — content://com.google.android.gsf.gservices with selectionArgs
    // == ["android_id"]. Anything else (a different key, a full dump, a null
    // selectionArgs) returns null so the real provider answers unchanged. Built via
    // reflection: hook_dex.h is compiled without android.jar, so no android.database.*
    // type may be named directly. query() is an instance method, so args[0] is the
    // receiver, args[1] the Uri and args[4] the selectionArgs (same in the 5- and
    // 6-arg overloads).
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
            return null;   // fall through to the real provider
        }
    }

    // GMS class-load watch (retType 9): run the real findClass, then — for classes
    // under "com.google.android.gms." — report the loaded Class to native so its
    // identifier getters can be hooked. Never alters the result; a failure to report
    // is swallowed so class loading is never disturbed. findClass is an instance
    // method: args[0] = receiver (ClassLoader), args[1] = the class name String.
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
            // Observing must never break the app's class loading.
        }
        return result;
    }

    // Implemented in native (sbx_lsplant.hpp sbx_on_class_loaded), bound via
    // RegisterNatives when the callback class is loaded and the watch is armed.
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
            if (c instanceof RuntimeException) throw (RuntimeException) c;
            if (c instanceof Error) throw (Error) c;
            throw new RuntimeException(c);
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }
}
