// ============================================================
// Ternak TT v1.0 — TikTok-focused Zygisk hook
// 6 hook layers: Build.*, native_get, Settings.Secure,
//                GAID, WiFi MAC, Telephony
// ============================================================
#include <jni.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include <android/log.h>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <cstdlib>
#include "zygisk.hpp"

#define LOG_TAG "TernakTT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

enum : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,
};

// TT package hardcoded (fork focused, no whitelist file)
static bool is_tt_pkg(const std::string& p) {
    return p == "com.zhiliaoapp.musically"        // TT Global
        || p == "com.ss.android.ugc.trill"        // TT Asia (some regions)
        || p == "com.zhiliaoapp.musically.go";    // TT Lite
}

static std::map<std::string, std::string> g_id;

static const std::string& val(const std::string& k) {
    static const std::string empty;
    auto it = g_id.find(k);
    return it != g_id.end() ? it->second : empty;
}

// ============================================================
// L2: SystemProperties.native_get hook
// ============================================================
static jstring hook_prop_get(JNIEnv* env, jclass, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    const char* raw = env->GetStringUTFChars(j_key, nullptr);
    std::string k(raw ? raw : "");
    env->ReleaseStringUTFChars(j_key, raw);

    static const std::map<std::string, std::string> map = {
        {"ro.serialno",              "SERIAL"},
        {"ro.boot.serialno",         "SERIAL"},
        {"ro.build.fingerprint",     "FINGERPRINT"},
        {"ro.bootimage.build.fingerprint", "FINGERPRINT"},
        {"ro.product.model",         "MODEL"},
        {"ro.product.brand",         "BRAND"},
        {"ro.product.manufacturer",  "MANUFACTURER"},
        {"ro.product.device",        "DEVICE"},
        {"ro.product.name",          "PRODUCT"},
        {"ro.product.board",         "BOARD"},
        {"ro.build.id",              "ID"},
        {"ro.build.display.id",      "DISPLAY"},
        {"ro.build.description",     "DESCRIPTION"},
        {"ro.build.version.release", "RELEASE"},
        {"ro.build.version.sdk",     "SDK_INT"},
        {"ro.build.version.security_patch", "SECURITY_PATCH"},
        {"ro.build.version.incremental",    "INCREMENTAL"},
        {"gsm.version.baseband",     "RADIO"},
    };

    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) return env->NewStringUTF(v.c_str());
    }
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(k.c_str(), buf) > 0)
        return env->NewStringUTF(buf);
    return j_def;
}

// ============================================================
// L3: Settings.Secure.getString hook (ANDROID_ID)
// NOTE: getString is Java-implemented; RegisterNatives alone won't take.
// For v1.0 POC we still register; v1.1 will bundle lsplant for real hook.
// ============================================================
static jstring (*orig_secure_get)(JNIEnv*, jclass, jobject, jstring) = nullptr;

static jstring hook_secure_get(JNIEnv* env, jclass c, jobject cr, jstring name) {
    if (name) {
        const char* raw = env->GetStringUTFChars(name, nullptr);
        std::string n(raw ? raw : "");
        env->ReleaseStringUTFChars(name, raw);
        if (n == "android_id") {
            const std::string& aid = val("ANDROID_ID");
            if (!aid.empty()) return env->NewStringUTF(aid.c_str());
        }
    }
    return orig_secure_get ? orig_secure_get(env, c, cr, name) : nullptr;
}

static void install_secure_hook(JNIEnv* env) {
    jclass c = env->FindClass("android/provider/Settings$Secure");
    if (!c) { env->ExceptionClear(); return; }
    JNINativeMethod m = {
        const_cast<char*>("getString"),
        const_cast<char*>("(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;"),
        reinterpret_cast<void*>(hook_secure_get)
    };
    env->RegisterNatives(c, &m, 1);
    env->ExceptionClear();
    env->DeleteLocalRef(c);
}

// ============================================================
// L4: AdvertisingIdClient.Info.getId hook (GAID) — stub for v1.1 lsplant
// ============================================================
static void install_gaid_hook(JNIEnv* env) {
    jclass c = env->FindClass("com/google/android/gms/ads/identifier/AdvertisingIdClient$Info");
    if (!c) { env->ExceptionClear(); return; }
    // TODO(v1.1): replace with lsplant::Hook for reliable Java bytecode hook
    env->DeleteLocalRef(c);
}

// ============================================================
// L5: WifiInfo.getMacAddress / getBSSID hook
// ============================================================
static jstring hook_wifi_mac(JNIEnv* env, jobject) {
    return env->NewStringUTF("02:00:00:00:00:00");
}
static jstring hook_wifi_bssid(JNIEnv* env, jobject) {
    return env->NewStringUTF("02:00:00:00:00:00");
}

static void install_wifi_hook(JNIEnv* env) {
    jclass c = env->FindClass("android/net/wifi/WifiInfo");
    if (!c) { env->ExceptionClear(); return; }
    JNINativeMethod m[] = {
        {const_cast<char*>("getMacAddress"), const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_wifi_mac)},
        {const_cast<char*>("getBSSID"), const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_wifi_bssid)},
    };
    env->RegisterNatives(c, m, 2);
    env->ExceptionClear();
    env->DeleteLocalRef(c);
}

// ============================================================
// L6: TelephonyManager hook (IMEI/DeviceId/Subscriber → null)
// ============================================================
static jstring hook_null_str(JNIEnv*, jobject) { return nullptr; }

