package id.sandboxid.tt;

/**
 * Pins {@link HwNaming} — the Java counterpart of the native suite
 * {@code tests/host/sbx_hw_naming.cpp}, asserting the same values.
 *
 * <p><b>Why this test exists.</b> The LSPosed variant had the identical bug the
 * native side had: {@code PersonaGenerator} derived {@code BOARD},
 * {@code BOARD_PLATFORM} and {@code HARDWARE} from the single {@code board}
 * column of {@code devices.tsv}. A Qualcomm persona therefore advertised
 * {@code ro.hardware=taro} — a token no Qualcomm device ships, since init
 * selects {@code /vendor/etc/init/hw/init.<ro.hardware>.rc} and every Qualcomm
 * SoC ships {@code init.qcom.rc}. It also left {@code ro.soc.manufacturer} as
 * "Qualcomm" where modern vendor.prop files say "QTI", and never set
 * {@code dalvik.vm.isa.arm64.variant} at all, so a persona claiming an SM8475
 * flagship reported the AOSP/GSI default "generic".
 *
 * <p>The measured values asserted here come from the reference device (Redmi
 * alioth / SM8250): {@code qcom / kona / alioth / QTI / kryo300}.
 *
 * <p>Deliberately JDK-only, like {@code RewriteTest3}: {@link HwNaming} has no
 * Android dependency, so CI can pin it without an SDK.
 */
public class HwNamingTest {

