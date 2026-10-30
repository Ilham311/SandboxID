package id.sandboxid.tt;

import java.util.Locale;

/**
 * Hardware-naming rules for a spoofed identity — the Java counterpart of
 * {@code native/include/hw_naming.hpp}, kept deliberately dependency-free so
 * the CI step that runs on a plain JDK can pin it (see
 * {@code lsp/app/src/test/java/id/sandboxid/tt/HwNamingTest.java}).
 *
 * <p><b>Why the rules exist.</b> A device advertises its hardware through
 * several props that are <i>not</i> interchangeable, and collapsing them into
 * one value is a strong tell:
 *
 * <ul>
 *   <li>{@code ro.hardware} / {@code ro.boot.hardware} — the token init uses to
 *       pick {@code /vendor/etc/init/hw/init.<token>.rc}. Qualcomm ships a
 *       single {@code qcom} for <i>every</i> SoC it makes, because its init rc
 *       is {@code init.qcom.rc} on all of them. MediaTek, Google Tensor and
 *       Exynos name the file after the platform token.</li>
 *   <li>{@code ro.board.platform} — the SoC platform ({@code taro},
 *       {@code kona}, {@code mt6789}).</li>
 *   <li>{@code ro.product.board} — the device codename on most OEMs
 *       ({@code alioth}, {@code shiba}). Samsung is the outlier and puts the
 *       <i>platform</i> here, on its Snapdragon devices too.</li>
 * </ul>
 *
 * <p>On the reference device (Redmi alioth / SM8250) the three differ:
 * {@code ro.hardware=qcom}, {@code ro.board.platform=kona},
 * {@code ro.product.board=alioth}. The generator used to derive all three from
 * the single {@code board} column of {@code devices.tsv}, so a Qualcomm persona
 * advertised {@code ro.hardware=taro} — a device that could not have booted,
 * since no Qualcomm device ships {@code init.taro.rc}.
 */
final class HwNaming {

    private HwNaming() {}

    static boolean isQualcomm(String socMan, String socModel) {
        String m = lower(socMan);
        if (m.contains("qualcomm") || m.contains("qti")) return true;
        return socModel.startsWith("SM") || socModel.startsWith("MSM")
                || socModel.startsWith("SDM") || socModel.startsWith("QCM")
                || socModel.startsWith("APQ");
    }

    static boolean isMediatek(String socMan, String socModel) {
        return lower(socMan).contains("mediatek") || socModel.startsWith("MT");
    }

    /**
     * Exynos must be tested AFTER Qualcomm and MediaTek: Samsung ships
     * Snapdragon parts too (Galaxy S23 is SM8550), so a bare "samsung"
     * manufacturer match cannot decide it.
     */
    static boolean isExynos(String socMan, String socModel) {
        if (socModel.startsWith("s5e") || socModel.startsWith("exynos")) return true;
        return lower(socMan).contains("samsung")
                && !isQualcomm(socMan, socModel)
                && !isMediatek(socMan, socModel);
    }

    /** Tensor platform tokens — the same set hw_naming.hpp pins. */
    static boolean isTensorPlatform(String plat) {
        return plat.equals("gs101") || plat.equals("gs201") || plat.equals("zuma")
                || plat.equals("zumapro") || plat.equals("laguna");
    }

    /**
     * {@code ro.hardware} / {@code ro.boot.hardware} — the init rc selector.
     * Qualcomm is the one vendor whose token is constant across every SoC it
     * makes; the others name the file after the platform.
     */
    static String hardwareToken(String socMan, String socModel, String platform) {
        return isQualcomm(socMan, socModel) ? "qcom" : platform;
    }

    /**
     * {@code ro.product.board} — the device codename on most OEMs. Samsung is
     * the outlier and puts the PLATFORM here, on its Snapdragon devices too
     * (Galaxy S23 = dm1q / SM8550 / Qualcomm, still {@code ro.product.board=
     * kalama}). Getting this backwards is not cosmetic: a Samsung persona whose
     * board says "dm1q" contradicts that vendor's every real device.
     */
    static String productBoard(String device, String platform,
                               String brand, String manufacturer) {
        if (lower(brand).contains("samsung") || lower(manufacturer).contains("samsung"))
            return platform;
        return device;
    }

    /**
     * {@code ro.soc.manufacturer}. Modern Qualcomm vendor.prop files say
     * "QTI" (the reference device reports QTI for SM8250); "Qualcomm" is the
     * older spelling, and a persona at SM8475 with "Qualcomm" contradicts the
     * platform it claims.
     */
    static String socManufacturerName(String socMan, String socModel) {
        return isQualcomm(socMan, socModel) ? "QTI" : socMan;
    }

