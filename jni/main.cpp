#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zygisk.hpp"
#include "tt_paths.hpp"
#include "companion_hardening.hpp"
#include "tt_scoped.hpp"
#include "tt_lookup.hpp"
#include "tt_proto.hpp"
#include "tt_as_safe.hpp"
#include "tt_bloom.hpp"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
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
#ifdef TT_DEBUG
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[D] " fmt, ##__VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

#ifndef TT_VARIANT_TAG
#ifdef TT_DEBUG
#define TT_VARIANT_TAG "debug"
#else
#define TT_VARIANT_TAG "release"
#endif
#endif

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

namespace {

struct IdEntry {
    std::string_view k;
    std::string_view v;
};

std::string          g_id_arena;
std::vector<IdEntry> g_id_flat;
tt::bloom::Filter    g_bloom;
bool                 g_bloom_ready = false;

constexpr tt::StrKV TT_IDENTITY_DEFAULTS[] = {
    {"SYS_BOOT_COMPLETED",    "1"},
    {"GSM_OPERATOR_NUMERIC",  "51010"},
    {"GSM_OPERATOR_ALPHA",    "Telkomsel"},
    {"GSM_OPERATOR_ISO",      "id"},
    {"BUILD_CHARACTERISTICS", "default"},
    {"PERSIST_TIMEZONE",      "Asia/Jakarta"},
    {"CPU_ABI",               "arm64-v8a"},
    {"CPU_ABI2",              ""},
    {"CPU_ABILIST",           "arm64-v8a,armeabi-v7a,armeabi"},
    {"CPU_ABILIST64",         "arm64-v8a"},
    {"CPU_ABILIST32",         "armeabi-v7a,armeabi"},
    {"DALVIK_HEAPGROWTHLIMIT","256m"},
    {"MEDIACODEC_MIN_RATE",   "8000"},
    {"MEDIACODEC_MAX_RATE",   "192000"},
    {"DEBUG_FORCE_RTL",       "false"},
    {"MULTISIM_CONFIG",       "ss"},
};

constexpr tt::StrKV L2_PROP_TO_ID[] = {
    {"dalvik.vm.heapgrowthlimit",      "DALVIK_HEAPGROWTHLIMIT"},
    {"debug.force_rtl",                "DEBUG_FORCE_RTL"},
    {"gsm.operator.alpha",             "GSM_OPERATOR_ALPHA"},
    {"gsm.operator.iso-country",       "GSM_OPERATOR_ISO"},
    {"gsm.operator.numeric",           "GSM_OPERATOR_NUMERIC"},
    {"gsm.sim.operator.alpha",         "GSM_OPERATOR_ALPHA"},
    {"gsm.sim.operator.iso-country",   "GSM_OPERATOR_ISO"},
    {"gsm.sim.operator.numeric",       "GSM_OPERATOR_NUMERIC"},
    {"gsm.version.baseband",           "RADIO"},
    {"persist.radio.multisim.config",  "MULTISIM_CONFIG"},
    {"persist.sys.timezone",           "PERSIST_TIMEZONE"},
    {"ro.boot.serialno",               "SERIAL"},
    {"ro.bootimage.build.fingerprint", "FINGERPRINT"},
    {"ro.build.characteristics",       "BUILD_CHARACTERISTICS"},
    {"ro.build.description",           "DESCRIPTION"},
    {"ro.build.display.id",            "DISPLAY"},
    {"ro.build.fingerprint",           "FINGERPRINT"},
    {"ro.build.host",                  "HOST"},
    {"ro.build.id",                    "ID"},
    {"ro.build.tags",                  "TAGS"},
    {"ro.build.type",                  "TYPE"},
    {"ro.build.user",                  "USER"},
    {"ro.build.version.incremental",   "INCREMENTAL"},
    {"ro.build.version.release",       "RELEASE"},
    {"ro.build.version.sdk",           "SDK_INT"},
    {"ro.build.version.security_patch","SECURITY_PATCH"},
    {"ro.mediacodec.max_sample_rate",  "MEDIACODEC_MAX_RATE"},
    {"ro.mediacodec.min_sample_rate",  "MEDIACODEC_MIN_RATE"},
    {"ro.product.board",               "BOARD"},
    {"ro.product.brand",               "BRAND"},
    {"ro.product.cpu.abi",             "CPU_ABI"},
    {"ro.product.cpu.abi2",            "CPU_ABI2"},
    {"ro.product.cpu.abilist",         "CPU_ABILIST"},
    {"ro.product.cpu.abilist32",       "CPU_ABILIST32"},
    {"ro.product.cpu.abilist64",       "CPU_ABILIST64"},
    {"ro.product.device",              "DEVICE"},
    {"ro.product.manufacturer",        "MANUFACTURER"},
    {"ro.product.model",               "MODEL"},
    {"ro.product.name",                "PRODUCT"},
    {"ro.serialno",                    "SERIAL"},
    {"sys.boot_completed",             "SYS_BOOT_COMPLETED"},
};

constexpr tt::StrKV L2_STATIC[] = {
    {"dalvik.vm.heapsize",               "512m"},
    {"dalvik.vm.isa.arm.features",       "default"},
    {"dalvik.vm.isa.arm.variant",        "generic"},
    {"dalvik.vm.isa.arm64.features",     "default"},
    {"dalvik.vm.isa.arm64.variant",      "generic"},
    {"gsm.operator.isroaming",           "false"},
    {"ro.allow.mock.location",           "0"},
    {"ro.board.platform",                "sm8250"},
    {"ro.build.version.preview_sdk",     "0"},
    {"ro.dalvik.vm.native.bridge",       "0"},
    {"ro.hardware",                      "qcom"},
    {"ro.zygote",                        "zygote64_32"},
    {"telephony.active_modems.max_count","1"},
};

constexpr tt::StrKV L7_INT_SPOOF[] = {
    {"build.version.extensions.ad_services", "15"},
    {"build.version.extensions.r",           "3"},
    {"build.version.extensions.s",           "4"},
    {"build.version.extensions.t",           "4"},
    {"build.version.extensions.u",           "13"},
    {"build.version.extensions.v",           "13"},
    {"debug.adservices.binder_timeout",      "10000"},
    {"debug.am.run_gc_trim_level",           "2147483647"},
    {"debug.am.run_mallopt_trim_level",      "2147483647"},
    {"debug.hwui.fps_divisor",               "1"},
    {"debug.sqlite.journalsizelimit",        "524288"},
    {"debug.sqlite.pagesize",                "4096"},
    {"debug.sqlite.wal.autocheckpoint",      "100"},
    {"debug.sqlite.wal.poolsize",            "0"},
    {"debug.sqlite.wal.truncatesize",        "1048576"},
    {"persist.wm.debug.ext_version_override","0"},
    {"ro.mediacodec.max_sample_rate",        "192000"},
    {"ro.mediacodec.min_sample_rate",        "8000"},
};

constexpr tt::StrKV L7_BOOL_SPOOF[] = {
    {"dalvik.vm.dexopt.secondary",              "1"},
    {"debug.force_rtl",                         "0"},
    {"debug.layout",                            "0"},
    {"debug.sqlite.no_double_quoted_strs",      "1"},
    {"framework.pause_bg_animations.enabled",   "0"},
    {"persist.sys.activity_anim_perf_override", "0"},
    {"persist.sys.lmk.reportkills",             "0"},
    {"sys.boot_completed",                      "1"},
    {"viewroot.profile_rendering",              "0"},
};

template <std::size_t N>
[[nodiscard]] std::string_view lookup_sorted(const tt::StrKV (&arr)[N], std::string_view k) noexcept {
    auto it = std::lower_bound(std::begin(arr), std::end(arr), k,
        [](const tt::StrKV& e, std::string_view q){ return e.k < q; });
    if (it != std::end(arr) && it->k == k) return it->v;
    return {};
}

[[nodiscard]] std::string_view id_val(std::string_view k) noexcept {
    auto it = std::lower_bound(g_id_flat.begin(), g_id_flat.end(), k,
        [](const IdEntry& e, std::string_view q){ return e.k < q; });
    if (it != g_id_flat.end() && it->k == k && !it->v.empty()) return it->v;
    return tt::lookup_kv(TT_IDENTITY_DEFAULTS, k);
}

[[nodiscard]] int parse_positive_int(std::string_view s) noexcept {
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    return v;
}

[[nodiscard]] jstring safe_new_utf(JNIEnv* env, std::string_view sv) {
    jstring j;
    if (sv.size() < 96) {
        char buf[96];
        std::memcpy(buf, sv.data(), sv.size());
        buf[sv.size()] = '\0';
        j = env->NewStringUTF(buf);
    } else {
        std::string tmp(sv);
        j = env->NewStringUTF(tmp.c_str());
    }
    if (!j) tt::clear_pending_exception(env);
    return j;
}

jstring hook_prop_get(JNIEnv* env, jclass, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    tt::ScopedUtfChars key_utf(env, j_key);
    std::string_view k = key_utf.view();

    std::string_view id_key = lookup_sorted(L2_PROP_TO_ID, k);
    if (!id_key.empty()) {
        std::string_view v = id_val(id_key);
        if (!v.empty()) {
            jstring out = safe_new_utf(env, v);
            return out ? out : j_def;
        }
    }
    std::string_view sv = lookup_sorted(L2_STATIC, k);
    if (!sv.empty()) {
        jstring out = safe_new_utf(env, sv);
        return out ? out : j_def;
    }
    return j_def;
}

[[nodiscard]] bool tt_should_suppress_key(std::string_view k) noexcept {
    if (k.size() >= 16 &&
        k.substr(0, 11) == "log.looper." &&
        k.substr(k.size() - 5) == ".slow") return true;
    if (k.size() >= 13 && k.substr(0, 13) == "debug.watson.") return true;
    return false;
}

[[nodiscard]] bool tt_lookup_int(std::string_view k, jint* out) noexcept {
    std::string_view v = lookup_sorted(L7_INT_SPOOF, k);
    if (!v.empty()) {
        int parsed = 0;
        bool neg = !v.empty() && v.front() == '-';
        if (neg) v.remove_prefix(1);
        for (char c : v) {
            if (c < '0' || c > '9') { parsed = 0; break; }
            parsed = parsed * 10 + (c - '0');
        }
        *out = neg ? -parsed : parsed;
        return true;
    }
    if (k == "ro.build.version.sdk") {
        int v_sdk = parse_positive_int(id_val("SDK_INT"));
        if (v_sdk > 0) { *out = v_sdk; return true; }
    }
    return false;
}

jint hook_prop_get_int(JNIEnv* env, jclass, jstring j_key, jint def) {
    if (!j_key) return def;
    tt::ScopedUtfChars key_utf(env, j_key);
    std::string_view k = key_utf.view();
    jint out = def;
    if (tt_lookup_int(k, &out)) return out;
    if (tt_should_suppress_key(k)) return def;
    return def;
}

constexpr tt::StrKV L7_LONG_SPOOF[] = {
    {"ro.gfx.driver_build_time", "1704067200"},
};

jlong hook_prop_get_long(JNIEnv* env, jclass, jstring j_key, jlong def) {
    if (!j_key) return def;
    tt::ScopedUtfChars key_utf(env, j_key);
    std::string_view k = key_utf.view();
    std::string_view v = lookup_sorted(L7_LONG_SPOOF, k);
    if (!v.empty()) {
        long long parsed = 0;
        for (char c : v) {
            if (c < '0' || c > '9') { parsed = 0; break; }
            parsed = parsed * 10 + (c - '0');
        }
        return (jlong)parsed;
    }
    return def;
}

jboolean hook_prop_get_bool(JNIEnv* env, jclass, jstring j_key, jboolean def) {
    if (!j_key) return def;
    tt::ScopedUtfChars key_utf(env, j_key);
    std::string_view k = key_utf.view();
    std::string_view v = lookup_sorted(L7_BOOL_SPOOF, k);
    if (!v.empty()) return v == "1" ? JNI_TRUE : JNI_FALSE;
    return def;
}

jstring hook_build_radio(JNIEnv* env, jclass) {
    std::string_view v = id_val("RADIO");
    if (v.empty()) return env->NewStringUTF("");
    jstring out = safe_new_utf(env, v);
    return out ? out : env->NewStringUTF("");
}

void set_str(JNIEnv* env, jclass c, const char* f, std::string_view v) {
    if (v.empty()) return;
    jfieldID id = env->GetStaticFieldID(c, f, "Ljava/lang/String;");
    if (!id) { env->ExceptionClear(); return; }
    jstring j = safe_new_utf(env, v);
    if (!j) return;
    env->SetStaticObjectField(c, id, j);
    env->DeleteLocalRef(j);
}

void set_int(JNIEnv* env, jclass c, const char* f, int v) {
    jfieldID id = env->GetStaticFieldID(c, f, "I");
    if (!id) { env->ExceptionClear(); return; }
    env->SetStaticIntField(c, id, v);
}

void install_build_hook(JNIEnv* env) {
    static constexpr tt::StrKV BUILD_FIELDS[] = {
        {"BRAND","BRAND"}, {"MANUFACTURER","MANUFACTURER"},
        {"MODEL","MODEL"}, {"DEVICE","DEVICE"}, {"PRODUCT","PRODUCT"},
        {"BOARD","BOARD"}, {"HARDWARE","HARDWARE"},
        {"FINGERPRINT","FINGERPRINT"}, {"ID","ID"},
        {"DISPLAY","DISPLAY"}, {"BOOTLOADER","BOOTLOADER"},
        {"HOST","HOST"}, {"USER","USER"}, {"TYPE","TYPE"},
        {"TAGS","TAGS"}, {"SERIAL","SERIAL"}, {"RADIO","RADIO"},
    };
    jclass build = env->FindClass("android/os/Build");
    if (build) {
        for (const auto& e : BUILD_FIELDS) {
            set_str(env, build, e.k.data(), id_val(e.v));
        }
        env->DeleteLocalRef(build);
    } else {
        env->ExceptionClear();
    }

    jclass ver = env->FindClass("android/os/Build$VERSION");
    if (ver) {
        set_str(env, ver, "RELEASE",        id_val("RELEASE"));
        set_str(env, ver, "INCREMENTAL",    id_val("INCREMENTAL"));
        set_str(env, ver, "SECURITY_PATCH", id_val("SECURITY_PATCH"));
        int sdk = parse_positive_int(id_val("SDK_INT"));
        if (sdk > 0) set_int(env, ver, "SDK_INT", sdk);
        env->DeleteLocalRef(ver);
    } else {
        env->ExceptionClear();
    }
}

using openat_t = int (*)(int, const char*, int, ...);
openat_t orig_openat = nullptr;

[[nodiscard]] bool is_sensitive_proc_path(const char* p) noexcept {
    if (!p) return false;
    if (::strncmp(p, "/proc/", 6) != 0) return false;
    size_t len = ::strlen(p);
    auto ends_with = [&](const char* suf) {
        size_t sl = ::strlen(suf);
        return len >= sl && ::strcmp(p + len - sl, suf) == 0;
    };
    return ends_with("/mountinfo") ||
           ends_with("/mounts")    ||
           ends_with("/maps")      ||
           ends_with("/status")    ||
           ends_with("/cgroup");
}

thread_local bool tt_in_openat_hook = false;

int hook_openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!orig_openat) return ::openat(dirfd, path, flags, mode);
    if (tt_in_openat_hook || !is_sensitive_proc_path(path)) {
        return orig_openat(dirfd, path, flags, mode);
    }

    tt_in_openat_hook = true;
    int real_fd = orig_openat(dirfd, path, flags, mode);
    if (real_fd < 0) { tt_in_openat_hook = false; return real_fd; }

    std::string content;
    content.reserve(8192);
    char buf[4096];
    ssize_t n;
    while ((n = ::read(real_fd, buf, sizeof(buf))) > 0) {
        content.append(buf, static_cast<size_t>(n));
    }
    ::close(real_fd);

    std::string filtered;
    filtered.reserve(content.size());
    const char* const data = content.data();
    const size_t      len  = content.size();
    size_t line_start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i < len && data[i] != '\n') continue;
        std::string_view line(data + line_start, i - line_start);
        line_start = i + 1;
        bool skip = false;
        for (size_t j = 0; j + 9 <= line.size(); ++j) {
            if (std::memcmp(line.data() + j, "ternak", 6) == 0) {
                char sep = line[j + 6];
                if ((sep == '_' || sep == '-') &&
                    line[j + 7] == 't' && line[j + 8] == 't') {
                    skip = true; break;
                }
            }
        }
        if (skip) continue;
        filtered.append(line.data(), line.size());
        filtered.push_back('\n');
    }

    int mfd = tt_memfd_create("clean", MFD_CLOEXEC);
    if (mfd < 0) {
        tt_in_openat_hook = false;
        return orig_openat(dirfd, path, flags, mode);
    }
    if (!filtered.empty() && !tt::write_all(mfd, filtered.data(), filtered.size())) {
        ::close(mfd);
        tt_in_openat_hook = false;
        return orig_openat(dirfd, path, flags, mode);
    }
    ::lseek(mfd, 0, SEEK_SET);
    tt_in_openat_hook = false;
    return mfd;
}

