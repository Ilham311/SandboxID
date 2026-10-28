#pragma once
//
// Hardware-naming rules for a spoofed identity.
//
// A device advertises its hardware through several props that are NOT
// interchangeable, and collapsing them into one value is a strong tell:
//
//   ro.hardware / ro.boot.hardware   the token init uses to pick
//                                    init.<token>.rc. Qualcomm ships a single
//                                    "qcom" for every SoC it makes; MediaTek
//                                    and Google Tensor use the platform token;
//                                    Exynos uses the platform token too.
//   ro.board.platform                the SoC platform (taro, kona, mt6789)
//   ro.product.board                 the device codename on most OEMs
//                                    (alioth, shiba). Samsung is the outlier
//                                    and puts the platform here.
//
// On the reference device (Redmi alioth / SM8250) the three differ:
//   ro.hardware=qcom  ro.board.platform=kona  ro.product.board=alioth
//
// The module used to derive all three from the single `board` column, so a
// persona reported ro.hardware=taro. That describes a device which could not
// have booted: init would look for /vendor/etc/init/hw/init.taro.rc, and no
// Qualcomm device ships one. The device's own /vendor/etc/init/hw/ holds
// init.qcom.rc, and its kernel cmdline says androidboot.hardware=qcom.
//
// Pure header: no JNI, no Android headers, no globals, so the host suites can
// compile and pin it without a device.

#include <cstring>
#include <string>

namespace sbxhw {

inline bool ci_contains(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return false;
    const size_t n = std::strlen(needle);
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t j = 0;
        while (j < n) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
            ++j;
        }
        if (j == n) return true;
    }
    return false;
}

