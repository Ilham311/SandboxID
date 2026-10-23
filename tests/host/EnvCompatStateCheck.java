/**
 * Pins the exception contract of the L3 callback
 * ({@code jni/SandboxIDHook.java}, compiled into DEX and shipped as
 * {@code EnvCompatState}).
 *
 * <p><b>Why this exists.</b> {@code invokeOriginal} calls the backup through
 * reflection, so whatever the real method threw arrives wrapped in
 * {@link java.lang.reflect.InvocationTargetException}. The first version
 * unwrapped only {@code RuntimeException} and {@code Error} and turned
 * everything else into {@code new RuntimeException(cause)}. That silently
 * changed the type of <i>checked</i> exceptions.
 *
 * <p>The failure it caused, seen on a real device: {@code LoadedApk.rewriteRValues}
 * probes for an obfuscated {@code <pkg>.R} class and does
 * {@code catch (ClassNotFoundException e) { ...; return; }} — a miss there is
 * normal. Because the hook rethrew the miss as a {@code RuntimeException}, the
 * catch did not match, {@code makeApplicationInner} wrapped it in
 * "Unable to instantiate application ...", and every target app died during
 * startup. The type has to survive exactly, checked or not.
 *
 * <p>JDK-only and device-free by design: {@code EnvCompatState} has no Android
 * dependency, so this runs on a plain JDK in CI. run.sh compiles a copy of the
 * source under the package directory javac requires.
 */
public class EnvCompatStateCheck {

