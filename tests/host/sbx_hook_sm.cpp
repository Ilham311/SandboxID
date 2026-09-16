// Verifies the per-fd synth state machine in jni/module_hooks.cpp by INCLUDING
// that TU, so the static hook_* / synth_* functions under test are the real
// production code. Stubs replace the Zygisk/JNI/shell plumbing; the state
// machine, the mutex, and the map are the real ones.
//
// Reads are served from the HOST's real /proc files, so the synth layer has to
// actually overwrite real kernel bytes — a spoof that leaks the real content
// shows up as a failed string check.
#include "module_hooks.cpp"

#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// ---- stubs for the parts that live in other TUs / the framework ----------
std::map<std::string, std::string> g_identity;
std::string g_pkg = "com.test.app";
bool spoof_prop_value(const std::string&, std::string&) { return false; }
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
// val() normally lives in prop_hooks.cpp; here it reads the test's g_identity.
const std::string empty_val;
const std::string& val(const std::string& k) {
    auto it = g_identity.find(k);
    return it == g_identity.end() ? empty_val : it->second;
}

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++fails; printf("FAIL: %s\n", msg); } \
    else printf("ok:   %s\n", msg); \
} while (0)

// Read the raw bytes the kernel would have handed the app, bypassing the hook.
static std::string raw_file(const char* path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    ::close(fd);
    return out;
}