[[nodiscard]] bool find_libc_dev_inode(dev_t* dev_out, ino_t* ino_out) {
    FILE* f = ::fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (::fgets(line, sizeof(line), f)) {
        char* nl = ::strchr(line, '\n'); if (nl) *nl = 0;
        char* sp = ::strrchr(line, ' '); if (!sp) continue;
        char* path = sp + 1;
        size_t plen = ::strlen(path);
        if (plen < 8) continue;
        if (::strcmp(path + plen - 8, "/libc.so") != 0) continue;
        unsigned long a1, a2, off;
        char perms[8] = {0};
        unsigned int dmaj = 0, dmin = 0;
        unsigned long ino = 0;
        if (::sscanf(line, "%lx-%lx %7s %lx %x:%x %lu",
                     &a1, &a2, perms, &off, &dmaj, &dmin, &ino) == 7) {
            *dev_out = (dev_t)((dmaj << 8) | dmin);
            *ino_out = (ino_t)ino;
            found = true; break;
        }
    }
    ::fclose(f);
    return found;
}

using api_level_t = int (*)();
api_level_t orig_api_level = nullptr;

int hook_api_level() {
    int sdk = parse_positive_int(id_val("SDK_INT"));
    if (sdk > 0) return sdk;
    return orig_api_level ? orig_api_level() : 0;
}

