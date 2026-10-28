#include "module_impl.hpp"
#include <climits>
#include <algorithm>
#include <cctype>
#include <spawn.h>

#if defined(SBX_ENABLE_LSPLANT) && SBX_ENABLE_LSPLANT
#include <dobby.h>
#endif

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
        {"USER",         "USER"},
        {"SOC_MANUFACTURER", "SOC_MANUFACTURER"},
        {"SOC_MODEL",    "SOC_MODEL"},
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
    // ro.build.date.utc — before this hook runs. Unpatched it keeps the *real*
    // image build date and contradicts the persona's own ro.build.date.utc (a
    // vdinfos dev:build_time_utc MISMATCH: Build.TIME/1000 != ro.build.date.utc).
    // ro.build.date.utc is in seconds while Build.TIME is in milliseconds, so
    // scale here as well.
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
    // app's process. The cap is far above any legitimate AppLog map.
    //
    // A truncation mid-element leaves the input unterminated, and
    // patch_applog_xml then fails to find the closing "</string>" and declines
    // it, so the hook falls back to the small synthetic map. (It also declines
    // when it finds no key it recognises at all - see the `replaced` flag
    // there. This comment previously claimed the truncation case was the only
    // route to that fallback, which was wrong in a way that mattered: a
    // map-shaped file with foreign keys used to be served verbatim.)
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
    //
    // The untracked case used to `return orig_read(...)` from INSIDE the guard's
    // scope. That is not "outside the lock": the return expression is evaluated
    // while the guard is still alive, so the real read ran with g_synth_mu held.
    // read() on a pipe, socket, inotify or eventfd blocks until data arrives -
    // and FileObserver's thread parks in exactly that read() for the life of the
    // process (android_os_fileobserver_observe -> read). One such thread took
    // the mutex permanently, every other thread of the app queued behind it, and
    // handleBindApplication never finished: the process ANR'd as "failed to
    // complete startup" and was SIGKILLed. Every configured target hit this.
    bool untracked = false;
    bool need_build = false;
    bool recycled = false;
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end()) {
            untracked = true;
        } else if (!synth_identity_ok_locked(fd, it->second)) {
            g_synth_fds.erase(it);
            recycled = true;
        } else {
            need_build = !it->second.ready;
        }
    }
    if (untracked)
        return orig_read ? orig_read(fd, buf, count) : ::read(fd, buf, count);
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
    // Same scope hazard as hook_read: the untracked path must not call the real
    // pread from inside the guard. pread does not block on a pipe, but it does
    // on a slow file, and the invariant is the same one.
    bool untracked = false;
    bool need_build = false;
    bool recycled = false;
    {
        std::lock_guard<std::mutex> lk(g_synth_mu);
        auto it = g_synth_fds.find(fd);
        if (it == g_synth_fds.end()) {
            untracked = true;
        } else if (!synth_identity_ok_locked(fd, it->second)) {
            g_synth_fds.erase(it);
            recycled = true;
        } else {
            need_build = !it->second.ready;
        }
    }
    if (untracked)
        return orig_pread64 ? orig_pread64(fd, buf, count, offset)
                            : ::syscall(__NR_pread64, fd, buf, count, offset);
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
    // Raw syscall, not ::readlink(): readlink is inline-hooked (hook_readlink),
    // so the libc body this would call is now a trampoline back into this
    // module. That is not a loop - sbx_meta_hidden answers false for
    // /proc/self/fd/N - but it is a wasted hook frame on every relative openat,
    // which is the hot path for glob(3) and DirectoryStream. The syscall has no
    // such coupling.
    long r = ::syscall(__NR_readlinkat, AT_FDCWD, link, dir,
                       static_cast<long>(sizeof(dir) - 1));
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

// ---------- file METADATA hooks (stat family) ----------
// The read hooks above cover a library that OPENS a file. They do not cover a
// library that only asks whether a file EXISTS, and that is the shape of the
// root check Grab actually ships.
//
// Measured, not assumed. libsafexEx.so (Neutralbase SafeX) exports
// Java_com_neutralbase_lib_safex_SafeXNative_isSUExist; disassembling that
// 196-byte body shows exactly eight `bl <stat@plt>` calls and nothing else, and
// the eight string literals it loads are /sbin/su, /system/bin/su,
// /system/xbin/su, /data/local/xbin/su, /data/local/bin/su, /system/sd/xbin/su,
// /system/bin/failsafe/su and /data/local/su. On the reference device
// /system/bin/su EXISTS (it is a hardlink to /system/bin/sh, inode 544, mode
// 0755 root:shell, label u:object_r:system_file:s0 - world-traversable, so an
// app-uid stat() reaches it). Every one of those stat() calls returns success
// for that path, and the module never sees it: stat is not in kPltRegs and was
// not inline-hooked, so SafeX reads the real answer for the one path that
// matters.
//
// The same library also exports checkFilesExistByPath, which walks a
// caller-supplied path list through stdio (fopen/fgets) and
// processLineForKeywords - the keyword list comes from Grab's DEX, not the .so,
// so it cannot be enumerated from the binary. Hiding the stat family is what
// makes BOTH checks answer "absent" without knowing that list.
//
// Deliberately narrow: only the root-binary basenames the module already
// refuses to exec (sbx_is_root_binary) are reported absent. Hiding every path
// would break the app's own file access and is not what the check asks about.
// bionic names the fstatat syscall differently per word size: the 64-bit ABIs
// expose __NR_newfstatat, while the 32-bit ABIs this module also ships
// (armeabi-v7a, x86) define only __NR_fstatat64. This is a rename, not a
// different ABI, and that matters because the fallback below pairs the number
// with a plain `struct stat*`:
//
//   On 32-bit bionic `stat` and `stat64` are the SAME function. In the device's
//   /apex/com.android.runtime/lib/bionic/libc.so both symbols resolve to
//   0x495c8, whose body sets r0 = AT_FDCWD and branches to fstatat64. So the
//   syscall that backs `stat` on a 32-bit process is 327 (__NR_fstatat64), and
//   the struct it fills is the 64-bit-off_t layout that bionic's `struct stat`
//   already is. Passing `struct stat*` to it is exactly what libc does.
//
// Verified directly on this tree: aarch64 defines __NR_newfstatat = 79 and no
// __NR_fstatat64; the device's 32-bit ARM libc uses syscall 327. The x86 pair
// follows the same bionic convention but was not measured here — the host
// toolchain ignores -m32, so there is no way to probe it from this machine.
//
// Without this, the build dies on armeabi-v7a with "use of undeclared
// identifier '__NR_newfstatat'". The #error keeps a future ABI from silently
// compiling a hook that cannot fall back.
#if defined(__NR_newfstatat)
#  define SBX_NR_FSTATAT __NR_newfstatat
#elif defined(__NR_fstatat64)
#  define SBX_NR_FSTATAT __NR_fstatat64
#else
#  error "no fstatat syscall number for this ABI"
#endif

typedef int (*stat_fn)(const char*, struct stat*);
typedef int (*lstat_fn)(const char*, struct stat*);
typedef int (*fstatat_fn)(int, const char*, struct stat*, int);
typedef int (*access_fn)(const char*, int);
typedef ssize_t (*readlink_fn)(const char*, char*, size_t);

static stat_fn     orig_stat     = nullptr;
static lstat_fn    orig_lstat    = nullptr;
static fstatat_fn  orig_fstatat  = nullptr;
static access_fn   orig_access   = nullptr;
static readlink_fn orig_readlink = nullptr;

