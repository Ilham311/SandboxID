#include "module_impl.hpp"
#include <climits>

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
    (void)set;   // counted for the debug log only
    env->DeleteLocalRef(clz);
}

// ---------- install_native_read_hooks ----------
// PLT-hooks read/open/openat/pread64/lseek/close (plus the FORTIFY __open_2 /
// __openat_2 variants) in the target process to intercept reads of
// /proc/version, /proc/meminfo, /proc/cpuinfo,
// /sys/class/net/*/address and the ByteDance AppLog identifier files.
//
// Spoofed content is tracked PER-FILE-DESCRIPTOR, not as a sticky "last open"
// flag: /proc/cpuinfo, /proc/meminfo and the AppLog XML files are routinely
// larger than a single read() buffer, and a one-shot flag leaks the real tail
// of the file on the second read. Every read (chunked, repeated, rewound with
// lseek, pread at an arbitrary offset, or issued from another thread) is served
// from the fd's own synthetic buffer, so no real device bytes can escape.
//
// Coverage note: PLT hooking only sees calls that cross a library boundary, so
// reads made by a library we do not scan — most importantly an app's OWN .so,
// which is not loaded yet at hook-install time — are not intercepted in-process.
// Those reads are covered by the on-disk pre-warm (rotate_ids.sh applog /
// applog_seed in helpers.sh), which writes the same deterministically-derived
// values the hook would have served.

static ssize_t (*orig_read)(int, void*, size_t) = nullptr;
static int     (*orig_open)(const char*, int, ...)        = nullptr;
static int     (*orig_openat)(int, const char*, int, ...) = nullptr;
// long long (64-bit on every supported ABI) instead of off64_t, which needs
// _LARGEFILE64_SOURCE on some NDKs; the passing convention is identical.
typedef ssize_t (*pread64_fn)(int, void*, size_t, long long);
static pread64_fn orig_pread64 = nullptr;
static int     (*orig_close)(int) = nullptr;

struct SynthFd {
    sbxnr::Kind kind  = sbxnr::NONE;
    std::string buf;      // synthetic content once `ready`
    size_t      off   = 0;
    bool        ready = false;
};
static std::mutex            g_synth_mu;
static std::map<int, SynthFd> g_synth_fds;

// AppLog IDs for the current process, derived from the SAME seed and epoch as
// `sandboxid applog-ids <pkg>` (see sbxnr::applog_seed / applog_epoch_or_default
// in native_read.hpp) so the CLI predictor and the L9 hook always agree.
static sbxnr::ApplogIds sbx_applog_ids() {
    return sbxnr::make_applog_ids(
        sbxnr::applog_seed(val("FINGERPRINT"), val("SERIAL"), val("ANDROID_ID"), g_pkg),
        sbxnr::applog_epoch_or_default(val("APPLOG_EPOCH")));
}

// Kinds whose synthetic content is derived from the real file bytes.
static bool synth_needs_real(sbxnr::Kind k) {
    return k == sbxnr::MEMINFO || k == sbxnr::CPUINFO || k == sbxnr::APPLOG_XML;
}

