package id.sandboxid.tt;

/**
 * The OEM class-deny table: which entry points to hook, and which names to
 * refuse. Android-free, so it can be tested without a device.
 *
 * <p>Both entry points matter. The native side denies through
 * {@code ClassLoader.loadClass(String,boolean)} <b>and</b> {@code
 * Class.forName(String,boolean,ClassLoader)} ({@code sbx_lsplant.hpp:1148-1165}),
 * and an app probing for an OEM class usually uses the first. Installing only
 * {@code forName} left the common path open: {@code miui.os.Build} still loaded
 * and the OEM branch still fired, with a log line that looked like success.
 *
 * <p>{@code loadClass(String)} is not listed separately: the framework declares
 * it as {@code loadClass(name, false)}, so the 2-arg hook covers it.
 */
final class DenyList {

    private DenyList() {}

    /**
     * One hook target: the class that declares the method, the method name, and
     * the parameter types.
     *
     * <p>{@code argIndex} is the position of the class name in Xposed's
     * {@code param.args}, which omits the receiver. It is 0 for both targets:
     * {@code forName} is static, and {@code loadClass}'s first declared
     * parameter is the name.
     */
    static final class Target {
        final String className;
        final String methodName;
        final Class<?>[] paramTypes;
        final int argIndex;

        Target(String className, String methodName, Class<?>[] paramTypes,
               int argIndex) {
            this.className = className;
            this.methodName = methodName;
            this.paramTypes = paramTypes;
            this.argIndex = argIndex;
        }

        @Override
        public String toString() { return className + "#" + methodName; }
    }

    /**
     * The entry points an OEM class probe can come through. Both are hooked;
     * dropping either reopens the hole this table exists to close.
     */
    static final Target[] TARGETS = {
        // Static, so the class name is args[0].
        new Target("java.lang.Class", "forName",
                new Class<?>[] {String.class, boolean.class, ClassLoader.class}, 0),
        // Instance method; Xposed's args[] starts at the declared parameters, so
        // the name is still args[0].
        new Target("java.lang.ClassLoader", "loadClass",
                new Class<?>[] {String.class, boolean.class}, 0),
    };

    /**
     * True when {@code a} is a String naming one of the denied classes.
     *
     * <p>Exact match, not prefix: {@code miui.os.Build} is denied while
     * {@code miui.os.BuildX} is not. A prefix test here would refuse classes the
     * persona legitimately needs, and the app would see a failure where a real
     * device would answer.
     */
    static boolean isDenied(Object a, String[] deny) {
        if (!(a instanceof String) || deny == null) return false;
        for (String d : deny) {
            if (a.equals(d)) return true;
        }
        return false;
    }

    /** The class name at {@code t.argIndex}, or null when absent or not a String. */
    static String nameAt(Object[] args, Target t) {
        if (args == null || t.argIndex >= args.length) return null;
        Object a = args[t.argIndex];
        return a instanceof String ? (String) a : null;
    }
}
