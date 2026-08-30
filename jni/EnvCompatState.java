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
        try {
            if (keyArgIndex < 0 || matches(args)) {
                Object v = spoofValue();
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

    private Object spoofValue() {
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
            default: // 0 String / 5 CharSequence
                return sval;
        }
    }

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