void install_proc_sanitizer(Api* api) {
    if (!api) return;
    dev_t dev = 0;
    ino_t ino = 0;
    if (!find_libc_dev_inode(&dev, &ino)) return;
    api->pltHookRegister(dev, ino, "openat",
                         reinterpret_cast<void*>(hook_openat),
                         reinterpret_cast<void**>(&orig_openat));
    api->pltHookRegister(dev, ino, "__openat",
                         reinterpret_cast<void*>(hook_openat),
                         reinterpret_cast<void**>(&orig_openat));
    api->pltHookRegister(dev, ino, "android_get_device_api_level",
                         reinterpret_cast<void*>(hook_api_level),
                         reinterpret_cast<void**>(&orig_api_level));
    api->pltHookCommit();
}

char g_watchdog_pkg[128] = "?";
long g_load_time_ms = 0;
int  g_crash_log_fd = -1;
std::atomic<int> g_crash_count[NSIG];
constexpr int CRASH_LIMIT = 3;
struct sigaction g_prev_sig[NSIG];
constexpr std::size_t TT_ALTSTACK_SIZE = 64 * 1024;
alignas(16) uint8_t g_altstack[TT_ALTSTACK_SIZE];

long tt_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

const char* tt_sig_name(int sig) noexcept {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGABRT: return "SIGABRT";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGSYS:  return "SIGSYS";
        case SIGTERM: return "SIGTERM";
        case SIGPIPE: return "SIGPIPE";
        default:      return "?";
    }
}

