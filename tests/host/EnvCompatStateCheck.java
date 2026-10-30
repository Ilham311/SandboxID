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

        public long getBlockSizeLong() { return 1024L; }
        public int getBlockSize() { return 1024; }
        public long getTotalBytes() { return 1000000L; }
        public String getVendor() { return "Xiaomi"; }
        public String getName() { return "Xiaomi alioth traffic_detection sensor"; }
        public String getInputName() { return "XiaoMi traffic_detection v2.4"; }
        public int getVendorId() { return 0x2717; }
        public String[] getCameraIdList() { return new String[]{"0", "1", "2", "3"}; }
        public String glGetString(int name) { return "Adreno (TM) 650"; }
        public static String glGetStringStatic(int name) { return "Adreno (TM) 650"; }

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

        /**
         * AccessibilityServiceInfo has no packageName field - the owner is
         * encoded in getId() as "pkg/.Service". Deliberately a plain class with
         * only getId(), so the test fails if the filter ever starts assuming a
         * packageName field it does not have.
         */
        public Object getInstalledAccessibilityServiceList() {
            java.util.List<Object> list = new java.util.ArrayList<Object>();
            list.add(new AccService("com.google.android.marvin.talkback/.TalkBackService"));
            list.add(new AccService("bin.mt.plus.canary/l.ۖۢۖ"));
            list.add(new AccService("com.fufufu.katrina.backup/.KatrinaIslandService"));
            list.add(new AccService("com.android.systemui.accessibility.accessibilitymenu/.AccessibilityMenuService"));
            return list;
        }

        public Object getEnabledAccessibilityServiceList(int feedbackType) {
            return getInstalledAccessibilityServiceList();
        }

        /**
         * Grab's bundled AdvertisingIdClient.getAdvertisingIdInfo. The real one
         * throws here: its b(false) binds to the Play-services advertising-id
         * service and wraps a failed bind in java.io.IOException before any
         * Info exists. Modelled exactly - static, taking a Context, returning
         * the Info type - because that is the control flow the M_GAID_INFO hook
         * has to pre-empt. A stub that returned an Info, or one whose return
         * type were Object, would make the test pass without the hook doing
         * anything: the hook takes the Info class from
         * backup.getReturnType().
         */
        public static com.google.android.gms.ads.identifier.AdvertisingIdClient.Info
                getAdvertisingIdInfo(Object context) throws java.io.IOException {
            throw new java.io.IOException("Google Play services not available");
        }

        /**
         * ContentResolver.query, as Grab calls it for the GSF id. The real
         * gservices provider carries the keys in selectionArgs and returns one
         * row per requested key that exists. Modelled faithfully - including
         * returning an empty cursor for a key it does not have - so the test can
         * tell "the hook answered" apart from "the provider answered", which is
         * the whole point of the gate.
         */
        public Object query(Object uri, String[] projection, String selection,
                            String[] selectionArgs, String sortOrder) {
            String u = String.valueOf(uri);
            if (u.equals("content://com.google.android.gsf.gservices")
                    || u.equals("content://settings/secure")) {
                // Both providers are queried with selectionArgs={"android_id"},
                // and both return an empty cursor here: gservices has no row for
                // the key (the er:103 state), and Settings.Secure's row is served
                // by a different code path entirely. Returning the same shape for
                // both is what lets the test prove the hook distinguishes them by
                // URI rather than by key.
                return new android.database.MatrixCursor(new String[]{"_id", "value"}, 1);
            }
            return "real-cursor:" + u;
        }
    }

    /** Minimal stand-in for android.accessibilityservice.AccessibilityServiceInfo. */
    public static class AccService {
        private final String id;
        AccService(String id) { this.id = id; }
        public String getId() { return id; }
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

        // The accessibility list is the same hidden set seen through a different
        // API. AccessibilityServiceInfo carries no packageName field, so before
        // isHiddenItem() learned to read getId() the modding tools stayed fully
        // visible here while getInstalledPackages denied they existed - a
        // contradiction inside one process, and Grab records every id.
        androidx.core.os.EnvCompatState accList = new androidx.core.os.EnvCompatState();
        accList.mode = androidx.core.os.EnvCompatState.M_FILTER_PACKAGES;
        accList.isStatic = false;
        accList.backup = Target.class.getDeclaredMethod("getInstalledAccessibilityServiceList");

        Object accObj = accList.handle(new Object[]{ t });
        check("accessibility list returns list", accObj instanceof java.util.List);
        if (accObj instanceof java.util.List) {
            java.util.List<?> list = (java.util.List<?>) accObj;
            boolean hasMt = false, hasKatrina = false, hasTalkback = false, hasSysui = false;
            for (Object o : list) {
                if (!(o instanceof AccService)) continue;
                String id = ((AccService) o).getId();
                if (id.startsWith("bin.mt.plus.canary/")) hasMt = true;
                if (id.startsWith("com.fufufu.katrina.backup/")) hasKatrina = true;
                if (id.startsWith("com.google.android.marvin.talkback/")) hasTalkback = true;
                if (id.startsWith("com.android.systemui.")) hasSysui = true;
            }
            check("MT Manager accessibility service filtered out", !hasMt);
            // Katrina IS in HIDDEN_PACKAGES, and that is deliberate. Grab's HVS
            // scan result recorded
            // com.fufufu.katrina.backup/.KatrinaIslandService among the enabled
            // accessibility services of a device claiming to be an OPPO
            // CPH2521 - a third-party backup tool riding an accessibility
            // service, which no retail handset ships. The filter hides exactly
            // the configured set, so this check pins the set: a list filter that
            // quietly widened itself to "anything sideloaded" would be a
            // different policy, and one that dropped the entry would let the
            // leak back in.
            check("Katrina accessibility service filtered out", !hasKatrina);
            check("TalkBack accessibility service kept", hasTalkback);
            check("SystemUI accessibility service kept", hasSysui);
        }

        // Same filter, the other entry point.
        androidx.core.os.EnvCompatState accEnabled = new androidx.core.os.EnvCompatState();
        accEnabled.mode = androidx.core.os.EnvCompatState.M_FILTER_PACKAGES;
        accEnabled.isStatic = false;
        accEnabled.backup = Target.class.getDeclaredMethod("getEnabledAccessibilityServiceList", int.class);
        Object enObj = accEnabled.handle(new Object[]{ t, -1 });
        boolean enHasMt = false;
        if (enObj instanceof java.util.List) {
            for (Object o : (java.util.List<?>) enObj) {
                if (o instanceof AccService
                        && ((AccService) o).getId().startsWith("bin.mt.plus.canary/")) enHasMt = true;
            }
        }
        check("getEnabledAccessibilityServiceList also filters MT Manager", !enHasMt);

        // ---- M_GAID_INFO: the advertising id for an app that bundles the client
        //
        // The failure this fixes, seen in Grab's scan result: ad:"er:109" and
        // adl:"er:109", both GENERAL_EXCEPTION. Grab ships its own
        // AdvertisingIdClient, whose getAdvertisingIdInfo() calls b(false) ->
        // bindService -> throws java.io.IOException before any Info exists. The
        // pre-existing hook on $Info.getId() therefore never fires: getId() is
        // never reached. The hook has to be on the static entry point.
        androidx.core.os.EnvCompatState g = new androidx.core.os.EnvCompatState();
        g.mode = androidx.core.os.EnvCompatState.M_GAID_INFO;
        g.isStatic = true;
        g.sval = "11111111-2222-4333-8444-555555555555";
        g.gaidLimitAdTracking = false;
        g.backup = Target.class.getDeclaredMethod("getAdvertisingIdInfo", Object.class);
        // Wrapped: if the hook does not pre-empt the original, the original's
        // IOException escapes here. Reporting that as a named failure is far more
        // useful than letting main() die with a stack trace that names the stub
        // instead of the regression.
        Object infoObj = null;
        Throwable gaidErr = null;
        try {
            infoObj = g.handle(new Object[]{ t });
        } catch (Throwable e) {
            gaidErr = e;
        }
        check("M_GAID_INFO does not let the original IOException escape",
                gaidErr == null);

        check("M_GAID_INFO returns an Info instead of propagating the IOException",
                infoObj instanceof com.google.android.gms.ads.identifier.AdvertisingIdClient.Info);
        if (infoObj instanceof com.google.android.gms.ads.identifier.AdvertisingIdClient.Info) {
            com.google.android.gms.ads.identifier.AdvertisingIdClient.Info info =
                    (com.google.android.gms.ads.identifier.AdvertisingIdClient.Info) infoObj;
            check("M_GAID_INFO carries the persona advertising id",
                    "11111111-2222-4333-8444-555555555555".equals(info.getId()));
            check("M_GAID_INFO reports limit-ad-tracking false (not opted out)",
                    !info.isLimitAdTrackingEnabled());
        }

        // The other direction: with no id to serve, the original must still run
        // and its own exception must reach the caller unchanged. A hook that
        // swallowed the IOException and returned null would hand Grab a null
        // Info to dereference.
        androidx.core.os.EnvCompatState gNoVal = new androidx.core.os.EnvCompatState();
        gNoVal.mode = androidx.core.os.EnvCompatState.M_GAID_INFO;
        gNoVal.isStatic = true;
        gNoVal.backup = Target.class.getDeclaredMethod("getAdvertisingIdInfo", Object.class);
        try {
            gNoVal.handle(new Object[]{ t });
            check("M_GAID_INFO without a value still throws", false);
        } catch (Throwable e) {
            check("M_GAID_INFO without a value propagates the original IOException",
                    e instanceof java.io.IOException);
        }

        // ---- M_GSF_CURSOR: the GSF id for a device with an empty gservices table
        //
        // The failure this fixes: gs:"er:103" (GSF_NUMBER_FORMAT_EXCEPTION).
        // Grab does Long.parseLong(cursor.getString(1)); the stub's column 1 is
        // null, which throws exactly the NumberFormatException that produces
        // er:103. The hook must answer with a cursor whose column 1 parses.
        androidx.core.os.EnvCompatState s = new androidx.core.os.EnvCompatState();
        s.mode = androidx.core.os.EnvCompatState.M_GSF_CURSOR;
        s.isStatic = false;
        s.keyArgIndex = 1;
        s.keyMatch = "content://com.google.android.gsf.gservices";
        s.keyContains = false;
        s.gsfId = "81985529216486895";
        s.backup = Target.class.getDeclaredMethod("query", Object.class, String[].class,
                String.class, String[].class, String.class);
        Object curObj = s.handle(new Object[]{ t,
                "content://com.google.android.gsf.gservices", null, null,
                new String[]{"android_id"}, null });

        check("M_GSF_CURSOR returns a MatrixCursor",
                curObj instanceof android.database.MatrixCursor);
        if (curObj instanceof android.database.MatrixCursor) {
            android.database.MatrixCursor c = (android.database.MatrixCursor) curObj;
            check("M_GSF_CURSOR cursor has the provider's two columns",
                    c.getColumnCount() == 2);
            // Read column 1 defensively: a one-column cursor would otherwise
            // throw and turn this regression into a stack trace from main()
            // rather than a named failure.
            String col0 = null, col1 = null;
            try { col0 = c.getString(0); } catch (Throwable ignored) {}
            try { col1 = c.getString(1); } catch (Throwable ignored) {}
            check("M_GSF_CURSOR column 0 is the queried key", "android_id".equals(col0));
            check("M_GSF_CURSOR column 1 is the persona GSF id",
                    "81985529216486895".equals(col1));
            // The whole point: the value Grab parses must actually parse, and
            // must round-trip through the same call chain Grab uses -
            // Long.parseLong(decimal) then Long.toHexString(). Asserting a fixed
            // hex length would be asserting something no device guarantees:
            // toHexString() trims leading zeros, so a value below 2^60 renders
            // in 15 digits, and that is a perfectly normal GSF id.
            boolean parsed = false;
            boolean roundTrips = false;
            try {
                long v = Long.parseLong(col1);
                String hex = Long.toHexString(v);
                parsed = true;
                roundTrips = !hex.isEmpty() && Long.parseLong(hex, 16) == v;
            } catch (Throwable ignored) {}
            check("M_GSF_CURSOR column 1 survives Long.parseLong", parsed);
            check("M_GSF_CURSOR column 1 round-trips through Long.toHexString", roundTrips);
        }

        // The other direction: a query for any other provider must reach the
        // real ContentResolver untouched. This hook sits on a method called for
        // every provider in the process, so a gate that leaked would corrupt
        // unrelated reads.
        Object other = s.handle(new Object[]{ t,
                "content://com.example.other", null, null, null, null });
        check("M_GSF_CURSOR leaves other providers alone",
                "real-cursor:content://com.example.other".equals(String.valueOf(other)));

        // And the same provider, a different key: gservices carries the keys in
        // selectionArgs, so a query for another key must reach the real provider
        // rather than be answered with the android_id row. The stub's own cursor
        // is empty and has no rows, while the hook's is one row - so "count == 0"
        // is what distinguishes "fell through" from "answered".
        Object otherKey = s.handle(new Object[]{ t,
                "content://com.google.android.gsf.gservices", null, null,
                new String[]{"logging_id2"}, null });
        check("M_GSF_CURSOR leaves other gservices keys alone",
                otherKey instanceof android.database.MatrixCursor
                        && ((android.database.MatrixCursor) otherKey).getCount() == 0);

        // A query with no selectionArgs asks for every row; this hook cannot
        // honestly serve that either.
        Object allRows = s.handle(new Object[]{ t,
                "content://com.google.android.gsf.gservices", null, null, null, null });
        check("M_GSF_CURSOR leaves an unfiltered gservices query alone",
                allRows instanceof android.database.MatrixCursor
                        && ((android.database.MatrixCursor) allRows).getCount() == 0);

        // The URI gate is NOT redundant with the key gate, and this is the case
        // that proves it: Settings.Secure's provider is queried with
        // selectionArgs={"android_id"} too, because that is how
        // Settings.Secure.getString(name) is implemented. The android_id and the
        // GSF id are different identifiers, so answering that query with the GSF
        // id would hand the app the wrong value under the right name - a subtler
        // and worse failure than the er:103 this fix removes. Only the gservices
        // URI may be answered.
        Object secureAndroidId = s.handle(new Object[]{ t,
                "content://settings/secure", null, null,
                new String[]{"android_id"}, null });
        check("M_GSF_CURSOR does not answer Settings.Secure's android_id lookup",
                secureAndroidId instanceof android.database.MatrixCursor
                        && ((android.database.MatrixCursor) secureAndroidId).getCount() == 0);

        // ---- Arkose Labs hardware telemetry modes ----
        // 1. M_STATFS
        androidx.core.os.EnvCompatState st = new androidx.core.os.EnvCompatState();
        st.mode = androidx.core.os.EnvCompatState.M_STATFS;
        st.backup = Target.class.getDeclaredMethod("getBlockSizeLong");
        Object bsLong = st.handle(new Object[]{ t });
        check("M_STATFS getBlockSizeLong", Long.valueOf(4096L).equals(bsLong));

        st.backup = Target.class.getDeclaredMethod("getBlockSize");
        Object bsInt = st.handle(new Object[]{ t });
        check("M_STATFS getBlockSize", Integer.valueOf(4096).equals(bsInt));

        st.backup = Target.class.getDeclaredMethod("getTotalBytes");
        Object tb = st.handle(new Object[]{ t });
        check("M_STATFS getTotalBytes", Long.valueOf(137438953472L).equals(tb));

        // 2. M_SENSOR
        androidx.core.os.EnvCompatState sn = new androidx.core.os.EnvCompatState();
        sn.mode = androidx.core.os.EnvCompatState.M_SENSOR;
        sn.sval = "Google";
        sn.backup = Target.class.getDeclaredMethod("getVendor");
        check("M_SENSOR getVendor", "Google".equals(sn.handle(new Object[]{ t })));

        sn.backup = Target.class.getDeclaredMethod("getName");
        Object snName = sn.handle(new Object[]{ t });
        check("M_SENSOR getName sanitized",
                "Google device sensor sensor".equals(snName));

        // 3. M_INPUT_DEVICE
        androidx.core.os.EnvCompatState inp = new androidx.core.os.EnvCompatState();
        inp.mode = androidx.core.os.EnvCompatState.M_INPUT_DEVICE;
        inp.backup = Target.class.getDeclaredMethod("getInputName");
        Object inpName = inp.handle(new Object[]{ t });
        check("M_INPUT_DEVICE getName sanitized", "touchscreen".equals(inpName));

        inp.backup = Target.class.getDeclaredMethod("getVendorId");
        Object inpVid = inp.handle(new Object[]{ t });
        check("M_INPUT_DEVICE getVendorId Xiaomi reset to 0", Integer.valueOf(0).equals(inpVid));

        // 4. M_CAMERA
        androidx.core.os.EnvCompatState cam = new androidx.core.os.EnvCompatState();
        cam.mode = androidx.core.os.EnvCompatState.M_CAMERA;
        cam.backup = Target.class.getDeclaredMethod("getCameraIdList");
        Object camList = cam.handle(new Object[]{ t });
        check("M_CAMERA getCameraIdList returns 0,1",
                camList instanceof String[]
                && ((String[]) camList).length == 2
                && "0".equals(((String[]) camList)[0])
                && "1".equals(((String[]) camList)[1]));

        // 5. M_GL_STRING
        androidx.core.os.EnvCompatState gl = new androidx.core.os.EnvCompatState();
        gl.mode = androidx.core.os.EnvCompatState.M_GL_STRING;
        gl.isStatic = false;
        gl.gpuVendor = "Qualcomm";
        gl.gpuRenderer = "Adreno (TM) 730";
        gl.backup = Target.class.getDeclaredMethod("glGetString", int.class);
        check("M_GL_STRING GL_VENDOR", "Qualcomm".equals(gl.handle(new Object[]{ t, 0x1F00 })));
        check("M_GL_STRING GL_RENDERER", "Adreno (TM) 730".equals(gl.handle(new Object[]{ t, 0x1F01 })));
        check("M_GL_STRING GL_VERSION", String.valueOf(gl.handle(new Object[]{ t, 0x1F02 })).startsWith("OpenGL ES 3.2"));

        androidx.core.os.EnvCompatState glStatic = new androidx.core.os.EnvCompatState();
        glStatic.mode = androidx.core.os.EnvCompatState.M_GL_STRING;
        glStatic.isStatic = true;
        glStatic.gpuVendor = "ARM";
        glStatic.gpuRenderer = "Mali-G715";
        glStatic.backup = Target.class.getDeclaredMethod("glGetStringStatic", int.class);
        check("M_GL_STRING static GL_RENDERER", "Mali-G715".equals(glStatic.handle(new Object[]{ 0x1F01 })));

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        if (fails != 0) System.exit(1);
    }
}