// Defined further down with the exec guard; declared here because the metadata
// hooks below are the second consumer. One definition, so the exec guard and
// the stat guard can never disagree about which basenames are root binaries.
static bool sbx_is_root_binary(const char* path);

// Shared by every metadata hook: the path is one this module reports as absent.
// A relative path is resolved against the caller's dirfd the same way
// synth_track_dirfd does, so a stat via a directory fd cannot slip past.
static bool sbx_meta_hidden(const char* path) {
    if (!path || !*path) return false;
    if (path[0] == '/') {
        if (!sbx_is_root_binary(path)) return false;
        // Logged because this is the layer that answers SafeX's root check, and
        // a silent hide is indistinguishable in a session log from a check that
        // never ran. summarize.sh counts this line.
        LOGD("NATIVE_META HIDE '%s'", path);
        return true;
    }
    // Relative: the probe is only meaningful when the caller resolved it to an
    // absolute root-binary path. Without the dirfd there is nothing to judge,
    // so fail open (report as present) rather than deny a legitimate stat.
    return false;
}

static int hook_stat(const char* path, struct stat* buf) {
    if (sbx_meta_hidden(path)) { errno = ENOENT; return -1; }
    return orig_stat ? orig_stat(path, buf)
                     : ::syscall(SBX_NR_FSTATAT, AT_FDCWD, path, buf, 0);
}

static int hook_lstat(const char* path, struct stat* buf) {
    if (sbx_meta_hidden(path)) { errno = ENOENT; return -1; }
    return orig_lstat ? orig_lstat(path, buf)
                      : ::syscall(SBX_NR_FSTATAT, AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
}

static int hook_fstatat(int dirfd, const char* path, struct stat* buf, int flags) {
    // Absolute paths are the common case for a hardcoded probe list, and the
    // ones the SafeX code uses. A relative path is resolved through
    // /proc/self/fd/<dirfd> so the dirfd form cannot bypass the check.
    //
    // The resolution uses the raw syscall, not ::readlink(): readlink is itself
    // inline-hooked below, and routing this through the hook would add a frame
    // that has to re-enter sbx_meta_hidden for "/proc/self/fd/N" on every
    // fstatat. The syscall has no such coupling.
    if (path && path[0] != '/' && dirfd != AT_FDCWD) {
        char link[64], dir[PATH_MAX];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
        long r = ::syscall(__NR_readlinkat, AT_FDCWD, link, dir,
                           static_cast<long>(sizeof(dir) - 1));
        if (r > 0) {
            dir[r] = '\0';
            std::string full = std::string(dir) + "/" + path;
            if (sbx_meta_hidden(full.c_str())) { errno = ENOENT; return -1; }
        }
    } else if (sbx_meta_hidden(path)) {
        errno = ENOENT;
        return -1;
    }
    return orig_fstatat ? orig_fstatat(dirfd, path, buf, flags)
                        : ::syscall(SBX_NR_FSTATAT, dirfd, path, buf, flags);
}

static int hook_access(const char* path, int mode) {
    if (sbx_meta_hidden(path)) { errno = ENOENT; return -1; }
    return orig_access ? orig_access(path, mode)
                       : ::syscall(__NR_faccessat, AT_FDCWD, path, mode, 0);
}

// readlink is how a probe learns that a path it DID find is really something
// else (the /system/bin/su hardlink on this device). Reporting ENOENT keeps the
// story consistent with the stat hooks above.
static ssize_t hook_readlink(const char* path, char* buf, size_t size) {
    if (sbx_meta_hidden(path)) { errno = ENOENT; return -1; }
    return orig_readlink ? orig_readlink(path, buf, size)
                         : ::syscall(__NR_readlinkat, AT_FDCWD, path, buf, size);
}

// ---------- uname (kernel identity) ----------
// /proc/version is synthesized from the persona (synth_proc_version), but
// uname(2) is a separate syscall wrapper and was never hooked, so the two
// disagreed in the same process. Measured on the reference device with the
// OPPO CPH2521 / taro persona: /proc/version would be served as
// "Linux version 6.1.1xx-android15-...", while uname() returned the real
// "4.19.337-FakeDreamer-1.9-Universe" - a kernel that no CPH2521 ever ran, and
// a build string that names the ROM. libpsec.so imports uname and calls it
// (`bl <uname@plt>` at 0x1be454) before building a string from the result, so
// this is not a hypothetical: Grab's own security library reads it.
//
// The synthetic value is derived from the SAME persona inputs as
// synth_proc_version, so the two lenses agree instead of contradicting.
typedef int (*uname_fn)(struct utsname*);
static uname_fn orig_uname = nullptr;

static int hook_uname(struct utsname* buf) {
    int rc = orig_uname ? orig_uname(buf) : ::syscall(__NR_uname, buf);
    if (rc != 0 || !buf) return rc;
    // Only rewrite when the persona is complete enough to describe a kernel;
    // otherwise leave the real value (failing open here beats inventing a
    // kernel that contradicts the rest of the persona's Build surface).
    const std::string& release    = val("RELEASE");
    const std::string& incremental = val("INCREMENTAL");
    const std::string& platform   = val("BOARD_PLATFORM");
    const std::string& host       = val("HOST");
    const std::string& serial     = val("SERIAL");
    if (release.empty() || platform.empty() || serial.empty()) return rc;

    std::string ver = sbxnr::synth_proc_version(
        release, incremental, platform, host, sbxnr::fnv1a(serial));
    if (ver.empty()) return rc;

    // synth_proc_version returns the whole /proc/version line:
    //   "Linux version <release> (...) #1 SMP PREEMPT <date>"
    // utsname.release wants only the <release> token, and utsname.version the
    // rest of the line after it. Split rather than re-deriving, so the two
    // lenses cannot drift.
    static const char kPrefix[] = "Linux version ";
    const size_t pl = sizeof(kPrefix) - 1;
    if (ver.compare(0, pl, kPrefix) != 0) return rc;
    std::string body = ver.substr(pl);
    size_t sp = body.find(' ');
    if (sp == std::string::npos) return rc;
    std::string rel = body.substr(0, sp);
    std::string rest = body.substr(sp + 1);
    if (rel.empty()) return rc;

    std::snprintf(buf->release, sizeof(buf->release), "%s", rel.c_str());
    std::snprintf(buf->version, sizeof(buf->version), "%s", rest.c_str());
    LOGD("NATIVE_UNAME SPOOF release='%s'", rel.c_str());
    // sysname/nodename/machine are left alone: the persona does not describe a
    // different OS or architecture, and rewriting them would be a new lie
    // rather than the removal of one.
    return rc;
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

// Read a property WITHOUT this module's spoofing. The UA hook needs the value
// the ROM actually emits, because its "from" half has to match what the
// WebView provider produces; __system_property_get() in this process returns
// the persona's value for every prop this module spoofs, which would make
// from == to and the rewrite a silent no-op.
int sbx_raw_prop_get(const char* name, char* value) {
    return orig_prop_get ? orig_prop_get(name, value) : 0;
}

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

typedef FILE* (*popen_fn)(const char*, const char*);
static popen_fn orig_popen = nullptr;

typedef int (*pclose_fn)(FILE*);
static pclose_fn orig_pclose = nullptr;

typedef int (*execve_fn)(const char*, char* const[], char* const[]);
static execve_fn orig_execve = nullptr;

typedef int (*fclose_fn)(FILE*);
static fclose_fn orig_fclose = nullptr;

static std::mutex g_popen_mu;
static std::map<FILE*, int> g_synthetic_popens;

// Number of synthetic popen streams currently outstanding. Read lock-free by
// hook_fclose so the hot path costs one relaxed load; see hook_fclose.
static std::atomic<int> g_synth_live{0};

// Returns a readable FILE* holding `output`, to be handed back to the app as
// the result of a popen() this module intercepted.
//
// The stream must be able to hold output larger than a pipe buffer, and the
// whole payload has to be in place BEFORE the caller gets the FILE* — the app
// only starts reading after popen() returns, so there is no reader to drain a
// pipe while we fill it. Writing a payload larger than the pipe capacity into
// a readerless pipe blocks forever in ::write(), wedging the calling app
// thread. That is not hypothetical here: the intercepted `pm list packages`
// path re-emits the package listing, and on the test device
// `pm list packages -f -i -u --show-versioncode` is 61752 bytes against a
// 65536-byte pipe buffer — within ~4KB of a hard hang, and any ROM with a few
// more packages crosses it.
//
// So the payload goes to an anonymous in-memory file (memfd), which is
// seekable and unbounded; the pipe is only a fallback for a kernel without
// memfd_create (Linux < 3.17), and there the write is non-blocking and
// truncates rather than deadlocking.
static FILE* create_synthetic_pipe(const std::string& output, int exit_code) {
#if defined(__NR_memfd_create)
    int fd = static_cast<int>(syscall(__NR_memfd_create, "sbx_popen", 0x0001 /* MFD_CLOEXEC */));
#else
    int fd = -1;
#endif
    if (fd >= 0) {
        size_t off = 0;
        while (off < output.size()) {
            ssize_t w = ::write(fd, output.data() + off, output.size() - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            off += static_cast<size_t>(w);
        }
        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            ::close(fd);
            return nullptr;
        }
        FILE* fp = fdopen(fd, "r");
        if (!fp) {
            ::close(fd);
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(g_popen_mu);
        g_synthetic_popens[fp] = exit_code;
        g_synth_live.fetch_add(1, std::memory_order_relaxed);
        return fp;
    }

    int pdes[2];
    if (pipe2(pdes, O_CLOEXEC) != 0) {
        LOGE("create_synthetic_pipe: memfd_create and pipe2 both failed errno=%d", errno);
        return nullptr;
    }
    if (!output.empty()) {
        // Non-blocking: on this fallback path a payload larger than the pipe
        // buffer is truncated instead of hanging the caller. Losing tail lines
        // of a package listing is recoverable; a wedged app thread is not.
        int fl = fcntl(pdes[1], F_GETFL, 0);
        if (fl != -1) fcntl(pdes[1], F_SETFL, fl | O_NONBLOCK);
        size_t off = 0;
        while (off < output.size()) {
            ssize_t w = ::write(pdes[1], output.data() + off, output.size() - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    LOGW("create_synthetic_pipe: pipe fallback truncated at %zu/%zu bytes",
                         off, output.size());
                }
                break;
            }
            off += static_cast<size_t>(w);
        }
    }
    ::close(pdes[1]);

    FILE* fp = fdopen(pdes[0], "r");
    if (!fp) {
        ::close(pdes[0]);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_popen_mu);
        g_synthetic_popens[fp] = exit_code;
        g_synth_live.fetch_add(1, std::memory_order_relaxed);
    }
    return fp;
}

// True when `path` names one of the root/superuser binaries this module hides.
//
// Matches the BASENAME exactly. The previous test was a substring search —
// find("/su"), find("/magisk") — which flagged any path that merely contained
// those bytes: /data/user/0/<pkg>/..., /storage/emulated/0/..., /sdcard/...,
// or an app's own file called "su-something". A false positive here denies a
// legitimate exec, which is a visible app bug; a false negative only leaks one
// probe result. The daemon names are matched by prefix because they ship
// versioned (daemonsu / daemonsu64 / daemonsud).
static bool sbx_is_root_binary(const char* path) {
    if (!path || !*path) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!*base) return false;
    if (strcmp(base, "su") == 0 || strcmp(base, "magisk") == 0 ||
        strcmp(base, "magiskpolicy") == 0 || strcmp(base, "supolicy") == 0 ||
        strcmp(base, "ksud") == 0 || strcmp(base, "apd") == 0)
        return true;
    return strncmp(base, "daemonsu", 8) == 0;
}

