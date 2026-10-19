package id.sandboxid.tt;

import android.content.Context;
import android.content.Intent;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The one-click "new device" path: rotate the persona, wipe TikTok's data,
 * restore the permissions the wipe revoked, launch TikTok, then read back the
 * did the register server issued.
 *
 * <h3>Why the wipe is part of the button</h3>
 *
 * <p>A persona rotation on its own does not produce a new did. The register
 * server binds the pair <b>(openudid, install_id cookie)</b> the first time it
 * sees it, permanently — measured against the live endpoint with curl, one
 * field changed per request:
 *
 * <pre>
 *   fresh openudid, no cookie            -> NEW did, new_user:1
 *   fresh openudid, Cookie: install_id=X -> OLD did, new_user:0
 *   openudid already sent once           -> OLD did, new_user:0
 * </pre>
 *
 * <p>TikTok keeps that cookie in four places, two of which are written and read
 * below the Java layer: {@code shared_prefs/applog_stats.xml},
 * {@code files/keva/repo/ug_install_settings_pref_i18n/*.blk},
 * {@code files/tt_net_config.config} and {@code app_webview/Default/Cookies}.
 * {@link KevaHook} and {@link CookieHook} filter the Java read path, but the
 * native path still sends the cookie, so a rotation without a wipe is
 * immediately re-bound to the did from the previous launch. Verified: rotating
 * without {@code pm clear} returned the did minted seconds earlier.
 *
 * <p>{@code pm clear} is the only operation that removes all four, and it needs
 * root. Hence {@link RootShell}.
 *
 * <h3>Why permissions are restored</h3>
 *
 * <p>{@code pm clear} also revokes every runtime permission, and TikTok's first
 * launch then opens {@code GrantPermissionsActivity} instead of its own splash —
 * the app never reaches the code that registers. So the set granted before the
 * wipe is captured first and re-granted after. Non-runtime permissions are
 * rejected by {@code pm grant} and ignored, which is why the list is read from
 * the device rather than hard-coded: it stays correct across TikTok releases.
 */
final class ResetFlow {

    private static final String TAG = "SandboxID-TT";

    private static final String TARGET = SandboxIdTT.TARGET;

    /** {@code "    android.permission.CAMERA: granted=true"} */
    private static final Pattern GRANTED = Pattern.compile(
            "^\\s+([A-Za-z0-9_.]+): granted=true\\s*$", Pattern.MULTILINE);

    /** {@code <string name="device_id">7698223092862371334</string>} */
    private static final Pattern DID = Pattern.compile(
            "<string name=\"device_id\">(\\d+)</string>");
    private static final Pattern IID = Pattern.compile(
            "<string name=\"install_id\">(\\d+)</string>");

    /** Reported to the UI as each step runs. */
    interface Step {
        void on(String line);
    }

    private ResetFlow() {}

    /** The ids the register server issued, once it has answered. */
    static final class Issued {
        final String deviceId;
        final String installId;
        final String newUser;

        Issued(String deviceId, String installId) {
            this(deviceId, installId, null);
        }

        Issued(String deviceId, String installId, String newUser) {
            this.deviceId = deviceId;
            this.installId = installId;
            this.newUser = newUser;
        }

        boolean valid() {
            return deviceId != null && !"0".equals(deviceId);
        }
    }

    /**
     * Runs the whole reset. Blocking — call it off the UI thread.
     *
     * @return a human-readable report, one line per step
     */
    static String run(Context ctx, Step step) {
        StringBuilder log = new StringBuilder();

        // 1. New persona first. APPLOG_EPOCH changes with it, which is what
        //    tells PurgeHook in the target process that the identity stores
        //    must be removed on the next start.
        java.util.Map<String, String> id = PersonaGenerator.generate(ctx);
        if (id == null || id.isEmpty()) {
            String msg = "Gagal: pool device tidak terbaca dari APK (bug packaging).";
            step.on(msg);
            return log.append(msg).toString();
        }
        IdentityStore.save(ctx, id);
        report(step, log, "persona baru: " + id.get("BRAND") + " "
                + id.get("MODEL") + " openudid="
                + id.get(RegisterHook.PERSONA_OPENUDID));

        boolean root = RootShell.available();
        report(step, log, root ? "root tersedia" : "root tidak tersedia");

        // 2. With root, wipe the whole data dir. Without it, PurgeHook removes
        //    just the identity stores from inside TikTok's own process — same
        //    effect for the register call, and it needs no permission at all.
        if (root) {
            List<String> granted = readGranted();
            report(step, log, "permission tersimpan: " + granted.size());

            RootShell.Result clear = RootShell.run("pm clear " + TARGET, 30000);
            report(step, log, "pm clear: " + clear.describe());
            if (!clear.saidSuccess()) {
                report(step, log, "pm clear gagal — lanjut dengan purge in-process");
            } else {
                int restored = 0;
                for (String p : granted) {
                    if (RootShell.run("pm grant " + TARGET + " " + p, 5000).ok()) restored++;
                }
                report(step, log, "permission dipulihkan: " + restored
                        + "/" + granted.size());
            }
        } else {
            report(step, log, "store identitas akan dihapus oleh PurgeHook di "
                    + "dalam proses TikTok saat start berikutnya");
        }

        // 3. End the running TikTok, then launch it. Hooks install once per
        //    process, and PurgeHook only runs at start — so a reset while the
        //    app is alive rotates the persona and registers nothing. Verified:
        //    the pid survived killBackgroundProcesses across three resets, and
        //    only the launch after a root force-stop minted a did.
        //
        //    Without root the process has to end itself, which is what
        //    SelfRestart is for: the receiver installed in TikTok's process
        //    purges and calls killProcess on request, authenticated by the
        //    persona nonce. A root device uses force-stop, which is immediate
        //    and does not depend on the module being loaded.
        if (root) {
            RootShell.run("am force-stop " + TARGET, 10000);
        } else {
            // The key must be the one the *running* process holds, so it is
            // stable across persona rotations rather than per-persona.
            requestRestart(ctx, IdentityStore.restartKey(ctx));
        }
        sleep(2500);

        Intent i = ctx.getPackageManager().getLaunchIntentForPackage(TARGET);
        if (i == null) {
            String msg = "TikTok tidak terpasang.";
            step.on(msg);
            return log.append(msg).toString();
        }
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(i);
        report(step, log, "TikTok dibuka ulang (proses lama dimatikan)");
        // 4. Read back what the server issued. With root, the answer is already
        //    on disk; without it, the module's own process forwards the register
        //    response as a broadcast (see RegisterHook.report). Either way the
        //    value is the server's, not a local guess.
        Issued issued = root
                ? awaitIssued(25000)
                : awaitBroadcast(ctx, id.get(RegisterHook.PERSONA_NONCE), 25000);

        if (issued.valid()) {
            String msg = "did baru: " + issued.deviceId
                    + "  install_id: " + issued.installId
                    + (issued.newUser == null ? "" : "  new_user:" + issued.newUser);
            step.on(msg);
            log.append(msg);
        } else {
            String msg = "did belum terlihat dalam 25 detik. Kemungkinan TikTok "
                    + "tidak sempat dibuka (Android memblokir pembukaan activity "
                    + "dari latar belakang). Jalankan dari layar modul, atau "
                    + "buka TikTok manual sekarang — persona baru tetap terpakai.";
            step.on(msg);
            log.append(msg);
        }
        return log.toString();
    }

    /**
     * Waits for the did report the target process broadcasts.
     *
     * <p>Registered only for this module's package and checked against the
     * persona nonce, so a third-party app cannot make a failed reset look like
     * a success — it cannot read the nonce.
     */
    static Issued awaitBroadcast(Context ctx, String nonce, int timeoutMs) {
        final java.util.concurrent.CountDownLatch latch =
                new java.util.concurrent.CountDownLatch(1);
        final Issued[] box = new Issued[1];
        final String want = nonce == null ? "" : nonce;

        android.content.BroadcastReceiver rx =
                new android.content.BroadcastReceiver() {
                    @Override
                    public void onReceive(Context c, Intent i) {
                        if (i == null) return;
                        String got = i.getStringExtra(RegisterHook.EXTRA_NONCE);
                        if (want.isEmpty() || !want.equals(got)) {
                            return;
                        }
                        String did = i.getStringExtra(RegisterHook.EXTRA_DID);
                        if (did == null || "0".equals(did)) return;
                        box[0] = new Issued(did,
                                i.getStringExtra(RegisterHook.EXTRA_IID),
                                i.getStringExtra(RegisterHook.EXTRA_NEW));
                        latch.countDown();
                    }
                };

        boolean registered = false;
        try {
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                // Context.RECEIVER_EXPORTED, spelled out: the android.jar used
                // to compile this module predates the constant. It must be
                // exported — the sender is TikTok's process, a different uid,
                // and RECEIVER_NOT_EXPORTED would drop its broadcast. The
                // nonce is what makes that safe: any app may send this action,
                // but only the one holding the persona can produce a report the
                // UI accepts.
                ctx.registerReceiver(rx,
                        new android.content.IntentFilter(RegisterHook.ACTION_ISSUED),
                        0x2);
            } else {
                ctx.registerReceiver(rx,
                        new android.content.IntentFilter(RegisterHook.ACTION_ISSUED));
            }
            registered = true;
            latch.await(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS);
        } catch (Throwable t) {
            // Fall through: the caller reports "did not appear", which is the
            // truth when the report cannot be received.
        } finally {
            if (registered) {
                try {
                    ctx.unregisterReceiver(rx);
                } catch (Throwable ignored) {
                }
            }
        }
        return box[0] == null ? new Issued(null, null, null) : box[0];
    }

    /**
     * Reads {@code device_id}/{@code install_id} out of TikTok's AppLog store
     * until a non-zero did appears or the deadline passes.
     *
     * <p>This is the register response as the app persisted it, so it is the
     * server's own answer rather than a local guess — the same value the app
     * will send on every later request.
     */
    static Issued awaitIssued(int timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        Issued last = new Issued(null, null);
        while (System.currentTimeMillis() < deadline) {
            String xml = RootShell.run("cat /data/data/" + TARGET
                    + "/shared_prefs/applog_stats.xml", 4000).out;
            String did = first(DID, xml);
            String iid = first(IID, xml);
            if (did != null) {
                last = new Issued(did, iid);
                if (last.valid()) return last;
            }
            try {
                Thread.sleep(1500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        return last;
    }

    /** Package names whose permissions are currently granted. */
    private static List<String> readGranted() {
        List<String> out = new ArrayList<>();
        String d = RootShell.run("dumpsys package " + TARGET, 20000).out;
        Matcher m = GRANTED.matcher(d);
        while (m.find()) {
            String p = m.group(1);
            // Only runtime permissions can be re-granted; keeping the whole
            // list is harmless (pm grant rejects the rest) but noisy, so the
            // obvious non-runtime prefixes are dropped.
            if (p.startsWith("android.permission.") || p.contains(".")) {
                out.add(p);
            }
        }
        return out;
    }

    private static String first(Pattern p, String s) {
        Matcher m = p.matcher(s);
        return m.find() ? m.group(1) : null;
    }

    /**
     * Stops the target's background processes.
     *
     * <p>Uses {@code ActivityManager.killBackgroundProcesses}, which works
     * without root. It is a no-op for a package in the foreground, so the
     * caller must already be showing its own screen — the module's UI is.
     */
    private static void kill(Context ctx, String pkg) {
        try {
            android.app.ActivityManager am = (android.app.ActivityManager)
                    ctx.getSystemService(Context.ACTIVITY_SERVICE);
            if (am != null) am.killBackgroundProcesses(pkg);
        } catch (Throwable ignored) {
        }
    }

    /**
     * Asks the target's own process to purge and exit ({@link SelfRestart}).
     *
     * <p>Falls back to {@code killBackgroundProcesses} when no process answers
     * — that call is a no-op for a running TikTok but does stop one that is
     * only cached, and it costs nothing to try.
     */
    private static void requestRestart(Context ctx, String nonce) {
        if (nonce == null || nonce.isEmpty()) {
            kill(ctx, TARGET);
            return;
        }
        try {
            Intent i = new Intent(SelfRestart.ACTION);
            i.setPackage(TARGET);
            i.putExtra(SelfRestart.EXTRA_NONCE, nonce);
            i.addFlags(Intent.FLAG_RECEIVER_FOREGROUND);
            ctx.sendBroadcast(i);
        } catch (Throwable ignored) {
        }
        kill(ctx, TARGET);
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static void report(Step step, StringBuilder log, String line) {
        log.append(line).append('\n');
        step.on(line);
    }
}
