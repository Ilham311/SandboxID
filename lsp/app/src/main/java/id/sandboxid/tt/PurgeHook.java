package id.sandboxid.tt;

import android.content.pm.ApplicationInfo;
import android.util.Log;

import java.io.File;
import java.util.Map;

/**
 * Removes the identity stores TikTok has already written, from inside its own
 * process, before the app reads them.
 *
 * <h3>Why this exists</h3>
 *
 * <p>The register server binds the pair <b>(openudid, install_id cookie)</b> the
 * first time it sees it, permanently. Measured against the live endpoint with
 * curl, one field changed per request:
 *
 * <pre>
 *   fresh openudid, no cookie            -> NEW did, new_user:1
 *   fresh openudid, Cookie: install_id=X -> OLD did, new_user:0
 *   openudid already sent once           -> OLD did, new_user:0
 * </pre>
 *
 * <p>A new openudid is therefore necessary but not sufficient: the app has to
 * start without a stored {@code install_id}. {@link KevaHook} and
 * {@link CookieHook} filter the Java read path, but part of the cookie path is
 * native and sits below any Xposed hook, so a rotation alone returns the did
 * from the previous launch.
 *
 * <p>{@code pm clear} fixes this and is what {@link ResetFlow} runs when root is
 * available. This class is the same operation without root: the module runs
 * inside TikTok's uid, so it can delete TikTok's own files. It runs from
 * {@code handleLoadPackage}, which LSPosed calls <b>before</b> the app's
 * {@code Application.onCreate}, so the stores are gone before KEVA or TTNet
 * opens them.
 *
 * <h3>The marker</h3>
 *
 * <p>Purging on every launch would re-register every launch — a new did each
 * time, and any logged-in state lost. So a marker file inside the target's data
 * dir records which persona has already been purged, keyed by
 * {@code APPLOG_EPOCH} (fresh on every Generate, stable for the life of the
 * persona). A launch purges only when the marker does not match.
 *
 * <h3>What is removed, and why each one</h3>
 *
 * <table>
 *   <tr><td>{@code shared_prefs/applog_stats.xml}</td>
 *       <td>holds {@code install_id}/{@code device_id} — the values TTNet builds
 *           the cookie from</td></tr>
 *   <tr><td>{@code files/keva/repo/ug_install_settings_pref_i18n/}</td>
 *       <td>{@code bd_did}, {@code device_id}, {@code install_id},
 *           {@code register_time}; the register replay gate reads it</td></tr>
 *   <tr><td>{@code files/keva/repo/pns_common_id_mini/}</td>
 *       <td>the push service's copy of the same ids</td></tr>
 *   <tr><td>{@code files/keva/repo/multi_process_config/}</td>
 *       <td>{@code {"device_id":…,"install_id":…,"openudid":…}} shared across
 *           processes</td></tr>
 *   <tr><td>{@code app_webview/Default/Cookies}</td>
 *       <td>Chromium cookie DB; {@code install_id} is set per TikTok domain</td></tr>
 *   <tr><td>{@code files/tt_net_config.config}</td>
 *       <td>TTNet's own store, keyed {@code device_id}</td></tr>
 * </table>
 *
 * <p>{@code files/bd_setting/} is absent on 47.1.4 (checked on-device) and needs
 * no handling; the list is deliberately the stores that were <i>observed</i> to
 * contain the ids, not a guess at what might.
 */
final class PurgeHook {

    private static final String TAG = "SandboxID-TT";

    /** Persona key that identifies one generation. */
    private static final String PERSONA_GEN = "APPLOG_EPOCH";

    /** Marker inside the target's data dir. */
    private static final String MARKER = "files/.sbx_purged";

    private PurgeHook() {}

    /**
     * Purges once per persona generation.
     *
     * @param info the target's ApplicationInfo (carries {@code dataDir})
     * @param id   the persona in effect
     * @return true when a purge ran on this launch
     */
    static boolean apply(ApplicationInfo info, Map<String, String> id) {
        return purge(info, id, false);
    }

    /**
     * Purges regardless of the marker.
     *
     * <p>Used by {@link SelfRestart}: the process is about to exit so the next
     * launch can register from nothing, and the marker would otherwise say
     * "already purged for this persona" and skip the delete.
     */
    static boolean applyForced(ApplicationInfo info, Map<String, String> id) {
        return purge(info, id, true);
    }

    private static boolean purge(ApplicationInfo info, Map<String, String> id,
                                 boolean force) {
        if (info == null || info.dataDir == null || id == null) return false;
        String gen = id.get(PERSONA_GEN);
        if (gen == null || gen.isEmpty()) {
            Log.w(TAG, "PurgeHook: persona has no " + PERSONA_GEN + ", skipping");
            return false;
        }

        File data = new File(info.dataDir);
        File marker = new File(data, MARKER);
        String seen = read(marker);
        if (!force && gen.equals(seen)) {
            Log.i(TAG, "PurgeHook: stores already purged for this persona");
            return false;
        }

        int n = 0;
        n += delete(new File(data, "shared_prefs/applog_stats.xml"));
        n += deleteTree(new File(data, "files/keva/repo/ug_install_settings_pref_i18n"));
        n += deleteTree(new File(data, "files/keva/repo/pns_common_id_mini"));
        n += deleteTree(new File(data, "files/keva/repo/multi_process_config"));
        n += deleteTree(new File(data, "files/keva/repo/push_multi_process_config"));
        n += delete(new File(data, "app_webview/Default/Cookies"));
        n += delete(new File(data, "app_webview/Default/Cookies-journal"));
        n += delete(new File(data, "files/tt_net_config.config"));

        write(marker, gen);
        Log.i(TAG, "PurgeHook: removed " + n + " identity store(s) for persona "
                + gen + " — the next register starts from no device");
        return true;
    }

    /** Deletes one file or directory tree; returns how many paths went away. */
    private static int deleteTree(File f) {
        if (f == null || !f.exists()) return 0;
        if (f.isDirectory()) {
            int n = 0;
            File[] kids = f.listFiles();
            if (kids != null) {
                for (File k : kids) n += deleteTree(k);
            }
            return n + (f.delete() ? 1 : 0);
        }
        return f.delete() ? 1 : 0;
    }

    private static int delete(File f) {
        return f != null && f.exists() && f.delete() ? 1 : 0;
    }

    private static String read(File f) {
        try {
            if (!f.exists()) return null;
            byte[] b = new byte[(int) Math.min(f.length(), 64)];
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            try {
                int off = 0, r;
                while (off < b.length && (r = in.read(b, off, b.length - off)) > 0) {
                    off += r;
                }
                return new String(b, 0, off, "UTF-8").trim();
            } finally {
                in.close();
            }
        } catch (Throwable t) {
            return null;
        }
    }

    private static void write(File f, String s) {
        try {
            File dir = f.getParentFile();
            if (dir != null && !dir.exists()) dir.mkdirs();
            java.io.FileOutputStream out = new java.io.FileOutputStream(f);
            try {
                out.write(s.getBytes("UTF-8"));
            } finally {
                out.close();
            }
        } catch (Throwable t) {
            Log.w(TAG, "PurgeHook: cannot write marker " + f + ": " + t);
        }
    }
}