// Computes the synthetic replacement for `kind`; `real` holds the real file
// contents for patch-style kinds. An empty result means "cannot synthesize"
// (identity incomplete): the caller must then serve nothing rather than the
// real device data — failing closed beats leaking.
static std::string synth_content(sbxnr::Kind k, const std::string& real) {
    switch (k) {
    case sbxnr::BOOTID: {
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        return sbxnr::uuid_from_seed(sbxnr::fnv1a(serial + ":bootid")) + "\n";
    }
    case sbxnr::MAC: {
        // Key is WIFI_MAC: rotate_ids.sh persists the MAC it also programs onto
        // the wlan0 interface under that name, so the hook and the real
        // interface agree. (An older revision read "WLAN_MAC", which nothing
        // ever writes — the persisted MAC was silently unused.)
        const std::string& mac = val("WIFI_MAC");
        if (!mac.empty() && sbxnr::is_valid_mac(mac)) return mac + "\n";
        const std::string& serial = val("SERIAL");
        if (serial.empty()) break;
        return sbxnr::mac_from_seed(sbxnr::fnv1a(serial + ":mac")) + "\n";
    }
    case sbxnr::VERSION: {
        std::string ver = sbxnr::synth_proc_version(
            val("RELEASE"), val("INCREMENTAL"), val("BOARD_PLATFORM"),
            val("HOST"), sbxnr::fnv1a(val("SERIAL")));
        if (!ver.empty()) return ver + "\n";
        break;
    }
    case sbxnr::MEMINFO: {
        // Patched in place. A known Pixel model substitutes that model's RAM;
        // an unknown model never leaks the real value either — the exact
        // MemTotal is itself a fingerprint (it varies per device by how much
        // memory the bootloader reserved), so it is rounded up to the nearest
        // marketing GB tier instead. See README "How it works", layer 4.
        return sbxnr::patch_meminfo(real, sbxnr::pixel_ram_gb(val("MODEL")));
    }
    case sbxnr::CPUINFO: {
        std::string repl;
        int action = sbxnr::cpu_action_for(val("SOC_MANUFACTURER"), val("SOC_MODEL"), repl);
        std::string patched;
        if (sbxnr::patch_cpuinfo(real, action, repl, patched)) return patched;
        return real;
    }
    case sbxnr::SELINUX_ENFORCE:
        return sbxnr::selinux_enforce_content();
    case sbxnr::APPLOG_XML: {
        if (val("SERIAL").empty()) break;      // fail closed
        sbxnr::ApplogIds ids = sbx_applog_ids();
        std::string patched;
        if (sbxnr::patch_applog_xml(real, ids, patched)) return patched;
        return sbxnr::applog_xml_synth(ids);
    }
    case sbxnr::BD_RAW_DID:        { if (val("SERIAL").empty()) break; return sbx_applog_ids().did; }
    case sbxnr::BD_RAW_IID:        { if (val("SERIAL").empty()) break; return sbx_applog_ids().iid; }
    case sbxnr::BD_RAW_OPENUDID:   { if (val("SERIAL").empty()) break; return sbx_applog_ids().openudid; }
    case sbxnr::BD_RAW_CLIENTUDID: { if (val("SERIAL").empty()) break; return sbx_applog_ids().clientudid; }
    case sbxnr::BD_RAW_CDID:       { if (val("SERIAL").empty()) break; return sbx_applog_ids().cdid; }
    default:
        break;
    }
    return std::string();
}

// Records `fd` as a spoofed read source if `path` is one we classify.
static void synth_track(int fd, const char* path) {
    if (fd < 0 || !path) return;
    sbxnr::Kind k = sbxnr::classify(path);
    if (k == sbxnr::NONE) return;
    std::lock_guard<std::mutex> lk(g_synth_mu);
    g_synth_fds[fd] = SynthFd{ k, std::string(), 0, false };
}

// Reads the whole real file (for patch-style kinds) and stores the synthetic
// replacement. Called lazily from read()/pread64(); the real read happens
// outside the mutex so a slow file cannot block other threads' reads.
static void synth_build(int fd) {
    sbxnr::Kind kind = sbxnr::NONE;
    bool need_real = false;
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end() || it->second.ready) return;
        kind      = it->second.kind;
        need_real = synth_needs_real(kind);
        it->second.ready = true;   // claim before the (unlocked) real read
    }

    std::string real;
    if (need_real) {
        // Bounded: /proc/meminfo and /proc/cpuinfo are kernel-sized (a few KB),
        // but applog.xml lives in the app's own dir and its size is app-
        // controlled. A pathological file must not make the hook buffer
        // unbounded amounts in the app's process. The cap is far above any
        // legitimate AppLog map; a truncation there makes patch_applog_xml
        // decline the input and the hook fall back to the small synthetic map.
        constexpr size_t kMaxReal = 1u * 1024 * 1024;
        char tmp[8192];
        for (;;) {
            const size_t want = sizeof(tmp) < kMaxReal - real.size()
                                ? sizeof(tmp) : kMaxReal - real.size();
            if (want == 0) break;
            ssize_t n = orig_read ? orig_read(fd, tmp, want)
                                  : ::read(fd, tmp, want);
            if (n <= 0) break;
            real.append(tmp, static_cast<size_t>(n));
            if (static_cast<size_t>(n) < want) break;   // short read = EOF
        }
    }

    std::string content = synth_content(kind, real);
    std::lock_guard<std::mutex> lk(g_synth_mu);
    auto it = g_synth_fds.find(fd);
    if (it != g_synth_fds.end()) { it->second.buf = std::move(content); it->second.off = 0; }
}

