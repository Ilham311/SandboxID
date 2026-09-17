#include "module_impl.hpp"
#include <climits>
#include <algorithm>

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

static bool sbx_set_build_long(JNIEnv* env, jclass clz,
                               const char* name, long long v) {
    jfieldID fid = env->GetStaticFieldID(clz, name, "J");
    if (!fid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    env->SetStaticLongField(clz, fid, v);
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

    // Build.TIME is a `long` initialised once, inside the zygote, from
    // ro.build.date.utc — long before apply-boot's resetprop rewrites that
    // property. Unpatched it keeps the *real* image build date and contradicts
    // the persona's own ro.build.date.utc (a vdinfos dev:build_time_utc
    // MISMATCH: Build.TIME/1000 != ro.build.date.utc). ro.build.date.utc is in
    // seconds while Build.TIME is in milliseconds, so scale here as well.
    const std::string& bt = val("BUILD_TIME_UTC");
    long long fields_set = set;
    if (!bt.empty()) {
        long long t = 0;
        if (sbx_parse_longlong(bt, t) && t > 0) {
            if (t < 4102444800LL) t *= 1000LL;   // seconds -> milliseconds
            if (sbx_set_build_long(env, clz, "TIME", t)) ++fields_set;
        }
    }

    LOGD("BUILD hook: %lld field(s) spoofed", fields_set);
    (void)fields_set;   // counted for the debug log only
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
// Declared here rather than beside hook_lseek: synth_build() rewinds a fd whose
// seq_file position another reader consumed, and must call the ORIGINAL lseek
// (hook_lseek would re-arm the synthetic cursor instead of moving the real one).
typedef off_t (*lseek_fn)(int, off_t, int);
static lseek_fn orig_lseek = nullptr;

struct SynthFd {
    sbxnr::Kind kind    = sbxnr::NONE;
    std::string buf;      // synthetic content once `ready`
    size_t      off     = 0;
    bool        ready   = false;   // buf is populated and servable
    bool        building = false;  // another thread is producing buf
    // Identity of the open file behind this fd, snapshotted when the entry was
    // created. `have_ident` is false only when that fstat failed.
    dev_t       dev     = 0;
    ino_t       ino     = 0;
    bool        have_ident = false;
};
static std::mutex            g_synth_mu;
static std::condition_variable g_synth_cv;   // waiters for an in-flight build
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
        if (action == sbxnr::CPU_STRIP) {
            // A Pixel/Tensor persona's cpuinfo has neither a "Hardware :" nor a
            // "Processor :" line, and every core reports ARM implementer 0x41 -
            // so rewriting the Hardware line alone still leaves the real SoC's
            // per-core MIDR fields (e.g. 0x51/0x805 Kryo silver) inside the
            // persona's own file. Rebuild the whole file from the persona's
            // platform instead, mirroring synth_proc_version() above.
            std::string s = sbxnr::cpuinfo_synth(val("BOARD_PLATFORM"), real,
                                                 sbxnr::fnv1a(val("SERIAL")));
            if (!s.empty()) return s;
            break;   // no cores could be parsed: fail closed, do not serve real
        }
        std::string patched;
        if (sbxnr::patch_cpuinfo(real, action, repl, patched)) return patched;
        // No Hardware line to rewrite (an upstream-arm64 kernel). Serving the
        // real file here would leak its per-core MIDR fields verbatim, so fail
        // closed like every other kind above instead.
        break;
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
    std::lock_guard<std::mutex> lk(g_synth_mu);
    if (k == sbxnr::NONE) {
        // This fd is now bound to a file we do not spoof. Erasing any stale entry
        // matters more than skipping the work: fd numbers are recycled, and an
        // entry left behind by a close() we did not intercept (hook_close is not
        // the only way a fd goes away) would serve synthetic /proc/meminfo bytes
        // to whatever unrelated file inherited the number. "Untracked" has to
        // mean passthrough in both directions.
        g_synth_fds.erase(fd);
        return;
    }
    SynthFd& s = g_synth_fds[fd];
    s.kind = k; s.buf.clear(); s.off = 0;
    s.ready = false; s.building = false;
    s.dev = 0; s.ino = 0; s.have_ident = false;
    // Snapshot who this fd currently is. See synth_identity_ok_locked().
    struct stat st{};
    if (::fstat(fd, &st) == 0) { s.dev = st.st_dev; s.ino = st.st_ino; s.have_ident = true; }
}

// A fd we recorded can be closed without hook_close ever seeing it: the app may
// issue a raw syscall, a library we did not PLT-hook may close it, or the fd can
// be handed to another process and closed there. The NUMBER is then handed out
// again by the next open() of an unrelated file, and the stale entry would serve
// synthetic /proc/meminfo bytes to a reader of that unrelated file. synth_track()
// cannot catch this - it only sees opens that come through our hooks - so check
// the file itself instead: (dev, ino) identifies the open file, and a recycled
// number names a different one.
//
// Called with g_synth_mu held. fstat is a raw syscall and cannot re-enter our
// own hooks, so holding the mutex across it is safe.
static bool synth_identity_ok_locked(int fd, const SynthFd& s) {
    if (!s.have_ident) return true;   // identity unknown: trust the entry
    struct stat st{};
    if (::fstat(fd, &st) != 0) return false;
    return st.st_dev == s.dev && st.st_ino == s.ino;
}

// Reads the whole real file (for patch-style kinds) and stores the synthetic
// replacement. Called lazily from read()/pread64(); the real read happens
// outside the mutex so a slow file cannot block other threads' reads.
//
// If another thread is already building this fd, the call waits for it rather
// than returning: an earlier revision claimed the fd by setting `ready` before
// filling `buf` and returned immediately when it saw the claim, so a concurrent
// first reader of a freshly opened classified fd took `ready` at face value and
// got EOF for a file that was about to have content. `ready` now means "buf is
// populated", and a concurrent builder is waited for.
static void synth_build(int fd) {
    sbxnr::Kind kind = sbxnr::NONE;
    bool need_real = false;
    {
        // unique_lock, not lock_guard: the build-wait below must release the
        // mutex while parked.
        std::unique_lock<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end() || it->second.ready) return;
        if (it->second.building) {
            // Bounded, and the bound is deliberately reachable in practice rather
            // than only on a dead builder: a /proc read is microseconds, but
            // applog.xml lives on app-controlled storage and can take far longer,
            // and any build can be descheduled on a loaded device. When this fires
            // the waiter below becomes a second builder, so the real read of this
            // fd is then contended - the empty-read handling further down is what
            // makes that safe rather than a source of wrong data.
            g_synth_cv.wait_for(lk, std::chrono::milliseconds(100));
            // Re-acquire state. Nothing below holds an iterator across the wait,
            // because the mutex was released while waiting and another thread
            // could have erased this fd.
            it = g_synth_fds.find(fd);
            if (it == g_synth_fds.end() || it->second.ready) return;
            if (!it->second.building) return;   // built; buf holds the content
            // Still building after the wait: the previous builder is gone or just
            // slow. Seize the work rather than leaving the fd stuck on EOF.
        }
        kind      = it->second.kind;
        need_real = synth_needs_real(kind);
        it->second.building = true;
    }

    // Bounded: /proc/meminfo and /proc/cpuinfo are kernel-sized (a few KB), but
    // applog.xml lives in the app's own dir and its size is app-controlled. A
    // pathological file must not make the hook buffer unbounded amounts in the
    // app's process. The cap is far above any legitimate AppLog map; a truncation
    // there makes patch_applog_xml decline the input and the hook fall back to the
    // small synthetic map.
    auto read_real = [fd]() {
        constexpr size_t kMaxReal = 1u * 1024 * 1024;
        char tmp[8192];
        std::string out;
        for (;;) {
            const size_t want = sizeof(tmp) < kMaxReal - out.size()
                                ? sizeof(tmp) : kMaxReal - out.size();
            if (want == 0) break;
            ssize_t n = orig_read ? orig_read(fd, tmp, want)
                                  : ::read(fd, tmp, want);
            if (n <= 0) break;
            out.append(tmp, static_cast<size_t>(n));
            if (static_cast<size_t>(n) < want) break;   // short read = EOF
        }
        return out;
    };

    std::string real;
    if (need_real) {
        real = read_real();
        if (real.empty()) {
            // The kernel shares one file position across every user of an fd, and
            // seq_file (/proc/meminfo, /proc/cpuinfo) returns 0 once that position
            // has advanced. The seize path above deliberately permits a second
            // builder when the first is unreasonably slow, and a reader of the app
            // that we did not intercept can consume the position too - so a build
            // can observe zero bytes for a file that provably has content
            // (verified: 7 of 8 concurrent readers of one /proc/meminfo fd get 0).
            // Rewind and retry before deciding anything; publishing an empty
            // buffer would serve EOF for the life of this fd.
            if (orig_lseek) (void)orig_lseek(fd, 0, SEEK_SET);
            real = read_real();
        }
    }

    std::string content = synth_content(kind, real);
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it != g_synth_fds.end()) {
            // A `need_real` build that captured nothing and then produced nothing
            // is a raced read, not the intentional fail-closed result of an
            // incomplete identity (that case has real bytes to work with and still
            // declines). Publishing it would freeze this fd on EOF; leaving the fd
            // unbuilt makes the next read rebuild, and it converges once the
            // concurrent readers drain. applog.xml is unaffected: an empty real
            // there falls back to the fully synthetic map, which is non-empty.
            if (need_real && real.empty() && content.empty()) {
                it->second.building = false;
                it->second.ready   = false;
                LOGW("NATIVE_READ: fd %d (kind %d) terbaca kosong karena pembaca "
                     "serentak dari fd yang sama - buffer tidak dipublikasikan, "
                     "build akan dicoba ulang pada pembacaan berikutnya",
                     fd, (int)kind);
            } else {
                it->second.buf = std::move(content);
                it->second.off = 0;
                it->second.ready = true;
                it->second.building = false;
            }
        }
    }
    g_synth_cv.notify_all();
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
    // `ready` now genuinely means "buf is populated", so this is a real EOF and
    // not a build in progress (synth_build waits for those before we get here).
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
    bool recycled = false;
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end())
            return orig_read ? orig_read(fd, buf, count) : ::read(fd, buf, count);
        if (!synth_identity_ok_locked(fd, it->second)) {
            g_synth_fds.erase(it);
            recycled = true;
        } else {
            need_build = !it->second.ready;
        }
    }
    if (recycled) {
        LOGD("NATIVE_READ: fd %d ditutup tanpa hook_close dan dipakai ulang untuk "
             "file lain - entry sintetis dibuang, read asli yang dilayani", fd);
        return orig_read ? orig_read(fd, buf, count) : ::read(fd, buf, count);
    }
    if (need_build) synth_build(fd);
    return synth_serve(fd, buf, count);
}

