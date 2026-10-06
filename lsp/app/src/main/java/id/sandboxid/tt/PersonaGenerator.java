package id.sandboxid.tt;

import android.content.Context;
import android.content.res.AssetManager;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Random;

/**
 * Generates a complete persona in pure Java — the LSPosed variant's replacement
 * for the root-side {@code autopif.sh device} + {@code sandboxid freshen} pair.
 *
 * <p><b>Why this class exists.</b> The earlier revision of this module read a
 * persona that the <i>Zygisk module's</i> root side had to publish. That made
 * this variant a dependent add-on rather than a standalone module. This class
 * is what removes that dependency: the whole persona lifecycle — device pick,
 * derived fields, identifiers — is produced here, in the module's own app
 * process, with no root and no other module installed.
 *
 * <p><b>One pool, one generator.</b> The root side has <i>three</i> generators
 * ({@code autopif.sh device} over {@code devices.tsv}, native {@code freshen}
 * over {@code personas.tsv}, and {@code autopif.sh fetch}) that recompute ~13 of
 * the same keys with <i>different formulas</i> over two overlapping device lists
 * in incompatible column orders. The shell side even carries a comment warning
 * the two drift. This port does not replicate that: it uses only
 * {@code devices.tsv} (40 real devices, 8 brands — bundled as an asset, since
 * the module cannot read {@code /data/adb}), with one formula per key.
 *
 * <p><b>Which side each divergent key follows</b> (chosen deliberately, not at
 * random):
 * <ul>
 *   <li><b>VBMETA_DIGEST</b> — native: deterministic
 *       {@code hex_from_seed(fnv1a(FINGERPRINT + "|" + SERIAL), 32)}. The shell
 *       side uses pure random hex, which <i>orphans every derived identifier
 *       from the seed</i> (IMEI/IMSI/ICCID/MEID/Widevine and all AppLog IDs key
 *       off that hash). Random here would break internal consistency for no
 *       benefit, so the native formula wins.</li>
 *   <li><b>BUILD_TIME_UTC</b> — native: includes the
 *       {@code + digitSum % 37} seconds term. The shell port omits it, so the
 *       two are already unequal; the native one is the one the runtime hooks
 *       were written against.</li>
 *   <li><b>SERIAL</b> — native: uppercase. {@code Build.getSerial()} on real
 *       Pixel hardware returns uppercase, and {@code TelephonyDrmHook} serves
 *       this value directly, so uppercase is the internally consistent choice.</li>
 *   <li><b>USER</b> — native: {@code android-build}. The shell's "builder" is a
 *       value no stock image uses.</li>
 *   <li><b>HOST</b> — native: the 9-prefix table. The shell's
 *       {@code <brand>-build-N} leaks the persona's brand into a field where
 *       real images use build-farm hostnames.</li>
 *   <li><b>RADIO</b> — native: Tensor modem synthesis; absent on other
 *       platforms, exactly as real devices report it.</li>
 *   <li><b>MARKETNAME</b> — native: falls back to MODEL when the column is
 *       empty, so the UI label is never blank.</li>
 * </ul>
 *
 * <p><b>Lifecycle keys are generated here too</b> — the native {@code freshen}
 * emits none of them ({@code BOOT_COUNT}, {@code UPTIME_*}, {@code AGE_DAYS}…).
 * The shell's {@code gen_lifecycle} is the only source, so it is ported here
 * (the native side has no equivalent to copy). {@code UPTIME_SECONDS} starts at
 * <b>0</b> by default, matching the module's shipped default: uptime spoofing is
 * opt-in there ({@code enable_uptime} present, {@code no_uptime} absent) and is
 * force-rewritten to 0 per spawn otherwise. Keeping 0 means this variant is
 * honest by default and changes nothing the persona does not claim.
 *
 * <p>Randomness comes from {@link SecureRandom} — the direct equivalent of the
 * root side's {@code /dev/urandom} / {@code std::random_device}. Only the
 * identifier-synthesis functions ({@link SeedId}) are deliberately deterministic.
 */
final class PersonaGenerator {

    private static final String DEVICES_ASSET = "devices.tsv";

