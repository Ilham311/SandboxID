#include "module_impl.hpp"

// ---------- install_build_hook ----------
// Reflects into android.os.Build and overwrites identity-mapped fields via JNI.

static bool sbx_set_build_field(JNIEnv* env, jclass clz,
                                const char* name, const std::string& v) {
    jfieldID fid = env->GetStaticFieldID(clz, name, "Ljava/lang/String;");
    if (!fid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jstring js = env->NewStringUTF(v.c_str());
    if (!js) return false;
    env->SetStaticObjectField(clz, fid, js);
    env->DeleteLocalRef(js);
    return true;
}

static bool sbx_set_build_int(JNIEnv* env, jclass clz,
                              const char* name, int v) {
    jfieldID fid = env->GetStaticFieldID(clz, name, "I");
    if (!fid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    env->SetStaticIntField(clz, fid, v);
    return true;
}

void install_build_hook(JNIEnv* env) {
    jclass clz = env->FindClass("android/os/Build");
    if (!clz || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    struct F { const char* field; const char* key; };
    static const F fields[] = {
        {"MODEL",        "MODEL"},
        {"BRAND",        "BRAND"},
        {"MANUFACTURER", "MANUFACTURER"},
        {"DEVICE",       "DEVICE"},
        {"PRODUCT",      "PRODUCT"},
        {"BOARD",        "BOARD"},
        {"HARDWARE",     "HARDWARE"},
        {"DISPLAY",      "DISPLAY"},
        {"ID",           "ID"},
        {"FINGERPRINT",  "FINGERPRINT"},
        {"HOST",         "HOST"},
        {"TAGS",         "TAGS"},
        {"TYPE",         "TYPE"},
    };
    int set = 0;
    for (const auto& f : fields) {
        const std::string& v = val(f.key);
        if (!v.empty() && sbx_set_build_field(env, clz, f.field, v)) ++set;
    }

    jclass vclz = env->FindClass("android/os/Build$VERSION");
    if (vclz && !env->ExceptionCheck()) {
        const std::string& rel = val("RELEASE");
        if (!rel.empty()) sbx_set_build_field(env, vclz, "RELEASE", rel);

        const std::string& sdk_s = val("SDK_INT");
        if (!sdk_s.empty()) {
            long long sdk = 0;
            if (sbx_parse_longlong(sdk_s, sdk))
                sbx_set_build_int(env, vclz, "SDK_INT", static_cast<int>(sdk));
        }

        const std::string& inc = val("INCREMENTAL");
        if (!inc.empty()) sbx_set_build_field(env, vclz, "INCREMENTAL", inc);

        const std::string& sp = val("SECURITY_PATCH");
        if (!sp.empty()) sbx_set_build_field(env, vclz, "SECURITY_PATCH", sp);

        env->DeleteLocalRef(vclz);
    } else {
        env->ExceptionClear();
    }

    LOGD("BUILD hook: %d field(s) spoofed", set);
    env->DeleteLocalRef(clz);
}

// ---------- install_native_read_hooks ----------
// PLT-hooks read/open/pread64 in the target process to intercept reads of
// /proc/version, /proc/meminfo, /sys/class/net/*/address, etc.

static ssize_t (*orig_read)(int, void*, size_t) = nullptr;
static int     (*orig_open)(const char*, int, ...)  = nullptr;

static thread_local sbxnr::Kind g_last_kind = sbxnr::NONE;
static thread_local std::string g_synth_buf;
static thread_local size_t     g_synth_off = 0;

static int hook_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    sbxnr::Kind k = sbxnr::classify(path);
    if (k != sbxnr::NONE) {
        g_last_kind = k;
        g_synth_buf.clear();
        g_synth_off = 0;
    }
    return orig_open ? orig_open(path, flags, mode)
                     : ::syscall(__NR_openat, AT_FDCWD, path, flags, mode);
}

static ssize_t hook_read(int fd, void* buf, size_t count) {
    ssize_t n = orig_read ? orig_read(fd, buf, count)
                         : ::read(fd, buf, count);
    if (n <= 0 || g_last_kind == sbxnr::NONE) return n;

    sbxnr::Kind kind = g_last_kind;
    g_last_kind = sbxnr::NONE;

    auto serve_synth = [&](const std::string& content) -> ssize_t {
        if (g_synth_buf.empty()) { g_synth_buf = content; g_synth_off = 0; }
        size_t avail = g_synth_buf.size() - g_synth_off;
        if (avail == 0) return 0;
        size_t cp = avail < count ? avail : count;
        ::memcpy(buf, g_synth_buf.data() + g_synth_off, cp);
        g_synth_off += cp;
        return static_cast<ssize_t>(cp);
    };

    switch (kind) {
    case sbxnr::BOOTID: {
        const std::string& serial = val("SERIAL");
        if (!serial.empty()) {
            uint64_t seed = sbxnr::fnv1a(serial + ":bootid");
            return serve_synth(sbxnr::uuid_from_seed(seed) + "\n");
        }
        break;
    }
    case sbxnr::MAC: {
        const std::string& mac = val("WLAN_MAC");
        if (!mac.empty() && sbxnr::is_valid_mac(mac))
            return serve_synth(mac + "\n");
        const std::string& serial = val("SERIAL");
        if (!serial.empty()) {
            uint64_t seed = sbxnr::fnv1a(serial + ":mac");
            return serve_synth(sbxnr::mac_from_seed(seed) + "\n");
        }
        break;
    }
    case sbxnr::VERSION: {
        std::string ver = sbxnr::synth_proc_version(
            val("RELEASE"), val("INCREMENTAL"), val("BOARD_PLATFORM"),
            val("HOST"), sbxnr::fnv1a(val("SERIAL")));
        if (!ver.empty()) return serve_synth(ver + "\n");
        break;
    }
    case sbxnr::MEMINFO: {
        std::string real(static_cast<const char*>(buf), static_cast<size_t>(n));
        int gb = sbxnr::pixel_ram_gb(val("MODEL"));
        std::string patched = sbxnr::patch_meminfo(real, gb);
        if (patched != real) return serve_synth(patched);
        break;
    }
    case sbxnr::CPUINFO: {
        std::string real(static_cast<const char*>(buf), static_cast<size_t>(n));
        std::string repl;
        int action = sbxnr::cpu_action_for(val("SOC_MANUFACTURER"), val("SOC_MODEL"), repl);
        std::string patched;
        if (sbxnr::patch_cpuinfo(real, action, repl, patched))
            return serve_synth(patched);
        break;
    }
    case sbxnr::SELINUX_ENFORCE:
        return serve_synth(sbxnr::selinux_enforce_content());
    case sbxnr::APPLOG_XML: {
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        uint64_t seed = sbxnr::fnv1a(serial + ":applog");
        sbxnr::ApplogIds ids = sbxnr::make_applog_ids(seed, 1700000000000ULL);
        std::string real(static_cast<const char*>(buf), static_cast<size_t>(n));
        std::string patched;
        if (sbxnr::patch_applog_xml(real, ids, patched))
            return serve_synth(patched);
        return serve_synth(sbxnr::applog_xml_synth(ids));
    }
    case sbxnr::BD_RAW_DID: {
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        uint64_t seed = sbxnr::fnv1a(serial + ":applog");
        return serve_synth(sbxnr::make_applog_ids(seed, 1700000000000ULL).did);
    }
    case sbxnr::BD_RAW_IID: {
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        uint64_t seed = sbxnr::fnv1a(serial + ":applog");
        return serve_synth(sbxnr::make_applog_ids(seed, 1700000000000ULL).iid);
    }
    case sbxnr::BD_RAW_OPENUDID: {
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        uint64_t seed = sbxnr::fnv1a(serial + ":applog");
        return serve_synth(sbxnr::make_applog_ids(seed, 1700000000000ULL).openudid);
    }
    case sbxnr::BD_RAW_CLIENTUDID: {
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        uint64_t seed = sbxnr::fnv1a(serial + ":applog");
        return serve_synth(sbxnr::make_applog_ids(seed, 1700000000000ULL).clientudid);
    }
    case sbxnr::BD_RAW_CDID: {
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        uint64_t seed = sbxnr::fnv1a(serial + ":applog");
        return serve_synth(sbxnr::make_applog_ids(seed, 1700000000000ULL).cdid);
    }
    default:
        break;
    }
    return n;
}

void install_native_read_hooks(Api* api) {
    const std::string& nr = val("SBX_NATIVE_READ");
    if (nr == "0") {
        LOGD("NATIVE_READ disabled by SBX_NATIVE_READ=0");
        return;
    }

    static const char* const kLibs[] = {
        "/libc.so",
    };
    int registered = 0;
    for (const char* suffix : kLibs) {
        dev_t dev = 0; ino_t ino = 0;
        FILE* f = fopen("/proc/self/maps", "re");
        if (!f) continue;
        char* line = nullptr;
        size_t cap = 0;
        size_t sl = strlen(suffix);
        bool found = false;
        while (getline(&line, &cap, f) != -1) {
            char* path = strchr(line, '/');
            if (!path) continue;
            size_t pl = strlen(path);
            if (pl && path[pl - 1] == '\n') path[--pl] = '\0';
            if (pl >= sl && strcmp(path + pl - sl, suffix) == 0) {
                struct stat st;
                if (stat(path, &st) == 0) { dev = st.st_dev; ino = st.st_ino; found = true; }
                break;
            }
        }
        free(line);
        fclose(f);
        if (!found) continue;

        api->pltHookRegister(dev, ino, "read",
                             reinterpret_cast<void*>(hook_read),
                             reinterpret_cast<void**>(&orig_read));
        api->pltHookRegister(dev, ino, "open",
                             reinterpret_cast<void*>(hook_open),
                             reinterpret_cast<void**>(&orig_open));
        ++registered;
    }
    if (registered == 0) {
        LOGW("NATIVE_READ: no hookable libs found");
        return;
    }
    if (!api->pltHookCommit()) {
        LOGW("NATIVE_READ: pltHookCommit failed");
        return;
    }
    LOGD("NATIVE_READ hooks installed (%d lib(s))", registered);
}

// ---------- install_crash_watchdog ----------
// Catches SIGSEGV/SIGABRT in the spoofed process and logs the package name
// so crashes in hooked code are diagnosable.

static std::atomic<struct sigaction*> g_old_segv{nullptr};
static std::atomic<struct sigaction*> g_old_abrt{nullptr};
static std::string g_watchdog_pkg;

static void sbx_crash_handler(int sig, siginfo_t* info, void* ctx) {
    __android_log_print(ANDROID_LOG_ERROR, "SandboxID",
        "CRASH sig=%d pkg=%s addr=%p", sig, g_watchdog_pkg.c_str(),
        info ? info->si_addr : nullptr);

    struct sigaction* old = (sig == SIGSEGV) ? g_old_segv.load() : g_old_abrt.load();
    if (old && (old->sa_flags & SA_SIGINFO) && old->sa_sigaction)
        old->sa_sigaction(sig, info, ctx);
    else if (old && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN)
        old->sa_handler(sig);
    else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

void install_crash_watchdog(const std::string& pkg) {
    g_watchdog_pkg = pkg;

    static struct sigaction old_segv, old_abrt;
    struct sigaction sa{};
    sa.sa_sigaction = sbx_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (::sigaction(SIGSEGV, &sa, &old_segv) == 0) g_old_segv.store(&old_segv);
    if (::sigaction(SIGABRT, &sa, &old_abrt) == 0) g_old_abrt.store(&old_abrt);
    LOGD("crash watchdog installed for '%s'", pkg.c_str());
}

// ---------- request_companion_mounts ----------
// Sends CMD_DO_MOUNTS to the companion to bind-mount spoofed build.prop files
// into this process's mount namespace.

void request_companion_mounts(int fd) {
    uint8_t cmd = sandboxid::CMD_DO_MOUNTS;
    uint32_t pid = static_cast<uint32_t>(getpid());
    if (!sandboxid::write_full(fd, &cmd, 1) ||
        !sandboxid::write_full(fd, &pid, sizeof(pid))) {
        LOGE("MOUNTS: failed to send request");
        return;
    }
    uint32_t ok = 0;
    if (!sandboxid::read_full(fd, &ok, sizeof(ok))) {
        LOGE("MOUNTS: failed to read response");
        return;
    }
    LOGD("MOUNTS: companion applied %u bind(s)", ok);
}

// ---------- request_companion_hide ----------
// Sends CMD_DO_HIDE to the companion to umount module traces from this
// process's mount namespace.

void request_companion_hide(int fd) {
    uint8_t cmd = sandboxid::CMD_DO_HIDE;
    uint32_t pid = static_cast<uint32_t>(getpid());
    if (!sandboxid::write_full(fd, &cmd, 1) ||
        !sandboxid::write_full(fd, &pid, sizeof(pid))) {
        LOGE("HIDE: failed to send request");
        return;
    }
    uint32_t n = 0;
    if (!sandboxid::read_full(fd, &n, sizeof(n))) {
        LOGE("HIDE: failed to read response");
        return;
    }
    LOGD("HIDE: companion detached %u mount(s)", n);
}
