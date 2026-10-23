package android.util;

/** Minimal framework-shaped fixture for the host-side display hook regression test. */
public class DisplayMetrics {
    public int widthPixels;
    public int heightPixels;
    public int densityDpi;
    public float density;
    public float scaledDensity;
    public float xdpi;
    public float ydpi;

    public DisplayMetrics() {}

    public void setTo(DisplayMetrics other) {
        widthPixels = other.widthPixels;
        heightPixels = other.heightPixels;
        densityDpi = other.densityDpi;
        density = other.density;
        scaledDensity = other.scaledDensity;
        xdpi = other.xdpi;
        ydpi = other.ydpi;
    }

    @Override
    public boolean equals(Object other) {
        if (!(other instanceof DisplayMetrics)) return false;
        DisplayMetrics metrics = (DisplayMetrics) other;
        return widthPixels == metrics.widthPixels
                && heightPixels == metrics.heightPixels
                && densityDpi == metrics.densityDpi
                && density == metrics.density
                && scaledDensity == metrics.scaledDensity
                && xdpi == metrics.xdpi
                && ydpi == metrics.ydpi;
    }
}
