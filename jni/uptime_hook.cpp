#include "module_impl.hpp"

int (*orig_clock_gettime)(clockid_t, struct timespec*) = nullptr;
static int64_t g_boot_off_sec = 0;

static int sbx_hooked_clock_gettime(clockid_t clk, struct timespec* ts) {
    int r = orig_clock_gettime ? orig_clock_gettime(clk, ts) : clock_gettime(clk, ts);
    if (r == 0 && ts && (clk == CLOCK_BOOTTIME || clk == CLOCK_BOOTTIME_ALARM))
        ts->tv_sec += g_boot_off_sec;
    return r;
}

static bool sbx_find_lib_dev_inode(const char* suffix, dev_t* out_dev, ino_t* out_ino) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return false;
    char*  line = nullptr;
    size_t cap  = 0;
    ssize_t n;
    size_t sl = strlen(suffix);
    bool found = false;
    while ((n = getline(&line, &cap, f)) != -1) {
        char* path = strchr(line, '/');
        if (!path) continue;
        size_t pl = strlen(path);
        if (pl && path[pl - 1] == '\n') path[--pl] = '\0';
        if (pl >= sl && strcmp(path + pl - sl, suffix) == 0) {
            struct stat st;
            if (stat(path, &st) == 0) { *out_dev = st.st_dev; *out_ino = st.st_ino; found = true; }
            break;
        }
    }
    free(line);
    fclose(f);
    return found;
}

void install_uptime_hook(Api* api, JNIEnv*) {
    const std::string& us = val("UPTIME_SECONDS");
    if (us.empty()) return;
    long long secs = 0;
    // Rejects trailing garbage ("100abc") and out-of-range values, which a
    // bare strtoll would silently truncate into a wrong offset.
    if (!sbx_parse_longlong(us, secs) || secs <= 0) return;

    // The persona claims it has already been up for `secs` seconds. A reader of
    // CLOCK_BOOTTIME from this point on should see a value consistent with that
    // age, so the offset has to solve  real_uptime + offset == persona_age,
    // i.e.  offset = secs - real_uptime_now. An earlier revision added `secs`
    // outright, which reports real_uptime + persona_age - never the persona's
    // age, and wrong by the whole real uptime on a device that has been up for
    // days. With the subtraction, the reader sees exactly `secs` now and ages at
    // the same rate as real time, which is what elapsedRealtime() consumers
    // (uptime displays, MIUI's looper monitor, ANR timing) expect.
    struct timespec now{};
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) return;
    g_boot_off_sec = (int64_t)secs - (int64_t)now.tv_sec;

    static const char* const kLibs[] = {
        "/libutils.so",
        "/libandroid_runtime.so",
    };
    int registered = 0;
    if (!orig_clock_gettime)
        orig_clock_gettime = reinterpret_cast<int (*)(clockid_t, timespec*)>(
            dlsym(RTLD_DEFAULT, "clock_gettime"));
    for (size_t i = 0; i < sizeof(kLibs) / sizeof(kLibs[0]); ++i) {
        dev_t dev = 0; ino_t ino = 0;
        if (!sbx_find_lib_dev_inode(kLibs[i], &dev, &ino)) continue;
        api->pltHookRegister(dev, ino, "clock_gettime",
                             reinterpret_cast<void*>(sbx_hooked_clock_gettime),
                             reinterpret_cast<void**>(&orig_clock_gettime));
        ++registered;
    }
    if (registered == 0) {
        g_boot_off_sec = 0;
        LOGW("UPTIME: chokepoint lib tak ketemu (scanned %zu) — uptime tak dispoof",
             sizeof(kLibs) / sizeof(kLibs[0]));
        return;
    }
    if (!api->pltHookCommit()) {
        g_boot_off_sec = 0;
        LOGW("UPTIME: pltHookCommit gagal (%d/%zu libs registered) — offset dinolkan, "
             "semua reader lihat jam asli (divergensi hang dihindari)",
             registered, sizeof(kLibs) / sizeof(kLibs[0]));
        return;
    }
    if (orig_clock_gettime == nullptr) {
        // Commit reported success but resolved no implementation: the hook would
        // call clock_gettime() recursively through a null trampoline. Zeroing
        // the offset makes the hook a no-op, but without this marker the layer
        // looks healthy in every release capture.
        g_boot_off_sec = 0;
        LOGW("UPTIME: pltHookCommit OK tapi orig_clock_gettime NULL — offset "
             "dinolkan, hook no-op (pembaca jam lihat jam asli)");
        return;
    }
    // LOGD (not LOGI) is deliberate for the success path: a fresh persona has
    // UPTIME_SECONDS unset, and this hook legitimately stays off for it, so a
    // per-fork INFO line would be noise on every target spawn.
    LOGD("UPTIME boottime PLT hook aktif (offset=%llds, persona=%llds, %d/%zu lib mapped) orig=%p",
         (long long)g_boot_off_sec, (long long)secs,
         registered, sizeof(kLibs) / sizeof(kLibs[0]),
         reinterpret_cast<void*>(orig_clock_gettime));
}