package id.sandboxid.tt;

/**
 * Pins {@link SeedId}'s identifier synthesis — the Java counterpart of the
 * native suite {@code tests/host/sbx_ident_synth.cpp}.
 *
 * <p><b>Why this test exists.</b> The LSPosed variant carried the identical bug
 * the native side had, in two places at once:
 *
 * <ul>
 *   <li>{@code SeedId.imsi()} substituted {@code "51010"} (Telkomsel) for any
 *       operator it could not parse, so a persona on an XL SIM — {@code 51011},
 *       the test device — advertised an IMSI naming a network it was not
 *       attached to. {@code SeedId.iccid()} had no guard at all and produced
 *       {@code 8901...} with a fabricated country code {@code 01}.</li>
 *   <li>{@code TelephonyDrmHook} defaulted a missing
 *       {@code GSM_OPERATOR_NUMERIC} to {@code "51010"} and
 *       {@code PersonaGenerator} wrote that key unconditionally, so the bug was
 *       reachable from every persona the UI ever generated.</li>
 * </ul>
 *
 * <p>The rule this test locks: a synthesized identifier is <b>consistent with
 * its operator, or empty</b>. Empty makes the caller skip the hook so the getter
 * falls through to the real value — never a third thing that merely looks
 * plausible.
 *
 * <p>Deliberately JDK-only, like {@code RewriteTest3} and {@code HwNamingTest}:
 * {@link SeedId} imports nothing from Android, so CI can pin it without an SDK.
 * The expected values were cross-checked against the native
 * {@code synth_imsi()}/{@code synth_iccid()} for the same seed, so a drift
 * between the two variants fails here.
 */
public class SeedIdTest {

