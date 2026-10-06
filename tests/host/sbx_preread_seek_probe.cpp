// Adversarial probe for the PRE-READ seek path: a seek issued BEFORE the first
// read, when the synthetic buffer does not exist yet. Neither sbx_hook_sm,
// sbx_race_probe, nor sbx_seek_cur_probe exercises this: in every one of them
// the first thing that touches the fd is a read, so the buffer is always
// `ready` by the time any lseek lands.
//
// What a correct implementation must do: the position the app is told and the
// bytes it is then served must agree, in the synthetic coordinate space. After
//   lseek(fd, 100, SEEK_CUR)   [before any read]  -> must report 100
// the first read must serve synth[100], not synth[0].
#include "module_hooks.cpp"

#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fstream>

std::map<std::string, std::string> g_identity;
std::string g_pkg = "com.test.app";
bool spoof_prop_value(const std::string&, std::string&) { return false; }
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" int __android_log_write(int, const char*, const char*) { return 0; }
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

int main() {
    g_identity["SERIAL"]        = "ABCDEF12";
    g_identity["FINGERPRINT"]   = "google/raven/raven:13/TQ3A.230901.001/10750268";
    g_identity["ANDROID_ID"]    = "deadbeefdeadbeef";
    g_identity["APPLOG_EPOCH"]  = "1700000000000";

    orig_read  = [](int fd, void* b, size_t n) { return ::read(fd, b, n); };
    orig_lseek = [](int fd, off_t o, int w) { return ::lseek(fd, o, w); };
    orig_close = [](int fd) { return ::close(fd); };

    const std::string base = (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    const std::string dir  = base + "/sbx-preread-seek";
    ::mkdir(dir.c_str(), 0755);
    ::mkdir((dir + "/shared_prefs").c_str(), 0755);
    const std::string path = dir + "/shared_prefs/applog.xml";

    static const char* keys[] = { "device_id", "install_id", "ssid",
                                  "openudid", "clientudid", "cdid" };
    std::string real = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n";
    for (int i = 0; i < 6; ++i) {
        char line[256];
        snprintf(line, sizeof(line),
                 "  <string name=\"%s\">realvalue-%020d-padding</string>\n", keys[i], i);
        real += line;
    }
    real += "</map>\n";
    { std::ofstream f(path, std::ios::binary); f << real; }

    // Reference synthetic stream: a separate fd read from 0 with no seek.
    int ref_fd = hook_open(path.c_str(), O_RDONLY);
    auto read_all = [](int fdd, size_t cap = 1 << 16) {
        std::string out; char buf[128];
        while (out.size() < cap) {
            ssize_t n = hook_read(fdd, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
        return out;
    };
    std::string synth = read_all(ref_fd);
    hook_close(ref_fd);
    if (synth.empty() || synth.size() < 300) {
        printf("== classification did not engage (synth=%zu), vacuous ==\n", synth.size());
        return 2;
    }
    printf("real=%zu synth=%zu\n", real.size(), synth.size());

    // ---- 1. the FileInputStream.skip() idiom BEFORE the first read ---------
    // libcore skip(n): cur = lseek(0, CUR); end = lseek(n, CUR); return end-cur.
    // Issued before any read, this is a pre-read seek to n. It must report n
    // AND the following read must serve synth[n].
    {
        int fd = hook_open(path.c_str(), O_RDONLY);
        off_t cur = hook_lseek(fd, 0, SEEK_CUR);
        off_t end = hook_lseek(fd, 100, SEEK_CUR);
        CHECK(cur == 0, "pre-read skip: lseek(0,CUR) on a fresh fd reports 0");
        CHECK(end == 100, "pre-read skip: lseek(100,CUR) reports 100");
        CHECK(end - cur == 100, "pre-read skip idiom reports exactly 100 skipped");
        char b[1];
        ssize_t n = hook_read(fd, b, 1);
        printf("  first byte served after pre-read skip(100): %d (synth[100]=%d)\n",
               n == 1 ? (int)(unsigned char)b[0] : -1,
               (int)(unsigned char)synth[100]);
        CHECK(n == 1 && (size_t)100 < synth.size() && b[0] == synth[100],
              "read after pre-read skip(100) serves synth[100], not synth[0]");
        // Position must still be consistent afterwards.
        CHECK(hook_lseek(fd, 0, SEEK_CUR) == 101,
              "position after skip+read is 101, not 1");
        hook_close(fd);
    }

    // ---- 2. absolute pre-read seek ---------------------------------------
    {
        int fd = hook_open(path.c_str(), O_RDONLY);
        CHECK(hook_lseek(fd, 50, SEEK_SET) == 50, "pre-read SEEK_SET reports 50");
        char b[1];
        ssize_t n = hook_read(fd, b, 1);
        printf("  SEEK_SET(50): served byte %d, synth[0]=%d, synth[50]=%d\n",
               n == 1 ? (int)(unsigned char)b[0] : -1,
               (int)(unsigned char)synth[0], (int)(unsigned char)synth[50]);
        CHECK(n == 1 && b[0] == synth[50],
              "first read after pre-read SEEK_SET(50) serves synth[50]");
        // WHY that passes: the build read the REAL file from the seeked position
        // and patched in place, so the served stream is synth[50:] served from
        // reported-position 50. The offsets line up only because the patch
        // preserves byte offsets in unmodified regions - and the app still
        // receives a truncated document starting mid-header.
        std::string served = read_all(fd);
        printf("  served stream after SEEK_SET(50) is synth[50:]: %s; len=%zu\n",
               served == synth.substr(50) ? "YES (masked)" : "NO",
               served.size());
        printf("  served prefix: \"%.60s\"\n", served.c_str());
        printf("  synth prefix:  \"%.60s\"\n", synth.c_str());
        CHECK(served == synth.substr(50),
              "masked case: served stream is synth[50:], a document missing its "
              "<?xml prefix - corrupt content that merely aligns by offset");
        hook_close(fd);
    }

    // ---- 3. pre-read SEEK_END, then read ---------------------------------
    // A real file read after seek(0, END) yields 0 bytes. The synthetic layer
    // must not rewind the cursor to 0 at publish and then serve the head.
    {
        int fd = hook_open(path.c_str(), O_RDONLY);
        off_t e = hook_lseek(fd, 0, SEEK_END);
        printf("  pre-read SEEK_END reported %lld (real=%zu synth=%zu)\n",
               (long long)e, real.size(), synth.size());
        CHECK(e == (off_t)synth.size(), "pre-read SEEK_END reports the synth size");
        char b[16];
        CHECK(hook_read(fd, b, sizeof(b)) == 0,
              "read immediately after pre-read SEEK_END returns 0 (EOF)");
        // ...and the fd must still be readable after an explicit rewind.
        CHECK(hook_lseek(fd, 0, SEEK_SET) == 0, "rewind after pre-read SEEK_END");
        CHECK(read_all(fd) == synth, "rewound read reproduces the synthetic stream");
        hook_close(fd);
    }

    printf("== sbx_preread_seek_probe: %d check(s), %d failure(s) ==\n", checks, fails);
    return fails ? 1 : 0;
}
