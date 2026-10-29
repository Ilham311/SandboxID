package id.sandboxid.tt;

/**
 * The two decisions the gservices hook makes, split out so they can be tested
 * without a device.
 *
 * <p>Everything here is Android-free at compile time: {@link #wantsGsfId} works
 * on an {@code Object} and {@link #newGsfCursor} reaches {@code MatrixCursor}
 * through the app's {@code ClassLoader} by name. That is deliberate — the
 * surrounding hook cannot be exercised without {@code ContentResolver}, but a
 * wrong gate or a malformed cursor makes the hook silently useless on every
 * device, and this variant has already shipped "installed but does nothing"
 * bugs twice. {@code GsfChk} pins both.
 */
final class GsfAnswer {

    private GsfAnswer() {}

    /** The gservices provider's content URI. */
    static final String GSF_URI = "content://com.google.android.gsf.gservices";

    /** The documented Bundle key for query selection args (API 26+). */
    static final String QUERY_ARG_SELECTION_ARGS =
            "android:query-arg-sql-selection-args";

    /**
     * True when {@code sel} is a selection-args array that asks for
     * {@code android_id}. A null or empty array means "every row", which this
     * hook cannot honestly serve — the real provider returns all of them.
     */
    static boolean wantsGsfId(Object sel) {
        if (!(sel instanceof String[])) return false;
        for (String k : (String[]) sel) {
            if ("android_id".equals(k)) return true;
        }
        return false;
    }

    /**
     * Builds the one-row cursor the gservices provider would have returned.
     * Returns null when {@code MatrixCursor} cannot be reached, so the caller
     * lets the real provider answer instead of crashing the framework.
     */
    static Object newGsfCursor(ClassLoader cl, String gsfId) {
        try {
            Class<?> mc = cl.loadClass("android.database.MatrixCursor");
            Object cursor = mc.getConstructor(String[].class, int.class)
                    .newInstance(new String[] {"_id", "value"}, Integer.valueOf(1));
            mc.getMethod("addRow", Object[].class)
                    .invoke(cursor, (Object) new Object[] {"android_id", gsfId});
            return cursor;
        } catch (Throwable ignored) {
            return null;
        }
    }
}
