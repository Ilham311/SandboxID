// Ternak TT — Path B Java hooker class
//
// Compiled to classes.dex by fetch_lsplant.sh (javac + d8), then embedded
// into libternak_tt.so as HELPER_DEX[] via xxd. At runtime, java_hooks.cpp
// loads this dex via InMemoryDexClassLoader, resolves TernakHookHelper,
// and calls lsplant::Hook() to swap ArtMethod entrypoints of target Java
// methods (Settings.Secure.getString, Settings.Global.getInt, etc.) to
// point to the *_h() static methods below.
//
// Each *_h() hooker:
//   1. Asks native code (nativeGetSpoof/nativeGetSpoofLong) whether the
//      current invocation should be spoofed.
//   2. If yes, returns the spoofed value.
//   3. Otherwise, invokes *_bak (the backup Method returned by
//      lsplant::Hook) to run the real original.
//
// The class must have NO external deps beyond core Android/Java classes
// so it can be loaded standalone via InMemoryDexClassLoader.

package com.ternak.tt;

import android.content.ContentResolver;
import java.lang.reflect.Method;

public class TernakHookHelper {
    // ==== Backup handles (set by native side after each lsplant::Hook) ====
    public static Method secureGetString_bak;
    public static Method globalGetString_bak;
    public static Method globalGetInt_bak;
    public static Method uptimeMillis_bak;
    public static Method elapsedRealtime_bak;

    // ==== Hookers ====

    // android.provider.Settings$Secure.getString(ContentResolver, String)
    public static String secureGetString_h(ContentResolver cr, String name) {
        try {
            String spoof = nativeGetSpoof("SEC:" + name);
            if (spoof != null) return spoof;
        } catch (Throwable ignored) {}
        try {
            return (String) secureGetString_bak.invoke(null, cr, name);
        } catch (Throwable t) { return null; }
    }

    // android.provider.Settings$Global.getString(ContentResolver, String)
    public static String globalGetString_h(ContentResolver cr, String name) {
        try {
            String spoof = nativeGetSpoof("GLB:" + name);
            if (spoof != null) return spoof;
        } catch (Throwable ignored) {}
        try {
            return (String) globalGetString_bak.invoke(null, cr, name);
        } catch (Throwable t) { return null; }
    }

    // android.provider.Settings$Global.getInt(ContentResolver, String, int)
    public static int globalGetInt_h(ContentResolver cr, String name, int def) {
        try {
            String spoof = nativeGetSpoof("GLBI:" + name);
            if (spoof != null) {
                try { return Integer.parseInt(spoof); } catch (Exception ignored) {}
            }
        } catch (Throwable ignored) {}
        try {
            Object r = globalGetInt_bak.invoke(null, cr, name, Integer.valueOf(def));
            return (r instanceof Integer) ? (Integer) r : def;
        } catch (Throwable t) { return def; }
    }

    // android.os.SystemClock.uptimeMillis()
    public static long uptimeMillis_h() {
        long real;
        try {
            Object r = uptimeMillis_bak.invoke(null);
            real = (r instanceof Long) ? (Long) r : 0L;
        } catch (Throwable t) { return 0L; }
        long off = nativeGetSpoofLong("UPTIME_OFFSET_MS");
        return real + off;
    }

    // android.os.SystemClock.elapsedRealtime()
    public static long elapsedRealtime_h() {
        long real;
        try {
            Object r = elapsedRealtime_bak.invoke(null);
            real = (r instanceof Long) ? (Long) r : 0L;
        } catch (Throwable t) { return 0L; }
        long off = nativeGetSpoofLong("UPTIME_OFFSET_MS");
        return real + off;
    }

    // ==== Native bridges (registered by java_hooks.cpp) ====
    public static native String nativeGetSpoof(String key);
    public static native long   nativeGetSpoofLong(String key);
}