void tt_signal_handler(int sig, siginfo_t* info, void*) {
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGPIPE);
    sigaddset(&blk, SIGCHLD);
    (void)sigprocmask(SIG_BLOCK, &blk, nullptr);

    int n = 0;
    if (sig >= 0 && sig < NSIG) {
        n = g_crash_count[sig].fetch_add(1, std::memory_order_relaxed) + 1;
    }

    if (n <= CRASH_LIMIT) {
        long alive = tt_now_ms() - g_load_time_ms;
        char buf[320];
        std::size_t p = 0;
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, "[ternak_tt/");
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, TT_VARIANT_TAG);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, "] CRASH pkg=");
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, g_watchdog_pkg);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, " pid=");
        p += tt::as_safe::write_int_dec(buf + p, sizeof(buf) - p, (long)getpid());
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, " sig=");
        p += tt::as_safe::write_int_dec(buf + p, sizeof(buf) - p, (long)sig);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, "(");
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, tt_sig_name(sig));
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, ") code=");
        p += tt::as_safe::write_int_dec(buf + p, sizeof(buf) - p, info ? (long)info->si_code : 0L);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, " addr=");
        p += tt::as_safe::write_hex_ptr(buf + p, sizeof(buf) - p, info ? info->si_addr : nullptr);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, " sender=");
        p += tt::as_safe::write_int_dec(buf + p, sizeof(buf) - p, info ? (long)info->si_pid : 0L);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, " alive=");
        p += tt::as_safe::write_int_dec(buf + p, sizeof(buf) - p, alive);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, "ms hit=");
        p += tt::as_safe::write_int_dec(buf + p, sizeof(buf) - p, (long)n);
        p += tt::as_safe::write_str(buf + p, sizeof(buf) - p, "/");
        p += tt::as_safe::write_int_dec(buf + p, sizeof(buf) - p, (long)CRASH_LIMIT);
        if (p < sizeof(buf)) buf[p++] = '\n';
        if (g_crash_log_fd >= 0) (void)::write(g_crash_log_fd, buf, p);
        (void)::write(2, buf, p);
    }

    if (sig >= 0 && sig < NSIG) {
        struct sigaction* pv = &g_prev_sig[sig];
        bool prev_is_real =
            ((pv->sa_flags & SA_SIGINFO) && pv->sa_sigaction != nullptr) ||
            (!(pv->sa_flags & SA_SIGINFO) &&
             pv->sa_handler != SIG_DFL && pv->sa_handler != SIG_IGN &&
             pv->sa_handler != nullptr);
        if (prev_is_real) {
            sigaction(sig, pv, nullptr);
        } else {
            struct sigaction dfl;
            std::memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            sigaction(sig, &dfl, nullptr);
        }
    }
}