// Scan a shell command line for any token naming a root binary, whether as the
// command itself ("su -c id", "/system/bin/su -c id") or as the argument of a
// probe ("which su", "command -v magisk", "ls /system/xbin/su"). Tokens are
// split on whitespace and on the shell metacharacters that can separate two
// commands, so `echo ok; su -c id` is caught too.
//
// `busybox` is deliberately NOT a root binary: apps legitimately ship and exec
// their own. Only the *probe* for it is a tell, and that is handled by
// sbx_cmd_probes_busybox below.
static bool sbx_cmd_probes_root(const std::string& cmd) {
    static const char kSeps[] = " \t\r\n;|&<>()$`\"'\\";
    size_t i = 0;
    while (i < cmd.size()) {
        while (i < cmd.size() && strchr(kSeps, cmd[i])) ++i;
        size_t start = i;
        while (i < cmd.size() && !strchr(kSeps, cmd[i])) ++i;
        if (i > start) {
            std::string tok = cmd.substr(start, i - start);
            if (sbx_is_root_binary(tok.c_str())) return true;
        }
    }
    return false;
}

// `which busybox` / `whereis busybox` / `command -v busybox`: the probe is the
// tell (a stock device has no busybox in PATH), not the binary.
static bool sbx_cmd_probes_busybox(const std::string& cmd) {
    static const char* const kProbes[] = {"which", "whereis", "command", "type"};
    static const char kSeps[] = " \t\r\n;|&<>()$`\"'\\";
    size_t i = 0;
    std::string first;
    while (i < cmd.size()) {
        while (i < cmd.size() && strchr(kSeps, cmd[i])) ++i;
        size_t start = i;
        while (i < cmd.size() && !strchr(kSeps, cmd[i])) ++i;
        if (i > start) {
            std::string tok = cmd.substr(start, i - start);
            if (first.empty()) {
                first = tok;
                bool is_probe = false;
                for (const char* p : kProbes) if (first == p) { is_probe = true; break; }
                if (!is_probe) return false;
            } else if (tok == "busybox") {
                return true;
            }
        }
    }
    return false;
}

// Rewrite one line of `getprop` output in place. `getprop` prints
// `[key]: [value]`, one property per line.
//
// Returns false when the line must be dropped entirely: a hidden property must
// not appear in an enumeration at all, or its absence is itself the tell.
// Otherwise returns true, with `*rewritten` set when the value was replaced.
static bool sbx_filter_getprop_line(std::string& line, bool* rewritten) {
    *rewritten = false;
    size_t lb = line.find('[');
    if (lb == std::string::npos) return true;
    size_t rb = line.find(']', lb);
    if (rb == std::string::npos || rb == lb + 1) return true;
    std::string key = line.substr(lb + 1, rb - lb - 1);
    if (sbx_prop_hidden(key.c_str())) return false;
    std::string v;
    if (!spoof_prop_value(key, v)) return true;
    size_t vb = line.find('[', rb);
    if (vb == std::string::npos) return true;
    // The closing bracket is the LAST one on the line, not the first after the
    // value: a value that itself contains ']' would otherwise be cut short and
    // the tail left as stray text.
    size_t ve = line.rfind(']');
    if (ve == std::string::npos || ve <= vb) return true;
    line = line.substr(0, vb + 1) + v + line.substr(ve);
    *rewritten = true;
    return true;
}

