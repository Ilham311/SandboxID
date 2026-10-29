package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedHelpers;

/**
 * Telephony and DRM identifiers, for the LSPosed variant.
 *
 * <p><b>This does NOT cover ground the Zygisk module leaves bare.</b> Since
 * commit {@code 55c68c8} the module carries the same Java-method layer
 * ({@code SBX_ENABLE_LSPLANT}, on by default in {@code build.sh}), so it hooks
 * {@link android.telephony.TelephonyManager}, {@link android.media.MediaDrm},
 * {@link android.net.wifi.WifiInfo} and {@link android.bluetooth.BluetoothAdapter}
 * too. This class is the LSPosed equivalent of that layer, not a superset of a
 * smaller module — an earlier version of this docstring claimed the opposite and
 * was simply out of date.
 *
 * <p>Because the two are equivalents, every surface here has to be kept in
 * step with {@code jni/sbx_lsplant.hpp}. That is not automatic, and it has
 * already failed twice: the cell-identity and {@code getSimCarrierId} specs from
 * v2.2.38 were never ported here, and the AppLog ids drifted. When adding a
 * surface on one side, add it on the other.
 *
 * <p>Values are deterministic from the persona seed ({@link SeedId}), so a given
 * persona always yields the same IMEI/IMSI/ICCID/MEID — a rotated persona stays
 * internally consistent, and the same persona reproduces the same identifiers
 * across the Zygisk and LSPosed variants. That last half is only true because
 * the seed is {@link SeedId#applogSeed}, which appends the package exactly as
 * {@code applog_seed()} does in the native module; the package-less seed this
 * class used to derive produced a different IMEI for the same persona.
 *
 * <p><b>Fail-soft, by design.</b> Every hook is resolved individually; a method
 * hidden or removed on the running API level simply fails resolution and is
 * skipped. One unavailable method must not disable the rest — mirrors the C++
 * loop's behaviour.
 */
final class TelephonyDrmHook {

    private static final String TAG = "SandboxID-TT";

    /**
     * The objects that expose the cell's and the SIM's own view of the
     * operator. Each has {@code getMccString}/{@code getMncString} (API 28+),
     * and all of them have to agree with
     * {@code TelephonyManager.getSimOperator()} — otherwise the app sees one
     * network from the manager and another from the cell.
     *
     * <p>{@code CellIdentityCdma} is absent: it has no MCC/MNC accessors at
     * all, despite Grab referencing it.
     */
    private static final String[] CELL_IDENTITY_CLASSES = {
            "android.telephony.CellIdentityLte",
            "android.telephony.CellIdentityWcdma",
            "android.telephony.CellIdentityGsm",
            "android.telephony.CellIdentityNr",
            "android.telephony.SubscriptionInfo",
    };

    /**
     * The subset that also has the int accessors {@code getMcc()}/{@code getMnc()}.
     *
     * <p>{@code CellIdentityNr} is deliberately NOT here. Its method list on
     * API 35 (verified against the test device's framework.jar) is
     * getMccString/getMncString plus NCI/NRARFCN/PCI/TAC — the NR class never
     * had the int forms that LTE/WCDMA/GSM carry. Hooking a method that does
     * not exist costs a failed resolution on every app start and, worse, hides
     * a real resolution failure in the L3 diagnostics.
     */
    private static final String[] CELL_IDENTITY_INT_CLASSES = {
            "android.telephony.CellIdentityLte",
            "android.telephony.CellIdentityWcdma",
            "android.telephony.CellIdentityGsm",
            "android.telephony.SubscriptionInfo",
    };

    private TelephonyDrmHook() {}

    static void apply(ClassLoader cl, Map<String, String> id) {
        apply(cl, id, SandboxIdTT.TARGET);
    }

    /** Back-compat overload: no real build values, so the UA rewrite is skipped. */
    static void apply(ClassLoader cl, Map<String, String> id, String pkg) {
        apply(cl, id, pkg, null, null, null);
    }

