package id.sandboxid.tt;

/**
 * Exercises the GSF gate and cursor builder directly, without a framework.
 *
 * <p>These are the two pieces that decide whether the hook answers at all and
 * what it hands back. The overload wiring around them can only be checked on a
 * device (it needs {@code ContentResolver}), but a wrong gate or a malformed
 * cursor would make the hook silently useless on every device, so it is worth
 * pinning here.
 *
 * <p>Runs on a plain JDK against a stubbed {@code android.database.MatrixCursor}
 * supplied by the CI step — {@link GsfAnswer} reaches that class by name through
 * a {@code ClassLoader}, exactly as it does on a device.
 */
public final class GsfChk {

    private static int pass = 0;
    private static int fail = 0;

    public static void main(String[] a) throws Exception {
        // --- the gate -------------------------------------------------------
        check("android_id alone", true,
                GsfAnswer.wantsGsfId(new String[] {"android_id"}));
        check("android_id among others", true,
                GsfAnswer.wantsGsfId(new String[] {
                        "something_else", "android_id", "third"}));
        check("unrelated key", false,
                GsfAnswer.wantsGsfId(new String[] {"android_id_2"}));
        check("prefix is not a match", false,
                GsfAnswer.wantsGsfId(new String[] {"xandroid_id"}));
        check("empty array", false, GsfAnswer.wantsGsfId(new String[0]));
        check("null", false, GsfAnswer.wantsGsfId(null));
        check("not an array", false, GsfAnswer.wantsGsfId("android_id"));

        // --- the cursor -----------------------------------------------------
        Object c = GsfAnswer.newGsfCursor(GsfChk.class.getClassLoader(), "1234567890");
        check("cursor built", true, c != null);
        if (c != null) {
            Class<?> mc = c.getClass();
            check("one row", 1, mc.getMethod("getCount").invoke(c));

            java.lang.reflect.Method getString =
                    mc.getMethod("getString", int.class);
            check("column 0 is the key", "android_id", getString.invoke(c, 0));
            check("column 1 is the id", "1234567890", getString.invoke(c, 1));

            String[] names = (String[]) mc.getMethod("getColumnNames").invoke(c);
            check("two columns", 2, names.length);
            check("col name 0", "_id", names[0]);
            check("col name 1", "value", names[1]);
        }

        // A different id must land in the cursor, not a captured constant.
        Object c2 = GsfAnswer.newGsfCursor(GsfChk.class.getClassLoader(), "999");
        check("second cursor carries its own id", "999",
                c2 == null ? null
                        : c2.getClass().getMethod("getString", int.class)
                                .invoke(c2, 1));

        // A loader that cannot see MatrixCursor must yield null, not throw:
        // the caller then lets the real provider answer.
        ClassLoader blind = new ClassLoader(null) { };
        check("unreachable MatrixCursor yields null", null,
                GsfAnswer.newGsfCursor(blind, "1"));

        System.out.println();
        System.out.println(pass + " checks, " + fail + " failed");
        System.out.println(fail == 0 ? "ALL PASS" : "FAILED");
        if (fail != 0) System.exit(1);
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
