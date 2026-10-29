package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedHelpers;

/**
 * Telephony and DRM identifiers — the layer the Zygisk module does NOT cover.
 *
 * <p>The Zygisk side spoofs {@code Build.*}, {@code SystemProperties} and file
 * reads, but never hooks {@link android.telephony.TelephonyManager},
 * {@link android.media.MediaDrm} or {@link android.net.wifi.WifiInfo}: those are
 * the module's former LSPlant layer (removed in the source-layout cleanup as
 * "unused experimental", because it was never built). That layer's hook spec is
 * restored here.
 *
 * <p>Values are deterministic from the persona seed ({@link SeedId}), so a given
 * persona always yields the same IMEI/IMSI/ICCID/MEID — a rotated persona stays
 * internally consistent, and the same persona reproduces the same identifiers
 * across the Zygisk and LSPosed variants.
 *
 * <p><b>Fail-soft, by design.</b> Every hook is resolved individually; a method
 * hidden or removed on the running API level simply fails resolution and is
 * skipped. One unavailable method must not disable the rest — mirrors the C++
 * loop's behaviour.
 */
final class TelephonyDrmHook {

    private static final String TAG = "SandboxID-TT";

    private TelephonyDrmHook() {}

    static void apply(ClassLoader cl, Map<String, String> id) {
        long seed = SeedId.personaSeed(id);
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

        int n = 0;

        // ---- P0: the classic device identifiers ----

        // Settings.Secure.getString(contentResolver, "android_id")
        n += hook(cl, "android.provider.Settings$Secure", "getString",
                android.content.ContentResolver.class, String.class,
                new KeyedReplacer(1, "android_id", androidId));

        // Build.getSerial()
        if (serial != null) {
            n += hook0(cl, "android.os.Build", "getSerial", serial);
        }

        // MediaDrm.getPropertyByteArray("deviceUniqueId")
        if (widevine != null) {
            n += hook(cl, "android.media.MediaDrm", "getPropertyByteArray",
                    String.class, new KeyedBytes(1, "deviceUniqueId", widevine));
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

        // ---- P1: MAC addresses ----
        String wifi = id.get("WIFI_MAC");
        if (wifi != null) {
            n += hook0(cl, "android.net.wifi.WifiInfo", "getMacAddress",
                    wifi.toUpperCase());
        }
        String bt = id.get("BLUETOOTH_ADDR");
        if (bt != null) {
            n += hook0(cl, "android.bluetooth.BluetoothAdapter", "getAddress",
                    bt.toUpperCase());
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

    /** Returns the spoof value only when args[keyIdx] equals keyMatch. */
    private static final class KeyedReplacer extends XC_MethodHook {
        private final int keyIdx;
        private final String keyMatch;
        private final String value;

        KeyedReplacer(int keyIdx, String keyMatch, String value) {
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