    /** Column indices in devices.tsv (1-based, as documented in the file header). */
    private static final int C_BRAND = 1, C_MANUFACTURER = 2, C_MARKETNAME = 3,
            C_MODEL = 4, C_DEVICE = 5, C_PRODUCT = 6, C_BOARD = 7,
            C_SOC_MANUFACTURER = 8, C_SOC_MODEL = 9, C_SDK = 10,
            C_RELEASE = 11, C_BUILD_ID = 12, C_INCREMENTAL = 13,
            C_SECURITY_PATCH = 14, C_RELEASE_DATE = 15;

    private static final Random RNG = new SecureRandom();

    private PersonaGenerator() {}

    /**
     * @return a fresh persona, or null if the bundled device pool cannot be read
     *         (which would be a packaging bug — the asset is in the APK).
     */
    static Map<String, String> generate(Context ctx) {
        List<String[]> pool = loadPool(ctx);
        if (pool.isEmpty()) return null;

        String[] row = pick(pool);
        Map<String, String> id = new LinkedHashMap<>();

        // --- the Build surface, straight from the device row ---
        put(id, "BRAND", col(row, C_BRAND));
        put(id, "MANUFACTURER", col(row, C_MANUFACTURER));
        put(id, "MARKETNAME", empty(col(row, C_MARKETNAME))
                ? col(row, C_MODEL) : col(row, C_MARKETNAME));
        put(id, "MODEL", col(row, C_MODEL));
        put(id, "DEVICE", col(row, C_DEVICE));
        put(id, "PRODUCT", col(row, C_PRODUCT));
        put(id, "BOARD", col(row, C_BOARD));
        put(id, "BOARD_PLATFORM", col(row, C_BOARD));
        put(id, "SOC_MANUFACTURER", col(row, C_SOC_MANUFACTURER));
        put(id, "SOC_MODEL", col(row, C_SOC_MODEL));

        String sdk = col(row, C_SDK);
        String release = col(row, C_RELEASE);
        String incremental = col(row, C_INCREMENTAL);
        String patch = col(row, C_SECURITY_PATCH);
        String model = col(row, C_MODEL);

        put(id, "SDK_INT", sdk);
        put(id, "RELEASE", release);
        put(id, "ID", col(row, C_BUILD_ID));
        put(id, "INCREMENTAL", incremental);
        put(id, "SECURITY_PATCH", patch);
        put(id, "RELEASE_DATE", col(row, C_RELEASE_DATE));

        // --- identifiers (random; the seed then derives everything else) ---
        String serial = randomHex(8, true);          // native: uppercase
        String androidId = randomHex(8, false);
        put(id, "SERIAL", serial);
        put(id, "ANDROID_ID", androidId);
        put(id, "GOOGLE_AID", uuidV4());

        // --- the deterministic descriptors ---
        String fp = fingerprint(row, release, sdk, col(row, C_BUILD_ID), incremental);
        put(id, "FINGERPRINT", fp);
        put(id, "DESCRIPTION",
                description(row, release, col(row, C_BUILD_ID), incremental));
        put(id, "DISPLAY", col(row, C_BUILD_ID));
        put(id, "VBMETA_DIGEST", SeedId.hexFromSeed(
                SeedId.fnv1a(fp + "|" + serial), 32));
        put(id, "BUILD_TIME_UTC", buildUtcFromPatch(patch, incremental));

        // --- build metadata: native values, which are what real images carry ---
        put(id, "USER", "android-build");
        put(id, "HOST", genHost());
        put(id, "TYPE", "user");
        put(id, "TAGS", "release-keys");
        put(id, "BOOTLOADER", "unknown");
        put(id, "FLAVOR", col(row, C_PRODUCT) + "-user");

        String radio = radio(row, patch, incremental);
        if (radio != null) put(id, "RADIO", radio);

        put(id, "MOD_DEVICE", col(row, C_PRODUCT) + "_global");
        put(id, "FOTA_OEM", col(row, C_MANUFACTURER));
        put(id, "GOOGLE_CLIENTIDBASE",
                "android-" + col(row, C_MANUFACTURER).toLowerCase(Locale.ROOT));

        put(id, "SUPPORTED_ABIS", "arm64-v8a,armeabi-v7a,armeabi");
        put(id, "SUPPORTED_64_BIT_ABIS", "arm64-v8a");
        put(id, "SUPPORTED_32_BIT_ABIS", "armeabi-v7a,armeabi");
        put(id, "CPU_ABI", "arm64-v8a");
        put(id, "CPU_ABI2", "");
        put(id, "SKU", "");
        put(id, "ODM_SKU", "");
        put(id, "BASE_OS", "");
        put(id, "PREVIEW_SDK_INT", "0");
        put(id, "PREVIEW_SDK_FINGERPRINT", "REL");
        put(id, "MEDIA_PERFORMANCE_CLASS", mediaPerfClass(model));

        // --- carrier defaults (config.hpp VAL_DEFAULTS; no carrier.conf here) ---
        put(id, "GSM_OPERATOR_NUMERIC", "51010");
        put(id, "GSM_OPERATOR_ALPHA", "Telkomsel");
        put(id, "GSM_OPERATOR_ISO", "id");
        put(id, "GSM_CARRIER_ID", "787");

        // --- AppLog epoch: rotation knob + determinism salt, NOT device age ---
        long epoch = Math.max(System.currentTimeMillis(), 1700000000000L);
        put(id, "APPLOG_EPOCH", Long.toString(epoch));

        // --- register ids: what makes the register endpoint mint a new did ---
        // These MUST be part of the persona. They are the values
        // RegisterHook puts into the device_register body, and they must be
        // fresh whenever the persona is rotated — a value the register server
        // has already seen is bound server-side to the did it first produced,
        // and the response will just echo that did back forever. Derived from
        // the same seed as everything else, so one persona = one consistent id
        // set, and Generate gives a set nobody has ever registered.
        SeedId.ApplogIds reg = SeedId.applogIds(
                SeedId.applogSeed(id, SandboxIdTT.TARGET), epoch);
        put(id, RegisterHook.PERSONA_OPENUDID, reg.openudid);
        put(id, RegisterHook.PERSONA_CLIENTUDID, reg.clientudid);
        put(id, RegisterHook.PERSONA_CDID, reg.cdid);
        put(id, "SSID", reg.ssid);

        // --- lifecycle (ported from gen_lifecycle; native has no equivalent) ---
        applyLifecycle(id, col(row, C_RELEASE_DATE));

        return id;
    }