    /**
     * @param pkg the package the hooks are being applied to. Part of the seed,
     *            because the native module seeds from
     *            {@code applog_seed(FINGERPRINT|SERIAL|ANDROID_ID|pkg)} — see
     *            {@code jni/module.cpp}. Using a package-less seed here made the
     *            two variants disagree: for the same persona the Zygisk module
     *            served one IMEI and this one served another, which is the
     *            opposite of what the class docstring promises and would show up
     *            as two different phones if both variants were ever installed
     *            together.
     * @param realRelease the platform's real {@code ro.build.version.release},
     *            captured before {@link PropHook} was installed. Null skips the
     *            User-Agent rewrite — a partial pair would be worse than none.
     * @param realModel the platform's real {@code ro.product.model}.
     * @param realId the platform's real {@code ro.build.id}.
     */
    static void apply(ClassLoader cl, Map<String, String> id, String pkg,
                      String realRelease, String realModel, String realId) {
        long seed = SeedId.applogSeed(id, pkg);
        // No fallback operator. A persona with no carrier configured must let
        // the IMSI/ICCID hooks fall through to the real values; substituting a
        // default here is what made the device claim Telkomsel 51010 while the
        // radio reported XL 51011. See SeedId.imsi().
        String mccmnc = id.get("GSM_OPERATOR_NUMERIC");
        if (mccmnc == null || mccmnc.isEmpty()) {
            // Persona has no carrier: use the one the modem actually reports, so
            // the synthesized IMSI/ICCID name the network the phone is on. This
            // mirrors install_java_hooks() in jni/module.cpp.
            mccmnc = realOperatorNumeric();
        }

        final String imei = SeedId.imei(seed);
        final String imsi = SeedId.imsi(seed, mccmnc);
        final String iccid = SeedId.iccid(seed, mccmnc);
        final String meid = SeedId.meid(seed);
        final byte[] widevine = hexToBytes(SeedId.widevineHex(seed));
        final String androidId = id.get("ANDROID_ID");
        final String serial = id.get("SERIAL");

        // Both MACs go through the same selector the native module uses, so a
        // persona's declared address always wins and the seed-derived fallback
        // is only for a persona that declares none. Case follows each platform
        // surface: WifiInfo.getMacAddress() is lowercase, while
        // BluetoothAdapter.getAddress() and Settings.Secure's bluetooth_address
        // row are uppercase.
        //
        // This used to read id.get("WIFI_MAC")/id.get("BLUETOOTH_ADDR") and pass
        // them straight through when non-null, which silently served a malformed
        // value and skipped the fallback entirely for a persona that had none.
        final String wifiMac = SeedId.pickMac(id.get("WIFI_MAC"), seed,
                0x5749464931L, false);                       // "WIFI1"
        final String btAddr  = SeedId.pickMac(id.get("BLUETOOTH_ADDR"), seed,
                0x424C554554L, true);                        // "BLUET"
        // The user-visible name. BLUETOOTH_NAME falls back to MODEL, exactly as
        // sync_device_name() does in rotate_ids.sh, so a persona that predates
        // the key still reports its model name instead of nothing.
        String btName = id.get("BLUETOOTH_NAME");
        if (btName == null || btName.isEmpty()) btName = id.get("MODEL");

        int n = 0;

        // ---- P0: the classic device identifiers ----

        // Settings.Secure.getString(contentResolver, key) — ONE method serving
        // every secure setting, so it takes a multi-key replacer rather than one
        // hook per key. Xposed, like LSPlant, refuses a second hook on a method
        // that already carries one, so separate hooks for android_id and
        // bluetooth_address would install only the first.
        //
        // bluetooth_address is the entry that matters most for Bluetooth: it
        // needs no permission, unlike BluetoothAdapter.getAddress() which throws
        // SecurityException without BLUETOOTH_CONNECT. Grab reads it directly in
        // Lc/g/zuo;->a() and returns the result verbatim, so an unhooked row
        // hands the app the real adapter address.
        java.util.LinkedHashMap<String, String> secure = new java.util.LinkedHashMap<>();
        secure.put("android_id", androidId);
        secure.put("bluetooth_address", btAddr);
        n += hook(cl, "android.provider.Settings$Secure", "getString",
                android.content.ContentResolver.class, String.class,
                new MultiKeyReplacer(1, secure));

        // Settings.Global.getString(contentResolver, key) — the user-visible
        // device name. sync_device_name() writes the same value to both rows.
        java.util.LinkedHashMap<String, String> global = new java.util.LinkedHashMap<>();
        global.put("bluetooth_name", btName);
        global.put("device_name", btName);
        n += hook(cl, "android.provider.Settings$Global", "getString",
                android.content.ContentResolver.class, String.class,
                new MultiKeyReplacer(1, global));

        // Build.getSerial()
        if (serial != null) {
            n += hook0(cl, "android.os.Build", "getSerial", serial);
        }

        // MediaDrm.getPropertyByteArray("deviceUniqueId")
        if (widevine != null) {
            n += hook(cl, "android.media.MediaDrm", "getPropertyByteArray",
                    String.class, new KeyedBytes(1, "deviceUniqueId", widevine));
        }

        // ---- P0b: WebView User-Agent ----
        //
        // The UA names the real phone in its "Android <release>; <model>
        // Build/<id>" run, and libwebviewchromium.so composes it from the
        // ro.build.* props. On this ROM that library cannot be PLT-hooked (its
        // segments are mapped from a shared_relro blob and an uncompressed APK
        // entry, neither of which starts with an ELF header), so the Java
        // surface is the only place left.
        //
        // Two hooks, exactly as the Zygisk module installs them:
        //   * WebSettings.getDefaultUserAgent(Context) — the framework entry
        //     point for the "…; wv) … Version/4.0 …" form;
        //   * X.04vW.LIZ(String) — the app's own cache writer. Rewriting its
        //     return fixes the read path AND the value the app re-persists,
        //     which a read-path-only fix cannot do for a populated cache. The
        //     class is per-build: X.04vW is musically, X.04v0 is trill (where
        //     X.04v0 is the writer and also has an unrelated LIZ(Context)V).
        //
        // The "from" half must be the REAL values. PropHook hooks
        // SystemProperties.get, so reading them back through it here would
        // return the persona and make from == to — a silent no-op. The native
        // side hit exactly this and had to add sbx_raw_prop_get; here the real
        // values are captured before PropHook is installed (see SandboxIdTT).
        String[] uaPair = SeedId.userAgentPair(realRelease, realModel, realId, id);
        if (uaPair != null) {
            for (String c : new String[] {
                    "android.webkit.WebSettings", "X.04vW", "X.04v0" }) {
                n += hookUa(cl, c, uaPair[0], uaPair[1]);
            }
        }

        // ---- P0c: screen geometry ----
        //
        // Screen size is the cheapest cross-check an SDK can make against
        // Build.MODEL, and the Zygisk module spoofs seven surfaces for it
        // (Resources.getDisplayMetrics, Display.getMetrics/getRealMetrics/
        // getSize/getRealSize/getWidth/getHeight, WindowMetrics.getBounds).
        // The LSPosed variant had NONE of them, so a persona claiming a Pixel 9
        // Pro XL still reported the real 1080x2400 panel.
        //
        // The values come from SeedId.screenGeometry(), the port of the
        // model->resolution table in module.cpp:450-503, so both variants
        // describe the same panel for the same persona.
        int[] geo = SeedId.screenGeometry(id);
        if (geo != null) {
            n += hookDisplayMetrics(cl, geo);
        }

        // ---- P0d: PackageManager cloaking ----
        //
        // Hides root/modding tooling and foreign-vendor ROM packages from the
        // app, while keeping the persona's OWN vendor visible. The Zygisk module
        // carries eight specs for this (four methods x two API shapes); the
        // LSPosed variant had none, so the app could see Magisk and LSPosed
        // installed on a device claiming to be a stock Pixel.
        //
        // Both directions matter: hiding everything would strip a Xiaomi
        // persona's own ROM packages, which is its own tell.
        n += hookPackageFilter(cl, id);

        // ---- P0d2: OEM class deny ----
        //
        // A Pixel cannot load miui.os.Build, and the app's OEM branches key off
        // exactly that: LX/00kR;->p2() turns "class loads" into the apm field
        // rom = "MIUI-" + INCREMENTAL, and LX/0D0h;->LIZ() turns it into
        // rom_version. Hiding the ro.miui.* props does not close this — both
        // helpers fall back to Runtime.exec("getprop") when SystemProperties
        // comes back empty, and a subprocess inherits none of our hooks.
        //
        // The Zygisk module denies these through its Class.forName observer
        // (M_OBSERVE, sbx_lsplant.hpp:1163-1165); the LSPosed variant had no
        // equivalent, so a Pixel persona on this Xiaomi device still reported
        // rom = "MIUI-…".
        n += hookDenyClasses(cl, SeedId.denyClasses(id));

        // ---- P0e: GSF id (gservices provider) ----
        //
        // Grab reads the GSF id by querying content://com.google.android.gsf.gservices
        // for the "android_id" row, then Long.parseLong(cursor.getString(1)).
        // On a device whose gservices table has no row (no Google account ever
        // signed in) the parse throws and Grab records gs:"er:103". The Zygisk
        // module answers that query with a synthesized cursor; the LSPosed
        // variant did not, so the id stayed absent.
        //
        // Gated hard: only the gservices URI, and only when "android_id" is
        // among the requested keys. Answering any other key with this row would
        // corrupt an unrelated read — Grab does query other keys through the
        // same provider.
        //
        // The id is DERIVED from the seed, not read from the persona. The native
        // module derives it the same way (module.cpp:447) and carries no
        // GSF_ID persona key at all, so reading one here would have left this
        // hook permanently uninstalled — id.get("GSF_ID") is null for every
        // persona PersonaGenerator can produce.
        n += hookGsfCursor(cl, SeedId.gsfId(seed));

        // ---- P0f: advertising id (Play services) ----
        //
        // Two hooks, and the second is the one that matters for an app that
        // bundles the client — exactly as the Zygisk module installs them
        // (sbx_lsplant.hpp:1125-1129).
        //
        // $Info.getId() only covers a caller that already obtained an Info,
        // i.e. one whose getAdvertisingIdInfo() succeeded. Grab ships its own
        // AdvertisingIdClient, and its getAdvertisingIdInfo calls b(false),
        // which binds to the Play-services advertising-id service and throws
        // java.io.IOException ("Google Play services not available",
        // "Connection failure") before any Info exists. The exception leaves
        // the method, getId() is never reached, and Grab's caller records
        // GENERAL_EXCEPTION (er:109) for both the adId and the flag. Hooking
        // the static entry point is what removes that.
        //
        // Both hooks are installed unconditionally and allowed to fail: the
        // classes live in the APP's DEX (Grab bundles them), so a build that
        // does not ship them simply misses and the hook count stays honest.
        n += hookGaid(cl, SeedId.gaid(seed));

        // ---- P1: TelephonyManager (all instance methods) ----
        String tel = "android.telephony.TelephonyManager";
        n += hook0(cl, tel, "getDeviceId", imei);
        n += hook(cl, tel, "getDeviceId", int.class, new Const<>(imei));
        n += hook0(cl, tel, "getImei", imei);
        n += hook(cl, tel, "getImei", int.class, new Const<>(imei));
        n += hook0(cl, tel, "getMeid", meid);
        n += hook(cl, tel, "getMeid", int.class, new Const<>(meid));
        // Installed only when the operator is known. An empty value would make
        // the hook a no-op that still costs a resolution, and — worse — a hook
        // that answers "" is not the same as no hook: getSubscriberId() on a
        // real device with no SIM returns null, not "".
        //
        // Both overloads, not just the no-arg one. The (int) form takes a subId
        // and is what a multi-SIM-aware caller uses; Grab's device-parameter
        // builder is one, and it recorded the REAL IMSI while every other
        // identifier on the same request was spoofed. See the matching comment
        // in jni/sbx_lsplant.hpp.
        if (!imsi.isEmpty()) {
            n += hook0(cl, tel, "getSubscriberId", imsi);
            n += hook(cl, tel, "getSubscriberId", int.class, new Const<>(imsi));
        }
        if (!iccid.isEmpty()) {
            n += hook0(cl, tel, "getSimSerialNumber", iccid);
            n += hook(cl, tel, "getSimSerialNumber", int.class, new Const<>(iccid));
        }

        String opNum = id.get("GSM_OPERATOR_NUMERIC");
        String opAlpha = id.get("GSM_OPERATOR_ALPHA");
        String opIso = id.get("GSM_OPERATOR_ISO");
        if (opNum != null && !opNum.isEmpty()) {
            n += hook0(cl, tel, "getNetworkOperator", opNum);
            n += hook(cl, tel, "getNetworkOperator", int.class, new Const<>(opNum));
            n += hook0(cl, tel, "getSimOperator", opNum);
            n += hook(cl, tel, "getSimOperator", int.class, new Const<>(opNum));
        }
        if (opAlpha != null && !opAlpha.isEmpty()) {
            n += hook0(cl, tel, "getNetworkOperatorName", opAlpha);
            n += hook0(cl, tel, "getSimOperatorName", opAlpha);
        }
        if (opIso != null && !opIso.isEmpty()) {
            n += hook0(cl, tel, "getSimCountryIso", opIso);
            n += hook0(cl, tel, "getNetworkCountryIso", opIso);
        }

        // ---- P1b: carrier id ----
        //
        // getSimCarrierId() answers a small integer naming the carrier, and it
        // is a DIFFERENT surface from getSimOperator(): Grab reads both, and
        // hooking only the numeric left the id describing the real SIM. There
        // is no safe synthetic value for it — -1 is the framework's own
        // "unknown", and 0 is a real carrier (T-Mobile US) — so it is installed
        // only when the persona (or carrier.conf) supplies one. Absent, the
        // hook falls through to the framework's own answer.
        String carrierId = id.get("GSM_CARRIER_ID");
        if (carrierId != null && !carrierId.isEmpty()) {
            // Integer, not String: getSimCarrierId() returns int, and a String
            // result would throw ClassCastException inside the framework. The
            // native spec expresses the same thing as ret_type=2, which makes
            // SandboxIDHook.spoofValue() do Integer.parseInt.
            try {
                n += hook(cl, tel, "getSimCarrierId",
                        new Const<>(Integer.valueOf(carrierId.trim())));
            } catch (NumberFormatException ignored) {
                // A malformed persona value must not install a hook that throws
                // on every call; falling through to the real id is the safe side.
            }
        }

        // ---- P1c: cell identity + subscription ----
        //
        // The MCC/MNC as the CELL and the SUBSCRIPTION report them. These are
        // different objects from TelephonyManager and different APIs from
        // getNetworkOperator(), and hooking only the TelephonyManager getters
        // left Grab's bare "mnc" field in pax.track.nk.measures.network_calls
        // reading the real network while every other carrier field on the same
        // event was spoofed.
        //
        // The string forms (getMccString/getMncString, API 28+) are installed
        // whenever an operator is known. The int forms are installed only then
        // too, because the int form of an unknown value is Integer.MAX_VALUE:
        // returning a made-up 0 would be a wrong answer rather than a missing
        // one. Mirrors the ordering and the reasoning in jni/sbx_lsplant.hpp.
        String opMcc = SeedId.operatorMcc(opNum);
        String opMnc = SeedId.operatorMnc(opNum);
        if (!opMcc.isEmpty()) {
            for (String cell : CELL_IDENTITY_CLASSES) {
                n += hook0(cl, cell, "getMccString", opMcc);
            }
            for (String cell : CELL_IDENTITY_INT_CLASSES) {
                n += hook(cl, cell, "getMcc", new Const<>(Integer.valueOf(opMcc)));
            }
        }
        if (!opMnc.isEmpty()) {
            for (String cell : CELL_IDENTITY_CLASSES) {
                n += hook0(cl, cell, "getMncString", opMnc);
            }
            for (String cell : CELL_IDENTITY_INT_CLASSES) {
                n += hook(cl, cell, "getMnc", new Const<>(Integer.valueOf(opMnc)));
            }
        }

        // ---- P1: MAC addresses ----
        //
        // Both are installed unconditionally now. They used to be gated on the
        // persona key being non-null, which left the REAL address served for a
        // persona that declared none - the same leak the seed-derived fallback
        // exists to prevent. pickMac() always returns a valid address.
        n += hook0(cl, "android.net.wifi.WifiInfo", "getMacAddress", wifiMac);
        n += hook0(cl, "android.bluetooth.BluetoothAdapter", "getAddress", btAddr);
        // The user-visible Bluetooth name, which would otherwise report the REAL
        // model beside a spoofed Build.MODEL.
        if (btName != null && !btName.isEmpty()) {
            n += hook0(cl, "android.bluetooth.BluetoothAdapter", "getName", btName);
        }

        Log.d(TAG, "Telephony/DRM hooks: " + n + " method(s) installed"
                + (mccmnc == null || mccmnc.isEmpty()
                        ? " (no operator configured: IMSI/ICCID pass through"
                          + " to the real values)" : ""));
    }

