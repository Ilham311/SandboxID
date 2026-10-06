// Adversarial probe for the SEEK_CUR branch of synth_seek() in the real
// jni/module_hooks.cpp TU. SEEK_SET/SEEK_END are covered by sbx_hook_sm and
// sbx_race_probe; SEEK_CUR is not covered by any existing suite, so this
// exercises it directly against a seekable classified file (applog.xml).
//
// What a correct implementation must do: the reported position and the byte
// stream must agree, in the SYNTHETIC coordinate space, exactly as they do for
// a real file. Concretely, after reading N synthetic bytes:
//   lseek(fd, 0, SEEK_CUR) == N
//   lseek(fd, k, SEEK_CUR) == N + k, and the next read serves synth[N + k]
// and the FileInputStream.skip() idiom (lseek(0,CUR) then lseek(n,CUR),
// result = second - first) must report exactly n.
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
    const std::string dir  = base + "/sbx-seek-cur-probe";
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

    struct stat st{};
    ::stat(path.c_str(), &st);

    int fd = hook_open(path.c_str(), O_RDONLY);
    CHECK(fd >= 0, "hook_open returns a fd");

    auto read_all_synth = [](int fdd, size_t cap = 1 << 16) {
        std::string out;
        char buf[128];
        while (out.size() < cap) {
            ssize_t n = hook_read(fdd, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
        return out;
    };

    std::string synth = read_all_synth(fd);
    CHECK(!synth.empty() && synth.size() != (size_t)st.st_size,
          "path is classified and synth length differs from real (probe is meaningful)");
    if (synth.empty() || synth.size() == (size_t)st.st_size) {
        printf("== sbx_seek_cur_probe: classification did not engage, vacuous ==\n");
        return 2;
    }
    printf("real=%zu synth=%zu\n", (size_t)st.st_size, synth.size());

    // ---- 1. position query after a partial read ---------------------------
    // Rewind, consume 64 synthetic bytes, then ask where we are. A real file
    // answers 64.
    CHECK(hook_lseek(fd, 0, SEEK_SET) == 0, "SEEK_SET to 0");
    char tmp[64];
    CHECK(hook_read(fd, tmp, sizeof(tmp)) == 64, "consumed 64 synthetic bytes");
    {
        off_t pos = hook_lseek(fd, 0, SEEK_CUR);
        printf("lseek(0,SEEK_CUR) after read(64) -> %lld\n", (long long)pos);
        CHECK(pos == 64, "lseek(0,SEEK_CUR) after read(64) reports 64");
    }

    // ---- 2. relative forward seek lands on the right synthetic byte -------
    // From 64, seek +50 relative: position must be 114 and the next read must
    // serve synth[114], not synth[100] or synth[64].
    {
        off_t pos = hook_lseek(fd, 50, SEEK_CUR);
        printf("lseek(50,SEEK_CUR) from 64 -> %lld\n", (long long)pos);
        CHECK(pos == 114, "lseek(50,SEEK_CUR) from 64 reports 114");
        char b[1];
        ssize_t n = hook_read(fd, b, 1);
        CHECK(n == 1 && (size_t)114 < synth.size() && b[0] == synth[114],
              "read after +50 relative seek serves synth[114]");
    }

    // ---- 3. the FileInputStream.skip() idiom ------------------------------
    // libcore's skip() is: cur = lseek(0, CUR); end = lseek(n, CUR);
    // bytes skipped = end - cur. Both the reported count AND the landing offset
    // must be right, or the app both misreports progress and reads wrong bytes.
    // The skip distance stays inside the synthetic buffer so the in-bounds
    // semantics are what is asserted; clamping past EOF is checked separately.
    {
        CHECK(hook_lseek(fd, 0, SEEK_SET) == 0, "rewind for skip test");
        CHECK(hook_read(fd, tmp, 100) == 100, "consumed 100 synthetic bytes");
        off_t cur = hook_lseek(fd, 0, SEEK_CUR);
        off_t end = hook_lseek(fd, 100, SEEK_CUR);
        off_t skipped = end - cur;
        printf("skip(100): cur=%lld end=%lld reported skipped=%lld\n",
               (long long)cur, (long long)end, (long long)skipped);
        CHECK(cur == 100, "skip idiom: cur position reported as 100");
        CHECK(skipped == 100, "skip idiom reports exactly 100 bytes skipped");
        CHECK(end == 200, "skip idiom lands at 200");
        char b[1];
        ssize_t n = hook_read(fd, b, 1);
        CHECK(n == 1 && (size_t)200 < synth.size() && b[0] == synth[200],
              "read after skip(100) serves synth[200]");
    }

    // ---- 3b. skip past the synthetic end clamps like a real file ----------
    // A real 428-byte file answers lseek(1000, CUR) from 100 with 428 and the
    // app then reads 0 bytes. The synthetic layer must match that, not report a
    // position beyond its own content.
    {
        hook_lseek(fd, 0, SEEK_SET);
        hook_read(fd, tmp, 100);
        off_t cur = hook_lseek(fd, 0, SEEK_CUR);
        off_t end = hook_lseek(fd, 1000, SEEK_CUR);
        CHECK(end == (off_t)synth.size(),
              "skip past the synthetic end clamps to synth size");
        CHECK(end - cur == (off_t)synth.size() - 100,
              "skip past EOF reports the clamped distance, not 1000");
        CHECK(hook_read(fd, tmp, 16) == 0, "read after clamped skip returns 0");
    }

    // ---- 4. negative relative seek is rejected, cursor untouched ----------
    // This must be decided in SYNTHETIC space. On a fresh fd the real position
    // sits at the real EOF (511) after synth_build consumed the whole file,
    // while the synthetic cursor starts at 0. Consuming 3 synthetic bytes puts
    // the cursor at 3; lseek(-5, CUR) is then legal in real space (511-5=506)
    // but must still be rejected, because the app has only consumed 3 bytes of
    // the synthetic stream. A fix that computes want from the real result
    // instead returns 506-5 and reports a clamped position instead of EINVAL.
    {
        int fd2 = hook_open(path.c_str(), O_RDONLY);
        CHECK(fd2 >= 0, "second fd for the negative-seek case");
        char small[3];
        CHECK(hook_read(fd2, small, sizeof(small)) == 3, "consumed 3 synthetic bytes");
        CHECK(hook_lseek(fd2, 0, SEEK_CUR) == 3, "cursor is at 3");
        errno = 0;
        off_t r = hook_lseek(fd2, -5, SEEK_CUR);
        CHECK(r == (off_t)-1 && errno == EINVAL, "negative SEEK_CUR rejected");
        CHECK(hook_lseek(fd2, 0, SEEK_CUR) == 3,
              "cursor unchanged after a rejected negative seek");
        hook_close(fd2);
    }

    // ---- 5. relative seek clamps at the synthetic EOF ---------------------
    {
        hook_lseek(fd, 0, SEEK_SET);
        off_t huge = hook_lseek(fd, (off_t)synth.size() + 4096, SEEK_CUR);
        CHECK(huge == (off_t)synth.size(),
              "SEEK_CUR past the synthetic end clamps to synth size");
        CHECK(hook_read(fd, tmp, 16) == 0, "read at clamped EOF returns 0");
    }

    hook_close(fd);
    printf("== sbx_seek_cur_probe: %d check(s), %d failure(s) ==\n", checks, fails);
    return fails ? 1 : 0;
}
