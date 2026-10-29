package de.robv.android.xposed;

/**
 * Compiler stub. LSPosed supplies the real implementation, which dispatches
 * {@link #beforeHookedMethod} and {@link #afterHookedMethod} around the hooked
 * call and honours {@code getResult} / {@code setObjectBody}.
 */
public abstract class XC_MethodHook {

    protected void beforeHookedMethod(MethodHookParam param) throws Throwable { }

    protected void afterHookedMethod(MethodHookParam param) throws Throwable { }

    public static class MethodHookParam {
        public Object thisObject;
        public Object[] args;
        /**
         * The hooked method. Real Xposed exposes this as {@code Member}; it is
         * widened to {@code Object} here only because the stub must compile
         * without pulling in a Member subtype. Callers use {@code getName()}
         * through reflection-free casts to {@code java.lang.reflect.Member}.
         */
        public java.lang.reflect.Member method;
        private Object mResult;
        private Throwable mThrowable;

        public Object getResult() { return mResult; }
        public void setResult(Object result) { mResult = result; }
        public Throwable getThrowable() { return mThrowable; }
        /**
         * Makes the hooked call throw {@code throwable} instead of running.
         *
         * <p>From {@link #beforeHookedMethod} this prevents the original call
         * entirely — the only way to make a hooked method <i>fail</i>. Throwing
         * from inside the callback does NOT do this: XposedBridge catches
         * anything the callback throws, logs it, and resets the result, so the
         * original method runs anyway. See {@code XposedBridge.handleHookedMethod}.
         *
         * <p>Clears the result and sets {@code returnEarly}, exactly as LSPosed
         * does ({@code XC_MethodHook.java:165-169}): a caller that sets a result
         * and then a throwable must get the throwable, not a half-applied result.
         * The stub has no {@code returnEarly} field of its own, so the flag is
         * modelled by {@link #hasThrowable()}, which the dispatch consults.
         */
        public void setThrowable(Throwable throwable) {
            mThrowable = throwable;
            mResult = null;
        }
        public boolean hasThrowable() { return mThrowable != null; }
        public Object getObjectField(Object obj, String name) { return null; }
        public void setObjectField(Object obj, String name, Object value) { }
    }

    /** Handle returned from findAndHookMethod; calling unhook() removes the hook. */
    public interface Unhook { void unhook(); }
}