// Run the real command and filter its `getprop` output.
//
// This is the path for every getprop form the single-key fast path cannot
// answer: a bare `getprop` (dump all), `getprop | grep ...`,
// `getprop; getprop <key>`, an option form, or a key no table knows. All of
// those used to fall through to the unfiltered real popen, so `getprop` on its
// own - which is exactly how a root or emulator probe enumerates a device -
// returned the REAL property table, real model, serial, fingerprint and MIUI
// tells included. That is the one command the single-key spoof cannot cover and
// the one a probe reaches for first.
static FILE* sbx_getprop_filtered(const char* command, const char* type) {
    FILE* real_fp = orig_popen ? orig_popen(command, type) : ::popen(command, type);
    if (!real_fp) return nullptr;
    std::string filtered;
    int dropped = 0, rewritten = 0;
    char line[4096];
    while (fgets(line, sizeof(line), real_fp)) {
        std::string l(line);
        bool rw = false;
        if (sbx_filter_getprop_line(l, &rw)) {
            if (rw) ++rewritten;
            filtered += l;
        } else {
            ++dropped;
        }
    }
    int rc = orig_pclose ? orig_pclose(real_fp) : ::pclose(real_fp);
    // LOGI, not LOGD: this is the path that closes the leak, and in a release
    // build LOGD is compiled out.
    LOGI("NATIVE_POPEN getprop dump filtered (%d rewritten, %d hidden) from '%s'",
         rewritten, dropped, command);
    return create_synthetic_pipe(filtered, (rc >> 8) & 0xff);
}

// True when the stream is one we can read the child's output from.
//
// popen(cmd, "w") connects the child's STDIN to us, not its stdout, so there is
// nothing to filter - and the filter's fgets on such a stream returns NULL
// immediately (EBADF), which used to make hook_popen answer a write-mode call
// with an empty READ-ONLY synthetic stream. A caller that then wrote to it got
// EBADF where the real popen would have accepted the write. Only "r" (and the
// "re" spelling) is readable.
static bool sbx_popen_readable(const char* type) {
    return type && type[0] == 'r';
}

static FILE* hook_popen(const char* command, const char* type) {
    if (!command) return orig_popen ? orig_popen(command, type) : ::popen(command, type);
    std::string cmd(command);

    // 1. Root & binary probing via shell
    if (sbx_cmd_probes_root(cmd) || sbx_cmd_probes_busybox(cmd)) {
        // LOGI for the same reason as sbx_exec_denied: a blocked probe is the
        // event this layer exists for, and LOGD vanishes in a release build.
        LOGI("NATIVE_POPEN BLOCKED root tool check: '%s'", command);
        // The shell's own wording for a command it cannot find, and the exit
        // status it uses for one (127). Returning the empty string with status
        // 1 here would be a different, equally detectable answer.
        std::string first = cmd.substr(0, cmd.find_first_of(" \t"));
        const char* slash = strrchr(first.c_str(), '/');
        std::string name = slash ? slash + 1 : first;
        return create_synthetic_pipe(
            "/system/bin/sh: " + name + ": inaccessible or not found\n", 127);
    }

    // 2. getprop queries via shell. Read mode only: in write mode the child's
    // stdout is not ours to filter, and the synthetic stream would be the wrong
    // kind of stream to hand back.
    size_t gp = sbx_popen_readable(type) ? cmd.find("getprop") : std::string::npos;
    if (gp != std::string::npos) {
        size_t prop_start = gp + 7;
        while (prop_start < cmd.size() && isspace(static_cast<unsigned char>(cmd[prop_start]))) ++prop_start;
        size_t prop_end = prop_start;
        while (prop_end < cmd.size() && !isspace(static_cast<unsigned char>(cmd[prop_end])) &&
               cmd[prop_end] != ';' && cmd[prop_end] != '&' && cmd[prop_end] != '|' && cmd[prop_end] != '>') {
            ++prop_end;
        }
        if (prop_end > prop_start) {
            std::string prop = cmd.substr(prop_start, prop_end - prop_start);
            if (!prop.empty()) {
                // Bootloader & AVB verified boot status
                if (prop == "ro.boot.vbmeta.device_state") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> locked", prop.c_str());
                    return create_synthetic_pipe("locked\n", 0);
                }
                if (prop == "ro.boot.verifiedbootstate") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> green", prop.c_str());
                    return create_synthetic_pipe("green\n", 0);
                }
                if (prop == "ro.boot.flash.locked") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> 1", prop.c_str());
                    return create_synthetic_pipe("1\n", 0);
                }
                if (prop == "ro.secure") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> 1", prop.c_str());
                    return create_synthetic_pipe("1\n", 0);
                }
                if (prop == "ro.debuggable") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> 0", prop.c_str());
                    return create_synthetic_pipe("0\n", 0);
                }
                if (prop == "ro.boot.warranty_bit" || prop == "ro.warranty_bit") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> 0", prop.c_str());
                    return create_synthetic_pipe("0\n", 0);
                }
                if (prop == "ro.build.type") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> user", prop.c_str());
                    return create_synthetic_pipe("user\n", 0);
                }
                if (prop == "ro.build.tags") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> release-keys", prop.c_str());
                    return create_synthetic_pipe("release-keys\n", 0);
                }
                if (prop == "ro.boot.veritymode") {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> enforcing", prop.c_str());
                    return create_synthetic_pipe("enforcing\n", 0);
                }
                if (sbx_prop_hidden(prop.c_str())) {
                    LOGD("NATIVE_POPEN HIDE getprop '%s'", prop.c_str());
                    return create_synthetic_pipe("\n", 0);
                }
                std::string spoofed;
                if (spoof_prop_value(prop, spoofed)) {
                    LOGD("NATIVE_POPEN SPOOF getprop '%s' -> '%s'", prop.c_str(), spoofed.c_str());
                    return create_synthetic_pipe(spoofed + "\n", 0);
                }
            }
        }
    }

    // 3. pm list packages via shell. Read mode only, for the same reason.
    if (sbx_popen_readable(type) && cmd.find("pm list packages") != std::string::npos) {
        FILE* real_fp = orig_popen ? orig_popen(command, type) : ::popen(command, type);
        if (real_fp) {
            std::string filtered;
            char line[1024];
            while (fgets(line, sizeof(line), real_fp)) {
                bool hide = false;
                static const char* const kHiddenApps[] = {
                    "magisk", "kernelsu", "apatch", "lsposed", "xposed",
                    "hidemyapplist", "termux", "bin.mt.plus", "zarchiver",
                    "fakegps", "gpsemulator"
                };
                for (const char* h : kHiddenApps) {
                    if (strcasestr(line, h)) { hide = true; break; }
                }
                if (!hide) filtered += line;
            }
            int rc = orig_pclose ? orig_pclose(real_fp) : ::pclose(real_fp);
            int exit_code = (rc >> 8) & 0xff;
            return create_synthetic_pipe(filtered, exit_code);
        }
    }

    // 4. Any other getprop form: dump-all, a pipeline, a compound command, or a
    // key no table recognises. The single-key branch above answered 'getprop
    // <known key>' and left everything else to the real popen - including bare
    // `getprop`, which prints the entire real property table. Reachable by
    // anything that can run a shell command, and the first thing a root or
    // emulator probe runs.
    if (sbx_popen_readable(type) && cmd.find("getprop") != std::string::npos) {
        FILE* fp = sbx_getprop_filtered(command, type);
        if (fp) return fp;
    }

    return orig_popen ? orig_popen(command, type) : ::popen(command, type);
}