void install_crash_watchdog(const std::string& pkg) {
    for (int i = 0; i < NSIG; ++i) g_crash_count[i].store(0, std::memory_order_relaxed);
    ::snprintf(g_watchdog_pkg, sizeof(g_watchdog_pkg), "%s", pkg.c_str());
    g_load_time_ms = tt_now_ms();

    stack_t ss;
    ss.ss_sp    = g_altstack;
    ss.ss_size  = sizeof(g_altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    if (g_crash_log_fd < 0 && !pkg.empty()) {
        char logpath[256];
        ::snprintf(logpath, sizeof(logpath),
                   "/data/data/%s/cache/tt-crash.log", pkg.c_str());
        g_crash_log_fd = ::open(logpath,
                                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = tt_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGSEGV);
    sigaddset(&sa.sa_mask, SIGBUS);
    sigaddset(&sa.sa_mask, SIGABRT);
    sigaddset(&sa.sa_mask, SIGFPE);
    sigaddset(&sa.sa_mask, SIGILL);
    sigaddset(&sa.sa_mask, SIGSYS);

    static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGSYS };
    for (int s : sigs) sigaction(s, &sa, &g_prev_sig[s]);
}

void load_bloom_filter() {
    int fd = ::open(tt::paths::BLOOM_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (::fstat(fd, &st) == 0 && (size_t)st.st_size == sizeof(tt::bloom::Filter)) {
        void* m = ::mmap(nullptr, sizeof(tt::bloom::Filter),
                         PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            std::memcpy(&g_bloom, m, sizeof(tt::bloom::Filter));
            ::munmap(m, sizeof(tt::bloom::Filter));
            g_bloom_ready = true;
        }
    }
    ::close(fd);
}

}

class TernakTT : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
        load_bloom_filter();
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        std::string pkg;
        if (args && args->nice_name) {
            tt::ScopedUtfChars nn(env_, args->nice_name);
            pkg.assign(nn.view());
        }
        if (pkg.empty()) { unload(); return; }
        if (g_bloom_ready && !g_bloom.might_contain(pkg)) { unload(); return; }

        int fd = api_->connectCompanion();
        if (fd < 0) { unload(); return; }

        tt::proto::Header hdr;
        tt::proto::InitAppRequestPayload payload;
        payload.pid     = static_cast<uint32_t>(::getpid());
        payload.pkg_len = static_cast<uint16_t>(pkg.size());
        uint32_t plen   = (uint32_t)(sizeof(payload) + payload.pkg_len);
        tt::proto::fill_header(hdr, tt::proto::CMD_INIT_APP, plen);

        if (!tt::write_all(fd, &hdr, sizeof(hdr)) ||
            !tt::write_all(fd, &payload, sizeof(payload)) ||
            (payload.pkg_len && !tt::write_all(fd, pkg.data(), payload.pkg_len))) {
            ::close(fd); unload(); return;
        }

        tt::proto::InitAppResponse resp{};
        if (!tt::read_all(fd, &resp, sizeof(resp))) { ::close(fd); unload(); return; }
        if (!tt::proto::check_header(resp.hdr, tt::proto::CMD_INIT_APP) || !resp.is_target) {
            ::close(fd); unload(); return;
        }

        if (resp.blob_len > (1u << 20)) { ::close(fd); unload(); return; }
        blob_.resize(resp.blob_len);
        if (resp.blob_len && !tt::read_all(fd, blob_.data(), resp.blob_len)) {
            ::close(fd); unload(); return;
        }
        ::close(fd);

        n_keys_   = resp.nkeys;
        mount_ok_ = resp.mount_ok;
        active_   = true;
        pkg_      = pkg;
        LOGI("target: %s nkeys=%u mount_ok=%u [%s]",
             pkg.c_str(), (unsigned)n_keys_, (unsigned)mount_ok_, TT_VARIANT_TAG);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;
        parse_binary_blob();

        install_build_hook(env_);

        JNINativeMethod sp_methods[] = {
            {const_cast<char*>("native_get"),
             const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
             reinterpret_cast<void*>(hook_prop_get)},
            {const_cast<char*>("native_get_int"),
             const_cast<char*>("(Ljava/lang/String;I)I"),
             reinterpret_cast<void*>(hook_prop_get_int)},
            {const_cast<char*>("native_get_long"),
             const_cast<char*>("(Ljava/lang/String;J)J"),
             reinterpret_cast<void*>(hook_prop_get_long)},
            {const_cast<char*>("native_get_boolean"),
             const_cast<char*>("(Ljava/lang/String;Z)Z"),
             reinterpret_cast<void*>(hook_prop_get_bool)},
        };
        api_->hookJniNativeMethods(env_, "android/os/SystemProperties", sp_methods, 4);

        JNINativeMethod build_methods[] = {
            {const_cast<char*>("getRadioVersion"),
             const_cast<char*>("()Ljava/lang/String;"),
             reinterpret_cast<void*>(hook_build_radio)},
        };
        api_->hookJniNativeMethods(env_, "android/os/Build", build_methods, 1);

        install_proc_sanitizer(api_);
        install_crash_watchdog(pkg_);
    }

    void preServerSpecialize(ServerSpecializeArgs*) override { unload(); }

