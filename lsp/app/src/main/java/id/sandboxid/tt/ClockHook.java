package id.sandboxid.tt;

import android.os.SystemClock;
import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedHelpers;

/**
 * Makes the device's uptime consistent with the persona's claimed age.
 *
 * <p>Port of {@code install_uptime_hook()} in {@code jni/uptime_hook.cpp}. The
 * native side PLT-hooks {@code clock_gettime(CLOCK_BOOTTIME)} in libutils.so /
 * libandroid_runtime.so; the Java-reachable equivalent is
 * {@link SystemClock#elapsedRealtime()} / {@link SystemClock#elapsedRealtimeNanos()},
 * which are backed by that same clock.
 *
 * <p>The offset arithmetic is copied, not reinvented:
 * <pre>   offset = persona_age_seconds - real_uptime_seconds_now</pre>
 * so a reader sees exactly the persona's age immediately and it then ages at the
 * real rate. An earlier C++ revision <i>added</i> the persona age outright, which
 * reports {@code real_uptime + persona_age} — never the persona's age, and wrong
 * by the whole real uptime on a device that has been up for days. The
 * subtraction is the fix; do not "simplify" it to an addition.
 */
final class ClockHook {

    private static final String TAG = "SandboxID-TT";

    private static volatile long offsetMs = 0L;

    private ClockHook() {}

    static void apply(ClassLoader cl, Map<String, String> id) {
        String us = id.get("UPTIME_SECONDS");
        if (us == null || us.isEmpty()) return;

        long secs;
        try {
            secs = Long.parseLong(us.trim());
        } catch (NumberFormatException ignored) {
            // sbx_parse_longlong rejects trailing garbage ("100abc") and
            // out-of-range; do the same rather than install a bogus offset.
            return;
        }
        if (secs <= 0) return;

        // Solve  real_uptime + offset == persona_age  (ms granularity here).
        offsetMs = secs * 1000L - SystemClock.elapsedRealtime();
        if (offsetMs == 0L) return;

        XposedHelpers.findAndHookMethod(SystemClock.class, "elapsedRealtime",
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult((Long) p.getResult() + offsetMs);
                    }
                });

        XposedHelpers.findAndHookMethod(SystemClock.class, "elapsedRealtimeNanos",
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult((Long) p.getResult() + offsetMs * 1_000_000L);
                    }
                });

        Log.d(TAG, "uptime offset applied: +" + offsetMs + "ms (persona age " + secs + "s)");
    }
}