static int hook_pclose(FILE* stream) {
    if (!stream) return -1;
    int exit_code = -1;
    bool synthetic = false;
    {
        std::lock_guard<std::mutex> lock(g_popen_mu);
        auto it = g_synthetic_popens.find(stream);
        if (it != g_synthetic_popens.end()) {
            exit_code = it->second;
            g_synthetic_popens.erase(it);
            synthetic = true;
        }
    }
    if (synthetic) {
        g_synth_live.fetch_sub(1, std::memory_order_relaxed);
        fclose(stream);
        return (exit_code & 0xff) << 8;
    }
    return orig_pclose ? orig_pclose(stream) : ::pclose(stream);
}

// ---------- exec-family guard ----------
//
// Every entry point of the exec family has to be guarded, not just execve.
// On bionic, `execve` is an 8-byte thunk (`bti c; b __execve`) and the rest of
// the family branches STRAIGHT to the internal `__execve`, bypassing it:
//
//   execv   0x63c98: bti c; ...; b   __execve
//   execve  0x63cac: bti c; b       __execve
//   execvp  0x63cb4: bti c; ...; b   execvpe@plt
//   execvpe 0x63cc8: ...; bl __execve   (two call sites)
//   fexecve 0x64084: ...; bl __execve
//
// So hooking the public `execve` symbol — which is what this module did —
// catches only callers that name execve. libjavacore.so, which is where
// java.lang.ProcessImpl / Runtime.exec lands, imports and calls BOTH:
//
//   $ readelf --dyn-syms libjavacore.so | grep execv
//   71: UND execv@LIBC
//   72: UND execve@LIBC
//   $ llvm-objdump -d libjavacore.so | grep 'bl.*execv'
//   1f758: bl 0x35ea0 <execv@plt>
//   1f90c: bl 0x35eb0 <execve@plt>
//
// i.e. half of Java's process-spawn surface ran unguarded. posix_spawn/p are
// guarded too: they are not on any path observed on the test device, but they
// are the modern spawn API and a native library can reach them directly.
typedef int  (*execv_fn)(const char*, char* const[]);
typedef int  (*execvp_fn)(const char*, char* const[]);
typedef int  (*execvpe_fn)(const char*, char* const[], char* const[]);
typedef int  (*fexecve_fn)(int, char* const[], char* const[]);
typedef int  (*posix_spawn_fn)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                               const posix_spawnattr_t*, char* const[], char* const[]);

// Resolve a libc symbol by name.
//
// Three of the entry points below cannot be named directly: bionic declares
// posix_spawn/posix_spawnp only from API 28 and fexecve only from API 30, while
// this module builds with MIN_SDK=26, so the declarations are not visible and a
// direct reference is a compile error ("no member named 'fexecve' in the global
// namespace"). The symbols themselves are present in libc on every device the
// module targets, so resolving them by name at runtime gives the same address a
// direct reference would, without pinning the build to a higher API level.
//
// A null return is a real possibility only on an API < 28 device, where the
// symbol genuinely does not exist; the Dobby table skips a null target, so the
// guard for that one entry point is simply not installed.
static void* sbx_libc_sym(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

static execv_fn        orig_execv = nullptr;
static execvp_fn       orig_execvp = nullptr;
static execvpe_fn      orig_execvpe = nullptr;
static fexecve_fn      orig_fexecve = nullptr;
static posix_spawn_fn  orig_posix_spawn = nullptr;
static posix_spawn_fn  orig_posix_spawnp = nullptr;
// The varargs spellings never call their own original - their bodies are
// rebuilt on the hooked execv/execvp above - but the hook tables still need a
// slot to receive the trampoline, so the pointers exist and stay unused.
static void*           orig_execl = nullptr;
static void*           orig_execlp = nullptr;
static void*           orig_execle = nullptr;

// `sh -c "<something>"` hides the real command inside argv[2], where a
// pathname/argv[0] check cannot see it: Runtime.exec(new String[]{"sh","-c",
// "su -c id"}) would otherwise sail through. Both -c and --command are
// accepted, and the string is scanned with the same tokenizer popen uses.
static bool sbx_shell_c_denied(char* const argv[]) {
    if (!argv || !argv[0]) return false;
    const char* b = strrchr(argv[0], '/');
    b = b ? b + 1 : argv[0];
    if (strcmp(b, "sh") != 0 && strcmp(b, "ash") != 0) return false;
    for (int i = 1; argv[i]; ++i) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--command") == 0) {
            if (argv[i + 1] && sbx_cmd_probes_root(argv[i + 1])) return true;
            return false;
        }
    }
    return false;
}

// Single decision point for the whole exec family. Returns true to deny.
static bool sbx_exec_denied(const char* pathname, char* const argv[],
                            const char* tag, const char* what) {
    // LOGI, not LOGD. A denial is the one event this whole layer exists to
    // produce, it is rare by construction (the app has to actually reach for
    // su/magisk), and in a release build LOGD compiles to nothing - so at DEBUG
    // the guard would block a root probe and leave no trace of having done it.
    // That is the same reasoning module.cpp applies to its L3 line: the signal
    // that a security layer fired must survive a release build.
    if (pathname && sbx_is_root_binary(pathname)) {
        LOGI("NATIVE_%s BLOCKED %s='%s'", tag, what, pathname);
        return true;
    }
    if (argv && argv[0] && sbx_is_root_binary(argv[0])) {
        LOGI("NATIVE_%s BLOCKED argv[0]='%s'", tag, argv[0]);
        return true;
    }
    if (sbx_shell_c_denied(argv)) {
        LOGI("NATIVE_%s BLOCKED shell -c root command", tag);
        return true;
    }
    return false;
}

static int hook_execve(const char* pathname, char* const argv[], char* const envp[]) {
    if (sbx_exec_denied(pathname, argv, "EXECVE", "path")) { errno = ENOENT; return -1; }
    return orig_execve ? orig_execve(pathname, argv, envp) : ::execve(pathname, argv, envp);
}

static int hook_execv(const char* pathname, char* const argv[]) {
    if (sbx_exec_denied(pathname, argv, "EXECV", "path")) { errno = ENOENT; return -1; }
    return orig_execv ? orig_execv(pathname, argv) : ::execv(pathname, argv);
}

static int hook_execvp(const char* file, char* const argv[]) {
    if (sbx_exec_denied(file, argv, "EXECVP", "file")) { errno = ENOENT; return -1; }
    return orig_execvp ? orig_execvp(file, argv) : ::execvp(file, argv);
}

static int hook_execvpe(const char* file, char* const argv[], char* const envp[]) {
    if (sbx_exec_denied(file, argv, "EXECVPE", "file")) { errno = ENOENT; return -1; }
    return orig_execvpe ? orig_execvpe(file, argv, envp) : ::execvpe(file, argv, envp);
}