    static int fails = 0;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "ok  " : "FAIL") + " " + name);
        if (!ok) fails++;
    }

    static void eq(String name, String got, String want) {
        boolean ok = want.equals(got);
        System.out.println((ok ? "ok  " : "FAIL") + " " + name
                + (ok ? "" : "  (got '" + got + "', want '" + want + "')"));
        if (!ok) fails++;
    }

    public static void main(String[] a) {
        System.out.println("-- SoC vendor classification --");
        check("qualcomm by name", HwNaming.isQualcomm("Qualcomm", "SM8475"));
        check("qualcomm by QTI", HwNaming.isQualcomm("QTI", "SM8250"));
        check("qualcomm by model only", HwNaming.isQualcomm("", "SM8550"));
        check("qualcomm MSM", HwNaming.isQualcomm("", "MSM8998"));
        check("not qualcomm mediatek", !HwNaming.isQualcomm("MediaTek", "MT6789"));
        check("not qualcomm google", !HwNaming.isQualcomm("Google", "GS301"));
        check("not qualcomm exynos", !HwNaming.isQualcomm("Samsung", "s5e9945"));

        check("mediatek by name", HwNaming.isMediatek("MediaTek", "MT6789"));
        check("mediatek by model", HwNaming.isMediatek("", "MT6893"));
        check("not mediatek qualcomm", !HwNaming.isMediatek("Qualcomm", "SM8475"));

        // Samsung ships Snapdragon parts, so the vendor name alone cannot
        // decide Exynos. Galaxy S23 is SM8550.
        check("exynos s5e", HwNaming.isExynos("Samsung", "s5e9945"));
        check("exynos platform token", HwNaming.isExynos("Samsung", "erd9945"));
        check("exynos bare", HwNaming.isExynos("", "exynos2100"));
        check("samsung snapdragon is not exynos",
                !HwNaming.isExynos("Samsung", "SM8550"));
        check("samsung mediatek is not exynos",
                !HwNaming.isExynos("Samsung", "MT6789"));

        System.out.println("-- ro.hardware / ro.boot.hardware --");
        // The regression: the platform name used to be copied here.
        eq("SM8475/taro", HwNaming.hardwareToken("Qualcomm", "SM8475", "taro"), "qcom");
        eq("SM8250/kona", HwNaming.hardwareToken("Qualcomm", "SM8250", "kona"), "qcom");
        eq("SM8550/kalama", HwNaming.hardwareToken("QTI", "SM8550", "kalama"), "qcom");
        eq("SM8650/pineapple",
                HwNaming.hardwareToken("Qualcomm", "SM8650", "pineapple"), "qcom");
        // MediaTek, Tensor and Exynos name the init rc after the platform.
        eq("MT6789", HwNaming.hardwareToken("MediaTek", "MT6789", "mt6789"), "mt6789");
        eq("GS301/zuma", HwNaming.hardwareToken("Google", "GS301", "zuma"), "zuma");
        eq("GS201", HwNaming.hardwareToken("Google", "GS201", "gs201"), "gs201");
        eq("exynos", HwNaming.hardwareToken("Samsung", "s5e9945", "erd9945"), "erd9945");

        System.out.println("-- ro.product.board --");
        eq("alioth", HwNaming.productBoard("alioth", "kona", "Redmi", "Xiaomi"), "alioth");
        eq("shiba", HwNaming.productBoard("shiba", "zuma", "google", "Google"), "shiba");
        eq("houji", HwNaming.productBoard("houji", "pineapple", "Xiaomi", "Xiaomi"), "houji");
        eq("oppo", HwNaming.productBoard("OP56D3L1", "taro", "OPPO", "OPPO"), "OP56D3L1");
        // Samsung puts the PLATFORM here, on its Snapdragon devices too.
        eq("s23 snapdragon",
                HwNaming.productBoard("dm1q", "kalama", "samsung", "samsung"), "kalama");
        eq("s23 exynos",
                HwNaming.productBoard("e1q", "erd9945", "samsung", "samsung"), "erd9945");
        eq("fold5 snapdragon",
                HwNaming.productBoard("q5q", "kalama", "samsung", "samsung"), "kalama");

        System.out.println("-- ro.soc.manufacturer --");
        eq("qualcomm -> QTI",
                HwNaming.socManufacturerName("Qualcomm", "SM8475"), "QTI");
        eq("qualcomm SM8250 -> QTI",
                HwNaming.socManufacturerName("Qualcomm", "SM8250"), "QTI");
        eq("mediatek unchanged",
                HwNaming.socManufacturerName("MediaTek", "MT6789"), "MediaTek");
        eq("google unchanged",
                HwNaming.socManufacturerName("Google", "GS301"), "Google");

        System.out.println("-- dalvik.vm.isa.arm64.variant --");
        // "generic" is the AOSP/GSI/emulator default, not a retail value.
        eq("SM8475", HwNaming.dalvikArm64Variant("Qualcomm", "SM8475", "taro"), "kryo300");
        eq("SM8250", HwNaming.dalvikArm64Variant("Qualcomm", "SM8250", "kona"), "kryo300");
        eq("SM8550", HwNaming.dalvikArm64Variant("Qualcomm", "SM8550", "kalama"), "kryo300");
        eq("MT6789", HwNaming.dalvikArm64Variant("MediaTek", "MT6789", "mt6789"),
                "cortex-a55");
        eq("GS201", HwNaming.dalvikArm64Variant("Google", "GS201", "gs201"), "cortex-a76");
        eq("zuma", HwNaming.dalvikArm64Variant("Google", "GS301", "zuma"), "cortex-a76");
        eq("exynos", HwNaming.dalvikArm64Variant("Samsung", "s5e9945", "erd9945"),
                "cortex-a76");
        // Never the emulator default, for any persona in the pool.
        check("SM8475 not generic",
                !"generic".equals(HwNaming.dalvikArm64Variant("Qualcomm", "SM8475", "taro")));
        check("MT6789 not generic",
                !"generic".equals(HwNaming.dalvikArm64Variant("MediaTek", "MT6789", "mt6789")));
        check("zuma not generic",
                !"generic".equals(HwNaming.dalvikArm64Variant("Google", "GS301", "zuma")));

        System.out.println("-- dalvik.vm.isa.arm.variant --");
        eq("little core", HwNaming.dalvikArmVariant(), "cortex-a55");
        check("not generic", !"generic".equals(HwNaming.dalvikArmVariant()));

        System.out.println("-- ro.gfx.driver.*: hidden, never derived --");
        // There is no gpuDriverPkg() to test any more, on purpose. The prop
        // names an INSTALLED package and GraphicsEnvironment.get() verifies it
        // with PackageManager; deriving com.qualcomm.qti.gpudrivers.<persona>.
        // api30 produced a package that is not installed here, and the
        // framework logged "updatable driver package '<name>' not installed".
        // The family is hidden instead — see PropHook.isVendorRomProp().

        System.out.println("-- the exact device case (OPPO CPH2521 persona) --");
        // The persona that was live on the device: OPPO Reno10 Pro+ 5G, taro,
        // SM8475. Before the fix all four of these were "taro".
        eq("hw", HwNaming.hardwareToken("Qualcomm", "SM8475", "taro"), "qcom");
        eq("board", HwNaming.productBoard("OP56D3L1", "taro", "OPPO", "OPPO"), "OP56D3L1");
        eq("soc man", HwNaming.socManufacturerName("Qualcomm", "SM8475"), "QTI");
        eq("dalvik", HwNaming.dalvikArm64Variant("Qualcomm", "SM8475", "taro"), "kryo300");

        System.out.println("-- the reference device's real values --");
        // Redmi alioth / SM8250. The rules must reproduce what the device itself
        // reports, or they describe a device that does not exist.
        eq("hw=qcom", HwNaming.hardwareToken("Qualcomm", "SM8250", "kona"), "qcom");
        eq("board=alioth", HwNaming.productBoard("alioth", "kona", "Redmi", "Xiaomi"),
                "alioth");
        eq("soc=QTI", HwNaming.socManufacturerName("Qualcomm", "SM8250"), "QTI");
        eq("dalvik=kryo300",
                HwNaming.dalvikArm64Variant("Qualcomm", "SM8250", "kona"), "kryo300");

        System.out.println("-- isTensorPlatform --");
        check("gs101", HwNaming.isTensorPlatform("gs101"));
        check("gs201", HwNaming.isTensorPlatform("gs201"));
        check("zuma", HwNaming.isTensorPlatform("zuma"));
        check("zumapro", HwNaming.isTensorPlatform("zumapro"));
        check("laguna", HwNaming.isTensorPlatform("laguna"));
        check("taro not tensor", !HwNaming.isTensorPlatform("taro"));
        check("kona not tensor", !HwNaming.isTensorPlatform("kona"));
        check("mt6789 not tensor", !HwNaming.isTensorPlatform("mt6789"));

        System.out.println();
        if (fails == 0) System.out.println("ALL PASS");
        else System.out.println(fails + " check(s) FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