static ssize_t hook_pread64(int fd, void* buf, size_t count, long long offset) {
    if (offset < 0) return -1;
    bool need_build = false;
    bool recycled = false;
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end())
            return orig_pread64 ? orig_pread64(fd, buf, count, offset)
                                : ::syscall(__NR_pread64, fd, buf, count, offset);
        if (!synth_identity_ok_locked(fd, it->second)) {
            g_synth_fds.erase(it);
            recycled = true;
        } else {
            need_build = !it->second.ready;
        }
    }
    if (recycled) {
        LOGD("NATIVE_READ: fd %d didaur ulang ke file lain sebelum pread - "
             "entry sintetis dibuang", fd);
        return orig_pread64 ? orig_pread64(fd, buf, count, offset)
                            : ::syscall(__NR_pread64, fd, buf, count, offset);
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

// Re-arms the synthetic cursor. `pos` is expressed in the SYNTHETIC file's
// coordinate space: the replacement content may be shorter or longer than the
// real file, so answering a seek with the kernel's real offset would make
// length()/size() queries (the canonical lseek(fd, 0, SEEK_END)) disagree with
// the bytes read() then returns. Callers that then size a buffer from the seek
// result would allocate for one length and receive another.
//
// Returns the offset to report to the caller, or `real_result` unchanged when
// the fd is not one we are spoofing (the real offset is then the truth).
template <typename T>
static T synth_seek(int fd, T real_result, T offset, int whence) {
    if (real_result == static_cast<T>(-1)) return real_result;   // seek failed
    std::lock_guard<std::mutex> lk(g_synth_mu);
    auto it = g_synth_fds.find(fd);
    if (it == g_synth_fds.end() || !it->second.ready || it->second.buf.empty())
        return real_result;
    if (!synth_identity_ok_locked(fd, it->second)) {
        // The fd number now belongs to a different file. Its real offset is the
        // only honest thing to report, and the entry must not outlive this call.
        g_synth_fds.erase(it);
        return real_result;
    }
    const size_t size = it->second.buf.size();
    long long want;
    switch (whence) {
        case SEEK_SET: want = static_cast<long long>(offset); break;
        case SEEK_CUR: want = static_cast<long long>(real_result) +
                              static_cast<long long>(offset); break;
        case SEEK_END: want = static_cast<long long>(size) +
                              static_cast<long long>(offset); break;
        default: return real_result;    // the kernel already validated/failed it
    }
    if (want < 0) { errno = EINVAL; return static_cast<T>(-1); }
    it->second.off = static_cast<size_t>(want) > size
                     ? size : static_cast<size_t>(want);
    return static_cast<T>(it->second.off);
}

// off_t is 64-bit on arm64/x86_64; on the 32-bit ABIs the small files we
// intercept never reach 2^32, so the truncated view is harmless. lseek64 uses
// long long unconditionally for the same reason pread64 does.
// (orig_lseek is declared with the other originals near the top of this file.)
typedef long long (*lseek64_fn)(int, long long, int);
static lseek64_fn orig_lseek64 = nullptr;

static off_t hook_lseek(int fd, off_t offset, int whence) {
    off_t r = orig_lseek ? orig_lseek(fd, offset, whence)
                         : ::lseek(fd, offset, whence);
    return synth_seek<off_t>(fd, r, offset, whence);
}

static long long hook_lseek64(int fd, long long offset, int whence) {
    long long r = orig_lseek64 ? orig_lseek64(fd, offset, whence)
                              : ::syscall(__NR_lseek, fd, offset, whence);
    return synth_seek<long long>(fd, r, offset, whence);
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

    // Collect the libraries to hook, scanning /proc/self/maps once:
    //  - the system set above, which covers every Java-level file and property
    //    read (libjavacore / libandroid_runtime) plus libc for native callers;
    //  - the app's OWN libraries. Without these, a native probe that lives in
    //    the app's own .so - vdinfos's NATIVE lens calls
    //    __system_property_read_callback straight from its own library - goes
    //    through none of the hooks below and reads real values, which is
    //    exactly the MISMATCH the report shows on gsm.sim.operator.*.
    //    (Libraries the app loads later, after postAppSpecialize, are still
    //    outside this one-shot scan - see README "Layer 9 limits".)
    std::vector<std::string> targets;
    {
        FILE* f = fopen("/proc/self/maps", "re");
        if (f) {
            char* line = nullptr;
            size_t cap = 0;
            while (getline(&line, &cap, f) != -1) {
                char* path = strchr(line, '/');
                if (!path) continue;
                size_t pl = strlen(path);
                if (pl && path[pl - 1] == '\n') path[--pl] = '\0';
                bool want = (pl >= 10 && strncmp(path, "/data/app/", 10) == 0);
                for (const char* sfx : kLibs) {
                    if (want) break;
                    size_t sl = strlen(sfx);
                    if (pl >= sl && strcmp(path + pl - sl, sfx) == 0) want = true;
                }
                if (!want) continue;
                if (std::find(targets.begin(), targets.end(),
                              std::string(path, pl)) == targets.end())
                    targets.emplace_back(path, pl);
            }
            free(line);
            fclose(f);
        }
    }
    if (targets.empty()) {
        LOGW("NATIVE_READ: no hookable libs found");
        return;
    }

    int registered = 0, app_libs = 0, symbols = 0;
    for (const std::string& path : targets) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (strncmp(path.c_str(), "/data/app/", 10) == 0) ++app_libs;
        for (const HookReg& r : regs) {
            // Zygisk's pltHookRegister returns void: a symbol this library does
            // not import is simply not redirected, which is harmless.
            api->pltHookRegister(st.st_dev, st.st_ino, r.name, r.fn, r.orig);
            ++symbols;
        }
        ++registered;
    }
    if (registered == 0) {
        // Distinct from the empty-scan case above: targets were found in
        // /proc/self/maps but every stat() failed, i.e. the library was
        // unmapped between the scan and registration. The shared prefix keeps
        // summarize.sh's "no hookable libs" count correct.
        LOGW("NATIVE_READ: no hookable libs survived stat() (unmapped between "
             "the maps scan and registration)");
        return;
    }
    // Zygisk defers the pltHookRegister()ed redirects until commit. Commit's
    // bool is a batch verdict, not a per-hook one, and that is why it cannot be
    // trusted to say whether OUR hooks landed:
    //   - Magisk's plt_hook_commit() -> lsplt::CommitHook() -> HookInfos::DoHook
    //     folds every registration's every GOT slot into one AND
    //     (`res = DoHook(addr, ...) && res`, LSPlt lsplt.cc), and CommitHook()
    //     also returns false outright when the maps scan finds nothing and
    //     - notably - true when nothing was registered at all.
    //   - ReZygisk's api_plt_hook_commit_v4() returns `!any_failed`, set by any
    //     single plti_add_hook() that did not land (PerformanC/ReZygisk
    //     loader/src/injector/hook.c).
    // So one symbol a library does not even import flips the batch false while
    // every other registration is live and working. A debug session showed
    // commit "failing" for all 8 targets while those same hooks spoofed
    // hundreds of in-process reads, and trusting the bool here would have
    // suppressed the only line that reports L9 as actually live. The
    // trampoline back-pointer each landed registration writes into orig_* is
    // the per-hook ground truth, so probe those instead.
    bool committed = api->pltHookCommit();
    int live = 0;
    auto landed = [](void* trampoline) { return trampoline ? 1 : 0; };
    live += landed(reinterpret_cast<void*>(orig_read));
    live += landed(reinterpret_cast<void*>(orig_open));
    live += landed(reinterpret_cast<void*>(orig_openat));
    live += landed(reinterpret_cast<void*>(orig_pread64));
    live += landed(reinterpret_cast<void*>(orig_close));
    live += landed(reinterpret_cast<void*>(orig_lseek));
    live += landed(reinterpret_cast<void*>(orig_lseek64));
    live += landed(reinterpret_cast<void*>(orig_open_2));
    live += landed(reinterpret_cast<void*>(orig_openat_2));
    live += landed(reinterpret_cast<void*>(orig_prop_find));
    live += landed(reinterpret_cast<void*>(orig_prop_get));
    live += landed(reinterpret_cast<void*>(orig_prop_read_cb));

    if (!committed && live == 0) {
        LOGW("NATIVE_READ: pltHookCommit failed and no trampolines landed "
             "(%d lib(s), %d symbol registration(s)) - L9 in-process file/prop "
             "spoofing is OFF for this process; only Build.* and SystemProperties "
             "are spoofed", registered, symbols);
        return;
    }
    if (!committed) {
        LOGW("NATIVE_READ: pltHookCommit() reported failure but %d trampoline(s) "
             "landed - the provider's bool is a batch verdict over every "
             "registration (one symbol a library does not import flips it), not "
             "a per-hook report; treating L9 as live on the trampolines",
             live);
    }
    LOGI("NATIVE_READ hooks installed (%d lib(s) incl. %d app-owned, "
         "%d registration(s), %d live trampoline(s)): "
         "open/openat/__open_2/__openat_2/read/pread64/lseek/lseek64/close + "
         "__system_property_find/get/read_callback",
         registered, app_libs, symbols, live);
    (void)symbols;   // counted for the log only
}