static void install_telephony_hook(JNIEnv* env) {
    jclass c = env->FindClass("android/telephony/TelephonyManager");
    if (!c) { env->ExceptionClear(); return; }
    JNINativeMethod m[] = {
        {const_cast<char*>("getDeviceId"),     const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_null_str)},
        {const_cast<char*>("getImei"),         const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_null_str)},
        {const_cast<char*>("getSubscriberId"), const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_null_str)},
        {const_cast<char*>("getMeid"),         const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_null_str)},
    };
    env->RegisterNatives(c, m, 4);
    env->ExceptionClear();
    env->DeleteLocalRef(c);
}

// ============================================================
// L1: Build.* static field override
// ============================================================
static void set_str(JNIEnv* env, jclass c, const char* f, const std::string& v) {
    if (v.empty()) return;
    jfieldID id = env->GetStaticFieldID(c, f, "Ljava/lang/String;");
    if (!id) { env->ExceptionClear(); return; }
    jstring j = env->NewStringUTF(v.c_str());
    env->SetStaticObjectField(c, id, j);
    env->DeleteLocalRef(j);
}
static void set_int(JNIEnv* env, jclass c, const char* f, int v) {
    jfieldID id = env->GetStaticFieldID(c, f, "I");
    if (!id) { env->ExceptionClear(); return; }
    env->SetStaticIntField(c, id, v);
}

static void install_build_hook(JNIEnv* env) {
    jclass build = env->FindClass("android/os/Build");
    if (build) {
        static const std::vector<std::pair<const char*, const char*>> f = {
            {"BRAND","BRAND"}, {"MANUFACTURER","MANUFACTURER"},
            {"MODEL","MODEL"}, {"DEVICE","DEVICE"}, {"PRODUCT","PRODUCT"},
            {"BOARD","BOARD"}, {"HARDWARE","HARDWARE"},
            {"FINGERPRINT","FINGERPRINT"}, {"ID","ID"},
            {"DISPLAY","DISPLAY"}, {"BOOTLOADER","BOOTLOADER"},
            {"HOST","HOST"}, {"USER","USER"}, {"TYPE","TYPE"},
            {"TAGS","TAGS"}, {"SERIAL","SERIAL"}, {"RADIO","RADIO"},
        };
        for (const auto& [fn, k] : f) set_str(env, build, fn, val(k));
        env->DeleteLocalRef(build);
    } else env->ExceptionClear();

    jclass ver = env->FindClass("android/os/Build$VERSION");
    if (ver) {
        set_str(env, ver, "RELEASE",        val("RELEASE"));
        set_str(env, ver, "INCREMENTAL",    val("INCREMENTAL"));
        set_str(env, ver, "SECURITY_PATCH", val("SECURITY_PATCH"));
        const std::string& s = val("SDK_INT");
        if (!s.empty()) {
            int sdk = std::atoi(s.c_str());
            if (sdk > 0) set_int(env, ver, "SDK_INT", sdk);
        }
        env->DeleteLocalRef(ver);
    } else env->ExceptionClear();
}

// ============================================================
// Zygisk Module
// ============================================================
class TernakTT : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override { api_ = api; env_ = env; }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        std::string pkg;
        if (args && args->nice_name) {
            const char* raw = env_->GetStringUTFChars(args->nice_name, nullptr);
            pkg = raw ? raw : "";
            env_->ReleaseStringUTFChars(args->nice_name, raw);
        }
        if (!is_tt_pkg(pkg)) { unload(); return; }

        int fd = api_->connectCompanion();
        if (fd < 0) { unload(); return; }

        uint8_t cmd = CMD_GET_IDENTITY;
        write(fd, &cmd, 1);
        uint32_t len = 0;
        if (read(fd, &len, sizeof(len)) != sizeof(len) || len == 0 || len > 65536) {
            close(fd); unload(); return;
        }
        blob_.resize(len);
        size_t got = 0;
        while (got < len) {
            ssize_t n = read(fd, blob_.data() + got, len - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        close(fd);
        if (got != len) { unload(); return; }

        active_ = true;
        LOGI("TT target: %s (%u B)", pkg.c_str(), len);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;
        parse_blob();
        install_build_hook(env_);
        // L2: SystemProperties.native_get
        {
            jclass sp = env_->FindClass("android/os/SystemProperties");
            if (sp) {
                JNINativeMethod m = {
                    const_cast<char*>("native_get"),
                    const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
                    reinterpret_cast<void*>(hook_prop_get)};
                env_->RegisterNatives(sp, &m, 1);
                env_->ExceptionClear();
                env_->DeleteLocalRef(sp);
            } else env_->ExceptionClear();
        }
        install_secure_hook(env_);
        install_gaid_hook(env_);
        install_wifi_hook(env_);
        install_telephony_hook(env_);
    }

    void preServerSpecialize(ServerSpecializeArgs*) override { unload(); }

private:
    Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool active_ = false;
    std::vector<uint8_t> blob_;

    void unload() {
        if (api_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void parse_blob() {
        std::string s(blob_.begin(), blob_.end());
        std::istringstream iss(s);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            g_id[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
};

REGISTER_ZYGISK_MODULE(TernakTT)

extern "C" void ternak_tt_companion(int client);
REGISTER_ZYGISK_COMPANION(ternak_tt_companion)