// Copies up to `count` synthetic bytes into `buf`. A fully-consumed buffer is
// KEPT and keeps returning EOF: dropping the entry would make the next read
// fall through to the real file, and after an lseek() back to 0 that serves raw
// device bytes (the offset is re-armed by hook_lseek instead, mirroring a real
// seekable file). An empty buffer likewise returns EOF: it is either the
// fail-closed result of an incomplete identity, or a buffer another thread is
// still building.
static ssize_t synth_serve(int fd, void* buf, size_t count) {
    std::lock_guard<std::mutex> lk(g_synth_mu);
    auto it = g_synth_fds.find(fd);
    if (it == g_synth_fds.end()) return 0;    // closed concurrently
    SynthFd& s = it->second;
    if (!s.ready || s.off >= s.buf.size()) return 0;
    size_t avail = s.buf.size() - s.off;
    size_t cp = avail < count ? avail : count;
    ::memcpy(buf, s.buf.data() + s.off, cp);
    s.off += cp;
    return static_cast<ssize_t>(cp);
}

// Same as synth_serve but slicing at `offset` (pread does not consume the
// file position, so the entry is left in place for later reads).
static ssize_t synth_serve_at(int fd, void* buf, size_t count, size_t offset) {
    std::lock_guard<std::mutex> lk(g_synth_mu);
    auto it = g_synth_fds.find(fd);
    if (it == g_synth_fds.end()) return 0;
    const std::string& s = it->second.buf;
    if (offset >= s.size()) return 0;
    size_t avail = s.size() - offset;
    size_t cp = avail < count ? avail : count;
    ::memcpy(buf, s.data() + offset, cp);
    return static_cast<ssize_t>(cp);
}

static ssize_t hook_read(int fd, void* buf, size_t count) {
    // Decide under the lock, act outside it: synth_serve takes the lock itself,
    // and a blocking real read must never hold it.
    bool need_build = false;
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end())
            return orig_read ? orig_read(fd, buf, count) : ::read(fd, buf, count);
        need_build = !it->second.ready;
    }
    if (need_build) synth_build(fd);
    return synth_serve(fd, buf, count);
}

static ssize_t hook_pread64(int fd, void* buf, size_t count, long long offset) {
    if (offset < 0) return -1;
    bool need_build = false;
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end())
            return orig_pread64 ? orig_pread64(fd, buf, count, offset)
                                : ::syscall(__NR_pread64, fd, buf, count, offset);
        need_build = !it->second.ready;
    }
    if (need_build) synth_build(fd);
    return synth_serve_at(fd, buf, count, static_cast<size_t>(offset));
}

static int hook_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    int fd = orig_open ? orig_open(path, flags, mode)
                       : (orig_openat ? orig_openat(AT_FDCWD, path, flags, mode)
                                      : ::syscall(__NR_openat, AT_FDCWD, path, flags, mode));
    synth_track(fd, path);
    return fd;
}

// Relative-to-dirfd: resolve the directory so classify() can still match
// app-data paths (glob(3) and DirectoryStream both open this way). If
// resolution fails the fd stays untracked (untracked = passthrough, which
// never serves wrong data).
static void synth_track_dirfd(int fd, int dirfd, const char* path) {
    if (path[0] == '/') { synth_track(fd, path); return; }
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
    char dir[PATH_MAX];
    ssize_t r = ::readlink(link, dir, sizeof(dir) - 1);
    if (r <= 0) return;
    dir[r] = '\0';
    std::string full = std::string(dir) + "/" + path;
    synth_track(fd, full.c_str());
}

