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
    char* end = nullptr;
    long long secs = std::strtoll(us.c_str(), &end, 10);
    if (end == us.c_str() || secs <= 0) return;
    g_boot_off_sec = (int64_t)secs;

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
        g_boot_off_sec = 0;
        return;
    }
    LOGD("UPTIME boottime PLT hook aktif (+%llds, %d/%zu lib mapped) orig=%p",
         (long long)secs, registered, sizeof(kLibs) / sizeof(kLibs[0]),
         reinterpret_cast<void*>(orig_clock_gettime));
}