// `execl` / `execle` / `execlp` are the varargs spellings, and on bionic they do
// not go through any symbol the table above names:
//
//   $ llvm-objdump -d libc.so | sed -n '/<execl>:/,+40p'
//   bl 0x63700 <__execl<0>>      ; execl, execle
//   bl 0x63b30 <__execl<2>>      ; execlp
//   ... and inside those:  bl 0xc3d60 <__execve>
//
// `__execve` is the internal entry point and is NOT exported (readelf --dyn-syms
// shows no __execve), so it cannot be PLT-hooked, and Dobby can only reach it
// through a caller's own PLT stub. The public `execve` thunk is bypassed
// entirely, so a caller using execl() reached the kernel unguarded while the
// comment above claimed the whole family was covered.
//
// So the varargs forms are hooked too, and their bodies are rebuilt on top of
// execv/execvp/execve/execvpe - which are the hooked entry points. The argv is
// read off the va_list and handed to that same single decision point, so no
// second copy of the deny policy exists here.
// The va_list is taken BY POINTER, and that is load-bearing on aarch64: there
// va_list is a struct passed by value, so a helper taking it by value walks its
// own copy and leaves the caller's list still pointing at the FIRST vararg. The
// caller's next va_arg then re-reads that first argument, so execle handed
// execve the last argv entry as its environment - observed as a segfault, since
// the string was dereferenced as a char* array.
static void sbx_collect_argv(va_list* ap, const char* arg0,
                             std::vector<char*>& argv) {
    argv.clear();
    argv.push_back(const_cast<char*>(arg0));
    for (;;) {
        const char* a = va_arg(*ap, const char*);
        if (!a) break;
        argv.push_back(const_cast<char*>(a));
    }
    argv.push_back(nullptr);
}

static int hook_execl(const char* path, const char* arg0, ...) {
    va_list ap; va_start(ap, arg0);
    std::vector<char*> argv;
    sbx_collect_argv(&ap, arg0, argv);
    va_end(ap);
    return hook_execv(path, argv.data());
}

static int hook_execlp(const char* file, const char* arg0, ...) {
    va_list ap; va_start(ap, arg0);
    std::vector<char*> argv;
    sbx_collect_argv(&ap, arg0, argv);
    va_end(ap);
    return hook_execvp(file, argv.data());
}

static int hook_execle(const char* path, const char* arg0, ...) {
    // execle's envp is the pointer AFTER the terminating null, so the collector
    // has to leave the list consumed past that null before it is read. Reading
    // it one slot early would hand execve the last argv entry as the
    // environment.
    va_list ap; va_start(ap, arg0);
    std::vector<char*> argv;
    sbx_collect_argv(&ap, arg0, argv);
    char* const* envp = va_arg(ap, char* const*);
    va_end(ap);
    return hook_execve(path, argv.data(), const_cast<char* const*>(envp));
}

static int hook_fexecve(int fd, char* const argv[], char* const envp[]) {
    // No pathname to inspect: the decision has to come from the fd. Reading it
    // back through /proc/self/fd also covers a caller that unlinked the file
    // after opening it, which is exactly how a root tool would try to hide.
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    char target[PATH_MAX];
    // Raw syscall, not readlink(): readlink is inline-hooked (hook_readlink),
    // and this body runs on the exec path where an extra trampoline frame buys
    // nothing - sbx_meta_hidden answers false for /proc/self/fd/N anyway.
    long n = ::syscall(__NR_readlinkat, AT_FDCWD, link, target,
                       static_cast<long>(sizeof(target) - 1));
    if (n > 0) {
        target[n] = '\0';
        if (sbx_exec_denied(target, argv, "FEXECVE", "fd target")) { errno = ENOENT; return -1; }
    } else if (sbx_exec_denied(nullptr, argv, "FEXECVE", "fd")) {
        errno = ENOENT;
        return -1;
    }
    // The fallback is the by-name resolution, not a direct call: ::fexecve is
    // not declared at MIN_SDK=26. It is only reached if the Dobby pass above
    // failed to install this hook, in which case this body is not on the call
    // path at all - but it still has to compile.
    if (orig_fexecve) return orig_fexecve(fd, argv, envp);
    auto real = reinterpret_cast<fexecve_fn>(sbx_libc_sym("fexecve"));
    return real ? real(fd, argv, envp) : -1;
}

static int hook_posix_spawn(pid_t* pid, const char* path,
                            const posix_spawn_file_actions_t* fa,
                            const posix_spawnattr_t* attr,
                            char* const argv[], char* const envp[]) {
    if (sbx_exec_denied(path, argv, "POSIX_SPAWN", "path")) return ENOENT;
    if (orig_posix_spawn) return orig_posix_spawn(pid, path, fa, attr, argv, envp);
    // By name: ::posix_spawn is not declared at MIN_SDK=26 (API 28+).
    auto real = reinterpret_cast<posix_spawn_fn>(sbx_libc_sym("posix_spawn"));
    return real ? real(pid, path, fa, attr, argv, envp) : ENOSYS;
}

static int hook_posix_spawnp(pid_t* pid, const char* file,
                             const posix_spawn_file_actions_t* fa,
                             const posix_spawnattr_t* attr,
                             char* const argv[], char* const envp[]) {
    if (sbx_exec_denied(file, argv, "POSIX_SPAWNP", "file")) return ENOENT;
    if (orig_posix_spawnp) return orig_posix_spawnp(pid, file, fa, attr, argv, envp);
    // By name: ::posix_spawnp is not declared at MIN_SDK=26 (API 28+).
    auto real = reinterpret_cast<posix_spawn_fn>(sbx_libc_sym("posix_spawnp"));
    return real ? real(pid, file, fa, attr, argv, envp) : ENOSYS;
}

// fclose on a synthetic stream: without this, an app that closes a popen()
// result with fclose() instead of pclose() leaves its entry in
// g_synthetic_popens forever. The map is keyed by FILE*, so a later stream
// allocated at the same address would inherit the stale exit code, and every
// leaked entry is unfreeable memory.
//
// fclose is extremely hot, so the hook is a single relaxed atomic load on the
// common path and only takes the lock when a synthetic stream is actually
// outstanding.
static int hook_fclose(FILE* stream) {
    if (stream && g_synth_live.load(std::memory_order_relaxed) > 0) {
        bool synthetic = false;
        {
            std::lock_guard<std::mutex> lock(g_popen_mu);
            auto it = g_synthetic_popens.find(stream);
            if (it != g_synthetic_popens.end()) {
                g_synthetic_popens.erase(it);
                synthetic = true;
            }
        }
        if (synthetic) g_synth_live.fetch_sub(1, std::memory_order_relaxed);
    }
    return orig_fclose ? orig_fclose(stream) : ::fclose(stream);
}


