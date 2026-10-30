package androidx.core.os;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
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
    /** Spoof DisplayMetrics, Configuration, Point, Rect, Display dimensions. */
    public static final int M_DISPLAY_METRICS = 6;
    /** Intercept Application attach/create and hook app-level targets. */
    public static final int M_APP_ATTACH = 7;
    /** Filter PackageManager queries (hide root, manager, modding apps). */
    public static final int M_FILTER_PACKAGES = 8;
    /**
     * Return the spoof value matching the gated String argument, looked up in
     * the parallel multiKeys/multiVals tables. For a method that serves many
     * keys (a SharedPreferences getString), where one hook per key is
     * impossible: LSPlant refuses a second hook on an already-hooked method.
     */
    public static final int M_VALUE_MULTI = 9;
    /**
     * Return a synthesized {@code AdvertisingIdClient$Info} instead of calling
     * the original. This is the only place the advertising id can be fixed for
     * an app that BUNDLES the Play-services client.
     *
     * <p>Hooking {@code $Info.getId()} is not enough, and the reason is a
     * control-flow fact rather than a naming one. Grab ships its own copy of
     * {@code com.google.android.gms.ads.identifier.AdvertisingIdClient} in its
     * DEX. Its {@code getAdvertisingIdInfo(Context)} constructs a client and
     * calls {@code b(false)}, which binds to the Play-services advertising-id
     * service and throws {@code java.io.IOException} ("Google Play services not
     * available" / "Connection failure") when that bind fails. The exception
     * leaves {@code getAdvertisingIdInfo} before any {@code Info} object exists,
     * so {@code getId()} is never invoked and its hook never fires. Grab's
     * caller then catches it and records {@code GENERAL_EXCEPTION} (er:109) for
     * both the adId and the limit-tracking flag.
     *
     * <p>The {@code Info} class is taken from the hooked method's own return
     * type, not from {@code Class.forName}: the class lives in the app's DEX, so
     * the callback's own loader cannot see it, while the backup Method's return
     * type was resolved by the app's loader and is therefore the right Class.
     */
    public static final int M_GAID_INFO = 10;
    /**
     * Answer a GSF gservices query with a synthesized cursor instead of calling
     * the original.
     *
     * <p>Grab reads the GSF id as
     * {@code query("content://com.google.android.gsf.gservices", null, null,
     * new String[]{"android_id"}, null)}, then {@code Long.parseLong(
     * cursor.getString(1))}, then {@code Long.toHexString()}. The value on the
     * wire is a DECIMAL long, so a device whose gservices table has no row for
     * the key (no Google account ever signed in - the state this test device is
     * in) hands back a null or non-numeric column and the parse throws
     * {@code NumberFormatException}, which Grab reports as gs:"er:103"
     * ({@code GSF_NUMBER_FORMAT_EXCEPTION}).
     *
     * <p>The projection is null and the key travels in selectionArgs - that is
     * the gservices provider's own idiom, and it is why hooking
     * {@code Settings$Secure.getString} (which the module already does) does not
     * cover this path: a different API is being used.
     */
    public static final int M_GSF_CURSOR = 11;
    public static final int M_STATFS = 12;
    public static final int M_SENSOR = 13;
    public static final int M_INPUT_DEVICE = 14;
    public static final int M_CAMERA = 15;
    public static final int M_GL_STRING = 16;

    /**
     * Packages hidden from every PackageManager query.
     *
     * <p>Two groups, and the second is the one that used to leak. The first is
     * root/modding tooling: naming it is a device tell in itself. The second is
     * OEM system packages that only exist on ONE vendor's ROM - a persona is a
     * single device, so a Xiaomi package visible under an OPPO persona is a
     * contradiction the app can read straight out of
     * {@code getInstalledAccessibilityServiceList}.
     *
     * <p>That list is why the vendor group exists. Grab records every enabled
     * accessibility service id in its HVS scan result ({@code files/hvs/
     * scanResult*.txt}, field "as"), and the hook that filters it already
     * worked - {@code bin.mt.plus} disappeared from the scan after it was
     * added here. What remained were {@code com.miui.securitycenter},
     * {@code com.milink.service}, {@code com.microsoft.appmanager} and
     * {@code com.fufufu.katrina.backup}: real packages on the host MIUI ROM,
     * absent from the OPPO persona the device was claiming to be.
     *
     * <p>Vendor names are matched by prefix, so one entry covers the whole
     * family (com.miui.securitycenter and com.miui.securitycenter.xxx alike).
     */
    public static final String[] HIDDEN_PACKAGES = new String[]{
        // ---- root / modding tooling ----
        "com.topjohnwu.magisk",
        "io.github.vvb2060.magisk",
        "io.github.vvb2060.kernelsu",
        "me.weishu.kernelsu",
        "me.bmax.apatch",
        "org.lsposed.manager",
        "org.meowcat.edxposed.manager",
        "com.solohsu.android.edxp.manager",
        "de.robv.android.xposed.installer",
        "com.tsng.hidemyapplist",
        "com.termux",
        "bin.mt.plus",
        "ru.zdevs.zarchiver",
        "ru.zdevs.zarchiver.pro",
        "com.lexa.fakegps",
        "com.rosteam.gpsemulator",
        "com.incorporateapps.fakegps.fre",
        "com.incorporateapps.fakegps",
        "com.applisto.appcloner",
        // ---- vendor ROM packages ----
        // MIUI / HyperOS
        "com.miui",
        "com.milink.service",
        "com.xiaomi",
        "com.mi.health",
        "com.mipay.wallet",
        "com.android.thememanager",
        // ColorOS / OxygenOS
        "com.oplus",
        "com.coloros",
        "com.oneplus",
        "com.realme",
        // cross-vendor desktop/screen-mirroring guests
        "com.microsoft.appmanager",
        // third-party backup tools that ride an accessibility service
        "com.fufufu.katrina.backup",
    };

    /**
     * Vendor prefixes that must stay VISIBLE, because the active persona is
     * that vendor's own device. Filled from native before Hook() with the
     * persona's MANUFACTURER/BRAND, so an OPPO persona keeps com.oplus.* while
     * a Pixel persona hides it. Empty means every vendor entry above is hidden.
     */
    public static String[] vendorKeep = new String[0];

    public static boolean isHiddenPackage(String pkgName) {
        if (pkgName == null) return false;
        String lower = pkgName.toLowerCase(java.util.Locale.US);
        for (int i = 0; i < HIDDEN_PACKAGES.length; ++i) {
            if (lower.equals(HIDDEN_PACKAGES[i]) || lower.startsWith(HIDDEN_PACKAGES[i] + ".")) {
                if (isVendorKept(lower)) continue;
                return true;
            }
        }
        return false;
    }

    private static boolean isVendorKept(String lower) {
        String[] keep = vendorKeep;
        if (keep == null) return false;
        for (int i = 0; i < keep.length; ++i) {
            String k = keep[i];
            if (k == null || k.length() == 0) continue;
            if (lower.equals(k) || lower.startsWith(k + ".")) return true;
        }
        return false;
    }

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

    // M_GAID_INFO: the limit-ad-tracking flag to report. A real device that has
    // not opted out reports false, and Grab records the flag as "f"/"t" - an
    // exception here (which is what er:109 was) is a far louder tell than either
    // value, so the flag is always answerable.
    public boolean gaidLimitAdTracking = false;

    // M_GSF_CURSOR: the decimal GSF id to serve. The URI it answers is carried
    // by the ordinary keyMatch/keyArgIndex gate, so one mechanism covers both
    // String-keyed and URI-keyed hooks.
    public String  gsfId;

    // M_GL_STRING: GPU vendor & renderer strings
    public String  gpuVendor;
    public String  gpuRenderer;

    // M_FORCE_FALSE
    public int     forceArgIndex = -1;

    // M_REWRITE_BODY
    public int     bodyArgIndex = -1;
    public int     urlArgIndex  = -1;
    public String  urlMatch;
    public String  openudid;
    public String  clientudid;
    public String  cdid;

    /**
     * M_VALUE_MULTI: parallel key/value tables for one hook that has to answer
     * several keys of the SAME method.
     *
     * This exists because LSPlant cannot stack two hooks on one method: the
     * second lsplant::Hook() call on an ArtMethod that already carries a hook
     * is refused (its backup would be the previous hooker, which recurses).
     * A SharedPreferences-style getString(String key, String def) is exactly
     * one method serving every key, so three separate specs for openudid,
     * clientudid and cdid could only ever install the first — the other two
     * silently did nothing and those ids leaked. Verified on device: three
     * specs for KevaSpFastAdapter.getString produced exactly one DoHook.
     */
    public String[] multiKeys;
    public String[] multiVals;

    // M_DISPLAY_METRICS
    public int   displayWidth;
    public int   displayHeight;
    public int   displayDensity;
    public float displayXdpi;
    public float displayYdpi;
    public int   displayMetricType; // 0 = metrics/config/point/rect, 1 = width, 2 = height

    private static volatile boolean appCreatedDone = false;

    // Cached reflection handles for UI display metrics hot path
    private static volatile boolean s_fieldsInit = false;
    private static Constructor<?> s_dmCtor;
    private static Method s_dmSetTo;
    private static Field s_dmWidthPixels;
    private static Field s_dmHeightPixels;
    private static Field s_dmDensityDpi;
    private static Field s_dmDensity;
    private static Field s_dmScaledDensity;
    private static Field s_dmXdpi;
    private static Field s_dmYdpi;

    private static Constructor<?> s_cfgCtor;
    private static Field s_cfgOrientation;
    private static Field s_cfgScreenWidthDp;
    private static Field s_cfgScreenHeightDp;
    private static Field s_cfgDensityDpi;
    private static Field s_cfgSmallestScreenWidthDp;

    private static Constructor<?> s_rectCtor;
    private static Field s_rectLeft, s_rectTop, s_rectRight, s_rectBottom;

    private static Field s_ptX, s_ptY;
    private static Method s_displayGetRotation;

    // Cache each spoofed copy alongside a snapshot of the live input. This avoids
    // allocations on unchanged hot UI paths while allowing source changes to
    // refresh the copy; caller mutations to the spoofed copy survive otherwise.
    private static final class SpoofCacheEntry {
        final Object sourceSnapshot;
        final Object spoofed;
        SpoofCacheEntry(Object sourceSnapshot, Object spoofed) {
            this.sourceSnapshot = sourceSnapshot;
            this.spoofed = spoofed;
        }
    }
    private static final java.util.Map<Object, SpoofCacheEntry> s_dmCache =
        java.util.Collections.synchronizedMap(new java.util.WeakHashMap<Object, SpoofCacheEntry>());
    private static final java.util.Map<Object, SpoofCacheEntry> s_cfgCache =
        java.util.Collections.synchronizedMap(new java.util.WeakHashMap<Object, SpoofCacheEntry>());

    private static void initReflection() {
        if (s_fieldsInit) return;
        synchronized (EnvCompatState.class) {
            if (s_fieldsInit) return;
            try {
                Class<?> dm = Class.forName("android.util.DisplayMetrics");
                s_dmCtor = dm.getConstructor();
                s_dmSetTo = dm.getMethod("setTo", dm);
                s_dmWidthPixels = dm.getField("widthPixels");
                s_dmHeightPixels = dm.getField("heightPixels");
                s_dmDensityDpi = dm.getField("densityDpi");
                s_dmDensity = dm.getField("density");
                s_dmScaledDensity = dm.getField("scaledDensity");
                s_dmXdpi = dm.getField("xdpi");
                s_dmYdpi = dm.getField("ydpi");
            } catch (Throwable ignored) {}
            try {
                Class<?> cfg = Class.forName("android.content.res.Configuration");
                s_cfgCtor = cfg.getConstructor(cfg);
                s_cfgOrientation = cfg.getField("orientation");
                s_cfgScreenWidthDp = cfg.getField("screenWidthDp");
                s_cfgScreenHeightDp = cfg.getField("screenHeightDp");
                s_cfgDensityDpi = cfg.getField("densityDpi");
                s_cfgSmallestScreenWidthDp = cfg.getField("smallestScreenWidthDp");
            } catch (Throwable ignored) {}
            try {
                Class<?> rect = Class.forName("android.graphics.Rect");
                s_rectCtor = rect.getConstructor(rect);
                s_rectLeft = rect.getField("left");
                s_rectTop = rect.getField("top");
                s_rectRight = rect.getField("right");
                s_rectBottom = rect.getField("bottom");
            } catch (Throwable ignored) {}
            try {
                Class<?> pt = Class.forName("android.graphics.Point");
                s_ptX = pt.getField("x");
                s_ptY = pt.getField("y");
            } catch (Throwable ignored) {}
            try {
                Class<?> disp = Class.forName("android.view.Display");
                s_displayGetRotation = disp.getMethod("getRotation");
            } catch (Throwable ignored) {}
            s_fieldsInit = true;
        }
    }

    public EnvCompatState() {}

    /**
     * Diagnostic switch, set from native only on a debug build.
     *
     * <p>Why this exists: a hook that silently does not fire is
     * indistinguishable from a hook that fires and returns the wrong value,
     * and the only evidence left is what the app happened to write to disk.
     * That is exactly how the missing {@code getSubscriberId(int)} overload
     * stayed invisible - the no-arg hook was installed and logged, the (int)
     * call went to the real TelephonyManager, and the only trace was an
     * adrIMSI built from the real carrier prefix.
     *
     * <p>Off by default and never enabled in a release build: a Log line that
     * names the spoofed field is itself a device tell, and {@code logcat} is
     * readable by anything with READ_LOGS.
     */
    public static boolean debugLog = false;

    // Resolved once, lazily, and by name. The callback is compiled without
    // android.jar (it is turned into DEX by gen_hook_dex.sh from this source
    // alone), so a direct `android.util.Log.d` reference does not compile - not
    // in the build, and not in tests/host/EnvCompatStateCheck, which compiles
    // this same file on a plain JDK. Reflection is how every other Android
    // class in here is reached for the same reason.
    private static volatile Method s_logD;
    private static volatile boolean s_logTried;

    private static void dbg(String what, Object val) {
        if (!debugLog) return;
        try {
            if (!s_logTried) {
                s_logTried = true;
                try {
                    Class<?> logCls = Class.forName("android.util.Log");
                    s_logD = logCls.getMethod("d", String.class, String.class);
                } catch (Throwable ignored) {}
            }
            Method m = s_logD;
            if (m != null) m.invoke(null, "SandboxID-L3", what + " -> " + String.valueOf(val));
        } catch (Throwable ignored) {}
    }

    public Object handle(Object[] args) {
        if (debugLog) dbg("hook mode=" + mode, sval);
        if (mode == M_OBSERVE) return observeAndPass(args);
        if (mode == M_FILTER_PACKAGES) return filterPackages(args);

        // One method, many keys. Runs before the switch below so the value
        // lookup happens against the tables rather than the single sval.
        if (mode == M_VALUE_MULTI) {
            String v = multiValue(args);
            if (v != null) return v;
            return invokeOriginal(args);
        }

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

        // Both of these must short-circuit BEFORE invokeOriginal: the whole
        // point is that the original throws or returns nothing usable.
        if (mode == M_GAID_INFO)   return gaidInfo(args);
        if (mode == M_GSF_CURSOR)  return gsfCursor(args);

        if (mode == M_STATFS)       return handleStatFs(args);
        if (mode == M_SENSOR)       return handleSensor(args);
        if (mode == M_INPUT_DEVICE) return handleInputDevice(args);
        if (mode == M_CAMERA)       return handleCamera(args);
        if (mode == M_GL_STRING)    return handleGlString(args);

        if (mode == M_DISPLAY_METRICS) {
            Object r = invokeOriginal(args);
            if (displayMetricType == 1 || displayMetricType == 2) {
                boolean isLand = false;
                if (args != null && args.length > 0 && args[0] != null) {
                    try {
                        initReflection();
                        if (s_displayGetRotation != null) {
                            Object rotObj = s_displayGetRotation.invoke(args[0]);
                            if (rotObj instanceof Integer) {
                                int rot = ((Integer) rotObj).intValue();
                                isLand = (rot == 1 || rot == 3);
                            }
                        }
                    } catch (Throwable ignored) {}
                }
                int minDim = Math.min(displayWidth > 0 ? displayWidth : 1080,
                                      displayHeight > 0 ? displayHeight : 2400);
                int maxDim = Math.max(displayWidth > 0 ? displayWidth : 1080,
                                      displayHeight > 0 ? displayHeight : 2400);
                int w = isLand ? maxDim : minDim;
                int h = isLand ? minDim : maxDim;
                return Integer.valueOf(displayMetricType == 1 ? w : h);
            }
            if (r != null) {
                String cname = r.getClass().getName();
                if (cname.endsWith("DisplayMetrics")) {
                    SpoofCacheEntry cached = s_dmCache.get(r);
                    try {
                        initReflection();
                        if (s_dmCtor != null && s_dmSetTo != null) {
                            if (cached != null && cached.sourceSnapshot.equals(r)) {
                                return cached.spoofed;
                            }
                            Object snapshot = s_dmCtor.newInstance();
                            s_dmSetTo.invoke(snapshot, r);
                            Object copy = s_dmCtor.newInstance();
                            s_dmSetTo.invoke(copy, r);
                            applyDisplayMetrics(copy);
                            s_dmCache.put(r, new SpoofCacheEntry(snapshot, copy));
                            r = copy;
                        } else {
                            applyDisplayMetrics(r);
                        }
                    } catch (Throwable ignored) {
                        applyDisplayMetrics(r);
                    }
                } else if (cname.endsWith("Configuration")) {
                    SpoofCacheEntry cached = s_cfgCache.get(r);
                    try {
                        initReflection();
                        if (s_cfgCtor != null) {
                            if (cached != null && cached.sourceSnapshot.equals(r)) {
                                return cached.spoofed;
                            }
                            Object snapshot = s_cfgCtor.newInstance(r);
                            Object copy = s_cfgCtor.newInstance(r);
                            applyConfiguration(copy);
                            s_cfgCache.put(r, new SpoofCacheEntry(snapshot, copy));
                            r = copy;
                        } else {
                            applyConfiguration(r);
                        }
                    } catch (Throwable ignored) {
                        applyConfiguration(r);
                    }
                } else if (cname.endsWith("Rect")) {
                    try {
                        initReflection();
                        if (s_rectCtor != null) {
                            Object copy = s_rectCtor.newInstance(r);
                            applyRect(copy);
                            r = copy;
                        } else {
                            applyRect(r);
                        }
                    } catch (Throwable ignored) {
                        applyRect(r);
                    }
                }
            }
            if (args != null && args.length > 1 && args[1] != null) {
                String a1name = args[1].getClass().getName();
                if (a1name.endsWith("DisplayMetrics")) {
                    applyDisplayMetrics(args[1]);
                } else if (a1name.endsWith("Point")) {
                    applyPoint(args[1]);
                }
            }
            return r;
        }

        if (mode == M_APP_ATTACH) {
            Object r = invokeOriginal(args);
            if (!appCreatedDone) {
                try {
                    ClassLoader cl = null;
                    // Any Context can hand back the app ClassLoader, not just an
                    // Application. Requiring an Application here made
                    // attachBaseContext(Context) useless: its only argument is
                    // a Context, so isApplication() was always false, cl stayed
                    // null, and the earliest and most reliable app-attach point
                    // never reached onAppCreated. Verified on device: the
                    // "app target(s) hooked via onAppCreated" line never
                    // appeared in any session, so every spec resolved only
                    // through the class-load observer - which misses a class
                    // that was already loaded before the hooks went in.
                    //
                    // Probed by reflection rather than with an `instanceof
                    // android.content.Context`: this file is compiled by
                    // jni/tools/gen_hook_dex.sh, which uses android.jar only
                    // when ANDROID_HOME/ANDROID_SDK_ROOT provides one, and it
                    // deliberately names every framework type as a String so it
                    // builds without. A real type reference here would break
                    // the build on a machine with no SDK - and getClassLoader
                    // is exactly the method we want to call anyway.
                    for (int i = 0; cl == null && args != null && i < args.length; i++) {
                        if (args[i] == null) continue;
                        try {
                            Method m = args[i].getClass().getMethod("getClassLoader");
                            Object res = m.invoke(args[i]);
                            if (res instanceof ClassLoader) cl = (ClassLoader) res;
                        } catch (Throwable ignored) {}
                    }
                    if (cl != null) {
                        if (onAppCreated(cl)) {
                            appCreatedDone = true;
                        }
                    }
                } catch (Throwable ignored) {}
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

    /**
     * M_GAID_INFO: build an {@code AdvertisingIdClient$Info} carrying the
     * persona's advertising id, without ever touching Play services.
     *
     * <p>The class comes from {@code backup.getReturnType()}: the backup Method
     * was produced by LSPlant from the hooked ArtMethod, whose return type was
     * resolved by the app's own ClassLoader. {@code Class.forName} from this
     * callback would resolve against the module's InMemoryDexClassLoader and
     * miss the class entirely - it lives in the app's DEX.
     *
     * <p>Constructor {@code (String, boolean)} is the only one the class has.
     * If it cannot be built, the original is called after all: returning null
     * would make Grab dereference a null Info and crash, and an exception would
     * be a louder tell than the er:109 this replaces.
     */
    private Object gaidInfo(Object[] args) {
        Class<?> infoCls = null;
        try {
            if (backup != null) infoCls = backup.getReturnType();
        } catch (Throwable ignored) {}
        if (infoCls != null && infoCls != Object.class && sval != null) {
            try {
                Constructor<?> c = infoCls.getConstructor(String.class, boolean.class);
                return c.newInstance(sval, Boolean.valueOf(gaidLimitAdTracking));
            } catch (Throwable ignored) {}
        }
        return invokeOriginal(args);
    }

    /**
     * M_GSF_CURSOR: answer the gservices "android_id" lookup with a one-row,
     * two-column cursor shaped exactly like the provider's own.
     *
     * <p>Shape matters as much as the value. Grab checks
     * {@code moveToFirst()}, then {@code getColumnCount() >= 2}, then reads
     * column 1; the provider returns {key, value} in that order. A single-column
     * cursor would make Grab take its own GSF_QUERY_FAILED branch, and a cursor
     * whose column 1 is not a decimal long makes it throw
     * NumberFormatException - the er:103 this exists to remove.
     *
     * <p>Gated on the URI: {@code ContentResolver.query} is called for every
     * provider in the process, and answering any of them with a gservices row
     * would break unrelated reads. Anything that is not the gservices URI falls
     * through to the real method untouched.
     *
     * <p>Which overload is hooked, and why it is the right one: Android 11+
     * deprecates the 5-arg form in favour of the Bundle one, but the
     * delegation runs the other way - the 5-arg body calls the 6-arg
     * (CancellationSignal) form, which in turn calls the Bundle form. So a
     * caller using the 5-arg form is caught by a hook on the 5-arg form, and
     * that is what Grab uses (verified in its DEX at {@code c.g.o7y1.a()},
     * which invokes exactly this signature). A caller using the Bundle form
     * directly is NOT covered by this hook; Grab is not such a caller.
     */
    private Object gsfCursor(Object[] args) {
        // The gate is keyArgIndex=1 (the Uri) with keyMatch set to the gservices
        // URI. matches() compares String.valueOf(args[1]), and a Uri's toString()
        // is exactly its string form, so the same helper that gates String-keyed
        // hooks gates this one.
        if (!matches(args)) return invokeOriginal(args);
        if (gsfId == null || gsfId.isEmpty()) return invokeOriginal(args);

        // The gservices provider's idiom is that selectionArgs ARE the keys
        // being looked up, and this hook only knows one of them. Answering a
        // query for any other key with the android_id row would corrupt an
        // unrelated read - and Grab does read other keys through the same
        // provider. So: answer only when "android_id" is among the requested
        // keys, and let everything else reach the real provider.
        if (!wantsAndroidId(args)) return invokeOriginal(args);

        try {
            Class<?> mc = Class.forName("android.database.MatrixCursor");
            Object cursor = mc.getConstructor(String[].class, int.class)
                    .newInstance(new String[]{"_id", "value"}, Integer.valueOf(1));
            mc.getMethod("addRow", Object[].class)
                    .invoke(cursor, (Object) new Object[]{"android_id", gsfId});
            return cursor;
        } catch (Throwable ignored) {
            // No MatrixCursor (or it refused) - the original still answers, just
            // with whatever the device really has. Better a real miss than a
            // crash inside a framework method.
            return invokeOriginal(args);
        }
    }

    /**
     * True when the query's selectionArgs name "android_id".
     *
     * <p>Index 4, not 3: for a non-static hook args[0] is the receiver, so the
     * signature (Uri, projection, selection, selectionArgs, sortOrder) lands at
     * args[1..5]. Verified against Grab's own call site in {@code c.g.o7y1.a()},
     * where the invoke-virtual/range register list is
     * {receiver, Uri, projection=null, selection=null, selectionArgs={"android_id"},
     * sortOrder=null}.
     *
     * <p>A null or empty selectionArgs means the caller wants every row, which
     * this hook cannot honestly serve - fall through.
     */
    private static boolean wantsAndroidId(Object[] args) {
        if (args == null || args.length <= 4) return false;
        Object sel = args[4];
        if (!(sel instanceof Object[])) return false;
        for (Object o : (Object[]) sel) {
            if ("android_id".equals(o)) return true;
        }
        return false;
    }

    private Object handleStatFs(Object[] args) {
        String name = backup != null ? backup.getName() : "";
        if ("getBlockSizeLong".equals(name)) return Long.valueOf(4096L);
        if ("getBlockSize".equals(name)) return Integer.valueOf(4096);
        if ("getBlockCountLong".equals(name)) return Long.valueOf(33554432L);
        if ("getBlockCount".equals(name)) return Integer.valueOf(33554432);
        if ("getFreeBlocksLong".equals(name)) return Long.valueOf(22440380L);
        if ("getFreeBlocks".equals(name)) return Integer.valueOf(22440380);
        if ("getAvailableBlocksLong".equals(name)) return Long.valueOf(21810380L);
        if ("getAvailableBlocks".equals(name)) return Integer.valueOf(21810380);
        if ("getTotalBytes".equals(name)) return Long.valueOf(137438953472L);
        if ("getFreeBytes".equals(name)) return Long.valueOf(91915796480L);
        if ("getAvailableBytes".equals(name)) return Long.valueOf(89335316480L);
        return invokeOriginal(args);
    }

    private Object handleSensor(Object[] args) {
        String name = backup != null ? backup.getName() : "";
        if ("getVendor".equals(name)) {
            if (sval != null && !sval.isEmpty()) return sval;
            return invokeOriginal(args);
        }
        if ("getName".equals(name)) {
            Object r = invokeOriginal(args);
            if (r instanceof String) {
                String s = (String) r;
                String man = (sval != null && !sval.isEmpty()) ? sval : "Generic";
                s = s.replace("XiaoMi", man)
                     .replace("Xiaomi", man)
                     .replace("xiaomi", man.toLowerCase(java.util.Locale.ROOT))
                     .replace("XIAOMI", man.toUpperCase(java.util.Locale.ROOT))
                     .replace("alioth", "device")
                     .replace("traffic_detection", "sensor");
                return s;
            }
            return r;
        }
        return invokeOriginal(args);
    }

    private Object handleInputDevice(Object[] args) {
        String name = backup != null ? backup.getName() : "";
        if ("getName".equals(name) || "getInputName".equals(name) || "getInputKona".equals(name)) {
            Object r = invokeOriginal(args);
            if (r instanceof String) {
                String s = (String) r;
                if (s.contains("traffic_detection") || s.contains("XiaoMi") || s.contains("xiaomi")) {
                    return "touchscreen";
                }
                if (s.contains("kona")) {
                    return "headset-jack";
                }
                return s;
            }
            return "touchscreen";
        }
        if ("getVendorId".equals(name)) {
            Object r = invokeOriginal(args);
            if (r instanceof Integer) {
                int vid = ((Integer) r).intValue();
                if (vid == 0x2717) return Integer.valueOf(0);
                return r;
            }
            return Integer.valueOf(0);
        }
        if ("getProductId".equals(name)) {
            Object r = invokeOriginal(args);
            if (r instanceof Integer) {
                return r;
            }
            return Integer.valueOf(0);
        }
        return invokeOriginal(args);
    }

    private Object handleCamera(Object[] args) {
        String name = backup != null ? backup.getName() : "";
        if ("getCameraIdList".equals(name)) {
            return new String[]{"0", "1"};
        }
        return invokeOriginal(args);
    }

    private Object handleGlString(Object[] args) {
        int pname = -1;
        if (isStatic && args != null && args.length > 0 && args[0] instanceof Integer) {
            pname = ((Integer) args[0]).intValue();
        } else if (!isStatic && args != null && args.length > 1 && args[1] instanceof Integer) {
            pname = ((Integer) args[1]).intValue();
        }
        String ven = (gpuVendor != null && !gpuVendor.isEmpty()) ? gpuVendor
                   : ((sval != null && !sval.isEmpty()) ? sval : "Qualcomm");
        String ren = (gpuRenderer != null && !gpuRenderer.isEmpty()) ? gpuRenderer : "Adreno (TM) 730";
        if (pname == 0x1F00) { // GL_VENDOR
            return ven;
        }
        if (pname == 0x1F01) { // GL_RENDERER
            return ren;
        }
        if (pname == 0x1F02) { // GL_VERSION
            if ("ARM".equals(ven)) {
                return "OpenGL ES 3.2 v1.r38p1-01eac0.d33190cb6bdf3b92f44ebadcf3bc4727";
            }
            return "OpenGL ES 3.2 V@0615.0 (GIT@8a4fafe, I37c6875887) (Date:11/17/23)";
        }
        return invokeOriginal(args);
    }

    private Object filterPackages(Object[] args) {
        if (args != null && args.length > 1 && args[1] instanceof String) {
            String pkg = (String) args[1];
            if (isHiddenPackage(pkg)) {
                try {
                    Class<?> exCls = Class.forName("android.content.pm.PackageManager$NameNotFoundException");
                    Constructor<?> ctor = exCls.getConstructor(String.class);
                    Throwable t = (Throwable) ctor.newInstance(pkg);
                    throw sneakyThrow(t);
                } catch (ClassNotFoundException e) {
                    throw sneakyThrow(new IllegalArgumentException("Package not found: " + pkg));
                } catch (InvocationTargetException e) {
                    throw sneakyThrow(e.getCause() != null ? e.getCause() : e);
                } catch (Exception e) {
                    throw sneakyThrow(e);
                }
            }
        }

        Object r = invokeOriginal(args);
        if (r instanceof java.util.List) {
            java.util.List<?> src = (java.util.List<?>) r;
            java.util.List<Object> filtered = new java.util.ArrayList<Object>(src.size());
            for (Object item : src) {
                if (item == null) continue;
                if (isHiddenItem(item)) continue;
                filtered.add(item);
            }
            return filtered;
        }
        return r;
    }

    /**
     * True when a PackageManager list item belongs to a hidden package.
     *
     * Two shapes reach this: PackageInfo/ApplicationInfo carry a public
     * `packageName` field, while AccessibilityServiceInfo does not - it names
     * its owner only through getId(), which is "pkg/.Service" or
     * "pkg/full.Class.Name". Without the second shape, hiding bin.mt.plus from
     * getInstalledPackages left its accessibility service fully visible in
     * getInstalledAccessibilityServiceList, so the app saw a service whose
     * package it had just been told does not exist. Grab reads exactly that
     * list (AccessibilityManager.getInstalledAccessibilityServiceList, via
     * c.g.joy1.b in classes.dex) and records every id in its scan result.
     */
    private static boolean isHiddenItem(Object item) {
        try {
            Field f = item.getClass().getField("packageName");
            Object val = f.get(item);
            if (val instanceof String && isHiddenPackage((String) val)) return true;
        } catch (Throwable ignored) {}
        try {
            Method m = item.getClass().getMethod("getId");
            Object val = m.invoke(item);
            if (val instanceof String) {
                String id = (String) val;
                int cut = id.indexOf('/');
                if (cut > 0 && isHiddenPackage(id.substring(0, cut))) return true;
            }
        } catch (Throwable ignored) {}
        return false;
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

    /**
     * The M_VALUE_MULTI lookup: the gated argument is a key name, and the
     * matching entry in multiVals is the value to return. Returns null when the
     * key is absent or has no spoofed value, which makes the caller fall
     * through to the real method — a keystore read of an unrelated preference
     * must keep working.
     */
    private String multiValue(Object[] args) {
        if (args == null || keyArgIndex < 0 || args.length <= keyArgIndex) return null;
        if (multiKeys == null || multiVals == null) return null;
        Object a = args[keyArgIndex];
        if (!(a instanceof String)) return null;
        String key = (String) a;
        int n = Math.min(multiKeys.length, multiVals.length);
        for (int i = 0; i < n; i++) {
            if (key.equals(multiKeys[i])) {
                String v = multiVals[i];
                return (v == null || v.isEmpty()) ? null : v;
            }
        }
        return null;
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

    /** Implemented in native; receives the application's ClassLoader. */
    public static native boolean onAppCreated(ClassLoader cl);

    private void applyDisplayMetrics(Object dm) {
        if (dm == null || displayWidth <= 0 || displayHeight <= 0 || displayDensity <= 0) return;
        try {
            initReflection();
            Class<?> c = dm.getClass();
            int curW = s_dmWidthPixels != null ? s_dmWidthPixels.getInt(dm) : c.getField("widthPixels").getInt(dm);
            int curH = s_dmHeightPixels != null ? s_dmHeightPixels.getInt(dm) : c.getField("heightPixels").getInt(dm);
            int tw = displayWidth, th = displayHeight;
            if (curW > curH && tw < th) {
                tw = displayHeight;
                th = displayWidth;
            }
            setField(s_dmWidthPixels, c, dm, "widthPixels", tw);
            setField(s_dmHeightPixels, c, dm, "heightPixels", th);
            setField(s_dmDensityDpi, c, dm, "densityDpi", displayDensity);
            float d = displayDensity / 160.0f;
            setField(s_dmDensity, c, dm, "density", d);
            setField(s_dmScaledDensity, c, dm, "scaledDensity", d);
            float xd = displayXdpi > 0 ? displayXdpi : (float) displayDensity;
            float yd = displayYdpi > 0 ? displayYdpi : (float) displayDensity;
            setField(s_dmXdpi, c, dm, "xdpi", xd);
            setField(s_dmYdpi, c, dm, "ydpi", yd);
        } catch (Throwable ignored) {}
    }

    private void applyPoint(Object pt) {
        if (pt == null || displayWidth <= 0 || displayHeight <= 0) return;
        try {
            initReflection();
            Class<?> c = pt.getClass();
            int curX = s_ptX != null ? s_ptX.getInt(pt) : c.getField("x").getInt(pt);
            int curY = s_ptY != null ? s_ptY.getInt(pt) : c.getField("y").getInt(pt);
            int tx = displayWidth, ty = displayHeight;
            if (curX > curY && tx < ty) {
                tx = displayHeight;
                ty = displayWidth;
            }
            setField(s_ptX, c, pt, "x", tx);
            setField(s_ptY, c, pt, "y", ty);
        } catch (Throwable ignored) {}
    }

    private void applyRect(Object rect) {
        if (rect == null || displayWidth <= 0 || displayHeight <= 0) return;
        try {
            initReflection();
            Class<?> c = rect.getClass();
            int left = s_rectLeft != null ? s_rectLeft.getInt(rect) : c.getField("left").getInt(rect);
            int top = s_rectTop != null ? s_rectTop.getInt(rect) : c.getField("top").getInt(rect);
            int right = s_rectRight != null ? s_rectRight.getInt(rect) : c.getField("right").getInt(rect);
            int bottom = s_rectBottom != null ? s_rectBottom.getInt(rect) : c.getField("bottom").getInt(rect);
            int curW = right - left;
            int curH = bottom - top;
            int tw = displayWidth, th = displayHeight;
            if (curW > curH && tw < th) {
                tw = displayHeight;
                th = displayWidth;
            }
            setField(s_rectLeft, c, rect, "left", 0);
            setField(s_rectTop, c, rect, "top", 0);
            setField(s_rectRight, c, rect, "right", tw);
            setField(s_rectBottom, c, rect, "bottom", th);
        } catch (Throwable ignored) {}
    }

    private void applyConfiguration(Object cfg) {
        if (cfg == null || displayDensity <= 0) return;
        try {
            initReflection();
            Class<?> c = cfg.getClass();
            int orient = 0;
            int curSw = 0, curSh = 0;
            try {
                orient = s_cfgOrientation != null ? s_cfgOrientation.getInt(cfg) : c.getField("orientation").getInt(cfg);
            } catch (Throwable ignored) {}
            try {
                curSw = s_cfgScreenWidthDp != null ? s_cfgScreenWidthDp.getInt(cfg) : c.getField("screenWidthDp").getInt(cfg);
            } catch (Throwable ignored) {}
            try {
                curSh = s_cfgScreenHeightDp != null ? s_cfgScreenHeightDp.getInt(cfg) : c.getField("screenHeightDp").getInt(cfg);
            } catch (Throwable ignored) {}

            boolean isLand = (orient == 2) || (curSw > curSh && curSh > 0);

            float d = displayDensity / 160.0f;
            int minDim = Math.min(displayWidth > 0 ? displayWidth : 1080,
                                  displayHeight > 0 ? displayHeight : 2400);
            int maxDim = Math.max(displayWidth > 0 ? displayWidth : 1080,
                                  displayHeight > 0 ? displayHeight : 2400);
            int portSwDp = (int) (minDim / d);
            int portShDp = (int) (maxDim / d);

            int finalSwDp = isLand ? portShDp : portSwDp;
            int finalShDp = isLand ? portSwDp : portShDp;

            setField(s_cfgDensityDpi, c, cfg, "densityDpi", displayDensity);
            setField(s_cfgScreenWidthDp, c, cfg, "screenWidthDp", finalSwDp);
            setField(s_cfgScreenHeightDp, c, cfg, "screenHeightDp", finalShDp);
            setField(s_cfgSmallestScreenWidthDp, c, cfg, "smallestScreenWidthDp", portSwDp);
        } catch (Throwable ignored) {}
    }

    private static void setField(Field f, Class<?> c, Object obj, String name, int val) {
        try {
            if (f != null) { f.setInt(obj, val); return; }
            if (c != null) c.getField(name).setInt(obj, val);
        } catch (Throwable ignored) {}
    }

    private static void setField(Field f, Class<?> c, Object obj, String name, float val) {
        try {
            if (f != null) { f.setFloat(obj, val); return; }
            if (c != null) c.getField(name).setFloat(obj, val);
        } catch (Throwable ignored) {}
    }

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