    static int fails = 0;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "ok  " : "FAIL") + " " + name);
        if (!ok) fails++;
    }

    static void eq(String name, String got, String want) {
        boolean ok = want.equals(got);
        System.out.println((ok ? "ok  " : "FAIL") + " " + name
                + (ok ? "" : "  (got '" + got + "', want '" + want + "')"));
        if (!ok) fails++;
    }

    // Same seed the native cross-check used, so the two variants' outputs are
    // directly comparable.
    static final long SEED = 0x1234ABCDL;

    public static void main(String[] a) {
        System.out.println("-- operator numeric validation --");
        check("51011 is valid", SeedId.isOperatorNumeric("51011"));
        check("310260 is valid", SeedId.isOperatorNumeric("310260"));
        check("null is not valid", !SeedId.isOperatorNumeric(null));
        check("empty is not valid", !SeedId.isOperatorNumeric(""));
        check("truncated is not valid", !SeedId.isOperatorNumeric("510"));
        check("over-long is not valid", !SeedId.isOperatorNumeric("5101111"));
        // The single-SIM per-slot form: getprop returns "51011," with the
        // separator still attached. The caller strips it; this must reject it.
        check("comma-joined is not valid", !SeedId.isOperatorNumeric("51011,"));
        check("non-numeric is not valid", !SeedId.isOperatorNumeric("51O11"));

        System.out.println("-- IMSI --");
        // The regression: these two used to come back as 51010-prefixed.
        eq("no operator -> empty", SeedId.imsi(SEED, ""), "");
        eq("null operator -> empty", SeedId.imsi(SEED, null), "");
        eq("comma-joined -> empty", SeedId.imsi(SEED, "51011,"), "");
        eq("truncated -> empty", SeedId.imsi(SEED, "510"), "");
        eq("non-numeric -> empty", SeedId.imsi(SEED, "51O11"), "");

        // A known operator yields a 15-digit IMSI carrying that operator.
        String imsi = SeedId.imsi(SEED, "51011");
        eq("imsi length", Integer.toString(imsi.length()), "15");
        check("imsi carries its operator", imsi.startsWith("51011"));
        check("imsi is all digits", allDigits(imsi));
        // Never the Telkomsel prefix unless the operator IS Telkomsel.
        check("imsi is not Telkomsel for an XL SIM", !imsi.startsWith("51010"));
        eq("imsi is deterministic", SeedId.imsi(SEED, "51011"), imsi);
        check("imsi follows the operator, not a constant",
                !SeedId.imsi(SEED, "51010").equals(imsi));

        // 3-digit MNC (North America) still produces 15 digits.
        String us = SeedId.imsi(SEED, "310260");
        eq("3-digit MNC imsi length", Integer.toString(us.length()), "15");
        check("3-digit MNC imsi carries its operator", us.startsWith("310260"));

        System.out.println("-- ICCID --");
        eq("no operator -> empty", SeedId.iccid(SEED, ""), "");
        eq("null operator -> empty", SeedId.iccid(SEED, null), "");
        eq("comma-joined -> empty", SeedId.iccid(SEED, "51011,"), "");
        eq("truncated -> empty", SeedId.iccid(SEED, "510"), "");

        String iccid = SeedId.iccid(SEED, "51011");
        eq("iccid length", Integer.toString(iccid.length()), "19");
        check("iccid starts with 89", iccid.startsWith("89"));
        // 89 + country 62 (Indonesia) for MCC 510. The fabricated "01" that the
        // unguarded path produced for an unknown operator is the regression.
        check("iccid country code follows the MCC", iccid.startsWith("8962"));
        check("iccid is all digits", allDigits(iccid));
        check("iccid Luhn valid", luhnOk(iccid));
        eq("iccid is deterministic", SeedId.iccid(SEED, "51011"), iccid);

        check("US iccid uses country 1", SeedId.iccid(SEED, "310260").startsWith("891"));
        check("US iccid Luhn valid", luhnOk(SeedId.iccid(SEED, "310260")));

        System.out.println("-- cross-variant agreement with the native side --");
        // These exact strings are what sbxid::synth_imsi / synth_iccid return for
        // the same seed. A change on either side that breaks the pair fails here.
        eq("imsi matches native", SeedId.imsi(SEED, "51011"), "510115509216614");
        eq("iccid matches native", SeedId.iccid(SEED, "51011"), "8962767996665767591");
        eq("iccid matches native (US)",
                SeedId.iccid(SEED, "310260"), "8917679966657675913");

        System.out.println("-- IMEI / MEID / Widevine are operator-independent --");
        // These must still synthesize with no operator: a device with no SIM
        // still has an IMEI, so emptying them would be wrong.
        String imei = SeedId.imei(SEED);
        eq("imei length", Integer.toString(imei.length()), "15");
        check("imei is all digits", allDigits(imei));
        check("imei Luhn valid", luhnOk(imei));
        eq("imei is deterministic", SeedId.imei(SEED), imei);

        String meid = SeedId.meid(SEED);
        eq("meid length", Integer.toString(meid.length()), "14");
        check("meid is uppercase hex", meid.matches("[0-9A-F]{14}"));

        eq("widevine length", Integer.toString(SeedId.widevineHex(SEED).length()), "64");
        check("widevine is lowercase hex",
                SeedId.widevineHex(SEED).matches("[0-9a-f]{64}"));

        // Value anchors, not just shape. Shape checks pass for ANY seed, so
        // they cannot catch a drift between this variant and the Zygisk module
        // — which is exactly how the AppLog ids diverged unnoticed. These five
        // strings are what sbxid::synth_imei/synth_meid/synth_widevine_hex
        // return for the same seed.
        eq("imei matches native", imei, "354381107373819");
        eq("meid matches native", meid, "0EE1910E29C47B");
        eq("widevine matches native", SeedId.widevineHex(SEED),
                "92ce2df31baa7714377d9544e2dea027f0aca28072b1c94eb7c732cc0883fa81");

        // ---- operatorMcc / operatorMnc: the cell-identity split -----------
        // sbxid::operator_mcc/operator_mnc, same inputs. Positional split after
        // three digits, and BOTH halves empty for anything that is not a valid
        // 5-6 digit numeric — a 4-digit or non-numeric value must not yield a
        // half-populated pair, or the cell hooks would answer an MCC with no MNC.
        eq("mcc of 51011", SeedId.operatorMcc("51011"), "510");
        eq("mnc of 51011", SeedId.operatorMnc("51011"), "11");
        eq("mcc of 310260", SeedId.operatorMcc("310260"), "310");
        eq("mnc of 310260", SeedId.operatorMnc("310260"), "260");
        eq("mcc of 510110", SeedId.operatorMcc("510110"), "510");
        eq("mnc of 510110", SeedId.operatorMnc("510110"), "110");
        eq("mcc of 4-digit", SeedId.operatorMcc("5101"), "");
        eq("mnc of 4-digit", SeedId.operatorMnc("5101"), "");
        eq("mcc of non-numeric", SeedId.operatorMcc("5101X"), "");
        eq("mnc of non-numeric", SeedId.operatorMnc("5101X"), "");
        eq("mcc of empty", SeedId.operatorMcc(""), "");
        eq("mcc of null", SeedId.operatorMcc(null), "");
        eq("mnc of null", SeedId.operatorMnc(null), "");

        System.out.println("-- pickMac: the persona address wins, or a safe fallback --");
        // The Bluetooth asymmetry, in the other variant. The Zygisk side built
        // its Bluetooth address from the seed unconditionally and ignored the
        // persona's BLUETOOTH_ADDR; this side passed the persona value straight
        // through and skipped the fallback when it was null. Both are wrong in
        // opposite directions, and both are pinned by the same rule here and in
        // tests/host/sbx_ident_synth.cpp.
        final String persona = "02:91:1a:ba:67:2a";
        eq("persona address served as declared",
                SeedId.pickMac(persona, SEED, 0x424C554554L, false), persona);
        eq("persona address uppercased for Bluetooth",
                SeedId.pickMac(persona, SEED, 0x424C554554L, true), "02:91:1A:BA:67:2A");

        for (boolean up : new boolean[] {false, true}) {
            String fb = SeedId.pickMac(null, SEED, 0x424C554554L, up);
            eq("fallback length", Integer.toString(fb.length()), "17");
            check("fallback is a valid MAC", SeedId.isValidMac(fb));
            check("fallback is locally administered", fb.startsWith("02:"));
            eq("fallback deterministic",
                    SeedId.pickMac(null, SEED, 0x424C554554L, up), fb);
            check("fallback rotates with the seed",
                    !SeedId.pickMac(null, SEED ^ 1, 0x424C554554L, up).equals(fb));
        }

        // A malformed persona value is treated exactly like an absent one.
        String[] bad = {"not-a-mac", "02:91:1a:ba:67", "02-91-1a-ba-67-2a",
                "00:00:00:00:00:00", "02:91:1a:ba:67:zz", ""};
        for (String b : bad) {
            String got = SeedId.pickMac(b, SEED, 0x424C554554L, false);
            eq("malformed falls back", got,
                    SeedId.pickMac(null, SEED, 0x424C554554L, false));
            check("never serves a malformed address", SeedId.isValidMac(got));
        }

        // The two salts must not collide, or a persona declaring neither would
        // report one address on both radios.
        check("wifi and bluetooth fallbacks differ",
                !SeedId.pickMac(null, SEED, 0x5749464931L, false)
                        .equals(SeedId.pickMac(null, SEED, 0x424C554554L, false)));

        System.out.println("-- cross-variant MAC agreement with the native side --");
        // These exact strings are what sbxid::pick_mac() returns for the same
        // seed and salt, and what tests/host/sbx_ident_synth.cpp asserts. A
        // change on either side that breaks the pair fails here.
        eq("wifi fallback matches native",
                SeedId.pickMac(null, SEED, 0x5749464931L, false),
                "02:f4:87:11:fc:ef");
        eq("bluetooth fallback matches native",
                SeedId.pickMac(null, SEED, 0x424C554554L, true),
                "02:0F:A0:CA:3B:D1");

        // AppLog ids. These six strings were produced by compiling and running
        // native/native_read.cpp's make_applog_ids() itself, not by hand.
        //
        // The LSPosed variant writes openudid/clientudid/cdid into the persona
        // that the Zygisk variant also serves, so a mismatch here means the
        // same persona registers as two different devices depending on which
        // variant is installed — and the register server binds (openudid,
        // install_id cookie) permanently on first sight, so the second variant
        // silently inherits the first variant's did.
        SeedId.ApplogIds reg = SeedId.applogIds(SEED, 1700000000000L);
        eq("applog did matches native", reg.did, "7130316800001884825");
        eq("applog iid matches native", reg.iid, "7130316800001862035");
        eq("applog ssid matches native", reg.ssid, "7130316800001309879");
        eq("applog cdid matches native", reg.cdid,
                "4831a5e5-ae29-49e6-897e-a50292a4cb12");
        eq("applog clientudid matches native", reg.clientudid,
                "383d033e-ba38-45ea-9f41-a82f20ede5ce");
        eq("applog openudid matches native", reg.openudid, "52c5eb42a728473b");

        // A bumped epoch must move the three formerly-frozen ids too, or
        // rotate_ids.sh applog would rewrite did/iid/ssid and then re-bind the
        // new registration to the old did via an unchanged openudid.
        SeedId.ApplogIds rot = SeedId.applogIds(SEED, 1700000000001L);
        check("epoch bump moves did", !reg.did.equals(rot.did));
        check("epoch bump moves cdid", !reg.cdid.equals(rot.cdid));
        check("epoch bump moves clientudid",
                !reg.clientudid.equals(rot.clientudid));
        check("epoch bump moves openudid", !reg.openudid.equals(rot.openudid));

        // Each id must have its own stream: did and clientudid once shared a
        // salt, which made two "different" ids the same value.
        check("did != clientudid stream",
                !reg.did.equals(reg.clientudid));
        check("cdid != clientudid", !reg.cdid.equals(reg.clientudid));
        check("openudid is 16 hex",
                reg.openudid.matches("[0-9a-f]{16}"));
        check("cdid is a uuid", reg.cdid.matches(
                "[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"));
        check("clientudid is a uuid", reg.clientudid.matches(
                "[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"));

        System.out.println("-- userAgentPair: the WebView UA rewrite --");
        // The pair is a literal {from, to} for String.replace, and both halves
        // must describe the SAME segment shape. A from-without-to (or a pair
        // built from the persona on both sides) is a silent no-op — exactly the
        // bug module.cpp:365-372 records hitting on device, where reading the
        // props back through the module's own hook made from == to.
        java.util.Map<String, String> uaPersona = new java.util.HashMap<>();
        uaPersona.put("RELEASE", "13");
        uaPersona.put("MODEL", "Pixel 7");
        uaPersona.put("ID", "TQ3A.230805.001");

        String[] ua = SeedId.userAgentPair(
                "14", "Redmi Note 10", "TKQ1.220829.002", uaPersona);
        check("ua pair built", ua != null && ua.length == 2);
        eq("ua from is the real segment", ua[0],
                "Android 14; Redmi Note 10 Build/TKQ1.220829.002");
        eq("ua to is the persona segment", ua[1],
                "Android 13; Pixel 7 Build/TQ3A.230805.001");
        // The whole point: the two halves must differ, or the rewrite does
        // nothing while still costing a hook.
        check("ua halves differ", !ua[0].equals(ua[1]));

        // A partial pair must be refused outright. Substituting a half-built
        // segment would produce a UA naming neither phone correctly.
        check("ua null when real release missing",
                SeedId.userAgentPair(null, "M", "ID", uaPersona) == null);
        check("ua null when real model missing",
                SeedId.userAgentPair("14", "", "ID", uaPersona) == null);
        check("ua null when real id missing",
                SeedId.userAgentPair("14", "M", null, uaPersona) == null);
        check("ua null when persona null",
                SeedId.userAgentPair("14", "M", "ID", null) == null);

        java.util.Map<String, String> noModel = new java.util.HashMap<>();
        noModel.put("RELEASE", "13");
        noModel.put("ID", "TQ3A.230805.001");
        check("ua null when persona lacks MODEL",
                SeedId.userAgentPair("14", "M", "ID", noModel) == null);

        System.out.println("-- screenGeometry: resolution must match the claimed model --");
        // Screen size is the cheapest cross-check an SDK can make against
        // Build.MODEL: a persona claiming a Pixel 9 Pro XL while
        // Display.getRealSize() reports the real 1080x2400 is a contradiction
        // that needs no privileged access. The table mirrors module.cpp:450-503.
        java.util.Map<String, String> px9xl = new java.util.HashMap<>();
        px9xl.put("MODEL", "Pixel 9 Pro XL");
        int[] g = SeedId.screenGeometry(px9xl);
        check("geometry built", g != null && g.length == 5);
        eq("Pixel 9 Pro XL width", Integer.toString(g[0]), "1344");
        eq("Pixel 9 Pro XL height", Integer.toString(g[1]), "2992");
        eq("Pixel 9 Pro XL density", Integer.toString(g[2]), "560");

        // Ordering: "Pixel 9 Pro XL" also contains "Pixel 9 Pro", and
        // "Pixel 9 Pro Fold" contains both. The most specific must win, or a
        // Pro Fold persona reports the Pro XL's panel.
        java.util.Map<String, String> fold = new java.util.HashMap<>();
        fold.put("MODEL", "Pixel 9 Pro Fold");
        eq("Pixel 9 Pro Fold width",
                Integer.toString(SeedId.screenGeometry(fold)[0]), "1080");
        java.util.Map<String, String> p9pro = new java.util.HashMap<>();
        p9pro.put("MODEL", "Pixel 9 Pro");
        eq("Pixel 9 Pro width",
                Integer.toString(SeedId.screenGeometry(p9pro)[0]), "1280");
        java.util.Map<String, String> p8 = new java.util.HashMap<>();
        p8.put("MODEL", "Pixel 8");
        eq("Pixel 8 width", Integer.toString(SeedId.screenGeometry(p8)[0]), "1080");
        java.util.Map<String, String> p8pro = new java.util.HashMap<>();
        p8pro.put("MODEL", "Pixel 8 Pro");
        eq("Pixel 8 Pro width",
                Integer.toString(SeedId.screenGeometry(p8pro)[0]), "1344");

        // Unknown model: the same 1080x2400/420 default native uses, not null —
        // a persona for a non-Pixel still needs a plausible panel.
        java.util.Map<String, String> other = new java.util.HashMap<>();
        other.put("MODEL", "Redmi Note 10");
        int[] o = SeedId.screenGeometry(other);
        eq("unknown model width", Integer.toString(o[0]), "1080");
        eq("unknown model density", Integer.toString(o[2]), "420");

        // No MODEL at all: nothing to be consistent with, so no geometry.
        check("no model -> null", SeedId.screenGeometry(new java.util.HashMap<>()) == null);
        check("null map -> null", SeedId.screenGeometry(null) == null);

        // Persona overrides win, but only when positive and parseable.
        java.util.Map<String, String> ovr = new java.util.HashMap<>();
        ovr.put("MODEL", "Pixel 7");
        ovr.put("SCREEN_WIDTH", "1440");
        ovr.put("SCREEN_HEIGHT", "3200");
        ovr.put("LCD_DENSITY", "560");
        int[] ov = SeedId.screenGeometry(ovr);
        eq("override width", Integer.toString(ov[0]), "1440");
        eq("override height", Integer.toString(ov[1]), "3200");
        eq("override density", Integer.toString(ov[2]), "560");

        java.util.Map<String, String> badOvr = new java.util.HashMap<>();
        badOvr.put("MODEL", "Pixel 7");
        badOvr.put("SCREEN_WIDTH", "0");
        badOvr.put("LCD_DENSITY", "not-a-number");
        int[] bo = SeedId.screenGeometry(badOvr);
        eq("zero override ignored", Integer.toString(bo[0]), "1080");
        eq("malformed override ignored", Integer.toString(bo[2]), "420");

        System.out.println();
        if (fails == 0) System.out.println("ALL PASS");
        else System.out.println(fails + " check(s) FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }

    static boolean allDigits(String s) {
        if (s == null || s.isEmpty()) return false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    /** Luhn over the whole string, check digit included: sum % 10 == 0. */
    static boolean luhnOk(String s) {
        int sum = 0;
        boolean dbl = false;
        for (int i = s.length() - 1; i >= 0; i--) {
            int n = s.charAt(i) - '0';
            if (dbl) {
                n *= 2;
                if (n > 9) n -= 9;
            }
            sum += n;
            dbl = !dbl;
        }
        return sum % 10 == 0;
    }
}