static int hook_openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    int fd = orig_openat ? orig_openat(dirfd, path, flags, mode)
                         : ::syscall(__NR_openat, dirfd, path, flags, mode);
    if (fd < 0 || !path) return fd;
    synth_track_dirfd(fd, dirfd, path);
    return fd;
}

static int hook_close(int fd) {
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        g_synth_fds.erase(fd);
    }
    return orig_close ? orig_close(fd) : ::close(fd);
}

// Re-arms the synthetic cursor: without it, an app that seeks back and re-reads
// (Java RandomAccessFile.seek, or a C parser that rewinds) would be served the
// synthetic bytes from the *old* offset, or — if the entry had been consumed —
// the real file. `pos` is the real post-seek offset reported by the kernel, so
// both SEEK_SET/SEEK_END/SEEK_CUR land in the right place, and a seek past the
// synthetic end simply leaves the cursor at EOF, exactly like a real file.
static void synth_seek(int fd, long long pos) {
    if (pos < 0) return;    // failed seek, or a negative offset
    std::lock_guard<std::mutex> lk(g_synth_mu);
    auto it = g_synth_fds.find(fd);
    if (it == g_synth_fds.end() || it->second.buf.empty()) return;
    it->second.off = static_cast<size_t>(pos) > it->second.buf.size()
                     ? it->second.buf.size()
                     : static_cast<size_t>(pos);
}

// off_t is 64-bit on arm64/x86_64; on the 32-bit ABIs the small files we
// intercept never reach 2^32, so the truncated view is harmless. lseek64 uses
// long long unconditionally for the same reason pread64 does.
typedef off_t (*lseek_fn)(int, off_t, int);
static lseek_fn orig_lseek = nullptr;
typedef long long (*lseek64_fn)(int, long long, int);
static lseek64_fn orig_lseek64 = nullptr;

static off_t hook_lseek(int fd, off_t offset, int whence) {
    off_t r = orig_lseek ? orig_lseek(fd, offset, whence)
                         : ::lseek(fd, offset, whence);
    synth_seek(fd, static_cast<long long>(r));
    return r;
}

static long long hook_lseek64(int fd, long long offset, int whence) {
    long long r = orig_lseek64 ? orig_lseek64(fd, offset, whence)
                              : ::syscall(__NR_lseek, fd, offset, whence);
    synth_seek(fd, r);
    return r;
}

// FORTIFY variants: code compiled with _FORTIFY_SOURCE and no O_CREAT calls
// __open_2 / __openat_2 instead of open/openat (no mode argument is passed at
// all). Unhooked, those fds were never tracked and the real file leaked
// through read().
static int (*orig_open_2)(const char*, int)         = nullptr;
static int (*orig_openat_2)(int, const char*, int)  = nullptr;

static int hook_open_2(const char* path, int flags) {
    int fd = orig_open_2 ? orig_open_2(path, flags)
                         : (orig_open ? orig_open(path, flags, 0)
                                      : ::syscall(__NR_openat, AT_FDCWD, path, flags, 0));
    synth_track(fd, path);
    return fd;
}

static int hook_openat_2(int dirfd, const char* path, int flags) {
    int fd = orig_openat_2 ? orig_openat_2(dirfd, path, flags)
                           : (orig_openat ? orig_openat(dirfd, path, flags, 0)
                                          : ::syscall(__NR_openat, dirfd, path, flags, 0));
    if (fd < 0 || !path) return fd;
    synth_track_dirfd(fd, dirfd, path);
    return fd;
}

// ---------- __system_property hooks (NATIVE layer) ----------
// Intercepts __system_property_find and __system_property_read_callback
// so that native-layer property reads (used by vdinfos NATIVE lens and
// getprop SHELL lens) return spoofed values instead of real device values.

typedef const prop_info* (*find_fn)(const char*);
typedef void (*read_cb_fn)(const prop_info*, void (*)(void*, const char*, const char*, uint32_t), void*);

static find_fn    orig_prop_find = nullptr;
static read_cb_fn orig_prop_read_cb = nullptr;

