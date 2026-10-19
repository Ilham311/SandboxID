package id.sandboxid.tt;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.concurrent.TimeUnit;

/**
 * Runs a shell command through {@code su}, with a deadline.
 *
 * <p><b>Why this exists.</b> Rotating the persona is only half of what the
 * register server needs. It binds an openudid to the {@code install_id} cookie
 * the first time it sees the pair, permanently — verified on-device against the
 * live endpoint (curl, no app): a fresh openudid with no cookie returns a fresh
 * did with {@code new_user:1}, but the same openudid arriving with a stored
 * {@code install_id} cookie returns the did it was first bound to.
 *
 * <p>TikTok always sends its stored {@code install_id}, from
 * {@code shared_prefs/applog_stats.xml}, the KEVA repos and
 * {@code app_webview/Default/Cookies}. Blanking the Java read path (see
 * {@link KevaHook}, {@link CookieHook}) reduces but does not remove that: part
 * of the path is native and sits below any Xposed hook. {@code pm clear} is the
 * only thing that removes all of it, and it needs root.
 *
 * <p><b>Absence of root is a normal outcome, not an error.</b> KernelSU and
 * Magisk both require per-app approval; an unapproved call blocks on the
 * manager's prompt. That is why every call has a deadline and why the caller
 * falls back to a manual data clear rather than hanging the UI.
 */
final class RootShell {

    private RootShell() {}

    /** Outcome of one command. */
    static final class Result {
        final int code;
        final String out;

        Result(int code, String out) {
            this.code = code;
            this.out = out == null ? "" : out;
        }

        boolean ok() {
            return code == 0;
        }

        /** {@code true} when the command reported success in its own words. */
        boolean saidSuccess() {
            return out.toLowerCase(java.util.Locale.ROOT).contains("success");
        }

        String describe() {
            String s = out.trim();
            if (s.length() > 400) s = s.substring(0, 400) + "…";
            return "exit=" + code + (s.isEmpty() ? "" : " out=" + s);
        }
    }

    /** True when {@code su} answers as uid 0 within the deadline. */
    static boolean available() {
        Result r = run("id", 8000);
        return r.ok() && r.out.contains("uid=0");
    }

    /**
     * Runs {@code su -c cmd} and waits at most {@code timeoutMs}.
     *
     * <p>Output is drained on a separate thread: a pipe that fills while the
     * caller is blocked in {@code waitFor} would deadlock the command, and
     * {@code su} on an unapproved app never writes anything at all, so the
     * deadline has to come from {@code waitFor} rather than from EOF.
     */
    static Result run(String cmd, int timeoutMs) {
        Process p = null;
        final StringBuilder sb = new StringBuilder();
        try {
            p = new ProcessBuilder("su", "-c", cmd)
                    .redirectErrorStream(true)
                    .start();
            try {
                p.getOutputStream().close();
            } catch (Throwable ignored) {
            }

            final Process proc = p;
            Thread reader = new Thread(new Runnable() {
                public void run() {
                    try {
                        BufferedReader r = new BufferedReader(
                                new InputStreamReader(proc.getInputStream(), "UTF-8"));
                        String line;
                        while ((line = r.readLine()) != null) {
                            sb.append(line).append('\n');
                        }
                    } catch (Throwable ignored) {
                    }
                }
            }, "sbx-su-reader");
            reader.setDaemon(true);
            reader.start();

            boolean done = p.waitFor(timeoutMs, TimeUnit.MILLISECONDS);
            if (!done) {
                p.destroy();
                return new Result(-1, sb + "timeout after " + timeoutMs + " ms");
            }
            try {
                reader.join(2000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            return new Result(p.exitValue(), sb.toString());
        } catch (Throwable t) {
            return new Result(-1, sb + String.valueOf(t));
        } finally {
            if (p != null) {
                try {
                    p.destroy();
                } catch (Throwable ignored) {
                }
            }
        }
    }
}
