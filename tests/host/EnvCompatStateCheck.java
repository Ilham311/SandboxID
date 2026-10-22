/**
 * Pins the exception contract of the L3 callback
 * ({@code jni/SandboxIDHook.java}, compiled into DEX and shipped as
 * {@code EnvCompatState}).
 *
 * <p><b>Why this exists.</b> {@code invokeOriginal} calls the backup through
 * reflection, so whatever the real method threw arrives wrapped in
 * {@link java.lang.reflect.InvocationTargetException}. The first version
 * unwrapped only {@code RuntimeException} and {@code Error} and turned
 * everything else into {@code new RuntimeException(cause)}. That silently
 * changed the type of <i>checked</i> exceptions.
 *
 * <p>The failure it caused, seen on a real device: {@code LoadedApk.rewriteRValues}
 * probes for an obfuscated {@code <pkg>.R} class and does
 * {@code catch (ClassNotFoundException e) { ...; return; }} — a miss there is
 * normal. Because the hook rethrew the miss as a {@code RuntimeException}, the
 * catch did not match, {@code makeApplicationInner} wrapped it in
 * "Unable to instantiate application ...", and every target app died during
 * startup. The type has to survive exactly, checked or not.
 *
 * <p>JDK-only and device-free by design: {@code EnvCompatState} has no Android
 * dependency, so this runs on a plain JDK in CI. run.sh compiles a copy of the
 * source under the package directory javac requires.
 */
public class EnvCompatStateCheck {

    static int fails = 0;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "ok  " : "FAIL") + " " + name);
        if (!ok) fails++;
    }

    /** Stand-ins for the hooked framework methods. */
    public static class Target {
        public Object load(String n) throws ClassNotFoundException {
            throw new ClassNotFoundException("Didn't find class \"" + n + "\"");
        }

        public Object ok(String n) {
            return "class " + n;
        }

        public Object boom(String n) {
            throw new IllegalStateException("boom");
        }

        public Object error(String n) {
            throw new StackOverflowError("err");
        }
    }

    static java.lang.reflect.Method m(String name) throws Exception {
        return Target.class.getDeclaredMethod(name, String.class);
    }

    static androidx.core.os.EnvCompatState observe(String backup) throws Exception {
        androidx.core.os.EnvCompatState s = new androidx.core.os.EnvCompatState();
        s.mode = androidx.core.os.EnvCompatState.M_OBSERVE;
        s.isStatic = false;
        s.backup = m(backup);
        return s;
    }

    public static void main(String[] a) throws Exception {
        Target t = new Target();
        androidx.core.os.EnvCompatState.watch = null;

        // The regression: a checked CNFE must come out as CNFE. handle() has no
        // throws clause (athrow in bytecode does not care), so the static checker
        // rejects `catch (ClassNotFoundException)` here; catch Throwable and assert
        // the runtime type, which is what ART's handler in rewriteRValues sees.
        try {
            observe("load").handle(new Object[]{ t, "com.x.R" });
            check("CNFE is thrown", false);
        } catch (Throwable e) {
            check("CNFE keeps its type (not wrapped in RuntimeException)",
                    e instanceof ClassNotFoundException);
            check("CNFE message preserved",
                    String.valueOf(e.getMessage()).contains("com.x.R"));
        }

        // A normal return must still come through untouched.
        check("normal return preserved",
                "class Foo".equals(observe("ok").handle(new Object[]{ t, "Foo" })));

        // Unchecked exceptions keep their own type too (this already worked, but
        // the unwrap is shared, so a change here would be a regression).
        try {
            observe("boom").handle(new Object[]{ t, "x" });
            check("ISE is thrown", false);
        } catch (Throwable e) {
            check("ISE keeps its type", e instanceof IllegalStateException);
        }

        try {
            observe("error").handle(new Object[]{ t, "x" });
            check("Error is thrown", false);
        } catch (Throwable e) {
            check("Error keeps its type", e instanceof StackOverflowError);
        }

        // M_ZERO_NUMERIC calls the original before deciding, on the same unwrap.
        androidx.core.os.EnvCompatState z = observe("load");
        z.mode = androidx.core.os.EnvCompatState.M_ZERO_NUMERIC;
        z.keyArgIndex = 1;
        try {
            z.handle(new Object[]{ t, "com.x.R" });
            check("M_ZERO_NUMERIC propagates CNFE", false);
        } catch (Throwable e) {
            check("M_ZERO_NUMERIC propagates CNFE unwrapped",
                    e instanceof ClassNotFoundException);
        }

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        if (fails != 0) System.exit(1);
    }
}
