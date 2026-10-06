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
        private Object mResult;
        private Throwable mThrowable;

        public Object getResult() { return mResult; }
        public void setResult(Object result) { mResult = result; }
        public Throwable getThrowable() { return mThrowable; }
        public boolean hasThrowable() { return mThrowable != null; }
        public Object getObjectField(Object obj, String name) { return null; }
        public void setObjectField(Object obj, String name, Object value) { }
    }

    /** Handle returned from findAndHookMethod; calling unhook() removes the hook. */
    public interface Unhook { void unhook(); }
}