int main() {
    g_identity["SERIAL"]        = "ABCDEF12";
    g_identity["FINGERPRINT"]   = "google/raven/raven:13/TQ3A.230901.001/10750268";
    g_identity["ANDROID_ID"]    = "deadbeefdeadbeef";
    g_identity["APPLOG_EPOCH"]  = "1700000000000";
    g_identity["MODEL"]         = "Pixel 8";
    g_identity["RELEASE"]       = "15";
    g_identity["INCREMENTAL"]   = "AP3A.240905.015";
    g_identity["BOARD_PLATFORM"] = "zuma";
    g_identity["HOST"]          = "abfarm-release-01";
    g_identity["SOC_MANUFACTURER"] = "Google";
    g_identity["SOC_MODEL"]     = "GS301";
    g_identity["WIFI_MAC"]      = "02:1a:2b:3c:4d:5e";

    // Point the hooked layer at the real syscalls. The hook builds its synth
    // content by reading the real file through orig_read, then must never
    // serve those bytes for a classified path.
    orig_read  = [](int fd, void* b, size_t n) { return ::read(fd, b, n); };
    orig_lseek = [](int fd, off_t o, int w) { return ::lseek(fd, o, w); };
    orig_close = [](int fd) { return ::close(fd); };

    auto read_all_synth = [](int fd) {
        std::string out;
        char buf[128];
        for (int i = 0; i < 64; ++i) {   // bounded: a bug must not hang the test
            ssize_t n = hook_read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
        return out;
    };

    // ---- 1. /proc/meminfo: open + full read + EOF -------------------------
    const std::string real_mem = raw_file("/proc/meminfo");
    CHECK(!real_mem.empty(), "host /proc/meminfo is readable (test precondition)");

    int fd = hook_open("/proc/meminfo", O_RDONLY);
    CHECK(fd >= 0, "hook_open returns a fd");

    std::string first = read_all_synth(fd);
    CHECK(!first.empty(), "hook serves non-empty content for /proc/meminfo");
    CHECK(first != real_mem, "served /proc/meminfo differs from the real file");
    {
        // The real MemTotal kB value must never appear in what the app gets.
        const char* p = strstr(real_mem.c_str(), "MemTotal:");
        CHECK(p != nullptr, "real MemTotal line located");
        if (p) {
            std::string real_line(p, strpbrk(p, "\n") ? (size_t)(strpbrk(p, "\n") - p) : strlen(p));
            CHECK(first.find(real_line) == std::string::npos,
                  "the real MemTotal line is never served verbatim");
        }
        uint64_t expect = 8ULL * 1024 * 1024 * 955 / 1000;   // Pixel 8 = 8 GB
        char want[64];
        snprintf(want, sizeof(want), "MemTotal:       %llu kB", (unsigned long long)expect);
        CHECK(first.find(want) == 0, "served MemTotal is the persona's 8GB tier");
    }
    CHECK(first.find("MemFree:") != std::string::npos, "rest of meminfo is preserved");

    char tiny[16];
    CHECK(hook_read(fd, tiny, sizeof(tiny)) == 0, "read after EOF returns 0 (not real bytes)");

    // ---- 2. rewind via lseek and re-read ----------------------------------
    hook_lseek(fd, 0, SEEK_SET);
    std::string again = read_all_synth(fd);
    CHECK(again == first, "lseek(0)+re-read serves the same synthetic content");
    CHECK(again.size() == first.size(), "rewound read is byte-for-byte the same length");

    // ---- 3. chunked reads assemble the same content ------------------------
    hook_lseek(fd, 0, SEEK_SET);
    std::string chunked;
    char c[7];   // deliberately not a divisor of the length
    for (int i = 0; i < 200; ++i) {
        ssize_t n = hook_read(fd, c, sizeof(c));
        if (n <= 0) break;
        chunked.append(c, (size_t)n);
    }
    CHECK(chunked == first, "7-byte chunked reads assemble identically");

    // ---- 4. short read, then close, then untracked passthrough ------------
    hook_lseek(fd, 0, SEEK_SET);
    char five[5];
    ssize_t n5 = hook_read(fd, five, 5);
    CHECK(n5 == 5, "short read of 5 bytes");
    CHECK(memcmp(five, first.data(), 5) == 0, "short read returns the first 5 synth bytes");

    hook_close(fd);
    CHECK(hook_read(fd, five, 5) == ::read(fd, five, 5),
          "after close the fd is untracked and reads pass through to the real file");

    // ---- 5. boot_id: synthetic, deterministic shape -----------------------
    const std::string real_boot = raw_file("/proc/sys/kernel/random/boot_id");
    CHECK(!real_boot.empty(), "host boot_id is readable (test precondition)");
    int bid = hook_open("/proc/sys/kernel/random/boot_id", O_RDONLY);
    CHECK(bid >= 0, "hook_open returns a fd for boot_id");
    std::string boot = read_all_synth(bid);
    CHECK(!boot.empty() && boot.back() == '\n', "boot_id synthetic content served");
    CHECK(boot != real_boot, "served boot_id differs from the real one");
    CHECK(boot.find_first_not_of("0123456789abcdef-\n") == std::string::npos,
          "boot_id is a hex uuid string");
    hook_close(bid);

    // ---- 6. fail-closed: classified path with an incomplete identity ------
    // boot_id derives from SERIAL; without it the hook must serve EOF rather
    // than the real kernel uuid.
    g_identity.erase("SERIAL");
    int mfd = hook_open("/proc/sys/kernel/random/boot_id", O_RDONLY);
    std::string closed = read_all_synth(mfd);
    CHECK(closed.empty(), "boot_id with no SERIAL fails closed (EOF, not real bytes)");
    CHECK(hook_read(mfd, tiny, sizeof(tiny)) == 0, "fail-closed read returns 0 repeatedly");
    hook_close(mfd);
    g_identity["SERIAL"] = "ABCDEF12";

    // ---- 7. unclassified path is untouched ---------------------------------
    int ufd = hook_open("/proc/self/cmdline", O_RDONLY);
    CHECK(ufd >= 0, "hook_open returns a fd for an unclassified path");
    char ub[256];
    ssize_t un = hook_read(ufd, ub, sizeof(ub) - 1);
    CHECK(un > 0, "unclassified path still serves the real file");
    if (un > 0) ub[un] = 0;
    hook_close(ufd);

    // ---- 8. relative openat resolution -------------------------------------
    // Resolves the dirfd and classifies the joined path. Uses "meminfo" because
    // this test host denies /proc/version; the resolution path is the same.
    // Only the MemTotal line is compared: /proc/meminfo is a live file, so the
    // memory-pressure lines legitimately differ between two opens in time.
    int dirfd = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dirfd >= 0, "opened a dirfd for /proc");
    int rfd = hook_openat(dirfd, "meminfo", O_RDONLY);
    CHECK(rfd >= 0, "hook_openat returns a fd");
    std::string rel = read_all_synth(rfd);
    CHECK(!rel.empty(), "openat with a relative path serves synthetic content");
    CHECK(rel.compare(0, first.find('\n'), first, 0, first.find('\n')) == 0,
          "relative openat resolves to the same persona MemTotal");
    CHECK(rel.find("MemFree:") != std::string::npos, "relative openat keeps the rest of the file");
    hook_close(rfd);
    ::close(dirfd);

    // ---- 9. pread64 at an offset -------------------------------------------
    // All comparisons are within one fd: /proc/meminfo is live, so two opens
    // are not byte-comparable, but one fd's synth buffer is frozen at open.
    int pfd = hook_open("/proc/meminfo", O_RDONLY);
    std::string mine = read_all_synth(pfd);
    CHECK(!mine.empty(), "sequential read fills the synth buffer for pread64 test");
    hook_lseek(pfd, 0, SEEK_SET);

    char pbuf[16];
    ssize_t pn = hook_pread64(pfd, pbuf, sizeof(pbuf), 5);
    CHECK(pn == (ssize_t)sizeof(pbuf), "pread64 serves from the given offset");
    CHECK(pn > 0 && memcmp(pbuf, mine.data() + 5, (size_t)pn) == 0,
          "pread64 bytes match offset 5 of the synth buffer");
    // pread64 must not advance the sequential read position
    char qbuf[8];
    ssize_t qn = hook_read(pfd, qbuf, sizeof(qbuf));
    CHECK(qn == (ssize_t)sizeof(qbuf), "read after pread64 still proceeds from offset 0");
    CHECK(qn > 0 && memcmp(qbuf, mine.data(), (size_t)qn) == 0,
          "pread64 left the sequential cursor alone");
    hook_close(pfd);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
