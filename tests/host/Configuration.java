package android.content.res;

/** Minimal framework-shaped fixture for the host-side configuration hook regression test. */
public class Configuration {
    public int orientation;
    public int screenWidthDp;
    public int screenHeightDp;
    public int densityDpi;
    public int smallestScreenWidthDp;

    public Configuration() {}

    public Configuration(Configuration other) {
        orientation = other.orientation;
        screenWidthDp = other.screenWidthDp;
        screenHeightDp = other.screenHeightDp;
        densityDpi = other.densityDpi;
        smallestScreenWidthDp = other.smallestScreenWidthDp;
    }

    @Override
    public boolean equals(Object other) {
        if (!(other instanceof Configuration)) return false;
        Configuration config = (Configuration) other;
        return orientation == config.orientation
                && screenWidthDp == config.screenWidthDp
                && screenHeightDp == config.screenHeightDp
                && densityDpi == config.densityDpi
                && smallestScreenWidthDp == config.smallestScreenWidthDp;
    }
}
