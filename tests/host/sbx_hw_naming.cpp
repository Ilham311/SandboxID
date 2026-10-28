// Pins the hardware-naming rules: ro.hardware, ro.board.platform,
// ro.product.board, ro.soc.manufacturer and the Dalvik ISA variant.
//
// These four props describe the hardware from different angles and are NOT
// interchangeable. The module used to derive all of them from the single
// `board` column of the persona table, which made a Qualcomm persona advertise
//
//   ro.hardware      = taro    (real convention: qcom)
//   ro.product.board = taro    (real convention: the device codename)
//   ro.soc.manufacturer = Qualcomm (real convention on modern parts: QTI)
//
// A device reporting ro.hardware=taro could not have booted: init selects
// /vendor/etc/init/hw/init.<ro.hardware>.rc, and no Qualcomm device ships
// init.taro.rc. The reference device's own vendor partition holds init.qcom.rc,
// and its kernel cmdline says androidboot.hardware=qcom.
//
// The values asserted here are the measured ones: the reference device
// (Redmi alioth / SM8250) reports qcom / kona / alioth / QTI / kryo300, and
// the conventions were cross-checked against vendor device trees.

#include "hw_naming.hpp"

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(got, want)                                                \
    do {                                                                   \
        const std::string g_ = (got), w_ = (want);                         \
        if (g_ != w_) {                                                    \
            std::printf("  FAIL %s:%d  got '%s', want '%s'\n",             \
                        __FILE__, __LINE__, g_.c_str(), w_.c_str());       \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

int main() {
    using namespace sbxhw;

    std::printf("==> SoC vendor classification\n");
    CHECK(is_qualcomm("Qualcomm", "SM8475"));
    CHECK(is_qualcomm("QTI", "SM8250"));
    CHECK(is_qualcomm("", "SM8550"));       // manufacturer missing, model decides
    CHECK(is_qualcomm("", "MSM8998"));
    CHECK(!is_qualcomm("MediaTek", "MT6789"));
    CHECK(!is_qualcomm("Google", "GS301"));
    CHECK(!is_qualcomm("Samsung", "s5e9945"));

    CHECK(is_mediatek("MediaTek", "MT6789"));
    CHECK(is_mediatek("", "MT6893"));
    CHECK(!is_mediatek("Qualcomm", "SM8475"));

    // Exynos is the awkward one: Samsung ships Snapdragon parts, so the vendor
    // name alone cannot decide it. Galaxy S23 is SM8550.
    CHECK(is_exynos("Samsung", "s5e9945"));
    CHECK(is_exynos("Samsung", "erd9945"));   // platform token, no s5e prefix
    CHECK(is_exynos("", "exynos2100"));
    CHECK(!is_exynos("Samsung", "SM8550"));   // Samsung, but Snapdragon
    CHECK(!is_exynos("Samsung", "MT6789"));   // Samsung, but MediaTek
    CHECK(!is_exynos("Qualcomm", "SM8475"));

    std::printf("==> ro.hardware / ro.boot.hardware: the init rc selector\n");
    // Qualcomm's token is constant across every SoC it makes, because its init
    // rc is init.qcom.rc on all of them. This is the regression: the platform
    // name used to be copied here.
    CHECK_EQ(hardware_token("Qualcomm", "SM8475", "taro"), "qcom");
    CHECK_EQ(hardware_token("Qualcomm", "SM8250", "kona"), "qcom");
    CHECK_EQ(hardware_token("QTI", "SM8550", "kalama"), "qcom");
    CHECK_EQ(hardware_token("Qualcomm", "SM8650", "pineapple"), "qcom");
    // MediaTek, Tensor and Exynos name the init rc after the platform token.
    CHECK_EQ(hardware_token("MediaTek", "MT6789", "mt6789"), "mt6789");
    CHECK_EQ(hardware_token("Google", "GS301", "zuma"), "zuma");
    CHECK_EQ(hardware_token("Google", "GS201", "gs201"), "gs201");
    CHECK_EQ(hardware_token("Samsung", "s5e9945", "erd9945"), "erd9945");

    std::printf("==> ro.product.board: the device codename\n");
    // Reference device: ro.product.board=alioth == ro.product.device=alioth.
    CHECK_EQ(product_board("alioth", "kona", "Redmi", "Xiaomi"), "alioth");
    CHECK_EQ(product_board("shiba", "zuma", "google", "Google"), "shiba");
    CHECK_EQ(product_board("garnet", "parrot", "Redmi", "Xiaomi"), "garnet");
    CHECK_EQ(product_board("houji", "pineapple", "Xiaomi", "Xiaomi"), "houji");
    CHECK_EQ(product_board("OP56D3L1", "taro", "OPPO", "OPPO"), "OP56D3L1");
    // Samsung is the outlier and puts the PLATFORM here - on its Snapdragon
    // devices too, not only its Exynos ones. Galaxy S23 is dm1q / SM8550 /
    // Qualcomm and still reports ro.product.board=kalama.
    CHECK_EQ(product_board("dm1q", "kalama", "samsung", "samsung"), "kalama");
    CHECK_EQ(product_board("e1q", "erd9945", "samsung", "samsung"), "erd9945");
    CHECK_EQ(product_board("b6q", "pineapple", "samsung", "samsung"), "pineapple");

    std::printf("==> ro.soc.manufacturer\n");
    CHECK_EQ(soc_manufacturer_name("Qualcomm", "SM8475"), "QTI");
    CHECK_EQ(soc_manufacturer_name("Qualcomm", "SM8250"), "QTI");
    CHECK_EQ(soc_manufacturer_name("MediaTek", "MT6789"), "MediaTek");
    CHECK_EQ(soc_manufacturer_name("Google", "GS301"), "Google");
    CHECK_EQ(soc_manufacturer_name("Samsung", "s5e9945"), "Samsung");

    std::printf("==> dalvik.vm.isa.arm64.variant\n");
    // "generic" is the AOSP/GSI/emulator default, not a retail value.
    CHECK_EQ(dalvik_arm64_variant("Qualcomm", "SM8475", "taro"), "kryo300");
    CHECK_EQ(dalvik_arm64_variant("Qualcomm", "SM8250", "kona"), "kryo300");
    CHECK_EQ(dalvik_arm64_variant("Qualcomm", "SM8550", "kalama"), "kryo300");
    CHECK_EQ(dalvik_arm64_variant("MediaTek", "MT6789", "mt6789"), "cortex-a55");
    CHECK_EQ(dalvik_arm64_variant("Google", "GS201", "gs201"), "cortex-a76");
    CHECK_EQ(dalvik_arm64_variant("Google", "GS301", "zuma"), "cortex-a76");
    CHECK_EQ(dalvik_arm64_variant("Samsung", "s5e9945", "erd9945"), "cortex-a76");
    // Never the emulator default, for any persona in the pool.
    CHECK(dalvik_arm64_variant("Qualcomm", "SM8475", "taro") != "generic");
    CHECK(dalvik_arm64_variant("MediaTek", "MT6789", "mt6789") != "generic");
    CHECK(dalvik_arm64_variant("Google", "GS301", "zuma") != "generic");

    std::printf("==> dalvik.vm.isa.arm.variant\n");
    CHECK_EQ(dalvik_arm_variant("Qualcomm", "SM8250", "kona"), "cortex-a55");
    CHECK(dalvik_arm_variant("MediaTek", "MT6789", "mt6789") != "generic");

    // There is no gpu_driver_pkg() to test any more, on purpose. The prop names
    // an INSTALLED package and GraphicsEnvironment.get() verifies that with
    // PackageManager; deriving com.qualcomm.qti.gpudrivers.<persona>.api30 gave
    // a package that is not installed on the device, and the framework logged
    // "updatable driver package '<name>' not installed" - a louder tell than the
    // real value it replaced. The whole family is hidden instead, for EVERY
    // persona; the hide-list checks live in sbx_pure, which links
    // native_read.cpp and therefore has should_hide_prop().

    std::printf("==> the exact device case (OPPO CPH2521 persona)\n");
    // The persona that was live on the device: OPPO Reno10 Pro+ 5G, taro,
    // SM8475. Before the fix all four of these were "taro".
    {
        const char* man = "Qualcomm";
        const char* mod = "SM8475";
        const char* plat = "taro";
        CHECK_EQ(hardware_token(man, mod, plat), "qcom");
        CHECK_EQ(product_board("OP56D3L1", plat, man, mod), "OP56D3L1");
        CHECK_EQ(soc_manufacturer_name(man, mod), "QTI");
        CHECK_EQ(dalvik_arm64_variant(man, mod, plat), "kryo300");
    }

    std::printf("==> the reference device's real values\n");
    // Redmi alioth / SM8250. The rules must reproduce what the device itself
    // reports, or they are describing a device that does not exist.
    {
        const char* man = "Qualcomm";
        const char* mod = "SM8250";
        const char* plat = "kona";
        CHECK_EQ(hardware_token(man, mod, plat), "qcom");         // getprop: qcom
        CHECK_EQ(product_board("alioth", plat, man, mod), "alioth"); // alioth
        CHECK_EQ(soc_manufacturer_name(man, mod), "QTI");          // QTI
        CHECK_EQ(dalvik_arm64_variant(man, mod, plat), "kryo300"); // kryo300
    }

    std::printf("==> is_tensor_platform\n");
    CHECK(is_tensor_platform("gs101"));
    CHECK(is_tensor_platform("gs201"));
    CHECK(is_tensor_platform("zuma"));
    CHECK(is_tensor_platform("zumapro"));
    CHECK(is_tensor_platform("laguna"));
    CHECK(!is_tensor_platform("taro"));
    CHECK(!is_tensor_platform("kona"));
    CHECK(!is_tensor_platform("mt6789"));
    CHECK(!is_tensor_platform("erd9945"));

    if (failures == 0) std::printf("all checks passed\n");
    else std::printf("%d check(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