// The PLT registration table, at file scope so tests/host/sbx_hook_sm.cpp can
// assert that every guarded entry point is actually listed. A hook body that
// exists but is never registered here is dead code, and that is precisely how
// execl/execlp/execle went unguarded while a comment claimed the whole exec
// family was covered: the bodies did not exist to be forgotten.
struct HookReg { const char* name; void* fn; void** orig; };
static const HookReg kPltRegs[] = {
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
    // Metadata family. A library that only asks whether a path EXISTS never
    // calls open(), so none of the hooks above fire for it. This is the exact
    // shape of SafeX's root check - see the block comment above hook_stat.
    {"stat",     reinterpret_cast<void*>(hook_stat),
                 reinterpret_cast<void**>(&orig_stat)},
    {"lstat",    reinterpret_cast<void*>(hook_lstat),
                 reinterpret_cast<void**>(&orig_lstat)},
    {"fstatat",  reinterpret_cast<void*>(hook_fstatat),
                 reinterpret_cast<void**>(&orig_fstatat)},
    {"access",   reinterpret_cast<void*>(hook_access),
                 reinterpret_cast<void**>(&orig_access)},
    {"readlink", reinterpret_cast<void*>(hook_readlink),
                 reinterpret_cast<void**>(&orig_readlink)},
    {"uname",    reinterpret_cast<void*>(hook_uname),
                 reinterpret_cast<void**>(&orig_uname)},
    {"__system_property_read_callback",
                reinterpret_cast<void*>(hook_system_property_read_callback),
                reinterpret_cast<void**>(&orig_prop_read_cb)},
    {"__system_property_find",
                reinterpret_cast<void*>(hook_system_property_find),
                reinterpret_cast<void**>(&orig_prop_find)},
    {"__system_property_get",
                reinterpret_cast<void*>(hook_system_property_get),
                reinterpret_cast<void**>(&orig_prop_get)},
    {"popen",   reinterpret_cast<void*>(hook_popen),
                reinterpret_cast<void**>(&orig_popen)},
    {"pclose",  reinterpret_cast<void*>(hook_pclose),
                reinterpret_cast<void**>(&orig_pclose)},
    {"fclose",  reinterpret_cast<void*>(hook_fclose),
                reinterpret_cast<void**>(&orig_fclose)},
    // The whole exec family, not just execve: on bionic every other member
    // branches straight to the internal __execve and never touches the
    // public thunk. See the exec-family guard comment above.
    //
    // The varargs spellings (execl/execle/execlp) are covered here too, but
    // through their own bodies rather than through __execve: that symbol is
    // not exported, so no PLT registration can reach it. See hook_execl.
    {"execve",  reinterpret_cast<void*>(hook_execve),
                reinterpret_cast<void**>(&orig_execve)},
    {"execv",   reinterpret_cast<void*>(hook_execv),
                reinterpret_cast<void**>(&orig_execv)},
    {"execvp",  reinterpret_cast<void*>(hook_execvp),
                reinterpret_cast<void**>(&orig_execvp)},
    {"execvpe", reinterpret_cast<void*>(hook_execvpe),
                reinterpret_cast<void**>(&orig_execvpe)},
    {"execl",   reinterpret_cast<void*>(hook_execl),
                reinterpret_cast<void**>(&orig_execl)},
    {"execlp",  reinterpret_cast<void*>(hook_execlp),
                reinterpret_cast<void**>(&orig_execlp)},
    {"execle",  reinterpret_cast<void*>(hook_execle),
                reinterpret_cast<void**>(&orig_execle)},
    {"fexecve", reinterpret_cast<void*>(hook_fexecve),
                reinterpret_cast<void**>(&orig_fexecve)},
    {"posix_spawn",  reinterpret_cast<void*>(hook_posix_spawn),
                reinterpret_cast<void**>(&orig_posix_spawn)},
    {"posix_spawnp", reinterpret_cast<void*>(hook_posix_spawnp),
                reinterpret_cast<void**>(&orig_posix_spawnp)},
};

