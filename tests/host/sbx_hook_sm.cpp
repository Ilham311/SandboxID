// Verifies the per-fd synth state machine in jni/module_hooks.cpp by INCLUDING
// that TU, so the static hook_* / synth_* functions under test are the real
// production code. Stubs replace the Zygisk/JNI/shell plumbing; the state
// machine, the mutex, and the map are the real ones.
//
// Reads are served from the HOST's real /proc files, so the synth layer has to
// actually overwrite real kernel bytes — a spoof that leaks the real content
// shows up as a failed string check.
#include "module_hooks.cpp"

#include <algorithm>
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

    // ---- 11b. root-tool lookup and package enumeration in argv ------------
    // Megvii's FaceID SDK runs `which su` and `pm list package -3` through
    // Runtime.exec("sh"), so the tell sits in an ARGUMENT, not in argv[0].
    // Two properties have to hold at once, and they pull in opposite
    // directions: the probe must be caught, and an ordinary file operation
    // that merely mentions "su" must NOT be. The second half is the one a
    // blanket argv scan gets wrong, which is why the check is positional.
    {
        auto argv_of = [](std::vector<char*>& v, std::vector<const char*>& keep) {
            v.clear();
            for (const char* s : keep) v.push_back(const_cast<char*>(s));
            v.push_back(nullptr);
            return v.data();
        };
        std::vector<char*> v;
        std::vector<const char*> keep;

        // The probes that must be denied.
        keep = {"which", "su"};                 CHECK(sbx_argv_probes_root(argv_of(v, keep)), "which su is a root-tool lookup");
        keep = {"/system/bin/which", "su"};     CHECK(sbx_argv_probes_root(argv_of(v, keep)), "absolute-path which su is a root-tool lookup");
        keep = {"which", "-a", "su"};           CHECK(sbx_argv_probes_root(argv_of(v, keep)), "which -a su skips the option token");
        keep = {"whereis", "magisk"};           CHECK(sbx_argv_probes_root(argv_of(v, keep)), "whereis magisk is a root-tool lookup");
        keep = {"command", "-v", "su"};         CHECK(sbx_argv_probes_root(argv_of(v, keep)), "command -v su is a root-tool lookup");
        keep = {"type", "ksud"};                CHECK(sbx_argv_probes_root(argv_of(v, keep)), "type ksud is a root-tool lookup");

        // The false positives a blanket argv scan would produce.
        keep = {"grep", "su", "notes.txt"};     CHECK(!sbx_argv_probes_root(argv_of(v, keep)), "grep su notes.txt is not a root-tool lookup");
        keep = {"cat", "su.log"};               CHECK(!sbx_argv_probes_root(argv_of(v, keep)), "cat su.log is not a root-tool lookup");
        keep = {"which", "sh"};                 CHECK(!sbx_argv_probes_root(argv_of(v, keep)), "which sh is not a root-tool lookup");
        keep = {"which"};                       CHECK(!sbx_argv_probes_root(argv_of(v, keep)), "bare which is not a root-tool lookup");
        CHECK(!sbx_argv_probes_root(nullptr), "null argv is not a root-tool lookup");

        // Package enumeration, in both spellings Megvii uses.
        keep = {"pm", "list", "package", "-3"};         CHECK(sbx_argv_lists_packages(argv_of(v, keep)), "pm list package -3 is package enumeration");
        keep = {"pm", "list", "packages"};              CHECK(sbx_argv_lists_packages(argv_of(v, keep)), "pm list packages is package enumeration");
        keep = {"/system/bin/cmd", "package", "list", "packages"};
        CHECK(sbx_argv_lists_packages(argv_of(v, keep)), "cmd package list packages is package enumeration");
        // Not enumeration: a single-package query and an unrelated pm call.
        keep = {"pm", "list", "permission-groups"};     CHECK(!sbx_argv_lists_packages(argv_of(v, keep)), "pm list permission-groups is not enumeration");
        keep = {"pm", "path", "com.grabtaxi.passenger"}; CHECK(!sbx_argv_lists_packages(argv_of(v, keep)), "pm path <pkg> is not enumeration");
        keep = {"am", "list", "packages"};              CHECK(!sbx_argv_lists_packages(argv_of(v, keep)), "am list packages is not pm/cmd enumeration");
        CHECK(!sbx_argv_lists_packages(nullptr), "null argv is not enumeration");
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

    // ---- 14. late-loader coverage: the PLT scan cannot reach these --------
    // The PLT pass walks /proc/self/maps ONCE, in postAppSpecialize, and only
    // registers GOT slots of libraries already mapped at that moment. A target
    // app whose security stack is dlopened later - Grab's is, from
    // Application.onCreate - resolves the real symbols and reads real values.
    // Measured: the PLT pass logged "11 lib(s) incl. 0 app-owned" for
    // com.grabtaxi.passenger, and libpsec.so / libCredolabNative.so /
    // libcore-lib.so each import __system_property_get directly.
    //
    // The fix is the second Dobby array, which patches function BODIES so a
    // late loader inherits the guard. These checks pin the two properties that
    // make it work and that are easy to lose in a refactor:
    //
    //   (a) every property/file entry point the PLT pass registers must also
    //       appear in the inline (late-loader) set - a name in one and not the
    //       other is a hole that no test on a stock device would show, because
    //       the PLT half keeps working;
    //   (b) the inline set must be resolved by NAME, not by taking the address
    //       of a libc function. A direct reference binds to this module's own
    //       PLT stub, and Dobby would relocate the stub rather than the body.
    {
        // The PLT registrations, as (name) strings, from kPltRegs. Kept as a
        // literal list rather than read from the table so that a symbol dropped
        // from kPltRegs fails here instead of silently shrinking both sets.
        static const char* const plt_expected[] = {
            "read", "open", "openat", "pread64", "close", "lseek", "lseek64",
            "__open_2", "__openat_2",
            "__system_property_read_callback",
            "__system_property_find",
            "__system_property_get",
            // Metadata + kernel identity. SafeX's root check is a bare
            // stat("/system/bin/su") loop - it never opens anything, so none of
            // the read hooks above can see it.
            "stat", "lstat", "fstatat", "access", "readlink", "uname",
            "dl_iterate_phdr",
        };
        for (const char* n : plt_expected) {
            bool in_plt = false;
            for (const HookReg& r : kPltRegs)
                if (std::strcmp(r.name, n) == 0) { in_plt = true; break; }
            CHECK(in_plt, (std::string("PLT pass registers ") + n).c_str());
        }

        // The inline set, from the production table the installer walks. It is
        // declared outside the SBX_ENABLE_LSPLANT guard for exactly this
        // reason: the host suites build with LSPLANT off, so this is the only
        // place the coverage invariant can be checked at all.
        //
        // Set equality, both directions, and neither direction is redundant:
        //   - PLT name missing from the inline set  -> a late loader reads the
        //     real value while an early one reads the persona. The early half
        //     keeps working, so on a stock device the app looks guarded.
        //   - inline name missing from the PLT set -> the GOT slot is never
        //     redirected, so a library mapped at specialize time is unguarded
        //     until Dobby's pass runs, and Dobby is a no-op there because the
        //     PLT pass already claimed the symbol (the `if (*d.orig)` skip).
        for (const char* n : plt_expected) {
            bool in_late = false;
            for (size_t i = 0; i < kLateRegsN; ++i)
                if (std::strcmp(kLateRegs[i].name, n) == 0) { in_late = true; break; }
            CHECK(in_late, (std::string("inline late-loader set covers ") + n).c_str());
        }
        for (size_t i = 0; i < kLateRegsN; ++i) {
            bool in_plt = false;
            for (const HookReg& r : kPltRegs)
                if (std::strcmp(r.name, kLateRegs[i].name) == 0) { in_plt = true; break; }
            CHECK(in_plt, (std::string("PLT pass covers inline name ")
                           + kLateRegs[i].name).c_str());
        }

        // Every entry must name a hook and a trampoline slot. A row with a null
        // hook would install a trampoline that jumps to address 0, and a row
        // with a null orig would make the installer's "already live" test
        // dereference null on the first pass.
        for (size_t i = 0; i < kLateRegsN; ++i) {
            CHECK(kLateRegs[i].hook != nullptr,
                  (std::string("late-loader row has a hook: ") + kLateRegs[i].name).c_str());
            CHECK(kLateRegs[i].orig != nullptr,
                  (std::string("late-loader row has a trampoline slot: ")
                   + kLateRegs[i].name).c_str());
        }

        // The three property entry points are the ones Grab's libraries call.
        // They are the reason the table exists, so name them explicitly rather
        // than trusting the loop above to have noticed their absence.
        for (const char* p : {"__system_property_get",
                              "__system_property_find",
                              "__system_property_read_callback"}) {
            bool found = false;
            for (size_t i = 0; i < kLateRegsN; ++i)
                if (std::strcmp(kLateRegs[i].name, p) == 0) { found = true; break; }
            CHECK(found, (std::string("late-loader set carries the property entry ")
                          + p).c_str());
        }
    }

    // ---- 15. metadata hooks hide the root binaries, and nothing else ------
    // SafeX's isSUExist is eight stat() calls over a hardcoded path list; on
    // this device /system/bin/su exists as a hardlink to /system/bin/sh, so
    // every one of them succeeds unless stat is hooked. These checks pin the
    // two halves that make the hide correct: the root basenames are reported
    // absent, and an ordinary path is NOT (a stat hook that denies everything
    // would break the app's own file access, which is a worse bug than the
    // leak it plugs).
    {
        // The exact list disassembled out of libsafexEx.so's isSUExist.
        static const char* const safeX_paths[] = {
            "/sbin/su", "/system/bin/su", "/system/xbin/su",
            "/data/local/xbin/su", "/data/local/bin/su", "/system/sd/xbin/su",
            "/system/bin/failsafe/su", "/data/local/su",
        };
        for (const char* p : safeX_paths)
            CHECK(sbx_meta_hidden(p),
                  (std::string("metadata hook hides ") + p).c_str());

        // The daemon spellings the exec guard already refuses; the stat guard
        // shares sbx_is_root_binary so the two can never disagree.
        CHECK(sbx_meta_hidden("/system/xbin/daemonsu"), "hides daemonsu");
        CHECK(sbx_meta_hidden("/system/bin/magisk"), "hides magisk");
        CHECK(sbx_meta_hidden("/data/adb/ksud"), "hides ksud");

        // Not root binaries: these must pass through untouched.
        CHECK(!sbx_meta_hidden("/system/bin/sh"), "does NOT hide /system/bin/sh");
        CHECK(!sbx_meta_hidden("/system/bin/abb"), "does NOT hide an ordinary binary");
        CHECK(!sbx_meta_hidden("/proc/self/maps"), "does NOT hide /proc/self/maps");
        CHECK(!sbx_meta_hidden("/data/data/com.grabtaxi.passenger/files/x"),
              "does NOT hide the app's own files");
        // A path that merely CONTAINS "su" is not a root binary. The exec guard
        // was once substring-based and denied legitimate execs; the metadata
        // guard must not repeat that.
        CHECK(!sbx_meta_hidden("/data/user/0/com.example/su-something"),
              "does NOT hide a path that merely contains 'su'");
        CHECK(!sbx_meta_hidden("/storage/emulated/0/sunset.jpg"),
              "does NOT hide an ordinary file containing 'su'");
        CHECK(!sbx_meta_hidden(nullptr), "null path is not hidden");
        CHECK(!sbx_meta_hidden(""), "empty path is not hidden");
    }

    // ---- 16. uname agrees with /proc/version ------------------------------
    // The whole point of the uname hook is that the two lenses stop
    // contradicting each other. That is a property of the SPLIT, not of the
    // synthesis: synth_proc_version returns a whole "Linux version <rel> (...)"
    // line, and utsname wants <rel> in .release and the remainder in .version.
    // A refactor that re-derives the release instead of splitting it would let
    // the two drift again while every other check still passed.
    {
        g_identity["RELEASE"]        = "15";
        g_identity["INCREMENTAL"]    = "T.R4T3.1c82702-2-1";
        g_identity["BOARD_PLATFORM"] = "taro";
        g_identity["HOST"]           = "oppo-build-936";
        g_identity["SERIAL"]         = "717f35a1af136526";

        struct utsname u{};
        std::snprintf(u.release, sizeof(u.release), "%s", "4.19.337-FakeDreamer-1.9-Universe");
        std::snprintf(u.version, sizeof(u.version), "%s", "#1 SMP PREEMPT Wed Sep 24 2025");

        int rc = hook_uname(&u);
        CHECK(rc == 0, "uname hook returns the syscall's success");

        // The real kernel string names the ROM and is a kernel no CPH2521 ran.
        CHECK(std::string(u.release) != "4.19.337-FakeDreamer-1.9-Universe",
              "uname hook does not serve the REAL kernel release");
        CHECK(std::string(u.release).find("FakeDreamer") == std::string::npos,
              "uname release does not leak the ROM name");

        // Must agree with what /proc/version would be served.
        std::string want = sbxnr::synth_proc_version(
            "15", "T.R4T3.1c82702-2-1", "taro", "oppo-build-936",
            sbxnr::fnv1a("717f35a1af136526"));
        static const char kPfx[] = "Linux version ";
        CHECK(want.compare(0, sizeof(kPfx) - 1, kPfx) == 0,
              "synth_proc_version still returns a full /proc/version line");
        std::string body = want.substr(sizeof(kPfx) - 1);
        size_t sp = body.find(' ');
        CHECK(sp != std::string::npos, "synthesized line has a release token");
        CHECK(std::string(u.release) == body.substr(0, sp),
              "uname.release equals the release /proc/version would serve");

        // .version is 65 bytes (__NEW_UTS_LEN is 64, +1 for NUL), and the
        // remainder of a /proc/version line is longer than that - so the kernel
        // itself would truncate, and so must we. Asserting equality here would
        // be asserting a lie; the contract is "a prefix of the remainder, cut
        // at the field boundary", which is exactly what a real kernel returns.
        std::string rest = body.substr(sp + 1);
        std::string got_ver(u.version);
        CHECK(sizeof(u.version) == 65, "utsname.version is 65 bytes");
        CHECK(got_ver.size() <= 64, "uname.version respects the 64-char field limit");
        CHECK(rest.compare(0, got_ver.size(), got_ver) == 0,
              "uname.version is a prefix of the remainder /proc/version would serve");
        CHECK(got_ver.size() == std::min<size_t>(64, rest.size()),
              "uname.version is truncated exactly at the field limit, not shorter");

        // sysname/nodename/machine must be left alone: the persona does not
        // describe a different OS or architecture, and rewriting them would be
        // a new lie rather than the removal of one.
        CHECK(std::string(u.sysname) == "Linux", "uname.sysname is untouched");
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