// Captures the (name, value) of a prop_info via the real read-callback API,
// avoiding the deprecated PROP_NAME_MAX/PROP_VALUE_MAX-truncating
// __system_property_read() and its allocation inside the hook.
struct PropCapture { std::string name; std::string value; };

static void capture_prop(void* cookie, const char* name, const char* value, uint32_t) {
    auto* c = static_cast<PropCapture*>(cookie);
    if (name)  c->name  = name;
    if (value) c->value = value;
}

static void hook_system_property_read_callback(
        const prop_info* pi,
        void (*callback)(void*, const char*, const char*, uint32_t),
        void* cookie) {
    if (!orig_prop_read_cb) return;   // hook installed but orig never resolved
    if (!pi || !callback) {
        orig_prop_read_cb(pi, callback, cookie);
        return;
    }
    PropCapture cap;
    orig_prop_read_cb(pi, capture_prop, &cap);
    if (cap.name.empty()) {
        if (orig_prop_read_cb) orig_prop_read_cb(pi, callback, cookie);
        return;
    }

    if (sbx_prop_hidden(cap.name.c_str())) {
        callback(cookie, cap.name.c_str(), "", 0);
        return;
    }

    std::string spoofed;
    if (spoof_prop_value(cap.name, spoofed)) {
        LOGD("NATIVE_PROP SPOOF '%s' -> '%s'", cap.name.c_str(), spoofed.c_str());
        callback(cookie, cap.name.c_str(), spoofed.c_str(), 0);
        return;
    }
    orig_prop_read_cb(pi, callback, cookie);
}

static const prop_info* hook_system_property_find(const char* name) {
    if (!name) return orig_prop_find ? orig_prop_find(name) : nullptr;
    if (sbx_prop_hidden(name)) {
        LOGD("NATIVE_PROP HIDE '%s'", name);
        return nullptr;
    }
    return orig_prop_find ? orig_prop_find(name) : nullptr;
}

typedef int (*prop_get_fn)(const char*, char*);
static prop_get_fn orig_prop_get = nullptr;

static int hook_system_property_get(const char* name, char* value) {
    if (!name) return orig_prop_get ? orig_prop_get(name, value) : 0;
    if (sbx_prop_hidden(name)) {
        LOGD("NATIVE_PROP_GET HIDE '%s'", name);
        if (value) value[0] = '\0';
        return 0;
    }
    std::string spoofed;
    if (spoof_prop_value(std::string(name), spoofed)) {
        LOGD("NATIVE_PROP_GET SPOOF '%s' -> '%s'", name, spoofed.c_str());
        if (value) {
            size_t len = spoofed.size();
            if (len >= PROP_VALUE_MAX) len = PROP_VALUE_MAX - 1;
            std::memcpy(value, spoofed.c_str(), len);
            value[len] = '\0';
            return static_cast<int>(len);
        }
        return static_cast<int>(spoofed.size());
    }
    return orig_prop_get ? orig_prop_get(name, value) : 0;
}