    // ------------------------------------------------------------------

    /**
     * The device's real MCC+MNC, read from the property service.
     *
     * <p>Used when the persona carries no operator, so the synthesized
     * IMSI/ICCID describe the network the phone is actually on instead of a
     * made-up one. Read through {@code android.os.SystemProperties.get} — the
     * same surface {@link PropHook} hooks — which means a persona that DOES set
     * GSM_OPERATOR_NUMERIC never reaches this path (the caller passes its own
     * value), and a persona that does not gets the real one.
     *
     * <p>The GSM props are per-slot and comma-joined: a single-SIM device still
     * writes the separator, so {@code gsm.sim.operator.numeric} returns
     * {@code "51011,"}. TelephonyManager reports the slot's value without it, so
     * the comma has to go here or {@link SeedId#isOperatorNumeric} rejects a
     * string that is actually valid.
     *
     * <p>Reflection, not a direct call: {@code android.os.SystemProperties} is a
     * hidden API, and this class is compiled against the SDK stub where it does
     * not exist. Fail-soft — any failure yields null, which makes the caller
     * leave the IMSI/ICCID hooks uninstalled.
     */
    private static String realOperatorNumeric() {
        String[] keys = {"gsm.sim.operator.numeric", "gsm.operator.numeric"};
        for (String key : keys) {
            try {
                Class<?> sp = Class.forName("android.os.SystemProperties");
                java.lang.reflect.Method get = sp.getMethod("get", String.class);
                Object v = get.invoke(null, key);
                if (v == null) continue;
                String s = v.toString();
                int comma = s.indexOf(',');
                if (comma >= 0) s = s.substring(0, comma);
                s = s.trim();
                if (SeedId.isOperatorNumeric(s)) return s;
            } catch (Throwable ignored) {
                // Hidden API missing or blocked on this ROM — try the next key.
            }
        }
        return null;
    }

