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
     * The four salts are the SplitMix64 increment and the two output mix
     * constants — exactly native_read.cpp:90-99. Note {@code did} and
     * {@code clientudid} share a salt but take different formatters
     * (snowflake vs UUID), and {@code openudid} uses the UN-XORed seed.
     */
    static ApplogIds applogIds(long seed, long epochMs) {
        return new ApplogIds(
                snowflake(seed ^ 0x9E3779B97F4A7C15L, epochMs),   // did
                snowflake(seed ^ 0xBF58476D1CE4E5B9L, epochMs),   // iid
                snowflake(seed ^ 0x94D049BB133111EBL, epochMs),   // ssid
                uuid(seed ^ 0x2545F4914F6CDD1DL),                 // cdid
                uuid(seed ^ 0x9E3779B97F4A7C15L),                 // clientudid
                hexFromSeed(seed, 8));                            // openudid (16 hex)
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
}