    // ------------------------------------------------------------------
    // device pool
    // ------------------------------------------------------------------

    private static List<String[]> loadPool(Context ctx) {
        List<String[]> out = new ArrayList<>();
        AssetManager am = ctx.getAssets();
        if (am == null) return out;
        BufferedReader br = null;
        try {
            br = new BufferedReader(new InputStreamReader(am.open(DEVICES_ASSET)));
            String line;
            while ((line = br.readLine()) != null) {
                String t = line.trim();
                if (t.isEmpty() || t.charAt(0) == '#') continue;
                String[] f = t.split("\t", -1);
                if (f.length >= 15) out.add(f);
            }
        } catch (IOException e) {
            // Asset missing = packaging bug; caller reports failure to the user.
        } finally {
            if (br != null) try { br.close(); } catch (IOException ignored) {}
        }
        return out;
    }

    /**
     * Two-level uniform pick, brand-first — this IS the pool's weighting:
     * uniform over distinct brands, then uniform within the chosen brand, so a
     * brand with 2 rows has the same odds as one with 14. Ported from
     * autopif.sh's brand-then-row selection.
     */
    private static String[] pick(List<String[]> pool) {
        Map<String, List<String[]>> byBrand = new LinkedHashMap<>();
        for (String[] row : pool) {
            String b = col(row, C_BRAND);
            List<String[]> l = byBrand.get(b);
            if (l == null) byBrand.put(b, l = new ArrayList<>());
            l.add(row);
        }
        List<List<String[]>> brands = new ArrayList<>(byBrand.values());
        List<String[]> chosen = brands.get(RNG.nextInt(brands.size()));
        return chosen.get(RNG.nextInt(chosen.size()));
    }