// The property and file entry points that must ALSO be inline-hooked, so that a
// library dlopened AFTER the one-shot PLT scan still inherits the guard.
//
// Why this table exists at all: the PLT pass walks /proc/self/maps once, in
// postAppSpecialize, and registers only the GOT slots of libraries already
// mapped at that moment. A target app whose security stack is dlopened later -
// Grab's is, from Application.onCreate - resolves the real symbols and reads
// real values. Measured on the reference device for com.grabtaxi.passenger: the
// PLT pass logged "11 lib(s) incl. 0 app-owned", and libpsec.so,
// libCredolabNative.so and libcore-lib.so each import __system_property_get
// directly (readelf --dyn-syms). A native probe in one of those libraries then
// read ro.product.model = alioth while the Java lens served Build.MODEL =
// CPH2521 - a direct contradiction, and libsafexEx.so ships
// SafeXNative.hasReadProcMaps() for exactly this class of check.
//
// Declared OUTSIDE the SBX_ENABLE_LSPLANT guard on purpose: the host suites
// build this translation unit with LSPLANT off, and they are the only place
// this invariant can be checked without a device. The table carries no `target`
// - the install site resolves each name through sbx_libc_sym() at hook time,
// which is also what keeps Dobby off this module's own PLT stub. Keeping the
// names in the SAME table the installer walks is deliberate: a second,
// parallel name list is the very drift this table exists to prevent, and the
// host test asserts set equality against kPltRegs in both directions.
struct LateReg { const char* name; void* hook; void** orig; };
static const LateReg kLateRegs[] = {
    {"__system_property_get",           reinterpret_cast<void*>(hook_system_property_get),           reinterpret_cast<void**>(&orig_prop_get)},
    {"__system_property_find",          reinterpret_cast<void*>(hook_system_property_find),          reinterpret_cast<void**>(&orig_prop_find)},
    {"__system_property_read_callback", reinterpret_cast<void*>(hook_system_property_read_callback), reinterpret_cast<void**>(&orig_prop_read_cb)},
    {"open",     reinterpret_cast<void*>(hook_open),     reinterpret_cast<void**>(&orig_open)},
    {"openat",   reinterpret_cast<void*>(hook_openat),   reinterpret_cast<void**>(&orig_openat)},
    {"read",     reinterpret_cast<void*>(hook_read),     reinterpret_cast<void**>(&orig_read)},
    {"pread64",  reinterpret_cast<void*>(hook_pread64),  reinterpret_cast<void**>(&orig_pread64)},
    {"close",    reinterpret_cast<void*>(hook_close),    reinterpret_cast<void**>(&orig_close)},
    {"lseek",    reinterpret_cast<void*>(hook_lseek),    reinterpret_cast<void**>(&orig_lseek)},
    {"lseek64",  reinterpret_cast<void*>(hook_lseek64),  reinterpret_cast<void**>(&orig_lseek64)},
    {"__open_2",   reinterpret_cast<void*>(hook_open_2),   reinterpret_cast<void**>(&orig_open_2)},
    {"__openat_2", reinterpret_cast<void*>(hook_openat_2), reinterpret_cast<void**>(&orig_openat_2)},
    // Metadata family and uname: the same reasoning as the PLT block above, but
    // for a library that dlopens after the maps scan. SafeX's isSUExist is in
    // exactly that position - libsafexEx.so is loaded from Grab's Application,
    // long after postAppSpecialize - so without these rows the stat hooks would
    // exist and still never see the check that matters.
    {"stat",     reinterpret_cast<void*>(hook_stat),     reinterpret_cast<void**>(&orig_stat)},
    {"lstat",    reinterpret_cast<void*>(hook_lstat),    reinterpret_cast<void**>(&orig_lstat)},
    {"fstatat",  reinterpret_cast<void*>(hook_fstatat),  reinterpret_cast<void**>(&orig_fstatat)},
    {"access",   reinterpret_cast<void*>(hook_access),   reinterpret_cast<void**>(&orig_access)},
    {"readlink", reinterpret_cast<void*>(hook_readlink), reinterpret_cast<void**>(&orig_readlink)},
    {"uname",    reinterpret_cast<void*>(hook_uname),    reinterpret_cast<void**>(&orig_uname)},
};
static const size_t kLateRegsN = sizeof(kLateRegs) / sizeof(kLateRegs[0]);

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

    // Collect the libraries to hook, scanning /proc/self/maps once:
    //  - the system set above, which covers every Java-level file and property
    //    read (libjavacore / libandroid_runtime) plus libc for native callers;
    //  - the app's OWN libraries. Without these, a native probe that lives in
    //    the app's own .so - vdinfos's NATIVE lens calls
    //    __system_property_read_callback straight from its own library - goes
    //    through none of the hooks below and reads real values, which is
    //    exactly the MISMATCH the report shows on gsm.sim.operator.*.
    //    (Libraries the app loads later, after postAppSpecialize, are still
    //    outside this one-shot scan - see README "How it works", where the
    //    prop-file / AppLog-cache side of that gap is documented as closed
    //    from disk by the bind-mounted build.prop layer and rotate_ids.sh.)
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
        for (const HookReg& r : kPltRegs) {
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
    live += landed(reinterpret_cast<void*>(orig_popen));
    live += landed(reinterpret_cast<void*>(orig_pclose));
    live += landed(reinterpret_cast<void*>(orig_execve));

#if defined(SBX_ENABLE_LSPLANT) && SBX_ENABLE_LSPLANT
    // Inline hooking for the shell-guard layer via Dobby. The PLT pass above
    // only redirects the GOT slots of libraries already mapped at
    // postAppSpecialize; a security library that dlopens later (libpsec.so and
    // friends) resolves these symbols after that scan and would otherwise see
    // the real functions. Dobby patches the function bodies themselves, so
    // late loaders inherit the guard too.
    //
    // The guard has to cover the whole exec family: on bionic only `execve` is
    // ever named by a caller, and `execv`/`execvpe`/`fexecve` branch straight
    // to the internal `__execve` past the public thunk. See the exec-family
    // guard comment above for the disassembly.
    int dobby_hooked = 0;
    struct DobbyReg { const char* name; void* target; void* hook; void** orig; };
    const DobbyReg dobby_regs[] = {
        {"popen",        reinterpret_cast<void*>(popen),        reinterpret_cast<void*>(hook_popen),        reinterpret_cast<void**>(&orig_popen)},
        {"pclose",       reinterpret_cast<void*>(pclose),       reinterpret_cast<void*>(hook_pclose),       reinterpret_cast<void**>(&orig_pclose)},
        {"fclose",       reinterpret_cast<void*>(fclose),       reinterpret_cast<void*>(hook_fclose),       reinterpret_cast<void**>(&orig_fclose)},
        {"execve",       reinterpret_cast<void*>(execve),       reinterpret_cast<void*>(hook_execve),       reinterpret_cast<void**>(&orig_execve)},
        {"execv",        reinterpret_cast<void*>(execv),        reinterpret_cast<void*>(hook_execv),        reinterpret_cast<void**>(&orig_execv)},
        {"execvp",       reinterpret_cast<void*>(execvp),       reinterpret_cast<void*>(hook_execvp),       reinterpret_cast<void**>(&orig_execvp)},
        {"execvpe",      reinterpret_cast<void*>(execvpe),      reinterpret_cast<void*>(hook_execvpe),      reinterpret_cast<void**>(&orig_execvpe)},
        // The varargs spellings: bionic routes them to the internal __execve,
        // which is not exported, so hooking them is the only way to cover
        // execl()/execlp()/execle() callers. See hook_execl.
        {"execl",        reinterpret_cast<void*>(execl),        reinterpret_cast<void*>(hook_execl),        reinterpret_cast<void**>(&orig_execl)},
        {"execlp",       reinterpret_cast<void*>(execlp),       reinterpret_cast<void*>(hook_execlp),       reinterpret_cast<void**>(&orig_execlp)},
        {"execle",       reinterpret_cast<void*>(execle),       reinterpret_cast<void*>(hook_execle),       reinterpret_cast<void**>(&orig_execle)},
        // By name, not by reference: see sbx_libc_sym() - these three are
        // hidden by bionic's API-level availability guards at MIN_SDK=26.
        {"fexecve",      sbx_libc_sym("fexecve"),      reinterpret_cast<void*>(hook_fexecve),      reinterpret_cast<void**>(&orig_fexecve)},
        {"posix_spawn",  sbx_libc_sym("posix_spawn"),  reinterpret_cast<void*>(hook_posix_spawn),  reinterpret_cast<void**>(&orig_posix_spawn)},
        {"posix_spawnp", sbx_libc_sym("posix_spawnp"), reinterpret_cast<void*>(hook_posix_spawnp), reinterpret_cast<void**>(&orig_posix_spawnp)},
    };
    for (const DobbyReg& d : dobby_regs) {
        if (!d.target) continue;
        void* orig = nullptr;
        if (DobbyHook(d.target, reinterpret_cast<dobby_dummy_func_t>(d.hook),
                      reinterpret_cast<dobby_dummy_func_t*>(&orig)) == 0 && orig) {
            *d.orig = orig;
            ++dobby_hooked;
            LOGI("NATIVE_READ: DobbyHook %s registered", d.name);
        } else {
            LOGW("NATIVE_READ: DobbyHook %s FAILED - that entry point is "
                 "unguarded for late-dlopened libraries", d.name);
        }
    }
    LOGI("NATIVE_READ: DobbyHook %d/%zu shell-guard entry point(s) inline-hooked",
         dobby_hooked, sizeof(dobby_regs) / sizeof(dobby_regs[0]));

    // ---- property and file family, inline ------------------------------
    // The PLT pass reaches only the libraries mapped when it ran, and the
    // exec/popen group above covers only the shell guard. Everything else a
    // late-dlopened library reads - the property table and the /proc files -
    // was left unguarded, and that is the widest of the three holes.
    //
    // The table itself is at file scope (kLateRegs) so the host suite can pin
    // its coverage; the reasoning and the Grab measurement are in the comment
    // there. What is local to this pass is only the resolution: each name is
    // looked up through sbx_libc_sym() here, not stored in the table, because
    // a direct reference would bind to this module's own PLT stub rather than
    // the libc body and Dobby would relocate the stub instead of the function.
    //
    // The file family is the same set the PLT pass registers, extended to late
    // loaders. It matters for a lib that reaches /proc through stdio: bionic's
    // fopen64() calls open@plt (verified by disassembling
    // /apex/com.android.runtime/lib64/bionic/libc.so at fopen64, which issues
    // `bl 0xd3330 <open@plt>`), and that stub resolves to libc's OWN open body
    // - the exact function Dobby patches. So a late loader's fopen() of
    // /proc/cpuinfo is covered even though it never crosses a library boundary.
    // (An earlier revision of this comment claimed fopen called __open_2; it
    // does not, and the distinction matters because __open_2 is a separate
    // body that fopen never reaches.)
    //
    // Kept in a SECOND pass with its own log line on purpose: the line above
    // is what sh/debug/summarize.sh counts as the shell-guard verdict, and
    // folding these in would relabel that count without changing the regex.
    int dobby_late = 0;
    for (const LateReg& d : kLateRegs) {
        void* target = sbx_libc_sym(d.name);
        if (!target) continue;
        void* orig = nullptr;
        if (DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(d.hook),
                      reinterpret_cast<dobby_dummy_func_t*>(&orig)) == 0 && orig) {
            *d.orig = orig;
            ++dobby_late;
            LOGI("NATIVE_READ: DobbyHook late %s registered", d.name);
        } else {
            LOGW("NATIVE_READ: DobbyHook late %s FAILED - a library that dlopens "
                 "after postAppSpecialize reads the real value for this entry "
                 "point", d.name);
        }
    }
    LOGI("NATIVE_READ: DobbyHook %d/%zu late-loader property/file entry point(s) "
         "inline-hooked", dobby_late, kLateRegsN);
#endif

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
         "__system_property_find/get/read_callback + "
         "popen/pclose/fclose + execve/execv/execvp/execvpe/execl/execlp/execle/fexecve + "
         "posix_spawn/posix_spawnp",
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