void install_native_read_hooks(Api* api) {
    const std::string& nr = val("SBX_NATIVE_READ");
    if (nr == "0") {
        LOGD("NATIVE_READ disabled by SBX_NATIVE_READ=0");
        return;
    }

    static const char* const kLibs[] = {
        "/libc.so",
        "/libbase.so",
        "/libcutils.so",
        "/libutils.so",
        // The Java-level file surface: every android.system.Os /
        // FileInputStream / RandomAccessFile / SharedPreferences read goes
        // through libcore.io.Linux's natives (Linux_open, Linux_readBytes,
        // Linux_preadBytes, Linux_lseek, Linux_close — registered by
        // register_libcore_io_Linux in Register.cpp), and those live in
        // libjavacore.so, which every Java process loads. Note Linux_lseek
        // wraps lseek64, which is why lseek64 must be hooked too.
        "/libjavacore.so",
        // Hosts android.os.SystemProperties natives (native_get etc.) — the
        // third identity surface, see prop_hooks.cpp.
        "/libandroid_runtime.so",
    };
    struct HookReg { const char* name; void* fn; void** orig; };
    static const HookReg regs[] = {
        {"read",    reinterpret_cast<void*>(hook_read),
                    reinterpret_cast<void**>(&orig_read)},
        {"open",    reinterpret_cast<void*>(hook_open),
                    reinterpret_cast<void**>(&orig_open)},
        {"openat",  reinterpret_cast<void*>(hook_openat),
                    reinterpret_cast<void**>(&orig_openat)},
        {"pread64", reinterpret_cast<void*>(hook_pread64),
                    reinterpret_cast<void**>(&orig_pread64)},
        {"close",   reinterpret_cast<void*>(hook_close),
                    reinterpret_cast<void**>(&orig_close)},
        {"lseek",   reinterpret_cast<void*>(hook_lseek),
                    reinterpret_cast<void**>(&orig_lseek)},
        {"lseek64", reinterpret_cast<void*>(hook_lseek64),
                    reinterpret_cast<void**>(&orig_lseek64)},
        {"__open_2",   reinterpret_cast<void*>(hook_open_2),
                       reinterpret_cast<void**>(&orig_open_2)},
        {"__openat_2", reinterpret_cast<void*>(hook_openat_2),
                       reinterpret_cast<void**>(&orig_openat_2)},
        {"__system_property_read_callback",
                    reinterpret_cast<void*>(hook_system_property_read_callback),
                    reinterpret_cast<void**>(&orig_prop_read_cb)},
        {"__system_property_find",
                    reinterpret_cast<void*>(hook_system_property_find),
                    reinterpret_cast<void**>(&orig_prop_find)},
        {"__system_property_get",
                    reinterpret_cast<void*>(hook_system_property_get),
                    reinterpret_cast<void**>(&orig_prop_get)},
    };

    int registered = 0, symbols = 0;
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

        for (const HookReg& r : regs) {
            // Zygisk's pltHookRegister returns void: a symbol this library does
            // not import is simply not redirected, which is harmless.
            api->pltHookRegister(dev, ino, r.name, r.fn, r.orig);
            ++symbols;
        }
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
    LOGD("NATIVE_READ hooks installed (%d lib(s), %d symbol registration(s)), "
         "incl. openat/__open_2/pread64/lseek/close + prop find/read_callback/get",
         registered, symbols);
    (void)symbols;   // counted for the debug log only
}

// ---------- install_crash_watchdog ----------
// Catches SIGSEGV/SIGABRT in the spoofed process and logs the package name
// so crashes in hooked code are diagnosable.

static std::atomic<struct sigaction*> g_old_segv{nullptr};
static std::atomic<struct sigaction*> g_old_abrt{nullptr};
static std::string g_watchdog_pkg;

// Async-signal-safe integer formatting (no malloc, no stdio, no locks — those
// are exactly what is broken when the process crashed inside them).
static size_t sbx_fmt_dec(char* out, unsigned v) {
    if (v == 0) { out[0] = '0'; return 1; }
    char tmp[10]; size_t i = 0;
    while (v) { tmp[i++] = static_cast<char>('0' + v % 10); v /= 10; }
    for (size_t j = 0; j < i; ++j) out[j] = tmp[i - 1 - j];
    return i;
}
static size_t sbx_fmt_hex(char* out, uintptr_t v) {
    static const char d[] = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    size_t i = 2;
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned nib = static_cast<unsigned>((v >> shift) & 0xF);
        if (nib || i > 2 || shift == 0) out[i++] = d[nib];
    }
    return i;
}

static void sbx_crash_handler(int sig, siginfo_t* info, void* ctx) {
    // Signal handler: write(2) only. __android_log_print / std::string copies
    // are NOT async-signal-safe and can deadlock on the malloc lock.
    char line[192];
    size_t p = 0;
    auto put = [&](const char* s) {
        while (*s && p + 1 < sizeof(line)) line[p++] = *s++;
    };
    put("SandboxID CRASH sig=");
    p += sbx_fmt_dec(line + p, static_cast<unsigned>(sig));
    put(" pkg=");
    put(g_watchdog_pkg.c_str());
    put(" addr=");
    p += sbx_fmt_hex(line + p, reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
    line[p++] = '\n';
    ssize_t rc = ::write(STDERR_FILENO, line, p);
    (void)rc;

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
