package id.sandboxid.tt;

import android.util.Log;

/**
 * Patches the TikTok register request body: the old device id goes to "0", and
 * openudid/clientudid/cdid are replaced with the persona's never-registered
 * values.
 *
 * <p>This operates on the serialized byte[] that {@code X.087U.post()} is about
 * to hand to the network stack, so it runs after every layer that assembled the
 * JSON and cannot be undone by a downstream merge. It is deliberately dumb:
 * string surgery on the wire format, no JSON parse, no dependency on any
 * obfuscated class. The body is small (about 2&nbsp;kB), so a scan is cheap.
 *
 * <p><b>Correctness.</b> Every replacement targets a key in the request object
 * and substitutes its value, always producing shorter-or-equal output so the
 * byte array can only shrink. Values are JSON-encoded with
 * {@link #jsonString(String)} so a value containing a quote or backslash cannot
 * corrupt the payload. Numeric keys ({@code device_id}) are replaced in place.
 */
final class Rewrite {

    private static final String TAG = "SandboxID-TT";

    private Rewrite() {}

    /**
     * @param body        the serialized register body
     * @param openudid    persona openudid (never registered before)
     * @param clientudid  persona clientudid
     * @param cdid        persona cdid
     * @return the patched body, or {@code body} unchanged if nothing matched
     */
    static byte[] body(byte[] body, String openudid, String clientudid,
                       String cdid) {
        String s;
        try {
            s = new String(body, "UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            return body;
        }

        boolean changed = false;

        // The did itself: "device_id":<value> -> "device_id":"0".
        // install_id and bd_did likewise. These are the fields that tell the
        // register server "I already am this device". TikTok emits them as
        // bare numbers, so both the string and the numeric form are handled.
        for (String k : new String[] { "device_id", "install_id", "bd_did" }) {
            String patched = replaceStringKey(s, k, "0");
            if (patched != null) { s = patched; changed = true; continue; }
            patched = replaceNumberKey(s, k, "0");
            if (patched != null) { s = patched; changed = true; }
        }

        // The identifying ids: must be the persona's own, never a value the
        // register server has bound to another did.
        String patched = replaceStringKey(s, "openudid", openudid);
        if (patched != null) { s = patched; changed = true; }
        patched = replaceStringKey(s, "clientudid", clientudid);
        if (patched != null) { s = patched; changed = true; }
        patched = replaceStringKey(s, "cdid", cdid);
        if (patched != null) { s = patched; changed = true; }

        if (!changed) return body;

        try {
            byte[] out = s.getBytes("UTF-8");
            Log.d(TAG, "register body patched");
            return out;
        } catch (java.io.UnsupportedEncodingException e) {
            return body;
        }
    }

    /**
     * Replaces the value of a JSON <b>numeric</b> key — {@code "key":12345} —
     * with a replacement <b>number</b>. Used for the did fields, which TikTok
     * emits unquoted; the replacement stays numeric so the patched payload
     * matches the wire form the server expects.
     *
     * @return the new string, or null if the key was not found or the value
     *         after the colon is not a bare number
     */
    private static String replaceNumberKey(String json, String key,
                                           String newNumber) {
        int at = 0;
        while (true) {
            int q = json.indexOf('"', at);
            if (q < 0) return null;
            int keyEnd = json.indexOf('"', q + 1);
            if (keyEnd < 0) return null;

            if (json.regionMatches(q + 1, key, 0, key.length())
                    && keyEnd == q + 1 + key.length()) {
                int i = keyEnd + 1;
                while (i < json.length() && isSpace(json.charAt(i))) i++;
                if (i < json.length() && json.charAt(i) == ':') {
                    i++;
                    while (i < json.length() && isSpace(json.charAt(i))) i++;
                    if (i < json.length() && json.charAt(i) == '"') return null;
                    int valStart = i;
                    int valEnd = i;
                    while (valEnd < json.length()
                            && isNumberChar(json.charAt(valEnd))) {
                        valEnd++;
                    }
                    if (valEnd > valStart) {
                        return json.substring(0, valStart)
                                + newNumber
                                + json.substring(valEnd);
                    }
                }
            }
            at = keyEnd + 1;
        }
    }

    /**
     * Replaces the value of a JSON string key, returning null if the key is
     * absent or the value could not be located safely.
     *
     * <p>Matches {@code "key"<whitespace>:<whitespace>"<value>"} and substitutes
     * an escaped replacement value. The match is anchored on the key, so a key
     * that is a substring of another (e.g. {@code device_id} inside
     * {@code last_update_sender_did}) is handled correctly by demanding the
     * opening quote and colon around it.
     *
     * @return the new string, or null if the key was not found
     */
    private static String replaceStringKey(String json, String key,
                                           String newValue) {
        // Find "key" as a quoted token, then the following : and quoted value.
        // Scanning quote-anchored avoids matching inside a longer key name.
        int at = 0;
        while (true) {
            int q = json.indexOf('"', at);
            if (q < 0) return null;
            int keyEnd = json.indexOf('"', q + 1);
            if (keyEnd < 0) return null;

            if (json.regionMatches(q + 1, key, 0, key.length())
                    && keyEnd == q + 1 + key.length()) {
                // The token matches exactly — now it must be followed by the
                // JSON punctuation "key": and a string value.
                int i = keyEnd + 1;
                while (i < json.length() && isSpace(json.charAt(i))) i++;
                if (i < json.length() && json.charAt(i) == ':') {
                    i++;
                    while (i < json.length() && isSpace(json.charAt(i))) i++;
                    if (i < json.length() && json.charAt(i) == '"') {
                        int valStart = i + 1;
                        // The opening quote is at (valStart-1) and the
                        // closing quote is at (valEnd-1). Preserve both by
                        // splicing strictly between them; splicing from
                        // valStart to valEnd would eat the closing quote and
                        // corrupt the JSON.
                        int valEnd = findValueEnd(json, valStart);
                        if (valEnd > valStart) {
                            return json.substring(0, valStart)
                                    + jsonString(newValue)
                                    + json.substring(valEnd - 1);
                        }
                    }
                }
            }
            at = keyEnd + 1;
        }
    }

    /** JSON-encodes a string value: surrounds with quotes, escapes the four
     * required characters plus a literal newline/tab. */
    private static String jsonString(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                default:   sb.append(c);      break;
            }
        }
        return sb.toString();
    }

    /** Finds the closing quote of a JSON string value, honouring backslash
     * escapes. Returns the index just past the closing quote, or -1 if the
     * string is unterminated. */
    private static int findValueEnd(String json, int start) {
        int i = start;
        int n = json.length();
        while (i < n) {
            char c = json.charAt(i);
            if (c == '"') return i + 1;
            if (c == '\\') {
                i += 2;      // skip the escaped char
                continue;
            }
            i++;
        }
        return -1;
    }

    private static boolean isSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    /** A character that can appear in an unquoted JSON number. */
    private static boolean isNumberChar(char c) {
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.'
                || c == 'e' || c == 'E';
    }
}