// ---------- install_crash_watchdog ----------
// Catches SIGSEGV/SIGABRT in the spoofed process and logs the package name
// so crashes in hooked code are diagnosable.

static std::atomic<struct sigaction*> g_old_segv{nullptr};
static std::atomic<struct sigaction*> g_old_abrt{nullptr};
static std::string g_watchdog_pkg;

// Async-signal-safe integer formatting (no malloc, no stdio, no locks — those
// are exactly what is broken when the process crashed inside them). Both take a
// remaining capacity and truncate instead of overrunning: the crash handler
// formats an external, manifest-controlled process name, so the length is not
// something this code controls.
static size_t sbx_fmt_dec(char* out, size_t cap, unsigned v) {
    char tmp[10]; size_t i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = static_cast<char>('0' + v % 10); v /= 10; }
    if (i > cap) i = cap;
    for (size_t j = 0; j < i; ++j) out[j] = tmp[i - 1 - j];
    return i;
}
static size_t sbx_fmt_hex(char* out, size_t cap, uintptr_t v) {
    static const char d[] = "0123456789abcdef";
    char tmp[18]; size_t i = 2;
    tmp[0] = '0'; tmp[1] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned nib = static_cast<unsigned>((v >> shift) & 0xF);
        if (nib || i > 2 || shift == 0) tmp[i++] = d[nib];
    }
    if (i > cap) i = cap;
    for (size_t j = 0; j < i; ++j) out[j] = tmp[j];
    return i;
}

