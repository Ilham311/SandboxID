package id.sandboxid.tt;

/**
 * Pins the register-body rewriter against the form TikTok actually puts on the
 * wire. See {@link Rewrite} for why this layer exists.
 *
 * <p>This test is deliberately JDK-only: it is run in CI before a release, and
 * {@link Rewrite} has no Android dependency worth an SDK install for. The one
 * Android call it makes ({@code Log.d}) is stubbed by the CI step that invokes
 * this class, via a {@code sed} on the copy it compiles — the original here is
 * untouched.
 *
 * <p><b>What each case is for.</b> These were all real failure modes found
 * while writing the rewriter, not speculative coverage:
 * <ul>
 *   <li>wire form — TikTok emits the did fields as <i>bare numbers</i>
 *       ({@code "device_id":7691...}), not strings. The first version of the
 *       rewriter handled only the string form and silently left the did
 *       intact.</li>
 *   <li>missing closing quote — an off-by-one in the slice ate the value's
 *       closing quote and corrupted the JSON.</li>
 *   <li>idempotent — patching an already-patched body must not drift, because
 *       a retry or a second hook fire rewrites the same buffer.</li>
 *   <li>escape — a persona value containing a quote or backslash must not
 *       break the payload.</li>
 *   <li>substring-key — {@code device_id} must not match the tail of
 *       {@code last_update_sender_did}, which would clobber an unrelated
 *       field.</li>
 * </ul>
 */
public class RewriteTest3 {

    static int fails = 0;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "ok  " : "FAIL") + " " + name);
        if (!ok) fails++;
    }

    public static void main(String[] a) throws Exception {
        // (1) the real TikTok wire form: numeric did fields, string ids
        String body = "{\"header\":{\"clientudid\":\"OLD\",\"openudid\":\"OLDOP\","
                + "\"cdid\":\"OLDCD\",\"device_id\":7691364758716679700,"
                + "\"install_id\":999,\"bd_did\":888,"
                + "\"last_update_sender_did\":777,\"sig_hash\":\"abc\","
                + "\"apk_first_install_time\":1790785494436},"
                + "\"magic_tag\":\"ss_app_log\"}";
        byte[] out = Rewrite.body(body.getBytes("UTF-8"),
                "7b65af4ae18115a4", "dbc46b87-b44d", "76832eec-c82f");
        String s = new String(out, "UTF-8");
        check("device_id 0", s.contains("\"device_id\":0"));
        check("install_id 0", s.contains("\"install_id\":0"));
        check("bd_did 0", s.contains("\"bd_did\":0"));
        check("clientudid", s.contains("\"clientudid\":\"dbc46b87-b44d\""));
        check("openudid", s.contains("\"openudid\":\"7b65af4ae18115a4\""));
        check("cdid", s.contains("\"cdid\":\"76832eec-c82f\""));
        check("untouched last_update_sender_did",
                s.contains("\"last_update_sender_did\":777"));
        check("untouched sig_hash", s.contains("\"sig_hash\":\"abc\""));
        check("untouched install_time",
                s.contains("\"apk_first_install_time\":1790785494436"));
        check("old did gone", !s.contains("7691364758716679700"));

        // (2) idempotent: patching an already-patched body is stable
        byte[] out2 = Rewrite.body(out, "7b65af4ae18115a4", "dbc46b87-b44d",
                "76832eec-c82f");
        check("idempotent", new String(out2, "UTF-8").equals(s));

        // (3) no keys present -> body returned untouched
        byte[] out3 = Rewrite.body("{\"a\":1}".getBytes("UTF-8"), "x", "y", "z");
        check("no-op when absent",
                new String(out3, "UTF-8").equals("{\"a\":1}"));

        // (4) escaping safety: a quote and a backslash in a persona value
        byte[] out4 = Rewrite.body("{\"openudid\":\"X\"}".getBytes("UTF-8"),
                "a\"b\\c", "d", "e");
        String s4 = new String(out4, "UTF-8");
        check("escape quote", s4.contains("\"openudid\":\"a\\\"b\\\\c\""));

        // (5) substring-key safety: 'device_id' must not match the tail of
        // another key, and must still match the real one
        byte[] out5 = Rewrite.body(
                "{\"x_device_id\":\"keep\",\"device_id\":5}".getBytes("UTF-8"),
                "a", "b", "c");
        String s5 = new String(out5, "UTF-8");
        check("no false substring match",
                s5.contains("\"x_device_id\":\"keep\"") && s5.contains("\"device_id\":0"));

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        if (fails != 0) System.exit(1);
    }
}
