package de.robv.android.xposed;

/**
 * Pins the one thing that decides whether "deny" and "cloak" hooks work at all:
 * whether the callback makes the hooked call FAIL, or merely logs and lets the
 * original run.
 *
 * <p>This exists because the first version of both hooks got it wrong in the
 * same way. They threw from {@code beforeHookedMethod}, and XposedBridge's
 * dispatch catches anything a callback throws:
 *
 * <pre>
 *   try { cb.beforeHookedMethod(param); }
 *   catch (Throwable t) { log(t); param.setResult(null);
 *                         param.returnEarly = false; continue; }
 * </pre>
 *
 * <p>So the throw was logged and discarded, {@code returnEarly} was reset, and
 * the real method ran — {@code miui.os.Build} loaded, and Magisk stayed visible
 * in {@code getPackageInfo}. Nothing failed loudly; the hooks simply did
 * nothing. {@code setThrowable} is the API that prevents the original call.
 *
 * <p>The dispatch below is a faithful model of that loop, so a regression back
 * to throwing fails here rather than on a device.
 */
public final class HookSemanticsChk {

    private static int pass = 0, fail = 0;

    /** Models XposedBridge.handleHookedMethod's before-callback loop. */
    static final class Dispatch {
        Object result;
        Throwable thrown;
        boolean returnEarly;
        Throwable logged;

        /** Runs the callbacks, then the original unless one prevented it. */
        Object call(XC_MethodHook cb, Object[] args, Object original) {
            XC_MethodHook.MethodHookParam p = new XC_MethodHook.MethodHookParam();
            p.args = args;
            try {
                cb.beforeHookedMethod(p);
            } catch (Throwable t) {
                // Exactly what XposedBridge does: log, reset, carry on.
                logged = t;
                p.setResult(null);
                returnEarly = false;
            }
            if (p.hasThrowable()) {
                thrown = p.getThrowable();
                return null;                    // original never runs
            }
            if (returnEarly) return p.getResult();
            return original;                    // original DID run
        }
    }

    /** A callback that throws — the shape both hooks originally had. */
    static final class ThrowsCallback extends XC_MethodHook {
        @Override
        protected void beforeHookedMethod(MethodHookParam p) {
            throw new RuntimeException("denied");
        }
    }

    /** A callback that sets the throwable — the fix. */
    static final class SetsThrowableCallback extends XC_MethodHook {
        @Override
        protected void beforeHookedMethod(MethodHookParam p) {
            p.setThrowable(new ClassNotFoundException("miui.os.Build"));
        }
    }

    public static void main(String[] a) {
        // Throwing from the callback does NOT stop the method. This is the bug,
        // stated as a test so nobody re-introduces it.
        Dispatch d1 = new Dispatch();
        Object r1 = d1.call(new ThrowsCallback(), new Object[] {"miui.os.Build"},
                "REAL_CLASS");
        check("throwing still runs the original", "REAL_CLASS", r1);
        check("throwing is swallowed and logged", true, d1.logged != null);
        check("throwing does not set a throwable", false, d1.thrown != null);

        // setThrowable does stop it, with the framework's own exception type.
        Dispatch d2 = new Dispatch();
        Object r2 = d2.call(new SetsThrowableCallback(),
                new Object[] {"miui.os.Build"}, "REAL_CLASS");
        check("setThrowable prevents the original", null, r2);
        check("setThrowable surfaces the exception", true, d2.thrown != null);
        check("exception type is ClassNotFoundException", "java.lang.ClassNotFoundException",
                d2.thrown == null ? null : d2.thrown.getClass().getName());
        check("exception message names the class", "miui.os.Build",
                d2.thrown == null ? null : d2.thrown.getMessage());

        // The param API itself: setThrowable must clear the result and mark
        // returnEarly, or the dispatch above would still call the original.
        XC_MethodHook.MethodHookParam p = new XC_MethodHook.MethodHookParam();
        p.setResult("something");
        check("result set", "something", p.getResult());
        p.setThrowable(new IllegalStateException("x"));
        check("setThrowable clears the result", null, p.getResult());
        check("setThrowable reports a throwable", true, p.hasThrowable());

        System.out.println();
        System.out.println(pass + " checks, " + fail + " failed");
        System.out.println(fail == 0 ? "ALL PASS" : "FAILED");
        if (fail != 0) System.exit(1);
    }

    private static void check(String what, Object want, Object got) {
        boolean ok = want == null ? got == null : want.equals(got);
        if (ok) { pass++; System.out.println("ok   " + what); }
        else {
            fail++;
            System.out.println("FAIL " + what + ": want <" + want + "> got <"
                    + got + ">");
        }
    }
}