    // ------------------------------------------------------------------

    /**
     * Returns the spoof value when {@code args[keyIdx]} names one of
     * {@code values}, and leaves the call alone otherwise.
     *
     * <p>One hook, many keys. Xposed — like LSPlant on the Zygisk side — refuses
     * a second hook on a method that already carries one, so
     * {@code Settings.Secure.getString(ContentResolver, String)} can only be
     * hooked once no matter how many settings it serves. A per-key hook would
     * install the first and silently drop the rest, which is how
     * {@code bluetooth_address} stayed real.
     *
     * <p>A null or empty mapped value falls through to the real method rather
     * than answering {@code ""}: a device with no value set returns null, and
     * {@code ""} is not the same thing.
     */
    private static final class MultiKeyReplacer extends XC_MethodHook {
        private final int keyIdx;
        private final Map<String, String> values;

        MultiKeyReplacer(int keyIdx, Map<String, String> values) {
            this.keyIdx = keyIdx;
            this.values = values;
        }

        @Override
        protected void beforeHookedMethod(MethodHookParam p) {
            if (values == null || values.isEmpty()) return;
            if (keyIdx < 0 || keyIdx >= p.args.length) return;
            Object a = p.args[keyIdx];
            if (a == null) return;
            String v = values.get(a.toString());
            if (v != null && !v.isEmpty()) p.setResult(v);
        }
    }

    /** Same, for byte[] returns. */
    private static final class KeyedBytes extends XC_MethodHook {
        private final int keyIdx;
        private final String keyMatch;
        private final byte[] value;

        KeyedBytes(int keyIdx, String keyMatch, byte[] value) {
            this.keyIdx = keyIdx;
            this.keyMatch = keyMatch;
            this.value = value;
        }

        @Override
        protected void beforeHookedMethod(MethodHookParam p) {
            if (keyIdx < 0 || keyIdx >= p.args.length) return;
            Object a = p.args[keyIdx];
            if (a != null && keyMatch.equals(a.toString()) && value != null) {
                p.setResult(value);
            }
        }
    }

    /** Unconditional replacement. */
    private static final class Const<T> extends XC_MethodHook {
        private final T value;

        Const(T value) { this.value = value; }

        @Override
        protected void beforeHookedMethod(MethodHookParam p) {
            if (value != null) p.setResult(value);
        }
    }

    private static int hook0(ClassLoader cl, String cls, String name, final Object value) {
        try {
            XposedHelpers.findAndHookMethod(cls, cl, name, new Const<>(value));
            return 1;
        } catch (Throwable t) {
            // Method hidden/removed on this API level — skip, keep going.
            return 0;
        }
    }

    /** No-parameter overload, for e.g. {@code TelephonyManager.getSimCarrierId()}. */
    private static int hook(ClassLoader cl, String cls, String name, XC_MethodHook cb) {
        try {
            XposedHelpers.findAndHookMethod(cls, cl, name, cb);
            return 1;
        } catch (Throwable t) {
            return 0;
        }
    }

