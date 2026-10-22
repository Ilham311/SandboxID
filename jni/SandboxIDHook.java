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
    /**
     * Call the original, then rewrite the device-identifying parts of the
     * returned String. Used for the WebView User-Agent.
     *
     * The UA cannot be fixed anywhere else: libwebviewchromium.so composes
     * "Mozilla/5.0 (%s) AppleWebKit/537.36 (KHTML, like Gecko) %s Safari/537.36"
     * from ro.product.model / ro.build.id / ro.build.version.release, and on
     * this ROM the library is mapped from two files at once - its read-only
     * segments come from /data/misc/shared_relro/libwebviewchromium64.relro
     * (not an ELF: a relocation blob) and its executable segment from an
     * uncompressed entry inside the WebView base.apk. There is no (dev, inode)
     * pair whose offset-0 mapping is the ELF header, so neither LSPlt nor PLTI
     * can resolve its base and the GOT hook that spoofs every other native
     * read never reaches it. The Java surface is where the app actually picks
     * the value up, so that is where it is rewritten.
     */
    public static final int M_REWRITE_STRING = 5;

    /**
     * Replacement fragments for M_REWRITE_STRING, as parallel {from, to} pairs
     * applied with String.replace(). Set from native before Hook().
     *
     * Substitution rather than a canned literal: the surrounding UA is built by
     * the WebView provider and changes with its version ("Version/4.0
     * Chrome/155.0.8059.43"), so only the two device-identifying slots - the
     * "Linux; Android 15; MODEL Build/ID; wv" segment and the "Dalvik/…"
     * prefix - are ours to rewrite. A canned string would go stale on the next
     * WebView update and would silently stop matching.
     */
    public String[] rwFrom;
    public String[] rwTo;

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

        // Same shape as M_ZERO_NUMERIC: the original runs first and its result
        // is rewritten, so this cannot fall through into the switch below.
        if (mode == M_REWRITE_STRING) return rewriteString(args);

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

    /**
     * Class names that must appear not to exist, for M_OBSERVE. Set from native
     * before Hook().
     *
     * Why not hook {@code Class.forName(String)}: that overload is
     * {@code @CallerSensitive} and resolves through
     * {@code Reflection.getCallerClass()}. Called from this hook it would see
     * EnvCompatState, loaded by the module's own InMemoryDexClassLoader, and
     * resolve the app's classes against the wrong loader - every by-name load
     * would break, not just the denied ones. The three-argument overload has no
     * such call: it hands the loader it was given straight to classForName, so
     * hooking it changes nothing about resolution. It is also where the
     * one-argument overload ends up, after that overload has resolved the
     * caller's own loader - which is exactly the loader a Pixel would use.
     *
     * Why deny at all: miui.os.Build ships in miui-framework.jar, so on a Pixel
     * Class.forName("miui.os.Build") throws and the OEM branch behind it is
     * dead. Here it loads, and that alone is the tell -
     * LX/00kR;->p2() becomes "MIUI-" + INCREMENTAL (apm field "rom") and
     * LX/0D0h;->LIZ() becomes "miui_" + get("ro.miui.ui.version.name") + "_" +
     * INCREMENTAL ("rom_version"). Hiding ro.miui.* cannot fix it: both helpers
     * fall back to Runtime.exec("getprop") when SystemProperties returns empty,
     * and a subprocess inherits none of these hooks. Denying the class keeps
     * the branch unreachable, which is what an AOSP device does.
     *
     * Every call site in both TikTok builds is invoke-static inside a try with
     * an Exception or catch-all handler, so the ClassNotFoundException thrown
     * here lands where a genuine AOSP miss would: p2() falls to
     * catch-all -> 160 and appends the bare INCREMENTAL, LIZ() falls to
     * Exception -> 457 and skips the miui_ branch. The other three targets
     * contain no reference to this class at all, so the deny cannot reach them.
     */
    public static String[] deny;

    /**
     * M_OBSERVE: refuse a denied class name, otherwise let the class load
     * normally and hand the Class to native.
     *
     * The class name is taken from {@link #keyArgIndex}, which each hook sets to
     * its own argument position: 1 for ClassLoader.loadClass(String,boolean),
     * 0 for Class.forName(String,boolean,ClassLoader).
     */
    private Object observeAndPass(Object[] args) {
        String want = null;
        if (args != null && keyArgIndex >= 0 && keyArgIndex < args.length
                && args[keyArgIndex] instanceof String) {
            want = (String) args[keyArgIndex];
        }
        if (want != null) {
            String[] d = deny;
            if (d != null) {
                for (int i = 0; i < d.length; i++) {
                    if (want.equals(d[i])) {
                        throw sneakyThrow(new ClassNotFoundException(want));
                    }
                }
            }
        }
        Object r = invokeOriginal(args);
        try {
            if (r instanceof Class && want != null) {
                String n = want;
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

    /**
     * M_REWRITE_STRING: run the original, then apply every {rwFrom, rwTo} pair
     * to the String it returned.
     *
     * A null/empty result and a non-String result pass through untouched: the
     * UA producer (LX/04v0;->LIZ) returns "" when it has nothing, and a hook
     * that turned that into a value would be inventing data.
     */
    private Object rewriteString(Object[] args) {
        Object r = invokeOriginal(args);
        if (!(r instanceof String)) return r;
        String s = (String) r;
        String[] from = rwFrom, to = rwTo;
        if (from == null || to == null || from.length != to.length || s.isEmpty()) return r;
        for (int i = 0; i < from.length; i++) {
            if (from[i] == null || from[i].isEmpty() || to[i] == null) continue;
            s = s.replace(from[i], to[i]);
        }
        return s;
    }

    private static boolean isAllDigits(String s) {        if (s == null || s.length() < 10) return false;
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
            //
            // The type must survive *exactly*, including checked exceptions:
            // LoadedApk.rewriteRValues() probes for an obfuscated R class and
            // catches ClassNotFoundException; wrapping that in a RuntimeException
            // turns an expected miss into "Unable to instantiate application".
            // A checked exception leaving a JNI-invoked callback is fine — ART
            // propagates the pending exception regardless of the declared type.
            Throwable c = e.getCause();
            throw sneakyThrow(c != null ? c : e);
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    /**
     * Rethrow {@code t} with its original type, bypassing javac's checked-exception
     * check. Never returns; declared to return RuntimeException so the call site can
     * write {@code throw sneakyThrow(t)} and javac sees a definite throw. ART only
     * cares that the right throwable is pending when the JNI call returns.
     */
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException sneakyThrow(Throwable t) throws T {
        throw (T) t;
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
