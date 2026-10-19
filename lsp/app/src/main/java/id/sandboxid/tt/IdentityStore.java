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

    /**
     * Key that authenticates control messages to a <i>running</i> target.
     *
     * <p>It has to survive persona rotation: the process that must be told to
     * exit is running with the previous persona, so a per-persona secret would
     * be rejected by the very process it is meant for (observed: the second
     * reset's RESTART broadcast was dropped, and the pid survived). Generated
     * once and copied into every persona, so old and new generations agree.
     *
     * <p>Still secret: it lives only in LSPosed's prefs directory, mode
     * {@code rwx--x--x}, readable by this module and the target.
     */
    static final String KEY_RESTART = "restart_key";

    private IdentityStore() {}

    /**
     * Returns the stable restart key, creating it on first use.
     *
     * <p>Called from the module's own process before writing a persona, so the
     * value is in the blob the target reads at start.
     */
    static String restartKey(Context ctx) {
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String k = sp.getString(KEY_RESTART, null);
        if (k != null && k.length() >= 16) return k;

        StringBuilder sb = new StringBuilder(32);
        java.util.Random r = new java.security.SecureRandom();
        for (int i = 0; i < 32; i++) {
            sb.append(Character.forDigit(r.nextInt(16), 16));
        }
        k = sb.toString();
        sp.edit().putString(KEY_RESTART, k).commit();
        return k;
    }

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
        // The restart key travels with the persona so the target process can
        // authenticate control messages from this app. It is copied, not
        // regenerated: a process running the previous persona must still
        // accept a request that was issued after the rotation.
        id.put(KEY_RESTART, restartKey(ctx));

        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> e : id.entrySet()) {
            sb.append(e.getKey()).append('=')
              .append(e.getValue() == null ? "" : e.getValue()).append('\n');
        }
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        // commit(), not apply(): apply() returns before the XML is on disk, so
        // the chmod below would race it and lose — the file would be written
        // afterwards with the default 0600 and the target could not read it.
        sp.edit()
          .putString(KEY_PERSONA, sb.toString())
          .putLong(KEY_VERSION, System.currentTimeMillis())
          .commit();

        // The file is written with MODE_PRIVATE (0600), but the reader is a
        // *different* uid: XSharedPreferences runs inside TikTok's process
        // (u0_aXXX of the target), not this module's. Without this the target
        // silently sees "no persona" and the module keeps spoofing with
        // whatever it last managed to read — which looks exactly like the
        // hooks being broken.
        //
        // Resolve the file through the SharedPreferences implementation rather
        // than guessing a path: with xposedsharedprefs declared, LSPosed
        // redirects this app's prefs to /data/misc/<uuid>/prefs/<pkg>/, so
        // <dataDir>/shared_prefs/<name>.xml does not exist and a chmod there
        // silently does nothing.
        try {
            // SharedPreferencesImpl has no public getFile() on this platform
            // (NoSuchMethodException was observed), but it does hold the path
            // in mFile. Walk the hierarchy for the field rather than the
            // method so this survives either shape.
            java.io.File f = null;
            for (Class<?> k = sp.getClass(); k != null && f == null;
                 k = k.getSuperclass()) {
                try {
                    java.lang.reflect.Field mf = k.getDeclaredField("mFile");
                    mf.setAccessible(true);
                    f = (java.io.File) mf.get(sp);
                } catch (NoSuchFieldException ignored) {
                }
            }
            if (f != null) {
                java.io.File dir = f.getParentFile();
                if (dir != null) {
                    dir.setExecutable(true, false);
                    dir.setReadable(true, false);
                }
                f.setReadable(true, false);
                android.util.Log.i("SandboxID-TT", "persona file " + f
                        + " readable=" + f.canRead());
            } else {
                android.util.Log.w("SandboxID-TT", "persona file path unknown");
            }
        } catch (Throwable t) {
            android.util.Log.w("SandboxID-TT", "persona chmod failed: " + t);
        }
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
