// Ternak TT — Path B hook helper (v1.1.0 scaffold)
// Compiled by fetch_lsplant.sh into classes.dex, then embedded in jni/helper_dex.h
// so libternak_tt.so can load it via InMemoryDexClassLoader at runtime.
//
// v1.1.1 will populate the native method bodies to call the original ART
// method via lsplant backup handle. For v1.1.0 this file exists so the CI
// build path is validated end-to-end.

import android.content.ContentResolver;
import android.media.MediaDrm;
import java.util.Locale;
import java.util.TimeZone;

public final class TernakHookHelper {
    private TernakHookHelper() {}

    public static native String   secureGetString(ContentResolver cr, String name);
    public static native String   globalGetString(ContentResolver cr, String name);
    public static native Locale   localeGetDefault();
    public static native TimeZone tzGetDefault();
    public static native long     uptimeMillis();
    public static native long     elapsedRealtime();
    public static native byte[]   mediaDrmGetPropertyByteArray(MediaDrm md, String key);
}