inline bool starts_with(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

// The Tensor platform tokens. Kept here as the single definition; the identity
// deriver used to carry its own copy.
inline bool is_tensor_platform(const std::string& plat) {
    return plat == "gs101" || plat == "gs201" || plat == "zuma" ||
           plat == "zumapro" || plat == "laguna";
}

inline bool is_qualcomm(const std::string& soc_man, const std::string& soc_model) {
    if (ci_contains(soc_man, "qualcomm") || ci_contains(soc_man, "qti")) return true;
    return starts_with(soc_model, "SM")  || starts_with(soc_model, "MSM") ||
           starts_with(soc_model, "SDM") || starts_with(soc_model, "QCM") ||
           starts_with(soc_model, "APQ");
}

inline bool is_mediatek(const std::string& soc_man, const std::string& soc_model) {
    if (ci_contains(soc_man, "mediatek")) return true;
    return starts_with(soc_model, "MT");
}

// Exynos must be tested AFTER Qualcomm and MediaTek: Samsung ships Snapdragon
// parts too (Galaxy S23 is SM8550), so a bare "samsung" manufacturer match
// cannot decide it.
inline bool is_exynos(const std::string& soc_man, const std::string& soc_model) {
    if (starts_with(soc_model, "s5e") || starts_with(soc_model, "exynos")) return true;
    return ci_contains(soc_man, "samsung") && !is_qualcomm(soc_man, soc_model) &&
           !is_mediatek(soc_man, soc_model);
}

// ro.hardware / ro.boot.hardware: the init rc selector.
//
// Qualcomm is the one vendor whose token is constant across every SoC it
// makes - "qcom" - because its init rc is init.qcom.rc on all of them. A
// device tree search for ro.hardware=taro|kona|pineapple|kalama|lahaina
// returns nothing, while ro.board.platform=<those> is everywhere.
inline std::string hardware_token(const std::string& soc_man,
                                  const std::string& soc_model,
                                  const std::string& platform) {
    if (is_qualcomm(soc_man, soc_model)) return "qcom";
    // MediaTek (init.mt6789.rc) and Tensor (init.gs201.rc) name the file after
    // the platform token, and so does Exynos (init.exynos2100.rc), which is
    // why the platform is the right answer for all three.
    return platform;
}

// ro.product.board: the device codename on most OEMs.
//
// Reference device: ro.product.board=alioth == ro.product.device=alioth.
// Pixel 8: shiba. Xiaomi 13: houji. Samsung is the outlier and puts the
// PLATFORM here instead - and it does so on its Snapdragon devices too, not
// only its Exynos ones, so this keys off the vendor and not the SoC:
//
//   Galaxy S23  (dm1q, SM8550, Qualcomm) -> ro.product.board=kalama
//   Galaxy Z Fold5 (q5q, SM8550)         -> ro.product.board=kalama
//   Tab S9      (gts9wifi, SM8550)       -> ro.product.board=kalama
//
// Getting this backwards is not cosmetic: a Samsung persona whose
// ro.product.board says "dm1q" contradicts the same vendor's every real device.
inline std::string product_board(const std::string& device,
                                 const std::string& platform,
                                 const std::string& brand,
                                 const std::string& manufacturer) {
    if (ci_contains(brand, "samsung") || ci_contains(manufacturer, "samsung"))
        return platform;
    return device;
}

// ro.soc.manufacturer. Modern Qualcomm vendor.prop files say "QTI" (the
// reference device reports QTI for SM8250); "Qualcomm" is the older spelling.
// A persona at SM8475 with "Qualcomm" contradicts the platform it claims.
inline std::string soc_manufacturer_name(const std::string& soc_man,
                                         const std::string& soc_model) {
    if (is_qualcomm(soc_man, soc_model)) return "QTI";
    return soc_man;
}

// dalvik.vm.isa.<arch>.variant.
//
// "generic" is not what a retail device ships - it is the AOSP/GSI/emulator
// default, and a fingerprinting app reading it off a device that claims to be
// an SM8475 flagship is looking at a contradiction. Measured values:
//   SM8250 / SM8450 / SM8550 -> kryo300   (reference device, Xiaomi 12,
//                                          OPPO Find X5, Moto X40)
//   mt6789                   -> cortex-a55
//   gs201 / zuma / zumapro   -> cortex-a76
inline std::string dalvik_arm64_variant(const std::string& soc_man,
                                        const std::string& soc_model,
                                        const std::string& platform) {
    if (is_qualcomm(soc_man, soc_model)) return "kryo300";
    if (is_mediatek(soc_man, soc_model)) return "cortex-a55";
    if (is_tensor_platform(platform))    return "cortex-a76";
    if (is_exynos(soc_man, soc_model))   return "cortex-a76";
    return "cortex-a55";
}

// The 32-bit variant. Only the little cluster runs AArch32 on an arm64 SoC, so
// this is the little core's name and it is the same family across every modern
// SoC in the pool (A53/A55 class). The reference device reports cortex-a75
// because its msm-4.19 kernel names the silver cluster that way; that value is
// specific to SM8250 and wrong for the newer parts the pool actually contains,
// so the generic little-core name is used instead. The point is only that it is
// a real core name: "generic" is the AOSP/GSI default.
inline std::string dalvik_arm_variant(const std::string& soc_man,
                                      const std::string& soc_model,
                                      const std::string& platform) {
    (void)soc_man; (void)soc_model; (void)platform;
    return "cortex-a55";
}

// There is deliberately NO gpu_driver_pkg() here any more.
//
// ro.gfx.driver.1 names an INSTALLED package
// (com.qualcomm.qti.gpudrivers.kona.api30 on the reference device) and
// android.os.GraphicsEnvironment.get() asks PackageManager whether it exists.
// Deriving a persona-shaped name produced one that is not installed on this
// device, and the framework then logs
//
//     updatable driver package '<name>' not installed
//
// which is a louder, more specific tell than the real value it replaced. The
// whole ro.gfx.driver.* family is hidden instead — see is_vendor_rom_prop() in
// native/native_read.cpp. "This device ships no UGD package" is an ordinary,
// coherent state; "this device names a UGD package that is not installed" is
// not. A helper that computes the name is a trap for the next reader, so it is
// gone rather than left unused.

}  // namespace sbxhw