    /**
     * Installs the User-Agent rewrite on one class, hooking whichever of the
     * two known shapes it declares.
     *
     * <p>{@code android.webkit.WebSettings.getDefaultUserAgent(Context)} is the
     * framework entry point; {@code X.04vW.LIZ(String)} / {@code X.04v0.LIZ(
     * String)} is the app's cache writer. Both return a String and take either
     * one Context or one String, so the shape is tried per class rather than
     * assumed. A class that declares neither is simply skipped — the other
     * hook still covers its path.
     */
    private static int hookUa(ClassLoader cl, String cls,
                              final String from, final String to) {
        XC_MethodHook cb = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam p) {
                Object r = p.getResult();
                if (!(r instanceof String)) return;
                String s = (String) r;
                if (s.isEmpty() || !s.contains(from)) return;
                p.setResult(s.replace(from, to));
            }
        };
        int n = 0;
        try {
            XposedHelpers.findAndHookMethod(cls, cl, "getDefaultUserAgent",
                    android.content.Context.class, cb);
            n++;
        } catch (Throwable ignored) {
            // Not the framework class, or the method is gone on this API level.
        }
        try {
            XposedHelpers.findAndHookMethod(cls, cl, "LIZ", String.class, cb);
            n++;
        } catch (Throwable ignored) {
            // Not the app-side writer.
        }
        return n;
    }

    private static int hook(ClassLoader cl, String cls, String name,
                            Class<?> p1, XC_MethodHook cb) {
        try {
            XposedHelpers.findAndHookMethod(cls, cl, name, p1, cb);
            return 1;
        } catch (Throwable t) {
            return 0;
        }
    }

    /** Two-parameter overload, for e.g. {@code Settings.Secure.getString(
     *  ContentResolver, String)}. */
    private static int hook(ClassLoader cl, String cls, String name,
                            Class<?> p1, Class<?> p2, XC_MethodHook cb) {
        try {
            XposedHelpers.findAndHookMethod(cls, cl, name, p1, p2, cb);
            return 1;
        } catch (Throwable t) {
            return 0;
        }
    }

    /**
     * Installs the screen-geometry hooks, mirroring the seven specs the Zygisk
     * module carries for this surface.
     *
     * <p>Three shapes, because the framework uses three:
     * <ul>
     *   <li>{@code Resources.getDisplayMetrics()} returns a {@code DisplayMetrics}
     *       — fields are overwritten in place;</li>
     *   <li>{@code Display.getMetrics/getRealMetrics(DisplayMetrics)} and
     *       {@code getSize/getRealSize(Point)} fill an out-parameter and return
     *       void — the argument is overwritten;</li>
     *   <li>{@code Display.getWidth/getHeight()} and
     *       {@code WindowMetrics.getBounds()} return a value — the result is
     *       replaced.</li>
     * </ul>
     *
     * <p>Rotation is honoured for the two scalar getters: a display whose
     * rotation is 1 or 3 is landscape, so width and height swap. The native
     * side does the same (SandboxIDHook.java:448-468); without it a landscape
     * read would report a portrait panel and contradict the orientation the
     * framework just reported.
     */
    private static int hookDisplayMetrics(ClassLoader cl, final int[] geo) {
        final int w = geo[0], h = geo[1], dpi = geo[2];
        final int xdpi = geo[3], ydpi = geo[4];

        /**
         * Writes the persona's geometry into a DisplayMetrics instance or a
         * Point, honouring the orientation the object ALREADY carries.
         *
         * <p>The swap is the part that is easy to miss. The framework fills an
         * out-parameter with the live panel's dimensions and only then hands it
         * to us, so a landscape {@code getSize(Point)} arrives holding
         * width > height. Writing the persona's portrait pair into it would
         * leave the caller reading a portrait panel while the display is
         * rotated — a contradiction inside one call. The native side swaps in
         * exactly this case (SandboxIDHook.java:1053-1058, 1084-1088); doing it
         * from the object's own current shape means it works for every caller
         * without needing the Display.
         */
        XC_MethodHook fillMetrics = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam p) {
                Object r = p.getResult();
                if (r != null) {
                    String cn = r.getClass().getName();
                    // Copy before overwriting. Resources.getConfiguration()
                    // returns the Resources' OWN Configuration instance and
                    // Resources.getDisplayMetrics() may hand back a cached one;
                    // writing the persona's geometry into either would corrupt
                    // the app's real resource state for every later caller, not
                    // just this one. The native side copies for the same reason
                    // (SandboxIDHook.java:486-511).
                    if (cn.endsWith("DisplayMetrics")) {
                        Object c = copyMetrics(r);
                        applyMetrics(c != null ? c : r, w, h, dpi, xdpi, ydpi);
                        if (c != null) p.setResult(c);
                    } else if (cn.endsWith("Configuration")) {
                        Object c = copyConfiguration(r);
                        applyConfiguration(c != null ? c : r, w, h, dpi);
                        if (c != null) p.setResult(c);
                    } else if (cn.endsWith("Point")) {
                        Object c = copyPoint(r);
                        applyMetrics(c != null ? c : r, w, h, dpi, xdpi, ydpi);
                        if (c != null) p.setResult(c);
                    }
                }
                // The out-parameter form: getMetrics(DisplayMetrics),
                // getSize(Point). The caller owns that object and expects it
                // filled, so it is written in place — no copy.
                if (p.args != null && p.args.length > 0 && p.args[0] != null) {
                    applyMetrics(p.args[0], w, h, dpi, xdpi, ydpi);
                }
            }
        };

        int n = 0;
        // Resources.getDisplayMetrics() — returns a DisplayMetrics.
        n += hook(cl, "android.content.res.Resources", "getDisplayMetrics", fillMetrics);
        // Resources.getConfiguration() — returns a Configuration, which carries
        // screenWidthDp/screenHeightDp/densityDpi. A separate surface from
        // DisplayMetrics and the one an app uses to size its layout, so leaving
        // it alone reports the real panel's dp geometry beside a spoofed pixel
        // geometry. The native module hooks it (sbx_lsplant.hpp:1204-1206).
        n += hook(cl, "android.content.res.Resources", "getConfiguration", fillMetrics);
        // Display.getMetrics(DisplayMetrics) / getRealMetrics(DisplayMetrics).
        n += hook(cl, "android.view.Display", "getMetrics",
                android.util.DisplayMetrics.class, fillMetrics);
        n += hook(cl, "android.view.Display", "getRealMetrics",
                android.util.DisplayMetrics.class, fillMetrics);
        // Display.getSize(Point) / getRealSize(Point).
        n += hook(cl, "android.view.Display", "getSize",
                android.graphics.Point.class, fillMetrics);
        n += hook(cl, "android.view.Display", "getRealSize",
                android.graphics.Point.class, fillMetrics);

        // Display.getWidth() / getHeight() — scalars, orientation-aware.
        XC_MethodHook scalar = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam p) {
                boolean landscape = isLandscape(p.thisObject);
                boolean wantWidth = "getWidth".equals(p.method.getName());
                int min = Math.min(w, h), max = Math.max(w, h);
                int v = wantWidth ? (landscape ? max : min)
                                  : (landscape ? min : max);
                p.setResult(Integer.valueOf(v));
            }
        };
        try {
            XposedHelpers.findAndHookMethod("android.view.Display", cl,
                    "getWidth", scalar);
            n++;
        } catch (Throwable ignored) { }
        try {
            XposedHelpers.findAndHookMethod("android.view.Display", cl,
                    "getHeight", scalar);
            n++;
        } catch (Throwable ignored) { }

        // WindowMetrics.getBounds() -> Rect.
        XC_MethodHook bounds = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam p) {
                Object r = p.getResult();
                if (r != null && r.getClass().getName().endsWith("Rect")) {
                    applyRect(r, w, h);
                }
            }
        };
        try {
            XposedHelpers.findAndHookMethod("android.view.WindowMetrics", cl,
                    "getBounds", bounds);
            n++;
        } catch (Throwable ignored) { }

        return n;
    }

    /** True when the Display reports a landscape rotation (1 or 3). */
    private static boolean isLandscape(Object display) {
        if (display == null) return false;
        try {
            Object r = display.getClass().getMethod("getRotation").invoke(display);
            if (r instanceof Integer) {
                int rot = (Integer) r;
                return rot == 1 || rot == 3;
            }
        } catch (Throwable ignored) { }
        return false;
    }

    /**
     * Overwrites the geometry fields of a {@code DisplayMetrics} or a
     * {@code Point}, swapping the pair when the object is already landscape.
     *
     * <p>Both carry public {@code width}/{@code height} (Point: {@code x}/
     * {@code y}); only DisplayMetrics has the density fields, so each is set
     * independently and a missing field is simply skipped.
     *
     * <p>The swap reads the object's CURRENT shape rather than the Display's
     * rotation. For an out-parameter that is the more reliable signal — the
     * framework has just filled it — and it needs no Display reference, so the
     * same helper serves {@code Resources.getDisplayMetrics()} too.
     */
    private static void applyMetrics(Object o, int w, int h, int dpi,
                                     int xdpi, int ydpi) {
        if (o == null) return;
        int curW = intField(o, "widthPixels");
        int curH = intField(o, "heightPixels");
        if (curW == 0 && curH == 0) {          // Point uses x/y
            curW = intField(o, "x");
            curH = intField(o, "y");
        }
        int tw = w, th = h;
        if (curW > curH && tw < th) {
            tw = h;
            th = w;
        }
        setIntField(o, "widthPixels", tw);
        setIntField(o, "heightPixels", th);
        setIntField(o, "densityDpi", dpi);
        setIntField(o, "x", tw);      // Point
        setIntField(o, "y", th);      // Point
        setFloatField(o, "density", dpi / 160.0f);
        setFloatField(o, "scaledDensity", dpi / 160.0f);
        setFloatField(o, "xdpi", xdpi);
        setFloatField(o, "ydpi", ydpi);
    }

    /**
     * Writes the persona's geometry into a {@code Configuration}.
     *
     * <p>Configuration carries dp units, not pixels, so the conversion is
     * {@code px / (densityDpi / 160)} — the same arithmetic as
     * {@code applyConfiguration()} in {@code jni/SandboxIDHook.java:1112-1146}.
     * {@code smallestScreenWidthDp} is always the PORTRAIT short side, which is
     * what makes it "smallest": it must not change when the device rotates.
     */
    private static void applyConfiguration(Object cfg, int w, int h, int dpi) {
        if (cfg == null || dpi <= 0) return;
        int curSw = intField(cfg, "screenWidthDp");
        int curSh = intField(cfg, "screenHeightDp");
        boolean land = curSw > curSh && curSh > 0;

        float d = dpi / 160.0f;
        int minDim = Math.min(w, h), maxDim = Math.max(w, h);
        int portSwDp = (int) (minDim / d);
        int portShDp = (int) (maxDim / d);

        setIntField(cfg, "densityDpi", dpi);
        setIntField(cfg, "screenWidthDp", land ? portShDp : portSwDp);
        setIntField(cfg, "screenHeightDp", land ? portSwDp : portShDp);
        setIntField(cfg, "smallestScreenWidthDp", portSwDp);
    }

    private static int intField(Object o, String name) {
        try {
            java.lang.reflect.Field f = o.getClass().getField(name);
            return f.getInt(o);
        } catch (Throwable ignored) {
            return 0;
        }
    }

    /**
     * A fresh {@code DisplayMetrics} holding {@code src}'s values, or null when
     * the class cannot be constructed.
     *
     * <p>{@code DisplayMetrics.setTo(DisplayMetrics)} is the framework's own
     * copy, and it exists on every API level this module supports. Falling back
     * to null makes the caller write in place, which is the pre-copy behaviour
     * rather than a crash.
     */
    private static Object copyMetrics(Object src) {
        try {
            Class<?> c = src.getClass();
            Object copy = c.getConstructor().newInstance();
            c.getMethod("setTo", c).invoke(copy, src);
            return copy;
        } catch (Throwable ignored) {
            return null;
        }
    }

    /** A fresh {@code Configuration} copied from {@code src}, or null. */
    private static Object copyConfiguration(Object src) {
        try {
            Class<?> c = src.getClass();
            return c.getConstructor(c).newInstance(src);
        } catch (Throwable ignored) {
            return null;
        }
    }

    /** A fresh {@code Point} copied from {@code src}, or null. */
    private static Object copyPoint(Object src) {
        try {
            Class<?> c = src.getClass();
            Object copy = c.getConstructor(int.class, int.class)
                    .newInstance(Integer.valueOf(intField(src, "x")),
                                 Integer.valueOf(intField(src, "y")));
            return copy;
        } catch (Throwable ignored) {
            return null;
        }
    }

    /**
     * Overwrites a {@code Rect} to (0,0,w,h), swapping when the Rect is already
     * landscape — the same rule and the same reason as {@link #applyMetrics}.
     *
     * <p>{@code WindowMetrics.getBounds()} returns the window bounds for the
     * CURRENT orientation, so a landscape window arrives wider than tall.
     */
    private static void applyRect(Object o, int w, int h) {
        int curW = intField(o, "right") - intField(o, "left");
        int curH = intField(o, "bottom") - intField(o, "top");
        int tw = w, th = h;
        if (curW > curH && tw < th) {
            tw = h;
            th = w;
        }
        setIntField(o, "left", 0);
        setIntField(o, "top", 0);
        setIntField(o, "right", tw);
        setIntField(o, "bottom", th);
    }

    private static void setIntField(Object o, String name, int v) {
        try {
            java.lang.reflect.Field f = o.getClass().getField(name);
            f.setInt(o, v);
        } catch (Throwable ignored) { }
    }

    private static void setFloatField(Object o, String name, float v) {
        try {
            java.lang.reflect.Field f = o.getClass().getField(name);
            f.setFloat(o, v);
        } catch (Throwable ignored) { }
    }

    /**
     * Installs the PackageManager cloaking hooks, mirroring the eight specs the
     * Zygisk module carries.
     *
     * <p>Two shapes: the list methods ({@code getInstalledPackages},
     * {@code getInstalledApplications}) return a {@code List} that is filtered,
     * and the single-item methods ({@code getPackageInfo},
     * {@code getApplicationInfo}) throw {@code NameNotFoundException} for a
     * hidden package — the same answer the framework gives for a package that
     * is not installed, so the app cannot tell cloaking from absence.
     *
     * <p>Both the int-flags and the API-33 flags-object overloads are hooked:
     * the latter is what a modern caller uses, and hooking only the int form
     * left the list unfiltered for it.
     */
    private static int hookPackageFilter(ClassLoader cl, Map<String, String> id) {
        final String[] vendorKeep = SeedId.vendorKeep(id);
        final String cls = "android.app.ApplicationPackageManager";
        int n = 0;

        XC_MethodHook listFilter = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam p) {
                Object r = p.getResult();
                if (!(r instanceof java.util.List)) return;
                java.util.List<?> src = (java.util.List<?>) r;
                java.util.List<Object> out = new java.util.ArrayList<>(src.size());
                for (Object item : src) {
                    if (item == null) continue;
                    String pkg = packageNameOf(item);
                    if (pkg != null && SeedId.isHiddenPackage(pkg, vendorKeep)) continue;
                    out.add(item);
                }
                p.setResult(out);
            }
        };

        XC_MethodHook singleFilter = new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam p) {
                if (p.args == null || p.args.length == 0) return;
                Object a = p.args[0];
                if (!(a instanceof String)) return;
                if (!SeedId.isHiddenPackage((String) a, vendorKeep)) return;
                // Throw exactly what the framework throws for an absent
                // package, so cloaking is indistinguishable from not installed.
                //
                // setThrowable, not a throw: XposedBridge catches anything the
                // callback throws, logs it, and lets the original method run —
                // so the first version of this hook logged a
                // NameNotFoundException for every root package and then
                // returned the package anyway. Cloaking never worked.
                try {
                    Class<?> ex = Class.forName(
                            "android.content.pm.PackageManager$NameNotFoundException");
                    p.setThrowable((Throwable) ex.getConstructor(String.class)
                            .newInstance(a));
                } catch (Throwable t) {
                    // Cannot build the framework's own exception: let the real
                    // method answer rather than invent a different failure.
                }
            }
        };

        n += hook(cl, cls, "getInstalledPackages", int.class, listFilter);
        n += hook(cl, cls, "getInstalledApplications", int.class, listFilter);
        n += hook(cl, cls, "getPackageInfo", String.class, int.class, singleFilter);
        n += hook(cl, cls, "getApplicationInfo", String.class, int.class, singleFilter);

        // API 33+ flags-object overloads. Looked up by name so an older
        // framework simply skips them. The flags type differs per method:
        // getInstalledPackages takes PackageInfoFlags, getInstalledApplications
        // takes ApplicationInfoFlags.
        n += hookFlags(cl, cls, "getInstalledPackages",
                "android.content.pm.PackageManager$PackageInfoFlags", listFilter);
        n += hookFlags(cl, cls, "getInstalledApplications",
                "android.content.pm.PackageManager$ApplicationInfoFlags", listFilter);
        n += hookFlagsSingle(cl, cls, "getPackageInfo",
                "android.content.pm.PackageManager$PackageInfoFlags", singleFilter);
        n += hookFlagsSingle(cl, cls, "getApplicationInfo",
                "android.content.pm.PackageManager$ApplicationInfoFlags", singleFilter);

        // AccessibilityManager — the same hidden set has to cover the
        // accessibility list, or the app sees a service whose package it was
        // just told does not exist. Grab reads this list
        // (getInstalledAccessibilityServiceList, reached through c.g.joy1.b in
        // classes.dex) and records every service id in its scan result, so a
        // hidden modding tool still showed up as "bin.mt.plus.canary/l.…"
        // while getInstalledPackages denied it.
        //
        // AccessibilityServiceInfo has no packageName field; listFilter's
        // packageNameOf() reads getId() for that shape.
        final String am = "android.view.accessibility.AccessibilityManager";
        n += hook(cl, am, "getInstalledAccessibilityServiceList", listFilter);
        n += hook(cl, am, "getEnabledAccessibilityServiceList", int.class, listFilter);

        return n;
    }

    /** {@code getInstalledX(PackageManager.PackageInfoFlags)} shape. */
    /**
     * {@code getInstalledX(PackageManager.<X>InfoFlags)} shape.
     *
     * <p>The flags type is a parameter, not fixed: {@code getInstalledPackages}
     * takes {@code PackageInfoFlags} while {@code getInstalledApplications}
     * takes {@code ApplicationInfoFlags}. Hardcoding one for both made the
     * second lookup fail every time — {@code findAndHookMethod} cannot match a
     * signature that does not exist — so {@code getInstalledApplications(flags)}
     * silently stayed unhooked while the code counted the attempt as a miss.
     * The native specs get this right by naming each type separately
     * ({@code sbx_lsplant.hpp:1230-1240}).
     */
    private static int hookFlags(ClassLoader cl, String cls, String name,
                                 String flagsClass, XC_MethodHook cb) {
        try {
            Class<?> flags = XposedHelpers.findClass(flagsClass, cl);
            XposedHelpers.findAndHookMethod(cls, cl, name, flags, cb);
            return 1;
        } catch (Throwable ignored) {
            return 0;
        }
    }

    /**
     * {@code getX(String, PackageManager.<X>InfoFlags)} shape.
     *
     * <p>Same per-method type split as {@link #hookFlags}:
     * {@code getPackageInfo} takes {@code PackageInfoFlags},
     * {@code getApplicationInfo} takes {@code ApplicationInfoFlags}. Using one
     * type for both made the {@code getPackageInfo} lookup fail every time, so
     * a hidden package was still returned when the caller used the API 33
     * overload — the one modern code actually calls.
     */
    private static int hookFlagsSingle(ClassLoader cl, String cls, String name,
                                       String flagsClass, XC_MethodHook cb) {
        try {
            Class<?> flags = XposedHelpers.findClass(flagsClass, cl);
            XposedHelpers.findAndHookMethod(cls, cl, name, String.class, flags, cb);
            return 1;
        } catch (Throwable ignored) {
            return 0;
        }
    }

    /**
     * The package an item in a PackageManager/AccessibilityManager list belongs
     * to, or null when neither shape applies.
     *
     * <p>Two shapes reach this, and both are needed. {@code PackageInfo} and
     * {@code ApplicationInfo} carry a public {@code packageName} field.
     * {@code AccessibilityServiceInfo} does not — it names its owner only
     * through {@code getId()}, which is "pkg/.Service" or
     * "pkg/full.Class.Name". Without the second shape, hiding bin.mt.plus from
     * {@code getInstalledPackages} left its accessibility service fully visible
     * in {@code getInstalledAccessibilityServiceList}, so the app saw a service
     * whose package it had just been told does not exist. Grab reads exactly
     * that list and records every service id in its scan result.
     *
     * <p>Port of {@code isHiddenItem()} in {@code jni/SandboxIDHook.java:883-899}.
     */
    private static String packageNameOf(Object item) {
        try {
            java.lang.reflect.Field f = item.getClass().getField("packageName");
            Object v = f.get(item);
            if (v instanceof String) return (String) v;
        } catch (Throwable ignored) { }
        try {
            java.lang.reflect.Method m = item.getClass().getMethod("getId");
            Object v = m.invoke(item);
            if (v instanceof String) {
                String id = (String) v;
                int cut = id.indexOf('/');
                if (cut > 0) return id.substring(0, cut);
            }
        } catch (Throwable ignored) { }
        return null;
    }

    /**
     * Answers the gservices {@code android_id} query with a synthesized cursor.
     *
     * <p>Port of {@code gsfCursor()} in {@code jni/SandboxIDHook.java:784-810}.
     * The provider's idiom is that {@code selectionArgs} ARE the keys being
     * looked up, so the hook answers only when {@code "android_id"} is among
     * them and lets every other query through. A null or empty selectionArgs
     * means "every row", which this hook cannot honestly serve.
     *
     * <p>Arg indices: Xposed omits the receiver, so the declared parameters
     * {@code (Uri, projection, selection, selectionArgs, sortOrder)} land at
     * args[0..4] — selectionArgs at 3. The native side uses index 4 because its
     * args[] includes the receiver.
     */
    private static int hookGsfCursor(ClassLoader cl, final String gsfId) {
        // ContentResolver declares three query overloads. Verified against the
        // device's framework.jar (API 35, /data/local/tmp/fwx):
        //
        //   query(Uri,String[],String,String[],String)
        //   query(Uri,String[],String,String[],String,CancellationSignal)
        //   query(Uri,String[],Bundle,CancellationSignal)
        //
        // The first two put the selection args at args[3]. The Bundle form puts
        // them under "android:query-arg-sql-selection-args" inside args[2].
        // Hooking only the 5-arg form left the other two answering from the real
        // provider, so a caller that passed a CancellationSignal — or used the
        // Bundle API, which is what anything targeting API 26+ is encouraged to
        // do — would read the real GSF id even with this hook installed.
        final XC_MethodHook cb = new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam p) {
                if (p.args == null || p.args.length < 5) return;
                Object uri = p.args[0];
                if (uri == null) return;
                if (!String.valueOf(uri).equals(GSF_URI)) return;

                if (!GsfAnswer.wantsGsfId(p.args[3])) return;

                Object cursor = GsfAnswer.newGsfCursor(cl, gsfId);
                if (cursor != null) p.setResult(cursor);
            }
        };

        // The Bundle overload. Separate callback because the selection args are
        // not at args[3] — args[3] is the CancellationSignal there.
        final XC_MethodHook bundleCb = new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam p) {
                if (p.args == null || p.args.length < 3) return;
                Object uri = p.args[0];
                if (uri == null) return;
                if (!String.valueOf(uri).equals(GSF_URI)) return;

                Object q = p.args[2];
                if (!(q instanceof android.os.Bundle)) return;
                Object sel = ((android.os.Bundle) q).get(QUERY_ARG_SELECTION_ARGS);
                if (!GsfAnswer.wantsGsfId(sel)) return;

                Object cursor = GsfAnswer.newGsfCursor(cl, gsfId);
                if (cursor != null) p.setResult(cursor);
            }
        };

        int n = 0;
        // The 5-arg form. Grab uses this one, so it is the load-bearing hook.
        try {
            XposedHelpers.findAndHookMethod("android.content.ContentResolver", cl,
                    "query", android.net.Uri.class, String[].class,
                    String.class, String[].class, String.class, cb);
            n++;
        } catch (Throwable ignored) { }
        // The 6-arg form with a CancellationSignal.
        try {
            XposedHelpers.findAndHookMethod("android.content.ContentResolver", cl,
                    "query", android.net.Uri.class, String[].class,
                    String.class, String[].class, String.class,
                    android.os.CancellationSignal.class, cb);
            n++;
        } catch (Throwable ignored) { }
        // The Bundle form (API 26+).
        try {
            XposedHelpers.findAndHookMethod("android.content.ContentResolver", cl,
                    "query", android.net.Uri.class, String[].class,
                    android.os.Bundle.class,
                    android.os.CancellationSignal.class, bundleCb);
            n++;
        } catch (Throwable ignored) { }
        return n;
    }

    // The gate and the cursor builder live in GsfAnswer: they are the two
    // pieces that decide whether this hook answers at all and what it hands
    // back, and neither needs a ContentResolver to exercise. The overload
    // wiring in hookGsfCursor() does, so it stays here and is checked on a
    // device.
    private static final String GSF_URI = GsfAnswer.GSF_URI;
    private static final String QUERY_ARG_SELECTION_ARGS =
            GsfAnswer.QUERY_ARG_SELECTION_ARGS;

    /**
     * Installs the two advertising-id hooks, mirroring the Zygisk module's
     * {@code M_VALUE}/{@code M_GAID_INFO} pair.
     *
     * <p>The second hook is the load-bearing one. {@code AdvertisingIdClient}
     * lives in Grab's own DEX, so {@code Class.forName} from this callback
     * would resolve against the module's loader and miss it. The Info class is
     * therefore taken from the hooked method's return type:
     * {@code p.method.getReturnType()} was resolved by the app's loader, which
     * is the loader that can see it. The native side does the same thing from
     * {@code backup.getReturnType()} (SandboxIDHook.java:731-753).
     *
     * <p>Returning null would make Grab dereference a null Info and crash, and
     * letting the original IOException escape is the er:109 this replaces — so
     * a failure to build the Info falls through to the original, which is the
     * pre-fix behaviour rather than a new one.
     *
     * <p>The limit-tracking flag is always {@code false}: a real device that has
     * not opted out reports false, and Grab records the flag as "f"/"t". An
     * exception there is a far louder tell than either value.
     */
    private static int hookGaid(ClassLoader cl, final String gaid) {
        int n = 0;

        // AdvertisingIdClient$Info.getId() — covers a caller that already has
        // an Info.
        n += hook(cl, "com.google.android.gms.ads.identifier.AdvertisingIdClient$Info",
                "getId", new Const<>(gaid));

        // AdvertisingIdClient.getAdvertisingIdInfo(Context) — the static entry
        // point. Replaces the whole call, so the bind that would throw never
        // runs.
        try {
            XposedHelpers.findAndHookMethod(
                    "com.google.android.gms.ads.identifier.AdvertisingIdClient", cl,
                    "getAdvertisingIdInfo", android.content.Context.class,
                    new XC_MethodHook() {
                        @Override
                        protected void beforeHookedMethod(MethodHookParam p) {
                            // Replace the whole call, so the bind that would
                            // throw never runs. Same semantics as the native
                            // gaidInfo(): build the Info when possible, else
                            // leave the original alone.
                            Object info = buildInfo(p.method, gaid);
                            if (info != null) p.setResult(info);
                        }
                    });
            n++;
        } catch (Throwable ignored) {
            // Class absent from this build — the getId() hook still stands.
        }
        return n;
    }

    /**
     * Builds an {@code AdvertisingIdClient$Info} for the return type of
     * {@code m}, or null when the type cannot be resolved or constructed.
     *
     * <p>{@code m} is the hooked method, whose return type the app's own loader
     * resolved. That is the only loader that can see the class: Grab bundles
     * it, so it is not on the module's path.
     */
    private static Object buildInfo(java.lang.reflect.Member m, String gaid) {
        try {
            Class<?> infoCls = ((java.lang.reflect.Method) m).getReturnType();
            if (infoCls == null || infoCls == Object.class) return null;
            java.lang.reflect.Constructor<?> c =
                    infoCls.getConstructor(String.class, boolean.class);
            return c.newInstance(gaid, Boolean.FALSE);
        } catch (Throwable ignored) {
            return null;
        }
    }

    /**
     * Makes {@code Class.forName} reject the persona's foreign OEM classes.
     *
     * <p>Hooks the <b>3-arg</b> overload, not the 1-arg one. {@code forName(
     * String)} is {@code @CallerSensitive} and resolves through
     * {@code Reflection.getCallerClass()}, which from inside this callback
     * would see the module's own class rather than the app — every by-name load
     * would then resolve against the wrong loader. The 3-arg overload takes the
     * loader as an argument and passes it straight to {@code classForName}, so
     * it is free of that hazard. Same reasoning as the native spec at
     * {@code sbx_lsplant.hpp:1155-1165}.
     *
     * <p>Throwing {@code ClassNotFoundException} is what a device that genuinely
     * lacks the class does, so the app's OEM branch takes its "not this ROM"
     * path rather than seeing an anomaly.
     *
     * <p><b>{@code setThrowable}, not a throw.</b> XposedBridge wraps every
     * callback in {@code catch (Throwable t) { log(t); param.setResult(null);
     * param.returnEarly = false; continue; }} — an exception thrown from
     * {@code beforeHookedMethod} is swallowed, and the real method then runs as
     * if nothing happened. The first version of this hook did exactly that, so
     * it logged a ClassNotFoundException on every OEM probe and still let
     * {@code miui.os.Build} load. {@code setThrowable} is the API that makes the
     * hooked call itself fail. See {@code XposedBridge.handleHookedMethod}.
     *
     * <p>Arg index 0: Xposed omits the receiver, and the method is static, so
     * the class name is the first argument.
     */
    private static int hookDenyClasses(ClassLoader cl, final String[] deny) {
        if (deny == null || deny.length == 0) return 0;
        try {
            XposedHelpers.findAndHookMethod("java.lang.Class", cl, "forName",
                    String.class, boolean.class, ClassLoader.class,
                    new XC_MethodHook() {
                        @Override
                        protected void beforeHookedMethod(MethodHookParam p) {
                            if (p.args == null || p.args.length < 1) return;
                            Object a = p.args[0];
                            if (!(a instanceof String)) return;
                            String want = (String) a;
                            for (String d : deny) {
                                if (want.equals(d)) {
                                    p.setThrowable(new ClassNotFoundException(want));
                                    return;
                                }
                            }
                        }
                    });
            return 1;
        } catch (Throwable t) {
            return 0;
        }
    }

    private static byte[] hexToBytes(String hex) {
        if (hex == null || hex.length() % 2 != 0) return null;
        byte[] out = new byte[hex.length() / 2];
        for (int i = 0; i < out.length; i++) {
            int hi = Character.digit(hex.charAt(i * 2), 16);
            int lo = Character.digit(hex.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) return null;
            out[i] = (byte) ((hi << 4) | lo);
        }
        return out;
    }
}