static void sbx_crash_handler(int sig, siginfo_t* info, void* ctx) {
    // Decide *first* whether this fault is genuinely fatal, because the
    // previous handler must be handed the signal and it may resume the
    // faulting thread rather than kill the process.
    //
    // This handler is the outermost SIGSEGV handler in an app process: it is
    // installed in postAppSpecialize, on top of the one ART registered in the
    // zygote. ART uses SIGSEGV for two routine, non-fatal purposes - implicit
    // null checks and the stack-overflow guard page - which fault, get
    // handled, and continue. Logging every one of those would record every
    // Java NPE in a target app as a module-attributed crash. ART reports a
    // fault it *cannot* handle through Runtime::Abort(), which arrives here as
    // SIGABRT, so SIGABRT is recorded unconditionally. SIGSEGV is recorded
    // only when there is no previous handler to defer to - the CLI binary, or
    // a process with no runtime installed - where it really is fatal.
    struct sigaction* old = (sig == SIGSEGV) ? g_old_segv.load() : g_old_abrt.load();
    const bool no_prior = (!old) || old->sa_handler == SIG_DFL ||
                          old->sa_handler == SIG_IGN;
    if (sig != SIGABRT && !no_prior) {
        if (old->sa_flags & SA_SIGINFO)
            old->sa_sigaction(sig, info, ctx);
        else
            old->sa_handler(sig);
        return;
    }

    // The marker is built by hand (no stdio, no std::string, no malloc - those
    // are what is broken when the process crashed inside them) and emitted
    // two ways:
    //
    //  - write(2): correct when stderr is a real pipe or tty, i.e. the CLI
    //    binary run from a shell.
    //  - __android_log_write: required for *app* processes. Second-stage init
    //    (SecondStageMain, system/core/init/init.cpp) calls SetStdioToDevNull()
    //    (system/core/init/util.cpp), which dup2()s fds 0/1/2 onto /dev/null;
    //    the zygote inherits that, and so does every app it forks. write(2) is
    //    therefore discarded, and the extractor in sh/lifecycle/service.sh
    //    sees nothing at all - the crash looks unattributed even though this
    //    handler ran.
    //
    // __android_log_write is not async-signal-safe, but the specific hazard
    // matters more than the general one:
    //
    //  - Modern liblog is lock-free (logd_writer.cpp: atomic_int sock_ with a
    //    CAS, and a non-blocking socket for every log id except LOG_ID_SECURITY),
    //    so the write itself cannot deadlock on liblog's own state.
    //  - Bionic's __android_log_write_log_message() does take a lock for FATAL:
    //        #if __BIONIC__
    //          if (log_message->priority == ANDROID_LOG_FATAL)
    //            android_set_abort_message(log_message->message);
    //        #endif
    //    android_set_abort_message() takes the process-global, non-recursive
    //    abort_msg_lock. A fault on a thread already holding that lock would
    //    then block here forever - hang, no tombstone.
    // Logging the marker at ERROR instead of FATAL avoids android_set_abort_message
    // entirely while still surfacing the marker, because sh/lifecycle/service.sh
    // and summarize.sh match the marker TEXT ('SandboxID CRASH sig='), not the
    // priority. The marker is diagnostic evidence, not an assertion, so FATAL
    // was never the semantically right level for it.
    char line[192];
    size_t p = 0;
    // Every writer below stays inside [0, CAP]. The last two bytes of the
    // buffer hold the '\n' write(2) needs and then the '\0' the log API needs,
    // so a process name of unbounded length truncates the marker instead of
    // overrunning a stack buffer inside a signal handler.
    static constexpr size_t CAP = sizeof(line) - 2;
    auto put = [&](const char* s) {
        while (*s && p < CAP) line[p++] = *s++;
    };
    put("SandboxID CRASH sig=");
    p += sbx_fmt_dec(line + p, CAP - p, static_cast<unsigned>(sig));
    put(" pkg=");
    put(g_watchdog_pkg.c_str());
    put(" addr=");
    p += sbx_fmt_hex(line + p, CAP - p,
                     reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
    // Write and log exactly [0, p]. A previous revision always wrote/logged the
    // full CAP-length buffer, appending whatever uninitialized stack bytes lay
    // after the marker to both the stderr write and the logcat line.
    line[p] = '\n';
    ssize_t rc = ::write(STDERR_FILENO, line, p + 1);
    (void)rc;
    line[p] = '\0';
    __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, line);

    // Hand the fault on so ART / debuggerd still see it and a tombstone is
    // produced. Reaching here implies no_prior when sig != SIGABRT.
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

    // Warm liblog BEFORE the handlers are armed: __android_log_write connects
    // to logd and may allocate on first use, and doing either inside the
    // handler is the allocation we are trying to avoid. This must actually run,
    // so it uses LOGI rather than ANDROID_LOG_DEBUG: __android_log_buf_write()
    // gates on __android_log_is_loggable() and returns -EPERM without ever
    // touching the logger when log.tag.SandboxID is set above DEBUG, which would
    // silently skip the warm-up and leave the connection to be made inside the
    // handler. LOGI also keeps the arming marker visible in a release capture,
    // where LOGD compiles out - summarize.sh counts this line.
    LOGI("crash watchdog armed for '%s'", pkg.c_str());

    if (::sigaction(SIGSEGV, &sa, &old_segv) == 0) g_old_segv.store(&old_segv);
    if (::sigaction(SIGABRT, &sa, &old_abrt) == 0) g_old_abrt.store(&old_abrt);
}

// ---------- request_companion_mounts ----------
// Sends CMD_DO_MOUNTS to the companion to bind-mount spoofed build.prop files
// into this process's mount namespace.

// Returns true only when the full request/response round trip completed, so the
// caller can avoid sending a second request onto a socket that is already dead.
bool request_companion_mounts(int fd) {
    uint8_t cmd = sandboxid::CMD_DO_MOUNTS;
    uint32_t pid = static_cast<uint32_t>(getpid());
    if (!sandboxid::write_full(fd, &cmd, 1) ||
        !sandboxid::write_full(fd, &pid, sizeof(pid))) {
        LOGE("MOUNTS: failed to send request");
        return false;
    }
    uint32_t ok = 0;
    if (!sandboxid::read_full(fd, &ok, sizeof(ok))) {
        LOGE("MOUNTS: failed to read response");
        return false;
    }
    LOGD("MOUNTS: companion applied %u bind(s)", ok);
    return true;
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
