package androidx.core.os;

public final class SandboxIDHook {

    public static volatile String spoof;

    public static volatile java.lang.reflect.Method original;

    public Object handle(Object[] args) throws Throwable {
        if (args != null && args.length >= 2
                && "android_id".equals(args[1]) && spoof != null) {
            return spoof;
        }
        if (original != null) {
            try {
                return original.invoke(null, args);
            } catch (java.lang.reflect.InvocationTargetException e) {
                throw e.getCause();
            }
        }
        return null;
    }

    public SandboxIDHook() {}
}
