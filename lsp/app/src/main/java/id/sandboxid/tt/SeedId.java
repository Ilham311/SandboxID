package id.sandboxid.tt;

import java.util.Map;

/**
 * Java port of the module's deterministic seed primitives and telephony/DRM
 * identifier synthesis.
 *
 * <p>Everything here is a pure function of a seed derived from
 * {@code fnv1a(FINGERPRINT + "|" + SERIAL + "|" + ANDROID_ID)} — the same string
 * the C++ side hashes — so a persona produces the same IMEI/IMSI/ICCID/MEID here
 * and in the native module. No randomness: the same persona always yields the
 * same identifiers, which is what keeps a rotated persona internally consistent.
 *
 * <p>Sources ported verbatim:
 * <ul>
 *   <li>{@code fnv1a}, {@code splitmix64}, {@code hex_from_seed} —
 *       {@code native/native_read.cpp}</li>
 *   <li>{@code synth_imei/imsi/iccid/meid}, {@code luhn_check_digit} — the
 *       module's former Java-method hook layer ({@code sbx_ident_synth.hpp},
 *       removed in the layout cleanup but restored here for this variant)</li>
 * </ul>
 */
final class SeedId {

    private static final long MASK = 0xFFFFFFFFL;

    private SeedId() {}

    /**
     * FNV-1a 64. Exactly native_read.cpp:5-9.
     *
     * <p>Hashes the string's <b>UTF-8 bytes</b>, as C++ does over
     * {@code std::string} — not its Java {@code char}s. The difference matters:
     * truncating each {@code char} to its low byte ({@code & 0xFF}) agrees with
     * C++ only for pure-ASCII input, and diverges for any non-ASCII fingerprint
     * (e.g. "caf&eacute;" or CJK), producing a different seed and therefore a
     * different IMEI/IMSI for the same persona. The current persona pool is
     * pure ASCII, so the bug would be invisible until it isn't.
     */
    static long fnv1a(String s) {
        byte[] b;
        try {
            b = s.getBytes("UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            b = s.getBytes();   // device default; UTF-8 is guaranteed on Android
        }
        long h = 1469598103934665603L;
        for (byte x : b) {
            h ^= (x & 0xFFL);
            h *= 1099511628211L;
        }
        return h;
    }

    /**
     * SplitMix64. Exactly native_read.cpp:11-17. Mutates the state array slot.
     *
     * <p><b>The output uses all 64 bits, so roughly half of all calls return a
     * negative Java {@code long}.</b> C++ sees the same bit pattern as an
     * {@code uint64_t} and every consumer below treats it as unsigned. Any Java
     * consumer that must match the C++ value — an array index, a decimal digit,
     * a modulo — has to go through {@link Long#remainderUnsigned}, or it gets a
     * negative index ({@link ArrayIndexOutOfBoundsException}, crashing the hook)
     * or a digit below {@code '0'} (garbage characters inside a synthesised
     * IMEI/IMSI/ICCID). {@link #snowflake} and {@link #hexFromSeed} are immune:
     * the former prints via {@link Long#toUnsignedString}, the latter masks
     * whole bytes with {@code >>>}.
     */
    static long splitmix64(long[] st, int slot) {
        st[slot] += 0x9E3779B97F4A7C15L;
        long z = st[slot];
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    /** hex_from_seed: lowercase hex, nbytes bytes. Exactly native_read.cpp:67-82. */
    static String hexFromSeed(long seed, int nbytes) {
        StringBuilder sb = new StringBuilder(nbytes * 2);
        long[] st = {seed};
        int i = 0;
        while (i < nbytes) {
            long v = splitmix64(st, 0);
            for (int b = 0; b < 8 && i < nbytes; b++, i++) {
                int by = (int) ((v >>> (b * 8)) & 0xFF);
                sb.append(Character.forDigit(by >>> 4, 16));
                sb.append(Character.forDigit(by & 0xF, 16));
            }
        }
        return sb.toString();
    }

    // There is deliberately no package-less "persona seed" here. One used to
    // exist and TelephonyDrmHook seeded every identifier from it, which made
    // this variant and the Zygisk module disagree: the native side seeds from
    // applog_seed() — fnv1a(FINGERPRINT|SERIAL|ANDROID_ID|pkg) — so for the same
    // persona the two produced different IMEIs and different MAC fallbacks.
    // Nothing in the native module hashes the package-less form, so a helper
    // that did was a trap rather than a primitive. Use applogSeed().

    /**
     * AppLog seed — the ONLY seed the native module has.
     * {@code fnv1a(FINGERPRINT|SERIAL|ANDROID_ID|pkg)}, exactly
     * native_read.hpp:118-123. Every synthesized identifier in this variant —
     * IMEI/IMSI/ICCID/MEID/Widevine and the MAC fallbacks — must come from this
     * one, or the two variants describe two different phones.
     */
    static long applogSeed(Map<String, String> id, String pkg) {
        String fp = or(id, "FINGERPRINT", "");
        String serial = or(id, "SERIAL", "");
        String aid = or(id, "ANDROID_ID", "");
        return fnv1a(fp + "|" + serial + "|" + aid + "|" + pkg);
    }

    /**
     * The MAC address to serve for one surface: the persona's own value when it
     * declares a valid one, otherwise a seed-derived fallback.
     *
     * <p>Port of {@code sbxid::pick_mac()} in {@code jni/sbx_ident_synth.hpp},
     * so the Zygisk and LSPosed variants report the same address for the same
     * persona. The two must not drift: an app that reads the address through
     * both lenses would otherwise see two different phones.
     *
     * <p>A malformed persona value is treated exactly like an absent one.
     * Serving {@code "not-a-mac"} from {@code getAddress()} is an obvious tell,
     * and serving the REAL address is worse than inventing a plausible one — so
     * the fallback is always locally administered ({@link #macFromSeed} forces
     * the {@code 02} prefix) and can never collide with a real vendor OUI.
     *
     * @param upper the platform's own convention for this surface:
     *              {@code WifiInfo.getMacAddress()} is lowercase, while
     *              {@code BluetoothAdapter.getAddress()} and
     *              {@code Settings.Secure}'s {@code bluetooth_address} row are
     *              uppercase. Matching the real form makes the spoofed value
     *              indistinguishable in shape from the one it replaces.
     */
    static String pickMac(String persona, long seed, long salt, boolean upper) {
        String m = isValidMac(persona) ? persona : macFromSeed(seed ^ salt);
        return upper ? m.toUpperCase() : m;
    }

    /**
     * Exactly {@code sbxnr::mac_from_seed()}: six little-endian bytes from
     * SplitMix64, with the first forced to {@code 0x02} (locally administered,
     * unicast). Lowercase, colon-separated.
     */
    static String macFromSeed(long seed) {
        byte[] b = fillBytes(seed, 6);
        b[0] = 0x02;
        StringBuilder sb = new StringBuilder(17);
        for (int i = 0; i < 6; i++) {
            if (i > 0) sb.append(':');
            sb.append(Character.forDigit((b[i] >>> 4) & 0xF, 16));
            sb.append(Character.forDigit(b[i] & 0xF, 16));
        }
        return sb.toString();
    }

    /**
     * Exactly {@code sbxnr::is_valid_mac()}: 17 chars, {@code :} at 2/5/8/11/14,
     * hex elsewhere, and not all-zero. Case-insensitive, because the persona
     * file and the platform surfaces disagree on case and both are valid MACs.
     */
    static boolean isValidMac(String m) {
        if (m == null || m.length() != 17) return false;
        boolean allZero = true;
        for (int i = 0; i < 17; i++) {
            char c = m.charAt(i);
            if ((i % 3) == 2) {
                if (c != ':') return false;
            } else {
                boolean hex = (c >= '0' && c <= '9')
                        || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!hex) return false;
                if (c != '0') allZero = false;
            }
        }
        return !allZero;
    }

    // ------------------------------------------------------------------

    static String imei(long seed) {
        String[] TAC = {
                "35161511", "35316010", "35404911", "35847313",
                "35692211", "35876554", "35291612", "35438110",
        };
        long[] st = {seed ^ 0x494D4549L};                     // "IMEI"
        int tac = (int) Long.remainderUnsigned(
                splitmix64(st, 0), TAC.length);
        StringBuilder d = new StringBuilder(TAC[tac]);
        appendDigits(d, st, 6);                               // SNR
        d.append(luhnCheck(d.toString()));                    // 15th digit
        return d.toString();
    }

    /**
     * IMSI for {@code mccmnc}, or <b>empty</b> when the operator is unknown.
     *
     * <p>The empty return is the whole point. This used to substitute
     * {@code "51010"} (Telkomsel) for any operator it could not parse, which
     * made every persona claim a network the device was not attached to: the
     * test SIM is XL {@code 51011}, and an app that reads the MNC from the radio
     * (a surface no hook here covers) recorded {@code "mnc":"11"} beside a
     * spoofed Telkomsel name in the same event. A caller that gets an empty
     * string must skip the hook so the getter falls through to the real value,
     * exactly as the native {@code synth_imsi()} does.
     *
     * <p>A malformed operator is rejected rather than repaired: {@code "51011,"}
     * is what {@code getprop gsm.sim.operator.numeric} returns on a single-SIM
     * device (the props are per-slot and comma-joined), and the caller is
     * responsible for stripping that separator before calling this.
     */
    static String imsi(long seed, String mccmnc) {
        if (!isOperatorNumeric(mccmnc)) return "";
        long[] st = {seed ^ 0x494D5349L};                     // "IMSI"
        StringBuilder d = new StringBuilder(mccmnc);
        appendDigits(d, st, 15 - mccmnc.length());
        return d.toString();
    }

    /**
     * MCC+MNC is 5 or 6 digits and nothing else. Shared by {@link #imsi} and
     * {@link #iccid} so the two cannot drift into accepting different inputs.
     */
    static boolean isOperatorNumeric(String mccmnc) {
        if (mccmnc == null) return false;
        if (mccmnc.length() < 5 || mccmnc.length() > 6) return false;
        for (int i = 0; i < mccmnc.length(); i++) {
            char c = mccmnc.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    /**
     * The MCC half of {@code mccmnc}, or empty when it is not a valid numeric.
     * Exactly {@code sbxid::operator_mcc()} in sbx_ident_synth.hpp:130-134.
     */
    static String operatorMcc(String mccmnc) {
        return isOperatorNumeric(mccmnc) ? mccmnc.substring(0, 3) : "";
    }

    /**
     * The MNC half of {@code mccmnc}, or empty when it is not a valid numeric.
     * Exactly {@code sbxid::operator_mnc()} in sbx_ident_synth.hpp:136-140.
     *
     * <p>Two or three digits, depending on the MCC: the split is positional
     * (first three are the MCC), not a fixed 2/3 guess, so an MNC like {@code 11}
     * and one like {@code 260} both come out right.
     */
    static String operatorMnc(String mccmnc) {
        return isOperatorNumeric(mccmnc) ? mccmnc.substring(3) : "";
    }

    /**
     * The device-identifying segment of a WebView User-Agent, or empty when it
     * cannot be built.
     *
     * <p>The UA arrives as
     * {@code Mozilla/5.0 (Linux; Android <release>; <model> Build/<id>; wv) ...}
     * and the {@code Android <release>; <model> Build/<id>} run is the part that
     * names the real phone. This builds the two halves of a literal
     * {@code replace(from, to)} so the caller can rewrite whatever the platform
     * produced without re-deriving the surrounding boilerplate.
     *
     * <p>Mirrors {@code module.cpp:377-386}. The native side reads the "from"
     * half through {@code sbx_raw_prop_get} — a plain
     * {@code __system_property_get} — because by the time it runs, the module's
     * own prop hook already answers the persona's values, which made
     * {@code from == to} and turned the rewrite into a silent no-op. The Java
     * variant has the same hazard: {@link PropHook} hooks
     * {@code SystemProperties.get}, so the caller must pass in the values it
     * read through a path that hook does not cover.
     *
     * <p>Both halves are required. If either is missing the caller must skip
     * the rewrite entirely rather than substitute a partial segment.
     *
     * @param realRelease the platform's real {@code ro.build.version.release}
     * @param realModel   the platform's real {@code ro.product.model}
     * @param realId      the platform's real {@code ro.build.id}
     * @param persona     the persona map
     * @return {@code {from, to}}, or {@code null} if either half is incomplete
     */
    static String[] userAgentPair(String realRelease, String realModel,
                                  String realId, Map<String, String> persona) {
        if (isEmpty(realRelease) || isEmpty(realModel) || isEmpty(realId)) return null;
        if (persona == null) return null;
        String prel = persona.get("RELEASE");
        String pmodel = persona.get("MODEL");
        String pid = persona.get("ID");
        if (isEmpty(prel) || isEmpty(pmodel) || isEmpty(pid)) return null;
        return new String[] {
                "Android " + realRelease + "; " + realModel + " Build/" + realId,
                "Android " + prel + "; " + pmodel + " Build/" + pid,
        };
    }

    private static boolean isEmpty(String s) {
        return s == null || s.isEmpty();
    }

    /**
     * Screen geometry for a persona's {@code MODEL}, or null when the persona
     * supplies no usable override and the model is not in the table.
     *
     * <p>Port of the resolution table in {@code jni/module.cpp:450-503}. The
     * table is deliberately small and Pixel-only: it covers the models the
     * bundled {@code devices.tsv} actually generates. Anything else falls back
     * to 1080x2400/420dpi — the same default native uses — so the value is
     * always plausible for the model the persona claims.
     *
     * <p>Why this matters: screen size is one of the cheapest cross-checks an
     * SDK can make against {@code Build.MODEL}. A persona claiming a Pixel 9
     * Pro XL while {@code Display.getRealSize()} reports the real 1080x2400 is
     * a contradiction that needs no privileged access to notice.
     *
     * <p>Persona keys win over the table, exactly as in {@code module.cpp}:
     * {@code SCREEN_WIDTH}, {@code SCREEN_HEIGHT}, {@code LCD_DENSITY},
     * {@code SCREEN_XDPI}, {@code SCREEN_YDPI}, each applied only when positive.
     *
     * @return {@code {width, height, density, xdpi, ydpi}}, or null if the
     *         persona has no MODEL at all (nothing to be consistent with)
     */
    static int[] screenGeometry(Map<String, String> id) {
        if (id == null) return null;
        String model = id.get("MODEL");
        if (isEmpty(model)) return null;

        int w, h, d, xd, yd;
        if (model.contains("Pixel 9 Pro XL"))      { w = 1344; h = 2992; d = 560; xd = 486; yd = 486; }
        else if (model.contains("Pixel 9 Pro Fold")) { w = 1080; h = 2424; d = 420; xd = 422; yd = 422; }
        else if (model.contains("Pixel 9 Pro"))    { w = 1280; h = 2856; d = 560; xd = 495; yd = 495; }
        else if (model.contains("Pixel 9"))        { w = 1080; h = 2424; d = 420; xd = 422; yd = 422; }
        else if (model.contains("Pixel 8 Pro"))    { w = 1344; h = 2992; d = 560; xd = 489; yd = 489; }
        else if (model.contains("Pixel 8"))        { w = 1080; h = 2400; d = 420; xd = 428; yd = 428; }
        else if (model.contains("Pixel 7 Pro"))    { w = 1440; h = 3120; d = 560; xd = 512; yd = 512; }
        else if (model.contains("Pixel 7"))        { w = 1080; h = 2400; d = 420; xd = 416; yd = 416; }
        else if (model.contains("Pixel 6 Pro"))    { w = 1440; h = 3120; d = 560; xd = 512; yd = 512; }
        else if (model.contains("Pixel 6"))        { w = 1080; h = 2400; d = 420; xd = 411; yd = 411; }
        else                                       { w = 1080; h = 2400; d = 420; xd = 420; yd = 420; }

        // Persona overrides, each only when it parses to a positive number. A
        // zero or malformed value must not replace a good table entry.
        int ow = positiveInt(id.get("SCREEN_WIDTH"));
        if (ow > 0) w = ow;
        int oh = positiveInt(id.get("SCREEN_HEIGHT"));
        if (oh > 0) h = oh;
        int od = positiveInt(id.get("LCD_DENSITY"));
        if (od > 0) d = od;
        int ox = positiveInt(id.get("SCREEN_XDPI"));
        if (ox > 0) xd = ox;
        int oy = positiveInt(id.get("SCREEN_YDPI"));
        if (oy > 0) yd = oy;

        return new int[] { w, h, d, xd, yd };
    }

    /** {@code Integer.parseInt} that yields 0 for null/blank/malformed. */
    private static int positiveInt(String s) {
        if (isEmpty(s)) return 0;
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    static String meid(long seed) {
        return hexFromSeed(seed ^ 0x4D454944L, 7).toUpperCase();   // 14 hex
    }

    static String widevineHex(long seed) {
        return hexFromSeed(seed ^ 0x57565F4944L, 32);              // "WV_ID"
    }

    /**
     * ICCID for {@code mccmnc}, or <b>empty</b> when the operator is unknown.
     *
     * <p>Same rule and same reason as {@link #imsi}: the country-code half comes
     * from the MCC, so without one the result would describe a SIM from a
     * country the device is not in. A device with no SIM reports null from
     * {@code getSimSerialNumber()}, and an empty return lets the hook fall
     * through to reproduce exactly that.
     */
    static String iccid(long seed, String mccmnc) {
        if (!isOperatorNumeric(mccmnc)) return "";
        long[] st = {seed ^ 0x49434349L};                     // "ICCI"
        StringBuilder d = new StringBuilder("89");
        d.append(ccForMcc(mccmnc));
        appendDigits(d, st, 18 - d.length());
        d.append(luhnCheck(d.toString()));
        return d.toString();
    }

    /** ITU-T E.118 country code for the SIM's MCC (best-effort; default 01). */
    private static String ccForMcc(String mccmnc) {
        if (mccmnc.startsWith("510")) return "62";        // Indonesia
        if (mccmnc.startsWith("310") || mccmnc.startsWith("311")) return "1";  // USA
        if (mccmnc.startsWith("505")) return "61";        // Australia
        if (mccmnc.startsWith("262")) return "49";        // Germany
        if (mccmnc.startsWith("234") || mccmnc.startsWith("235")) return "44"; // UK
        return "01";
    }

    private static void appendDigits(StringBuilder d, long[] st, int count) {
        for (int i = 0; i < count; i++) {
            d.append((char) ('0' + Long.remainderUnsigned(
                    splitmix64(st, 0), 10)));
        }
    }

    /** Luhn check digit over a payload, doubled from the rightmost payload digit. */
    private static char luhnCheck(String payload) {
        int sum = 0;
        boolean dbl = true;
        for (int i = payload.length() - 1; i >= 0; i--) {
            int n = payload.charAt(i) - '0';
            if (dbl) {
                n *= 2;
                if (n > 9) n -= 9;
            }
            sum += n;
            dbl = !dbl;
        }
        return (char) ('0' + ((10 - (sum % 10)) % 10));
    }

    // ------------------------------------------------------------------
    // AppLog identifiers — port of make_applog_ids() (native_read.cpp:90-99)
    // ------------------------------------------------------------------

    /** The six ByteDance AppLog IDs, all derived from one seed. */
    static final class ApplogIds {
        final String did, iid, ssid, cdid, clientudid, openudid;

        ApplogIds(String did, String iid, String ssid, String cdid,
                  String clientudid, String openudid) {
            this.did = did;
            this.iid = iid;
            this.ssid = ssid;
            this.cdid = cdid;
            this.clientudid = clientudid;
            this.openudid = openudid;
        }
    }

    /**
     * Exactly {@code make_applog_ids()} in native_read.cpp:110-133.
     *
     * <p>Two properties are load-bearing and easy to lose:
     *
     * <ul>
     *   <li><b>Every</b> field mixes in {@code epochMs}, including the three
     *       that were once frozen. The register server binds
     *       (openudid, install_id cookie) permanently on first sight, so a
     *       frozen openudid would make {@code rotate_ids.sh applog} rewrite
     *       did/iid/ssid and then silently re-bind the new registration to the
     *       old did.
     *   <li>Each id has its own salt, so two ids can never collapse onto the
     *       same stream. {@code did} and {@code clientudid} once shared one.
     * </ul>
     *
     * <p>This must stay byte-identical to the native module: the LSPosed
     * variant writes these into the persona ({@code PERSONA_OPENUDID},
     * {@code PERSONA_CLIENTUDID}, {@code PERSONA_CDID}), and the Zygisk
     * variant serves the same persona. If they diverge, the same persona
     * registers as two different devices depending on which variant is
     * installed.
     */
    static ApplogIds applogIds(long seed, long epochMs) {
        return new ApplogIds(
                snowflake(seed ^ 0x9E3779B97F4A7C15L, epochMs),      // did
                snowflake(seed ^ 0xBF58476D1CE4E5B9L, epochMs),      // iid
                snowflake(seed ^ 0x94D049BB133111EBL, epochMs),      // ssid
                uuid(seed ^ 0x2545F4914F6CDD1DL ^ epochMs),          // cdid
                uuid(seed ^ 0x6A09E667F3BCC909L ^ epochMs),          // clientudid
                hexFromSeed(seed ^ 0xBB67AE8584CAA73BL ^ epochMs, 8)); // openudid (16 hex)
    }

    /**
     * Snowflake: {@code (epoch_ms << 22) | (splitmix64(seed) & 0x3FFFFF)},
     * printed as an UNSIGNED decimal. The shift overflows Java's signed long
     * for any real epoch (~7.1e21 at 1.7e12 ms), so {@link Long#toUnsignedString}
     * is mandatory — {@code Long.toString} would emit a negative device id.
     * Exactly native_read.cpp:83-88.
     */
    private static String snowflake(long seed, long epochMs) {
        long v = (epochMs << 22) | (splitmix64(new long[] {seed}, 0) & 0x3FFFFFL);
        return Long.toUnsignedString(v);
    }

    /** UUID v4, lowercase, dashes before indices 4/6/8/10. Exactly :29-42. */
    private static String uuid(long seed) {
        byte[] b = fillBytes(seed, 16);
        b[6] = (byte) ((b[6] & 0x0F) | 0x40);
        b[8] = (byte) ((b[8] & 0x3F) | 0x80);
        StringBuilder sb = new StringBuilder(36);
        for (int i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) sb.append('-');
            sb.append(Character.forDigit((b[i] >>> 4) & 0xF, 16));
            sb.append(Character.forDigit(b[i] & 0xF, 16));
        }
        return sb.toString();
    }

    /**
     * Draws bytes little-endian from successive SplitMix64 outputs. Exactly
     * fill_bytes() :19-27 — the same byte order as hex_from_seed, so the two
     * must agree or openudid diverges from the native module.
     */
    private static byte[] fillBytes(long seed, int n) {
        byte[] out = new byte[n];
        long[] st = {seed};
        int i = 0;
        while (i < n) {
            long v = splitmix64(st, 0);
            for (int b = 0; b < 8 && i < n; b++, i++) {
                out[i] = (byte) ((v >>> (b * 8)) & 0xFF);
            }
        }
        return out;
    }

    /** {@code id.get(k)} with a default when the key is absent. */
    private static String or(Map<String, String> id, String k, String def) {
        String v = id.get(k);
        return (v == null) ? def : v;
    }

    // ---- PackageManager cloaking -------------------------------------------
    //
    // Port of HIDDEN_PACKAGES / isHiddenPackage() / isVendorKept() in
    // jni/SandboxIDHook.java:138-207, plus the vendor_keep carve-out built in
    // module.cpp:415-431.
    //
    // Two directions, and a variant needs both or it is worse than neither:
    //   * hide root/modding tooling, so the app cannot see that the phone is
    //     rooted or that a hooking framework is installed;
    //   * KEEP the persona's own vendor packages, because a persona claiming a
    //     Pixel that still reports com.miui.* installed is a contradiction the
    //     app can read straight off getInstalledPackages().

    /** Root, modding, and foreign-vendor packages to hide from the app. */
    static final String[] HIDDEN_PACKAGES = {
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
            "com.miui",
            "com.milink.service",
            "com.xiaomi",
            "com.mi.health",
            "com.mipay.wallet",
            "com.android.thememanager",
            "com.oplus",
            "com.coloros",
            "com.oneplus",
            "com.realme",
            "com.microsoft.appmanager",
            "com.fufufu.katrina.backup",
    };

    /**
     * The vendor prefixes this persona is allowed to keep, from its BRAND.
     *
     * <p>Exactly the carve-out {@code module.cpp:415-431} builds: a Xiaomi/
     * Redmi/POCO persona keeps the MIUI packages, an OPPO/OnePlus/realme
     * persona keeps the ColorOS ones, and everybody else keeps none. Returning
     * an empty array is correct for a Pixel persona — it should see no vendor
     * packages at all.
     */
    static String[] vendorKeep(Map<String, String> id) {
        String brand = id == null ? null : id.get("BRAND");
        if (brand == null) return new String[0];
        if (brand.equals("Xiaomi") || brand.equals("Redmi") || brand.equals("POCO")) {
            return new String[] {
                    "com.miui", "com.milink.service", "com.xiaomi",
                    "com.mi.health", "com.mipay.wallet", "com.android.thememanager",
            };
        }
        if (brand.equals("OPPO") || brand.equals("OnePlus") || brand.equals("realme")) {
            return new String[] {
                    "com.oplus", "com.coloros", "com.oneplus", "com.realme",
            };
        }
        return new String[0];
    }

    /**
     * True when {@code pkg} must be hidden from the app.
     *
     * <p>Case-insensitive, and a prefix match on a dot boundary so
     * {@code com.miui.home} is covered by {@code com.miui} without
     * {@code com.miuix} being caught. A package the persona's own vendor keeps
     * is never hidden, even though its prefix is in the list.
     */
    static boolean isHiddenPackage(String pkg, String[] vendorKeep) {
        if (pkg == null) return false;
        String lower = pkg.toLowerCase(java.util.Locale.US);
        for (String h : HIDDEN_PACKAGES) {
            if (!lower.equals(h) && !lower.startsWith(h + ".")) continue;
            if (isVendorKept(lower, vendorKeep)) continue;
            return true;
        }
        return false;
    }

    private static boolean isVendorKept(String lower, String[] vendorKeep) {
        if (vendorKeep == null) return false;
        for (String k : vendorKeep) {
            if (k == null || k.isEmpty()) continue;
            if (lower.equals(k) || lower.startsWith(k + ".")) return true;
        }
        return false;
    }
}