    // ------------------------------------------------------------------
    // derived build fields
    // ------------------------------------------------------------------

    private static String fingerprint(String[] row, String release, String sdk,
                                      String buildId, String incremental) {
        // native derive_identity:
        //   "%s/%s/%s:%s/%s/%s:user/release-keys"
        //   brand / product / device : release / id / incremental
        return String.format(Locale.ROOT, "%s/%s/%s:%s/%s/%s:user/release-keys",
                col(row, C_BRAND), col(row, C_PRODUCT), col(row, C_DEVICE),
                release, buildId, incremental);
    }

    private static String description(String[] row, String release, String buildId,
                                      String incremental) {
        // native derive_identity: "%s-user %s %s %s release-keys"
        //   product - user release id incremental release-keys
        return String.format(Locale.ROOT, "%s-user %s %s %s release-keys",
                col(row, C_PRODUCT), release, buildId, incremental);
    }

    /**
     * Port of build_utc_from_patch(): UTC = timegm(patch 03:00Z)
     * - (1 + digitSum%6) days + (digitSum%60) minutes + (digitSum%37) seconds,
     * where digitSum is the SUM OF THE DIGIT CHARACTERS in incremental (not the
     * number). The shell variant omits the %37 term — we follow native.
     */
    private static String buildUtcFromPatch(String patch, String incremental) {
        if (patch.length() < 10 || patch.charAt(4) != '-' || patch.charAt(7) != '-')
            return "";
        int y = parseInt(patch.substring(0, 4));
        int m = parseInt(patch.substring(5, 7));
        int d = parseInt(patch.substring(8, 10));
        if (y < 2008 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return "";

        int digits = 0;
        for (int i = 0; i < incremental.length(); i++) {
            char c = incremental.charAt(i);
            if (c >= '0' && c <= '9') digits += c - '0';
        }

        // Gregorian days from civil epoch (Howard Hinnant's algorithm), then to
        // epoch seconds — the same calculation ymd_to_epoch performs in shell.
        long days = daysFromCivil(y, m, d);
        long t = days * 86400L + 3 * 3600L;
        t -= (long) (1 + digits % 6) * 86400L;
        t += (long) (digits % 60) * 60L + (long) (digits % 37);
        return Long.toString(t);
    }

    /** Tensor modem string; null on non-Tensor platforms (native behaviour). */
    private static String radio(String[] row, String patch, String incremental) {
        String platform = col(row, C_BOARD).toLowerCase(Locale.ROOT);
        String prefix = null;
        if (platform.equals("gs101")) prefix = "g5123b";
        else if (platform.equals("gs201")) prefix = "g5300b";
        else if (platform.equals("zuma")) prefix = "g5300q";
        else if (platform.equals("zumapro")) prefix = "g5400";
        else if (platform.equals("laguna")) prefix = "g5500";
        if (prefix == null) return null;

        String pdate = "000000";
        if (patch.length() >= 10)
            pdate = "" + patch.charAt(2) + patch.charAt(3) + patch.charAt(5)
                    + patch.charAt(6) + patch.charAt(8) + patch.charAt(9);
        return prefix + "-" + pdate + "-B-" + incremental;
    }

    private static String mediaPerfClass(String model) {
        if (model.startsWith("Pixel 9")) return "35";
        if (model.startsWith("Pixel 8")) return "34";
        if (model.startsWith("Pixel 7")) return "33";
        if (model.startsWith("Pixel 6")) return "31";
        return "";
    }

    private static String genHost() {
        String[] prefixes = {"abfarm", "abfarm-release", "abfarm-server",
                "build", "build-server", "release", "release-server", "farm",
                "buildfarm"};
        return prefixes[RNG.nextInt(prefixes.length)] + "-"
                + (RNG.nextInt(900) + 100);
    }

    // ------------------------------------------------------------------
    // lifecycle (ported from gen_lifecycle; native emits none of these)
    // ------------------------------------------------------------------

    private static void applyLifecycle(Map<String, String> id, String releaseDate) {
        long now = System.currentTimeMillis() / 1000L;

        long relEpoch = epochFromYm(releaseDate);
        long ageDays = relEpoch > 0 ? (now - relEpoch) / 86400L : 0;
        if (ageDays < 1) ageDays = 1 + RNG.nextInt(30);
        if (ageDays > 1825) ageDays = 1825;

        long ownedDays = 1 + RNG.nextInt(30);
        if (ownedDays > ageDays) ownedDays = ageDays;

        int setupBoots = 2 + RNG.nextInt(3);
        int extraCap = (int) Math.min(ownedDays, 26);
        int bootCount = setupBoots + RNG.nextInt(extraCap + 1);
        if (bootCount < 1) bootCount = 1;
        if (bootCount > 30) bootCount = 30;

        long uptimeS = 180 + RNG.nextInt(3600 - 180 + 1);
        if (uptimeS > ownedDays * 86400L) uptimeS = ownedDays * 86400L;

        put(id, "AGE_DAYS", Long.toString(ageDays));
        put(id, "OWNED_DAYS", Long.toString(ownedDays));
        put(id, "BOOT_COUNT", Integer.toString(bootCount));
        // Uptime spoofing is opt-in on the root side and force-zeroed by default;
        // keep 0 here so this variant claims nothing it does not deliver.
        put(id, "UPTIME_SECONDS", "0");
        put(id, "UPTIME_HUMAN", "0d 0h 0m");
        put(id, "FIRST_BOOT", ymdFromEpoch(now - ownedDays * 86400L));
        put(id, "LAST_BOOT", ymdFromEpoch(now - uptimeS));
        put(id, "RESET", RNG.nextInt(100) < 40 ? "1" : "0");
        put(id, "FRESH", "yes");
        put(id, "USAGE_PROFILE", "fresh");
    }

    // ------------------------------------------------------------------
    // helpers
    // ------------------------------------------------------------------

    private static String col(String[] row, int idx) {
        String v = idx - 1 < row.length ? row[idx - 1] : "";
        return v == null ? "" : v;
    }

    private static boolean empty(String s) { return s == null || s.isEmpty(); }

    private static void put(Map<String, String> id, String k, String v) {
        if (!empty(v)) id.put(k, v);
    }

    private static int parseInt(String s) {
        try { return Integer.parseInt(s.trim()); } catch (NumberFormatException e) { return 0; }
    }

    private static String randomHex(int bytes, boolean upper) {
        StringBuilder sb = new StringBuilder(bytes * 2);
        String al = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        for (int i = 0; i < bytes * 2; i++) sb.append(al.charAt(RNG.nextInt(16)));
        return sb.toString();
    }

    private static String uuidV4() {
        String h = randomHex(16, false);
        StringBuilder sb = new StringBuilder(h);
        sb.insert(20, "-"); sb.insert(16, "-"); sb.insert(12, "-"); sb.insert(8, "-");
        sb.setCharAt(14, '4');
        sb.setCharAt(19, "89ab".charAt(RNG.nextInt(4)));
        return sb.toString();
    }

    /** Release date is "YYYY-MM"; the day is unknown, so use the 1st. */
    private static long epochFromYm(String ym) {
        if (ym == null || ym.length() < 7 || ym.charAt(4) != '-') return 0;
        int y = parseInt(ym.substring(0, 4));
        int m = parseInt(ym.substring(5, 7));
        if (y < 2008 || m < 1 || m > 12) return 0;
        return daysFromCivil(y, m, 1) * 86400L;
    }

    private static String ymdFromEpoch(long epoch) {
        long days = Math.floorDiv(epoch, 86400L);
        // Hinnant civil_from_days
        long z = days + 719468;
        long era = Math.floorDiv(z, 146097);
        long doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long y = yoe + era * 400;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        long mp = (5 * doy + 2) / 153;
        long d = doy - (153 * mp + 2) / 5 + 1;
        long m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2) y += 1;
        return String.format(Locale.ROOT, "%04d-%02d-%02d", y, m, d);
    }

    private static long daysFromCivil(int y, int m, int d) {
        if (m <= 2) { y -= 1; m += 12; }
        long era = Math.floorDiv((long) y, 400);
        long yoe = y - era * 400;
        long doy = (153 * (m - 3) + 2) / 5 + d - 1;
        long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }
}