    /**
     * {@code dalvik.vm.isa.arm64.variant}. "generic" is the AOSP/GSI/emulator
     * default, not a retail value, so it must never appear for a persona in the
     * pool. Measured: SM8250/SM8450/SM8550 → kryo300; mt6789 → cortex-a55;
     * gs201/zuma/zumapro → cortex-a76.
     */
    static String dalvikArm64Variant(String socMan, String socModel, String platform) {
        if (isQualcomm(socMan, socModel)) return "kryo300";
        if (isMediatek(socMan, socModel)) return "cortex-a55";
        if (isTensorPlatform(platform)) return "cortex-a76";
        if (isExynos(socMan, socModel)) return "cortex-a76";
        return "cortex-a55";
    }

    /**
     * The 32-bit variant. Only the little cluster runs AArch32 on an arm64 SoC,
     * so this is the little core's name — the same family across every modern
     * SoC in the pool. The reference device reports cortex-a75 because its
     * msm-4.19 kernel names the silver cluster that way; that value is specific
     * to SM8250 and wrong for newer parts, so the generic little-core name is
     * used. The point is only that it is a real core name.
     */
    static String dalvikArmVariant() {
        return "cortex-a55";
    }

    /**
     * GPU Vendor and Renderer string derivation for GL10 / GLES20 and Arkose telemetry.
     */
    static String gpuVendor(String socMan, String socModel) {
        if (isQualcomm(socMan, socModel)) return "Qualcomm";
        if (isMediatek(socMan, socModel)) return "ARM";
        if (lower(socMan).contains("google")) return "ARM";
        if (isExynos(socMan, socModel)) {
            if ("s5e9945".equals(socModel) || "s5e8845".equals(socModel)) return "Samsung";
            return "ARM";
        }
        return "Qualcomm";
    }

    static String gpuRenderer(String socMan, String socModel, String platform) {
        if (isQualcomm(socMan, socModel)) {
            if ("SM8650".equals(socModel)) return "Adreno (TM) 750";
            if ("SM8550".equals(socModel)) return "Adreno (TM) 740";
            if ("SM8475".equals(socModel) || "SM8450".equals(socModel)) return "Adreno (TM) 730";
            if ("SM8350".equals(socModel)) return "Adreno (TM) 660";
            if ("SM8250".equals(socModel)) return "Adreno (TM) 650";
            if ("SM7475".equals(socModel)) return "Adreno (TM) 725";
            if ("SM7435".equals(socModel)) return "Adreno (TM) 710";
            if ("SM7325".equals(socModel)) return "Adreno (TM) 642L";
            if ("SM6225".equals(socModel)) return "Adreno (TM) 610";
            return "Adreno (TM) 730";
        }
        if (isTensorPlatform(platform)) {
            if ("gs101".equals(platform)) return "Mali-G78";
            if ("gs201".equals(platform)) return "Mali-G710";
            if ("zuma".equals(platform) || "zumapro".equals(platform) || "laguna".equals(platform)) return "Mali-G715";
            return "Mali-G715";
        }
        if (isMediatek(socMan, socModel)) {
            if ("MT6989".equalsIgnoreCase(socModel)) return "Immortalis-G720 MC12";
            if ("MT6985".equalsIgnoreCase(socModel)) return "Immortalis-G715 MC11";
            if ("MT6897".equalsIgnoreCase(socModel)) return "Mali-G610 MC6";
            if ("MT6895".equalsIgnoreCase(socModel)) return "Mali-G610 MC6";
            if ("MT6893".equalsIgnoreCase(socModel)) return "Mali-G77 MC9";
            if ("MT6789".equalsIgnoreCase(socModel)) return "Mali-G57 MC2";
            if ("MT6833".equalsIgnoreCase(socModel)) return "Mali-G57 MC2";
            if ("MT6769Z".equalsIgnoreCase(socModel) || "MT6768".equalsIgnoreCase(socModel)) return "Mali-G52 MC2";
            return "Mali-G77";
        }
        if (isExynos(socMan, socModel)) {
            if ("s5e9945".equals(socModel)) return "Samsung Xclipse 940";
            if ("s5e8845".equals(socModel)) return "Samsung Xclipse 530";
            return "Mali-G78";
        }
        return "Adreno (TM) 730";
    }

    // There is deliberately NO gpuDriverPkg() here any more.
    //
    // ro.gfx.driver.1 names an INSTALLED package, and
    // android.os.GraphicsEnvironment.get() asks PackageManager whether it
    // exists. Deriving a persona-shaped name produced one that is not installed
    // on this device, and the framework logs
    // "updatable driver package '<name>' not installed" — a louder tell than
    // the real value it replaced. The whole ro.gfx.driver.* family is hidden by
    // isVendorRomProp() instead. Mirrors native/include/hw_naming.hpp.

    private static String lower(String s) {
        return s == null ? "" : s.toLowerCase(Locale.ROOT);
    }
}