private:
    Api*                 api_      = nullptr;
    JNIEnv*              env_      = nullptr;
    std::string          pkg_;
    bool                 active_   = false;
    uint16_t             n_keys_   = 0;
    uint16_t             mount_ok_ = 0;
    std::vector<uint8_t> blob_;

    void unload() {
        if (api_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void parse_binary_blob() {
        g_id_arena.assign(reinterpret_cast<const char*>(blob_.data()), blob_.size());
        g_id_flat.clear();
        g_id_flat.reserve(n_keys_);
        const char* base = g_id_arena.data();
        size_t off = 0;
        for (uint16_t i = 0; i < n_keys_ && off + sizeof(tt::proto::BinaryEntry) <= g_id_arena.size(); ++i) {
            tt::proto::BinaryEntry e;
            std::memcpy(&e, base + off, sizeof(e));
            off += sizeof(e);
            if (off + e.klen + e.vlen > g_id_arena.size()) break;
            std::string_view k(base + off, e.klen); off += e.klen;
            std::string_view v(base + off, e.vlen); off += e.vlen;
            g_id_flat.push_back({k, v});
        }
        std::sort(g_id_flat.begin(), g_id_flat.end(),
            [](const IdEntry& a, const IdEntry& b){ return a.k < b.k; });
        std::atomic_thread_fence(std::memory_order_release);
    }
};

REGISTER_ZYGISK_MODULE(TernakTT)

extern "C" void ternak_tt_companion(int client);
REGISTER_ZYGISK_COMPANION(ternak_tt_companion)
