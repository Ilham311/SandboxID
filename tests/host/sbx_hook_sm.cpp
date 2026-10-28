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
// The real spoof_prop_value lives in prop_hooks.cpp behind the identity map;
// the test drives it directly so the popen getprop filter can be exercised
// without pulling in the whole prop layer.
static std::map<std::string, std::string> g_test_props;
bool spoof_prop_value(const std::string& k, std::string& out) {
    auto it = g_test_props.find(k);
    if (it == g_test_props.end()) return false;
    out = it->second;
    return true;
}
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" int __android_log_write(int, const char*, const char*) { return 0; }
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

    auto read_all_synth = [](int fd, size_t cap = 1 << 16) {
        std::string out;
        char buf[128];
        // Bounded in bytes, not in passes: a fixed pass count would silently
        // truncate any file longer than passes * sizeof(buf), and /proc files
        // vary in length with the host kernel's config.
        while (out.size() < cap) {
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
    // Bound by the buffer being reassembled, not by a fixed pass count: at 7
    // bytes a pass, a 200-pass cap only ever assembles 1400 bytes and silently
    // truncates anything longer. /proc/meminfo is ~1 KB on a phone but longer
    // on a stock server kernel, which is exactly why this check passed locally
    // and failed on the CI runner.
    while (chunked.size() < first.size()) {
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

    // ---- 10. popen("getprop") must not leak the real property table -------
    // The single-key branch answered 'getprop <known key>'. Every other form -
    // bare `getprop` above all, which is how a probe enumerates a device - fell
    // through to the unfiltered real command. These checks drive the filter
    // that now sits on that path.
    g_test_props["ro.product.model"] = "Pixel 8";
    g_test_props["ro.serialno"]      = "SPOOFED123";
    // The host's real model string also appears in ro.product.odm.model, which
    // production spoofs from NATIVE_PROPS. The stub has to mirror that key set
    // or the end-to-end check below fails on the harness, not on the filter.
    g_test_props["ro.product.odm.model"] = "Pixel 8";

    {
        // A spoofed key's value is replaced; an ordinary key passes through.
        std::string l = "[ro.product.model]: [M2012K11AG]\n";
        bool rw = false;
        CHECK(sbx_filter_getprop_line(l, &rw), "getprop line with a spoofed key is kept");
        CHECK(rw, "getprop line with a spoofed key is marked rewritten");
        CHECK(l == "[ro.product.model]: [Pixel 8]\n", "spoofed value replaces the real one");
        CHECK(l.find("M2012K11AG") == std::string::npos, "the real model never survives the rewrite");

        std::string p = "[ro.other.thing]: [keepme]\n";
        rw = true;
        CHECK(sbx_filter_getprop_line(p, &rw), "unrelated getprop line is kept");
        CHECK(!rw, "unrelated getprop line is not marked rewritten");
        CHECK(p == "[ro.other.thing]: [keepme]\n", "unrelated getprop line is untouched");

        // A hidden property must disappear from the enumeration entirely.
        std::string h = "[ro.miui.ui.version.name]: [V816]\n";
        rw = false;
        CHECK(!sbx_filter_getprop_line(h, &rw), "a hidden prop is dropped from the dump");

        // A value containing ']' must not be cut short at the first bracket.
        g_test_props["ro.weird"] = "a]b";
        std::string w = "[ro.weird]: [real]\n";
        rw = false;
        CHECK(sbx_filter_getprop_line(w, &rw), "a value with a bracket is still filtered");
        CHECK(w == "[ro.weird]: [a]b]\n", "the closing bracket is found from the end, not the first match");
    }

    {
        // End to end through the real popen path, with a canned dump so the
        // check does not depend on the host having a getprop binary - a CI
        // runner does not, and an assertion that silently degrades to "the
        // command produced nothing" is worse than no assertion. The command
        // string contains "getprop" so it takes the same branch hook_popen
        // would, and the filter, pclose and synthetic pipe are the real ones.
        g_test_props["ro.product.model"] = "Pixel 8";
        FILE* fp = sbx_getprop_filtered(
            "printf '[ro.product.model]: [M2012K11AG]\\n"
            "[ro.miui.ui.version.name]: [V816]\\n"
            "[ro.other]: [keepme]\\n' # getprop", "r");
        CHECK(fp != nullptr, "the filtered dump returns a stream");
        std::string dump;
        if (fp) {
            char b[4096];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), fp)) > 0) dump.append(b, n);
            hook_pclose(fp);
        }
        CHECK(!dump.empty(), "the filtered dump is non-empty");
        CHECK(dump.find("M2012K11AG") == std::string::npos,
              "the real model value is not in the filtered dump");
        CHECK(dump.find("Pixel 8") != std::string::npos,
              "the persona model is served in the filtered dump");
        CHECK(dump.find("V816") == std::string::npos,
              "a hidden prop is dropped from the dump entirely");
        CHECK(dump.find("keepme") != std::string::npos,
              "an unrelated prop survives the filter");
    }

    // The same path against the host's real getprop, when there is one. The
    // host model string is read from the command itself rather than hardcoded,
    // so this stays meaningful on any machine.
    {
        FILE* probe = orig_popen ? orig_popen("getprop ro.product.model", "r")
                                 : ::popen("getprop ro.product.model", "r");
        std::string host_model;
        if (probe) {
            char b[256];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), probe)) > 0) host_model.append(b, n);
            (orig_pclose ? orig_pclose(probe) : ::pclose(probe));
            while (!host_model.empty() &&
                   (host_model.back() == '\n' || host_model.back() == '\r'))
                host_model.pop_back();
        }
        if (host_model.empty()) {
            printf("skip: host has no getprop, real-dump check not run\n");
        } else {
            g_test_props["ro.product.model"] = "Pixel 8";
            FILE* fp = hook_popen("getprop", "r");
            std::string dump;
            if (fp) {
                char b[4096];
                size_t n;
                while ((n = fread(b, 1, sizeof(b), fp)) > 0) dump.append(b, n);
                hook_pclose(fp);
            }
            CHECK(!dump.empty(), "the real getprop dump is non-empty");
            CHECK(dump.find(host_model) == std::string::npos,
                  "the host's real ro.product.model is not in the filtered dump");
            CHECK(dump.find("Pixel 8") != std::string::npos,
                  "the persona model replaces it in the real dump");
        }
    }
    g_test_props.clear();

    // ---- 11. the varargs exec family is guarded too ------------------------
    // execl/execle/execlp call the internal __execve, which is not exported and
    // cannot be PLT-hooked, so their bodies are rebuilt on the hooked
    // execv/execvp. These checks pin that: each varargs form must reach the
    // same deny decision, and must pass the argv through intact.
    {
        // Route the hooked entry points to a recorder instead of a real exec,
        // so the decision and the argv are observable from the host.
        static std::string got_path, got_argv0, got_argv1;
        static int calls = 0;
        orig_execv = [](const char* p, char* const a[]) -> int {
            ++calls; got_path = p ? p : "";
            got_argv0 = (a && a[0]) ? a[0] : "";
            got_argv1 = (a && a[1]) ? a[1] : "";
            return 0;
        };
        orig_execvp = [](const char* p, char* const a[]) -> int {
            ++calls; got_path = p ? p : "";
            got_argv0 = (a && a[0]) ? a[0] : "";
            got_argv1 = (a && a[1]) ? a[1] : "";
            return 0;
        };

        calls = 0;
        CHECK(hook_execl("/bin/echo", "echo", "hello", (char*)nullptr) == 0,
              "execl reaches the guarded execv");
        CHECK(calls == 1 && got_path == "/bin/echo", "execl passes its path through");
        CHECK(got_argv0 == "echo" && got_argv1 == "hello",
              "execl rebuilds argv from the va_list, in order");

        calls = 0;
        CHECK(hook_execlp("echo", "echo", "hi", (char*)nullptr) == 0,
              "execlp reaches the guarded execvp");
        CHECK(calls == 1 && got_path == "echo", "execlp passes its file through");

        // The deny decision itself must fire for a varargs caller naming su.
        calls = 0;
        errno = 0;
        CHECK(hook_execl("/system/bin/su", "su", "-c", "id", (char*)nullptr) == -1,
              "execl('/system/bin/su') is denied");
        CHECK(errno == ENOENT, "a denied execl reports ENOENT, like the other forms");
        CHECK(calls == 0, "a denied execl never reaches the real execv");

        calls = 0;
        errno = 0;
        CHECK(hook_execlp("su", "su", "-c", "id", (char*)nullptr) == -1,
              "execlp('su') is denied");
        CHECK(calls == 0, "a denied execlp never reaches the real execvp");

        // execle is the one that can silently corrupt the call: its envp sits
        // AFTER the argv's terminating null, so reading it one slot early hands
        // execve the last argv entry as the environment. Capture what actually
        // arrives.
        static const char* got_envp0 = nullptr;
        orig_execve = [](const char* p, char* const a[], char* const e[]) -> int {
            ++calls; got_path = p ? p : "";
            got_argv0 = (a && a[0]) ? a[0] : "";
            got_argv1 = (a && a[1]) ? a[1] : "";
            got_envp0 = (e && e[0]) ? e[0] : nullptr;
            return 0;
        };

        static char e0[] = "PATH=/bin";
        static char* const envp[] = { e0, nullptr };
        calls = 0; got_envp0 = nullptr;
        CHECK(hook_execle("/bin/echo", "echo", "hello", (char*)nullptr, envp) == 0,
              "execle reaches the guarded execve");
        CHECK(calls == 1 && got_path == "/bin/echo", "execle passes its path through");
        CHECK(got_argv0 == "echo" && got_argv1 == "hello",
              "execle rebuilds argv without swallowing the envp");
        CHECK(got_envp0 == e0,
              "execle hands the real envp to execve, not the last argv entry");

        orig_execv = nullptr;
        orig_execvp = nullptr;
        orig_execve = nullptr;
    }

    // ---- 12. every guarded entry point is actually registered --------------
    // A hook body that exists but is never listed in the PLT table is dead
    // code: the guard silently does nothing. That is how execl/execlp/execle
    // went uncovered while a comment claimed the whole exec family was handled.
    // The table is at file scope precisely so this can be asserted.
    {
        auto registered = [](const char* name) {
            for (const HookReg& r : kPltRegs)
                if (strcmp(r.name, name) == 0) return true;
            return false;
        };
        for (const char* n : {"execve", "execv", "execvp", "execvpe",
                              "execl", "execlp", "execle",
                              "fexecve", "posix_spawn", "posix_spawnp"})
            CHECK(registered(n), (std::string("PLT table registers ") + n).c_str());
        // And the reverse: a table entry pointing at a null body would be
        // registered but do nothing.
        for (const HookReg& r : kPltRegs)
            CHECK(r.fn != nullptr && r.orig != nullptr,
                  (std::string("PLT entry '") + r.name + "' has a body and a slot").c_str());
    }

    // ---- 13. write-mode popen is not answered with a read-only stream ------
    // The filter reads the child's stdout, which only exists in read mode.
    // Answering popen(cmd, "w") with a synthetic read-only stream changed the
    // call's contract: the real one accepts writes, that one returned EBADF.
    {
        CHECK(!sbx_popen_readable("w"), "write mode is not treated as readable");
        CHECK(!sbx_popen_readable("we"), "write+cloexec mode is not readable");
        CHECK(sbx_popen_readable("r"), "read mode is readable");
        CHECK(sbx_popen_readable("re"), "read+cloexec mode is readable");
        CHECK(!sbx_popen_readable(nullptr), "a null mode is not readable");

        // A write-mode command must go to the real popen, and the stream it
        // returns must be writable, exactly as the real one's is. The command
        // deliberately contains "getprop": without the mode check it takes the
        // filter branch and comes back as a read-only synthetic stream, which
        // is the defect. A command that does not name getprop would pass either
        // way and prove nothing.
        FILE* wfp = hook_popen("getprop > /dev/null", "w");
        CHECK(wfp != nullptr, "write-mode popen still returns a stream");
        if (wfp) {
            int rc = fputc('x', wfp);
            CHECK(rc == 'x', "write-mode stream accepts a write (not a read-only synth)");
            hook_pclose(wfp);
        }
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