    static int fails = 0;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "ok  " : "FAIL") + " " + name);
        if (!ok) fails++;
    }

    /** Stand-ins for the hooked framework methods. */
    public static class Target {
        public final android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
        public final android.content.res.Configuration configuration =
                new android.content.res.Configuration();

        public Object metrics() {
            return metrics;
        }

        public Object configuration() {
            return configuration;
        }

        public Object load(String n) throws ClassNotFoundException {
            throw new ClassNotFoundException("Didn't find class \"" + n + "\"");
        }

        public Object ok(String n) {
            return "class " + n;
        }

        public Object boom(String n) {
            throw new IllegalStateException("boom");
        }

        /** The shape of a real WebView UA on the test device. */
        public Object ua(String ignored) {
            return "Mozilla/5.0 (Linux; Android 15; M2012K11AG Build/AQ3A.250226.002; wv) "
                 + "AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
                 + "Chrome/155.0.8059.43 Mobile Safari/537.36";
        }

        public Object emptyUa(String ignored) {
            return "";
        }

        public Object nullUa(String ignored) {
            return null;
        }

        public Object error(String n) {
            throw new StackOverflowError("err");
        }

        public Object getPackageInfo(String name, int flags) throws android.content.pm.PackageManager.NameNotFoundException {
            if ("not.found".equals(name)) throw new android.content.pm.PackageManager.NameNotFoundException(name);
            return new android.content.pm.PackageInfo(name);
        }

        public Object getInstalledPackages(int flags) {
            java.util.List<android.content.pm.PackageInfo> list = new java.util.ArrayList<android.content.pm.PackageInfo>();
            list.add(new android.content.pm.PackageInfo("com.google.android.gms"));
            list.add(new android.content.pm.PackageInfo("com.topjohnwu.magisk"));
            list.add(new android.content.pm.PackageInfo("com.termux"));
            list.add(new android.content.pm.PackageInfo("com.grabtaxi.passenger"));
            return list;
        }
    }

    static java.lang.reflect.Method m(String name) throws Exception {
        return Target.class.getDeclaredMethod(name, String.class);
    }

    static androidx.core.os.EnvCompatState observe(String backup) throws Exception {
        androidx.core.os.EnvCompatState s = new androidx.core.os.EnvCompatState();
        s.mode = androidx.core.os.EnvCompatState.M_OBSERVE;
        s.isStatic = false;
        s.backup = m(backup);
        return s;
    }

    public static void main(String[] a) throws Exception {
        Target t = new Target();
        androidx.core.os.EnvCompatState.watch = null;

        // The regression: a checked CNFE must come out as CNFE. handle() has no
        // throws clause (athrow in bytecode does not care), so the static checker
        // rejects `catch (ClassNotFoundException)` here; catch Throwable and assert
        // the runtime type, which is what ART's handler in rewriteRValues sees.
        try {
            observe("load").handle(new Object[]{ t, "com.x.R" });
            check("CNFE is thrown", false);
        } catch (Throwable e) {
            check("CNFE keeps its type (not wrapped in RuntimeException)",
                    e instanceof ClassNotFoundException);
            check("CNFE message preserved",
                    String.valueOf(e.getMessage()).contains("com.x.R"));
        }

        // A normal return must still come through untouched.
        check("normal return preserved",
                "class Foo".equals(observe("ok").handle(new Object[]{ t, "Foo" })));

        // Unchecked exceptions keep their own type too (this already worked, but
        // the unwrap is shared, so a change here would be a regression).
        try {
            observe("boom").handle(new Object[]{ t, "x" });
            check("ISE is thrown", false);
        } catch (Throwable e) {
            check("ISE keeps its type", e instanceof IllegalStateException);
        }

        try {
            observe("error").handle(new Object[]{ t, "x" });
            check("Error is thrown", false);
        } catch (Throwable e) {
            check("Error keeps its type", e instanceof StackOverflowError);
        }

        // M_ZERO_NUMERIC calls the original before deciding, on the same unwrap.
        androidx.core.os.EnvCompatState z = observe("load");
        z.mode = androidx.core.os.EnvCompatState.M_ZERO_NUMERIC;
        z.keyArgIndex = 1;
        try {
            z.handle(new Object[]{ t, "com.x.R" });
            check("M_ZERO_NUMERIC propagates CNFE", false);
        } catch (Throwable e) {
            check("M_ZERO_NUMERIC propagates CNFE unwrapped",
                    e instanceof ClassNotFoundException);
        }

        // M_REWRITE_STRING: the WebView UA must lose the real model and build id
        // while keeping the rest of the provider's string byte-identical.
        androidx.core.os.EnvCompatState r = observe("ua");
        r.mode = androidx.core.os.EnvCompatState.M_REWRITE_STRING;
        r.rwFrom = new String[]{ "Android 15; M2012K11AG Build/AQ3A.250226.002" };
        r.rwTo   = new String[]{ "Android 15; Pixel 9 Build/AP3A.241005.015" };
        String got = String.valueOf(r.handle(new Object[]{ t, "x" }));
        check("UA model+id replaced", got.contains("Android 15; Pixel 9 Build/AP3A.241005.015"));
        check("UA real model gone", !got.contains("M2012K11AG"));
        check("UA real build id gone", !got.contains("AQ3A.250226.002"));
        check("UA version suffix preserved", got.contains("Chrome/155.0.8059.43"));
        check("UA wv marker preserved", got.contains("; wv)"));

        // A cached spoof copy is reused while its live source is unchanged, but
        // a source orientation/configuration change must produce fresh spoofed data.
        t.metrics.widthPixels = 1080;
        t.metrics.heightPixels = 2400;
        t.metrics.densityDpi = 440;
        t.metrics.density = 2.75f;
        t.metrics.scaledDensity = 2.75f;
        t.metrics.xdpi = 440f;
        t.metrics.ydpi = 440f;
        androidx.core.os.EnvCompatState dm = new androidx.core.os.EnvCompatState();
        dm.mode = androidx.core.os.EnvCompatState.M_DISPLAY_METRICS;
        dm.backup = Target.class.getDeclaredMethod("metrics");
        dm.displayWidth = 720;
        dm.displayHeight = 1600;
        dm.displayDensity = 320;
        Object dmFirst = dm.handle(new Object[]{ t });
        check("DisplayMetrics initially spoofed",
                dmFirst instanceof android.util.DisplayMetrics
                        && ((android.util.DisplayMetrics) dmFirst).widthPixels == 720
                        && ((android.util.DisplayMetrics) dmFirst).heightPixels == 1600);
        ((android.util.DisplayMetrics) dmFirst).widthPixels = 1234;
        Object dmUnchanged = dm.handle(new Object[]{ t });
        check("unchanged DisplayMetrics source reuses caller-mutated cache",
                dmUnchanged == dmFirst
                        && ((android.util.DisplayMetrics) dmUnchanged).widthPixels == 1234);
        t.metrics.widthPixels = 2400;
        t.metrics.heightPixels = 1080;
        Object dmUpdated = dm.handle(new Object[]{ t });
        check("changed DisplayMetrics orientation refreshes cached spoof",
                dmUpdated != dmFirst
                        && ((android.util.DisplayMetrics) dmUpdated).widthPixels == 1600
                        && ((android.util.DisplayMetrics) dmUpdated).heightPixels == 720);

        t.configuration.orientation = 1;
        t.configuration.screenWidthDp = 400;
        t.configuration.screenHeightDp = 800;
        androidx.core.os.EnvCompatState cfg = new androidx.core.os.EnvCompatState();
        cfg.mode = androidx.core.os.EnvCompatState.M_DISPLAY_METRICS;
        cfg.backup = Target.class.getDeclaredMethod("configuration");
        cfg.displayWidth = 720;
        cfg.displayHeight = 1600;
        cfg.displayDensity = 320;
        Object cfgFirst = cfg.handle(new Object[]{ t });
        check("Configuration initially spoofed",
                cfgFirst instanceof android.content.res.Configuration
                        && ((android.content.res.Configuration) cfgFirst).screenWidthDp == 360
                        && ((android.content.res.Configuration) cfgFirst).screenHeightDp == 800);
        t.configuration.orientation = 2;
        Object cfgUpdated = cfg.handle(new Object[]{ t });
        check("changed Configuration refreshes cached spoof",
                cfgUpdated != cfgFirst
                        && ((android.content.res.Configuration) cfgUpdated).screenWidthDp == 800
                        && ((android.content.res.Configuration) cfgUpdated).screenHeightDp == 360);

        // Empty / null results pass through: a hook that invented a UA for a
        // provider that returned nothing would be fabricating data.
        androidx.core.os.EnvCompatState r2 = observe("emptyUa");
        r2.mode = androidx.core.os.EnvCompatState.M_REWRITE_STRING;
        r2.rwFrom = r.rwFrom;
        r2.rwTo = r.rwTo;
        check("empty UA untouched", "".equals(r2.handle(new Object[]{ t, "x" })));
        androidx.core.os.EnvCompatState r3 = observe("nullUa");
        r3.mode = androidx.core.os.EnvCompatState.M_REWRITE_STRING;
        r3.rwFrom = r.rwFrom;
        r3.rwTo = r.rwTo;
        check("null UA untouched", r3.handle(new Object[]{ t, "x" }) == null);

        // Mismatched pair arrays must not index out of bounds.
        androidx.core.os.EnvCompatState r4 = observe("ua");
        r4.mode = androidx.core.os.EnvCompatState.M_REWRITE_STRING;
        r4.rwFrom = new String[]{ "Android 15" };
        r4.rwTo = new String[0];
        check("mismatched pairs are a no-op",
                String.valueOf(r4.handle(new Object[]{ t, "x" })).contains("M2012K11AG"));

        // Denied OEM classes. A Pixel cannot load miui.os.Build, so the app's
        // OEM branch stays dead instead of reporting "rom": "MIUI-…".
        //
        // Two shapes, because the deny must read the class name from whichever
        // argument holds it: ClassLoader.loadClass(String,boolean) is an
        // instance method (args[0] is the receiver, name at 1), while
        // Class.forName(String,boolean,ClassLoader) is static (name at 0).
        androidx.core.os.EnvCompatState d = observe("ok");
        d.keyArgIndex = 1;
        androidx.core.os.EnvCompatState.deny =
                new String[]{ "miui.os.Build", "com.oplus.os.OplusBuild" };
        try {
            d.handle(new Object[]{ t, "miui.os.Build" });
            check("denied class throws", false);
        } catch (Throwable e) {
            check("denied class throws ClassNotFoundException",
                    e instanceof ClassNotFoundException);
            check("denied class name preserved in the message",
                    String.valueOf(e.getMessage()).contains("miui.os.Build"));
        }
        check("a non-denied class still loads",
                "class com.other.Thing".equals(d.handle(new Object[]{ t, "com.other.Thing" })));
        check("a denied prefix does not deny the longer name",
                "class miui.os.BuildExtra".equals(d.handle(new Object[]{ t, "miui.os.BuildExtra" })));

        // The static overload: no receiver, name at index 0.
        androidx.core.os.EnvCompatState ds = observe("ok");
        ds.isStatic = true;
        ds.keyArgIndex = 0;
        try {
            ds.handle(new Object[]{ "miui.os.Build" });
            check("static overload denies too", false);
        } catch (Throwable e) {
            check("static overload denies too", e instanceof ClassNotFoundException);
        }

        // A null list is a pass-through, not a crash or a blanket deny.
        androidx.core.os.EnvCompatState.deny = null;
        check("null deny list is a pass-through",
                "class miui.os.Build".equals(d.handle(new Object[]{ t, "miui.os.Build" })));
        androidx.core.os.EnvCompatState.deny = new String[0];
        check("empty deny list is a pass-through",
                "class miui.os.Build".equals(d.handle(new Object[]{ t, "miui.os.Build" })));
        androidx.core.os.EnvCompatState.deny = null;

        // M_FILTER_PACKAGES: hiding root & mod apps from PackageManager
        androidx.core.os.EnvCompatState pmGet = new androidx.core.os.EnvCompatState();
        pmGet.mode = androidx.core.os.EnvCompatState.M_FILTER_PACKAGES;
        pmGet.isStatic = false;
        pmGet.backup = Target.class.getDeclaredMethod("getPackageInfo", String.class, int.class);

        // Legit package passes through
        Object legit = pmGet.handle(new Object[]{ t, "com.grabtaxi.passenger", 0 });
        check("legit package returns PackageInfo", legit instanceof android.content.pm.PackageInfo &&
                "com.grabtaxi.passenger".equals(((android.content.pm.PackageInfo) legit).packageName));

        // Magisk throws NameNotFoundException
        try {
            pmGet.handle(new Object[]{ t, "com.topjohnwu.magisk", 0 });
            check("Magisk throws NameNotFoundException", false);
        } catch (Throwable e) {
            check("Magisk throws NameNotFoundException", e instanceof android.content.pm.PackageManager.NameNotFoundException);
            check("NameNotFoundException carries package name", String.valueOf(e.getMessage()).contains("com.topjohnwu.magisk"));
        }

        // List filtering
        androidx.core.os.EnvCompatState pmList = new androidx.core.os.EnvCompatState();
        pmList.mode = androidx.core.os.EnvCompatState.M_FILTER_PACKAGES;
        pmList.isStatic = false;
        pmList.backup = Target.class.getDeclaredMethod("getInstalledPackages", int.class);

        Object listObj = pmList.handle(new Object[]{ t, 0 });
        check("getInstalledPackages returns list", listObj instanceof java.util.List);
        if (listObj instanceof java.util.List) {
            java.util.List<?> list = (java.util.List<?>) listObj;
            boolean hasMagisk = false, hasTermux = false, hasGms = false, hasGrab = false;
            for (Object o : list) {
                if (o instanceof android.content.pm.PackageInfo) {
                    String pn = ((android.content.pm.PackageInfo) o).packageName;
                    if ("com.topjohnwu.magisk".equals(pn)) hasMagisk = true;
                    if ("com.termux".equals(pn)) hasTermux = true;
                    if ("com.google.android.gms".equals(pn)) hasGms = true;
                    if ("com.grabtaxi.passenger".equals(pn)) hasGrab = true;
                }
            }
            check("Magisk filtered out", !hasMagisk);
            check("Termux filtered out", !hasTermux);
            check("GMS kept", hasGms);
            check("Grab kept", hasGrab);
        }

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        if (fails != 0) System.exit(1);
    }
}
