package id.sandboxid.tt;

import android.content.Context;
import android.content.SharedPreferences;

import de.robv.android.xposed.XSharedPreferences;

import java.util.HashMap;
import java.util.Map;

/**
 * Reads the persona the module's own UI wrote, from inside the target process.
 *
 * <p><b>This is what makes the variant standalone.</b> The earlier revision read
 * a persona that the <i>Zygisk module's</i> root side had to publish to a
 * world-readable path — so the variant was useless without that other module.
 * It now reads through LSPosed's own preference-redirect:
 *
 * <ul>
 *   <li>the module's {@code MainActivity} writes the persona with a normal
 *       {@code SharedPreferences} editor;</li>
 *   <li>LSPosed stores it under {@code /data/misc/<uuid>/prefs/id.sandboxid.tt/}
 *       (created mode {@code rwx--x--x} and chowned to <b>this app's</b> uid —
 *       see {@code ConfigManager.getPrefsPath} in the LSPosed source);</li>
 *   <li>here, in the target process, {@link XSharedPreferences} reads that same
 *       file. The manifest declares {@code xposedsharedprefs}, which is what
 *       routes the lookup there instead of the target's own data dir.</li>
 * </ul>
 *
 * <p>No root, no {@code /data/adb}, no world-readable {@code /data/local/tmp}
 * file for every app on the device to read, and no dependency on the Zygisk
 * module at all.
 *
 * <p><b>Why read-only is fine.</b> {@link XSharedPreferences#edit()} throws by
 * design; the target process is not allowed to change the persona. Rotation
 * happens in the module's own UI, and this class re-reads on every load.
 */
final class IdentityStore {

    /** The preference file MainActivity writes. */
    static final String PREFS = "sandboxid_persona";

    static final String KEY_PERSONA = "persona_kv";
    private static final String KEY_VERSION = "persona_version";

    private IdentityStore() {}

    /**
     * @return the parsed persona, or an empty map if the UI has not generated
     *         one yet (or this LSPosed build predates the redirect).
     */
    static Map<String, String> load() {
        Map<String, String> out = new HashMap<>();
        XSharedPreferences xs;
        try {
            xs = new XSharedPreferences("id.sandboxid.tt", PREFS);
        } catch (Throwable t) {
            // XSharedPreferences only resolves inside a process LSPosed injected
            // us into; in any other context (e.g. the module's own app) it is
            // not the right reader and MainActivity handles storage itself.
            return out;
        }
        try {
            if (!xs.getFile().canRead()) return out;
        } catch (Throwable t) {
            return out;
        }
        String blob = xs.getString(KEY_PERSONA, null);
        if (blob == null) return out;
        parse(blob, out);
        return out;
    }

    /** Used by the module's own UI to store the persona it generated. */
    static void save(Context ctx, Map<String, String> id) {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> e : id.entrySet()) {
            sb.append(e.getKey()).append('=')
              .append(e.getValue() == null ? "" : e.getValue()).append('\n');
        }
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        sp.edit()
          .putString(KEY_PERSONA, sb.toString())
          .putLong(KEY_VERSION, System.currentTimeMillis())
          .apply();
    }

    /** key=value lines, '#' comments — the same format parse_blob() accepts. */
    private static void parse(String blob, Map<String, String> out) {
        for (String line : blob.split("\n", -1)) {
            String t = line.trim();
            if (t.isEmpty() || t.charAt(0) == '#') continue;
            int eq = t.indexOf('=');
            if (eq < 0) continue;
            out.put(t.substring(0, eq).trim(), t.substring(eq + 1).trim());
        }
    }
}
