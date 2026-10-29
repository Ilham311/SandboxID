package id.sandboxid.tt;

/**
 * Pins the OEM class-deny table and its dispatch, without a device.
 *
 * <p>The deny hook has now failed twice in ways that looked like success: first
 * by throwing from the callback (XposedBridge swallows it and runs the original
 * anyway — see {@code HookSemanticsChk}), then by hooking only
 * {@code Class.forName} while the app probed through
 * {@code ClassLoader.loadClass}. Both failures were invisible in the log.
 *
 * <p>So this test asserts the outcome, not the installation: for each entry
 * point in {@link DenyList#TARGETS}, a denied name must make the modelled call
 * fail with {@code ClassNotFoundException}, and a name that is not denied must
 * reach the original.
 */
public final class DenyListTest {

    private static int pass = 0, fail = 0;

    /**
     * Models XposedBridge.handleHookedMethod's before-callback loop, then the
     * one thing that matters: did the original run?
     */
    private static final class Call {
        Object result;
        Throwable thrown;

        Object run(DenyList.Target t, Object[] args, String[] deny,
                   Object original) {
            de.robv.android.xposed.XC_MethodHook.MethodHookParam p =
                    new de.robv.android.xposed.XC_MethodHook.MethodHookParam();
            p.args = args;
            // The callback body from TelephonyDrmHook.hookDenyClasses, inline:
            // the point of the test is that this body's decision, not the
            // hooking call, is what makes the method fail.
            String want = DenyList.nameAt(p.args, t);
            if (want != null && DenyList.isDenied(want, deny)) {
                p.setThrowable(new ClassNotFoundException(want));
            }
            if (p.hasThrowable()) {
                thrown = p.getThrowable();
                return null;
            }
            return original;
        }
    }

    public static void main(String[] a) {
        String[] deny = {"miui.os.Build", "com.oplus.os.OplusBuild"};

        // ---- both entry points must exist -------------------------------
        check("two deny targets", 2, DenyList.TARGETS.length);
        check("forName is hooked", true,
                has("java.lang.Class", "forName"));
        check("loadClass is hooked", true,
                has("java.lang.ClassLoader", "loadClass"));

        // ---- every target denies, and only what it should ---------------
        for (DenyList.Target t : DenyList.TARGETS) {
            Object[] deniedArgs = argsFor(t, "miui.os.Build");
            Call c = new Call();
            Object r = c.run(t, deniedArgs, deny, "REAL_CLASS");
            check(t + ": denied class does not load", null, r);
            check(t + ": throws ClassNotFoundException", true,
                    c.thrown instanceof ClassNotFoundException);
            check(t + ": message names the class", "miui.os.Build",
                    c.thrown == null ? null : c.thrown.getMessage());

            // A persona that IS Xiaomi keeps miui.os.Build: the deny list is
            // derived from the persona, so for such a persona this name is
            // simply absent from it.
            Call keep = new Call();
            Object kr = keep.run(t, deniedArgs, new String[] {"com.oplus.os.OplusBuild"},
                    "REAL_CLASS");
            check(t + ": class not in the list still loads", "REAL_CLASS", kr);
            check(t + ": no exception for a kept class", null, keep.thrown);

            // Exact match only: a prefix must not be refused, or the app sees a
            // failure where a real device answers.
            Call prefix = new Call();
            Object pr = prefix.run(t, argsFor(t, "miui.os.BuildX"), deny,
                    "REAL_CLASS");
            check(t + ": prefix is not a match", "REAL_CLASS", pr);

            // Non-String and missing arguments must pass through untouched.
            Call nul = new Call();
            check(t + ": null name passes through", "REAL_CLASS",
                    nul.run(t, argsFor(t, null), deny, "REAL_CLASS"));
            Call shortArgs = new Call();
            check(t + ": missing args pass through", "REAL_CLASS",
                    shortArgs.run(t, new Object[0], deny, "REAL_CLASS"));
        }

        // ---- the table itself -------------------------------------------
        check("isDenied exact", true, DenyList.isDenied("miui.os.Build", deny));
        check("isDenied prefix", false, DenyList.isDenied("miui.os.BuildX", deny));
        check("isDenied non-String", false, DenyList.isDenied(Integer.valueOf(1), deny));
        check("isDenied null list", false, DenyList.isDenied("miui.os.Build", null));
        check("nameAt out of range", null,
                DenyList.nameAt(new Object[0], DenyList.TARGETS[0]));
        check("nameAt null args", null,
                DenyList.nameAt(null, DenyList.TARGETS[0]));

        System.out.println();
        System.out.println(pass + " checks, " + fail + " failed");
        System.out.println(fail == 0 ? "ALL PASS" : "FAILED");
        if (fail != 0) System.exit(1);
    }

    private static boolean has(String cls, String m) {
        for (DenyList.Target t : DenyList.TARGETS) {
            if (t.className.equals(cls) && t.methodName.equals(m)) return true;
        }
        return false;
    }

    /** Builds an args[] of the right length with {@code name} at argIndex. */
    private static Object[] argsFor(DenyList.Target t, String name) {
        Object[] args = new Object[t.paramTypes.length];
        args[t.argIndex] = name;
        return args;
    }

    private static void check(String what, Object want, Object got) {
        boolean ok = want == null ? got == null : want.equals(got);
        if (ok) { pass++; }
        else {
            fail++;
            System.out.println("FAIL " + what + ": want <" + want + "> got <"
                    + got + ">");
        }
    }
}
