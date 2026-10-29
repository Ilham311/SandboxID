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
