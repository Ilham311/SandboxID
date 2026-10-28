package com.google.android.gms.ads.identifier;

/**
 * Host stand-in for Grab's bundled {@code AdvertisingIdClient$Info}.
 *
 * <p>Deliberately mirrors the real class's shape, which the fix depends on:
 * {@code public final} with two public final fields {@code a}/{@code b}, a single
 * public {@code (String, boolean)} constructor, and {@code getId()} returning
 * field {@code a}. The M_GAID_INFO hook builds this by reflection from
 * {@code backup.getReturnType()}, so the constructor signature is the contract:
 * if it drifts, the hook falls back to the original and Grab records er:109
 * again.
 */
public final class AdvertisingIdClient {

    public static final class Info {
        public final String a;
        public final boolean b;

        public Info(String id, boolean limitAdTrackingEnabled) {
            this.a = id;
            this.b = limitAdTrackingEnabled;
        }

        public String getId() {
            return a;
        }

        public boolean isLimitAdTrackingEnabled() {
            return b;
        }
    }
}
