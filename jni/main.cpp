// ============================================================
// Ternak TT v1.0 — TikTok-focused Zygisk hook
// 6 hook layers: Build.*, native_get, Settings.Secure,
//                GAID, WiFi MAC, Telephony
// ============================================================
#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <android/log.h>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <cstdlib>
#include "zygisk.hpp"

// memfd_create wrapper (not in older NDK headers)
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#include <sys/syscall.h>
#ifndef __NR_memfd_create
  #if defined(__aarch64__)
    #define __NR_memfd_create 279
  #elif defined(__arm__)
    #define __NR_memfd_create 385
  #elif defined(__x86_64__)
    #define __NR_memfd_create 319
  #elif defined(__i386__)
    #define __NR_memfd_create 356
  #endif
#endif
static inline int tt_memfd_create(const char* name, unsigned flags) {
    return (int)syscall(__NR_memfd_create, name, flags);
}

#define LOG_TAG "TernakTT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

enum : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,  // v1.0.4: companion-side mount agent
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
// v1.0.3: Mount namespace overlay + /proc sanitizer
// ============================================================
static const char* MOUNTDIR = "/data/adb/modules/ternak_tt/mount";

struct BindEntry { const char* src_rel; const char* dst; };
static const BindEntry BIND_ENTRIES[] = {
    {"system/build.prop",     "/system/build.prop"},
    {"vendor/build.prop",     "/vendor/build.prop"},
    {"odm/build.prop",        "/odm/build.prop"},
    {"product/build.prop",    "/product/build.prop"},
    {"system_ext/build.prop", "/system_ext/build.prop"},
    {"settings_secure.xml",   "/data/system/users/0/settings_secure.xml"},
};

// v1.0.4: mount() dari preAppSpecialize gagal EACCES di Zygisk-Next fork
// (HMA-OSS) karena caps di-drop. Solusi: minta companion (yang masih root
// + full caps) untuk setns() masuk ns kita + mount di sana.
static void request_companion_mounts(zygisk::Api* api) {
    if (!api) return;
    int fd = api->connectCompanion();
    if (fd < 0) { LOGE("companion connect for mounts failed"); return; }

    uint8_t cmd = CMD_DO_MOUNTS;
    uint32_t pid = (uint32_t)::getpid();
    if (::write(fd, &cmd, 1) != 1 ||
        ::write(fd, &pid, sizeof(pid)) != (ssize_t)sizeof(pid)) {
        LOGE("companion write failed");
        ::close(fd); return;
    }
    uint32_t ok = 0;
    ssize_t n = ::read(fd, &ok, sizeof(ok));
    ::close(fd);
    if (n != (ssize_t)sizeof(ok)) {
        LOGE("companion read ack failed");
        return;
    }
    LOGI("bind-mount via companion: %u ok (pid=%u)", ok, pid);
}

// PLT hook openat to sanitize /proc/self/{mountinfo,mounts,maps}
// so anti-detect can't see our bind mounts by grepping mountinfo.
using openat_t = int (*)(int, const char*, int, ...);
static openat_t orig_openat = nullptr;

static bool is_sensitive_proc_path(const char* p) {
    if (!p) return false;
    if (!strstr(p, "/proc/")) return false;
    if (strstr(p, "/mountinfo")) return true;
    if (strstr(p, "/mounts"))    return true;
    if (strstr(p, "/maps"))      return true;
    return false;
}

static int hook_openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!orig_openat || !is_sensitive_proc_path(path)) {
        return orig_openat ? orig_openat(dirfd, path, flags, mode)
                           : ::openat(dirfd, path, flags, mode);
    }
    int real_fd = orig_openat(dirfd, path, flags, mode);
    if (real_fd < 0) return real_fd;

    std::string content;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(real_fd, buf, sizeof(buf))) > 0) content.append(buf, n);
    ::close(real_fd);

    std::string filtered;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("ternak_tt") != std::string::npos) continue;
        if (line.find("ternak-tt") != std::string::npos) continue;
        filtered.append(line);
        filtered.push_back('\n');
    }

    int mfd = tt_memfd_create("clean", MFD_CLOEXEC);
    if (mfd < 0) return orig_openat(dirfd, path, flags, mode);
    if (!filtered.empty()) {
        ::write(mfd, filtered.data(), filtered.size());
    }
    ::lseek(mfd, 0, SEEK_SET);
    return mfd;
}

// v1.0.5: real Zygisk-Next pltHookRegister takes (dev_t, ino_t, symbol, ...).
// Parse /proc/self/maps to resolve libc.so's dev+inode.
static bool find_libc_dev_inode(dev_t* dev_out, ino_t* ino_out) {
    FILE* f = ::fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (::fgets(line, sizeof(line), f)) {
        // Line format:
        //   addr1-addr2 perms offset dev inode path
        //   7f...-7f... r-xp 00000000 fd:00 12345 /apex/.../libc.so
        char* nl = ::strchr(line, '\n');
        if (nl) *nl = 0;
        // Path is the last space-separated token; make sure it ends with /libc.so
        char* sp = ::strrchr(line, ' ');
        if (!sp) continue;
        char* path = sp + 1;
        size_t plen = ::strlen(path);
        if (plen < 8) continue;
        if (::strcmp(path + plen - 8, "/libc.so") != 0) continue;
        // Parse fields
        unsigned long a1, a2, off;
        char perms[8] = {0};
        unsigned int dmaj = 0, dmin = 0;
        unsigned long ino = 0;
        if (::sscanf(line, "%lx-%lx %7s %lx %x:%x %lu",
                     &a1, &a2, perms, &off, &dmaj, &dmin, &ino) == 7) {
            *dev_out = (dev_t)((dmaj << 8) | dmin);
            *ino_out = (ino_t)ino;
            found = true;
            break;
        }
    }
    ::fclose(f);
    return found;
}

static void install_proc_sanitizer(Api* api) {
    if (!api) return;
    dev_t dev = 0;
    ino_t ino = 0;
    if (!find_libc_dev_inode(&dev, &ino)) {
        LOGI("proc sanitizer: libc.so not found in maps (skip)");
        return;
    }
    api->pltHookRegister(dev, ino, "openat",
                         reinterpret_cast<void*>(hook_openat),
                         reinterpret_cast<void**>(&orig_openat));
    api->pltHookRegister(dev, ino, "__openat",
                         reinterpret_cast<void*>(hook_openat),
                         reinterpret_cast<void**>(&orig_openat));
    if (!api->pltHookCommit()) {
        LOGI("proc sanitizer: PLT commit false (best-effort skipped)");
    } else {
        LOGI("proc sanitizer installed (dev=%lu ino=%lu)",
             (unsigned long)dev, (unsigned long)ino);
    }
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

        // v1.0.4: request companion to setns+mount our overlay into this
        // TT process's mount namespace. In-process mount fails EACCES on
        // modern Zygisk-Next forks (caps stripped in preAppSpecialize).
        request_companion_mounts(api_);
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
        // v1.0.3: hide bind mounts from /proc/self/mountinfo|mounts|maps
        install_proc_sanitizer(api_);
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
