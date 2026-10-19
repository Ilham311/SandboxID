package androidx.core.os;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * L3 LSPlant callback. Disguised as an androidx support class so it does not
 * stand out in a heap/class dump. ONE instance is created per hooked method;
 * each instance carries its own spoof value and the reflected backup Method,
 * so a single class can dispatch for Settings.Secure, Build, TelephonyManager,
 * MediaDrm, WifiInfo, BluetoothAdapter and the TikTok register path without any
 * shared state.
 *
 * LSPlant contract: the callback is the instance method {@link #handle(Object[])}.
 * For a hooked NON-static method, args[0] is the receiver ("this") and the real
 * parameters follow; for a static method, args holds only the real parameters.
 *
 * This class is compiled to DEX at build time by jni/tools/gen_hook_dex.sh and
 * embedded into the module as jni/hook_dex.h. It is loaded at runtime through
 * InMemoryDexClassLoader, which is how a native Zygisk module gets Java classes
 * into an app process without shipping them in an APK.
 */
public final class EnvCompatState {

    /** Return a spoof value instead of calling the original. */
    public static final int M_VALUE = 0;
    /** Overwrite one Boolean argument with FALSE, then call the original. */
    public static final int M_FORCE_FALSE = 1;
    /** Rewrite a byte[] request body, then call the original. */
    public static final int M_REWRITE_BODY = 2;
    /** Call the original, then report the loaded Class to native. */
    public static final int M_OBSERVE = 3;
    /**
     * Force the result to "0" when the gated argument is a long all-digit
     * string. This is the device-parameter write-back guard: the SDK merges the
     * server-issued did into its store under a numeric key, and once merged it
     * outranks every read-path override for the rest of the process.
     */
    public static final int M_ZERO_NUMERIC = 4;

    // Populated from native (JNI) immediately after construction, before Hook().
    public int     mode;
    public boolean isStatic;    // was the hooked method static?
    public int     keyArgIndex = -1; // absolute index into args[] to gate on, or -1 = always
    public String  keyMatch;    // required String.valueOf(args[keyArgIndex]), or null
    public boolean keyContains; // true = substring test instead of equality
    public int     retType;     // 0 String, 1 byte[], 2 int, 3 long, 4 boolean, 5 CharSequence
    public String  sval;        // spoof value (also the source for int/long/boolean)
    public byte[]  bval;        // spoof bytes (retType == 1)
    public Method  backup;      // original method returned by lsplant::Hook
    private boolean accessibleTried;

    // M_FORCE_FALSE
    public int     forceArgIndex = -1;

    // M_REWRITE_BODY
    public int     bodyArgIndex = -1;
    public int     urlArgIndex  = -1;
    public String  urlMatch;
    public String  openudid;
    public String  clientudid;
    public String  cdid;

    public EnvCompatState() {}

    public Object handle(Object[] args) {
        if (mode == M_OBSERVE) return observeAndPass(args);

        // Handled before the try below: this mode has to call the original
        // itself, and falling through on an exception would call it twice.
        if (mode == M_ZERO_NUMERIC) {
            Object r = invokeOriginal(args);
            if (args != null && keyArgIndex >= 0 && keyArgIndex < args.length
                    && isAllDigits(String.valueOf(args[keyArgIndex]))) {
                return "0";
            }
            return r;
        }

        try {
            switch (mode) {
                case M_FORCE_FALSE:
                    // TikTok's register replay gate takes a boolean; forcing it
                    // false is what makes the client re-register instead of
                    // replaying the cached response for the old did.
                    if (args != null && forceArgIndex >= 0 && forceArgIndex < args.length
                            && args[forceArgIndex] instanceof Boolean) {
                        args[forceArgIndex] = Boolean.FALSE;
                    }
                    break;

                case M_REWRITE_BODY:
                    if (args != null
                            && bodyArgIndex >= 0 && bodyArgIndex < args.length
                            && args[bodyArgIndex] instanceof byte[]
                            && urlArgIndex >= 0 && urlArgIndex < args.length
                            && args[urlArgIndex] instanceof String
                            && urlMatch != null
                            && ((String) args[urlArgIndex]).contains(urlMatch)) {
                        args[bodyArgIndex] = body((byte[]) args[bodyArgIndex],
                                openudid, clientudid, cdid);
                    }
                    break;

                default:
                    if (keyArgIndex < 0 || matches(args)) {
                        Object v = spoofValue();
                        if (v != null) return v;   // null -> fall through to the real method
                    }
            }
        } catch (Throwable ignored) {
            // Any failure in the spoof path must never crash the app: fall through.
            // A hook that throws here would surface inside whichever framework
            // method was hooked, with a stack trace pointing at the framework
            // rather than at this module.
        }
        return invokeOriginal(args);
    }

    /**
     * Class-name suffixes worth reporting to native. Set from native before the
     * loadClass hook is installed; the check stays here so the JNI call (and the
     * native class-name walk) happens only for a class we actually care about.
     * ClassLoader.loadClass runs for every class the app loads, so a per-load
     * JNI round trip would be measurable during startup.
     */
    public static String[] watch;

    /** M_OBSERVE: let the class load normally, then hand the Class to native. */
    private Object observeAndPass(Object[] args) {
        Object r = invokeOriginal(args);
        try {
            if (r instanceof Class && args != null && args.length > 1
                    && args[1] instanceof String) {
                String n = (String) args[1];
                String[] w = watch;
                if (w != null) {
                    for (int i = 0; i < w.length; i++) {
                        String s = w[i];
                        if (s != null && n.endsWith(s)) {
                            observe(n, (Class<?>) r);
                            break;
                        }
                    }
                }
            }
        } catch (Throwable ignored) {
            // The observer installs more hooks; a failure there must not turn a
            // successful class load into an exception for the app.
        }
        return r;
    }

    private static boolean isAllDigits(String s) {
        if (s == null || s.length() < 10) return false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    private boolean matches(Object[] args) {
        if (args == null || keyArgIndex < 0 || args.length <= keyArgIndex || keyMatch == null) {
            return false;
        }
        String s = String.valueOf(args[keyArgIndex]);
        return keyContains ? s.contains(keyMatch) : keyMatch.equals(s);
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
            default: // 0 String / 5 CharSequence
                return sval;
        }
    }

    private Object invokeOriginal(Object[] args) {
        try {
            if (backup == null) return null;
            if (!accessibleTried) {
                accessibleTried = true;
                try { backup.setAccessible(true); } catch (Throwable ignored) { }
            }
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
            // Reflection wraps whatever the original method threw. Unwrap it so the
            // caller sees the exception the framework intended to throw, not a
            // reflection artefact — a wrapped SecurityException would otherwise
            // change control flow in the app.
            Throwable c = e.getCause();
            if (c instanceof RuntimeException) throw (RuntimeException) c;
            if (c instanceof Error) throw (Error) c;
            throw new RuntimeException(c);
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    // ------------------------------------------------------------------
    // Method resolution helper for the TikTok layer.
    //
    // Obfuscated classes are only reachable once the app's own class loader has
    // loaded them, so the native side observes ClassLoader.loadClass() and hands
    // the resulting Class here. Signatures drift between releases, so methods are
    // matched by name plus the *names* of their parameter types (already loaded,
    // so no further class loading is triggered). "*" accepts any type.
    // ------------------------------------------------------------------

    /**
     * @param sig comma-separated parameter type names, "" for none, "*" wildcard
     * @return the matching declared method, or null
     */
    public static Method find(Class<?> c, String name, String sig) {
        if (c == null || name == null) return null;
        String[] want;
        if (sig == null || sig.length() == 0) {
            want = new String[0];
        } else {
            want = sig.split(",", -1);
        }
        try {
            for (Method m : c.getDeclaredMethods()) {
                if (!m.getName().equals(name)) continue;
                Class<?>[] have = m.getParameterTypes();
                if (have.length != want.length) continue;
                boolean ok = true;
                for (int i = 0; i < have.length; i++) {
                    if ("*".equals(want[i])) continue;
                    if (!have[i].getName().equals(want[i])) { ok = false; break; }
                }
                if (ok) return m;
            }
        } catch (Throwable ignored) {
            // LinkageError / NoClassDefFoundError while resolving parameter
            // types: treat as "not found" rather than propagating into a class
            // load that is already in progress.
        }
        return null;
    }

    /** Implemented in native; receives every Class the app loads. */
    public static native void observe(String name, Class<?> cls);

    // ------------------------------------------------------------------
    // Register body rewrite (port of the LSPosed variant's Rewrite.java).
    //
    // This operates on the serialized byte[] the network layer is about to send,
    // so it runs after every layer that assembled the JSON and cannot be undone
    // by a downstream merge. Deliberately dumb: string surgery on the wire
    // format, no JSON parse, no dependency on any obfuscated class.
    // ------------------------------------------------------------------

    private static byte[] body(byte[] in, String openudid, String clientudid, String cdid) {
        if (in == null) return null;
        String s;
        try {
            s = new String(in, "UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            return in;
        }

        boolean changed = false;

        // "device_id":<n> / install_id / bd_did -> 0. These are what tell the
        // register server "I already am this device"; TikTok emits them as bare
        // numbers, so both the string and the numeric form are handled.
        String[] zeroKeys = { "device_id", "install_id", "bd_did" };
        for (int i = 0; i < zeroKeys.length; i++) {
            String patched = replaceStringKey(s, zeroKeys[i], "0");
            if (patched != null) { s = patched; changed = true; continue; }
            patched = replaceNumberKey(s, zeroKeys[i], "0");
            if (patched != null) { s = patched; changed = true; }
        }

        // The identifying ids: must be the persona's own, never a value the
        // register server has bound to another did.
        String patched = replaceStringKey(s, "openudid", openudid);
        if (patched != null) { s = patched; changed = true; }
        patched = replaceStringKey(s, "clientudid", clientudid);
        if (patched != null) { s = patched; changed = true; }
        patched = replaceStringKey(s, "cdid", cdid);
        if (patched != null) { s = patched; changed = true; }

        if (!changed) return in;
        try {
            return s.getBytes("UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            return in;
        }
    }

    private static String replaceNumberKey(String json, String key, String newNumber) {
        int at = 0;
        while (true) {
            int q = json.indexOf('"', at);
            if (q < 0) return null;
            int keyEnd = json.indexOf('"', q + 1);
            if (keyEnd < 0) return null;

            if (json.regionMatches(q + 1, key, 0, key.length())
                    && keyEnd == q + 1 + key.length()) {
                int i = keyEnd + 1;
                while (i < json.length() && isSpace(json.charAt(i))) i++;
                if (i < json.length() && json.charAt(i) == ':') {
                    i++;
                    while (i < json.length() && isSpace(json.charAt(i))) i++;
                    if (i < json.length() && json.charAt(i) == '"') return null;
                    int valStart = i;
                    int valEnd = i;
                    while (valEnd < json.length() && isNumberChar(json.charAt(valEnd))) valEnd++;
                    if (valEnd > valStart) {
                        return json.substring(0, valStart) + newNumber + json.substring(valEnd);
                    }
                }
            }
            at = keyEnd + 1;
        }
    }

    private static String replaceStringKey(String json, String key, String newValue) {
        if (newValue == null) return null;
        int at = 0;
        while (true) {
            int q = json.indexOf('"', at);
            if (q < 0) return null;
            int keyEnd = json.indexOf('"', q + 1);
            if (keyEnd < 0) return null;

            if (json.regionMatches(q + 1, key, 0, key.length())
                    && keyEnd == q + 1 + key.length()) {
                int i = keyEnd + 1;
                while (i < json.length() && isSpace(json.charAt(i))) i++;
                if (i < json.length() && json.charAt(i) == ':') {
                    i++;
                    while (i < json.length() && isSpace(json.charAt(i))) i++;
                    if (i < json.length() && json.charAt(i) == '"') {
                        int valStart = i + 1;
                        // The opening quote is at (valStart-1) and the closing
                        // quote at (valEnd-1). Preserve both by splicing strictly
                        // between them.
                        int valEnd = findValueEnd(json, valStart);
                        if (valEnd > valStart) {
                            return json.substring(0, valStart)
                                    + jsonString(newValue)
                                    + json.substring(valEnd - 1);
                        }
                    }
                }
            }
            at = keyEnd + 1;
        }
    }

    private static String jsonString(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                default:   sb.append(c);      break;
            }
        }
        return sb.toString();
    }

    private static int findValueEnd(String json, int start) {
        int i = start;
        int n = json.length();
        while (i < n) {
            char c = json.charAt(i);
            if (c == '"') return i + 1;
            if (c == '\\') { i += 2; continue; }
            i++;
        }
        return -1;
    }

    private static boolean isSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    private static boolean isNumberChar(char c) {
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.'
                || c == 'e' || c == 'E';
    }
}
