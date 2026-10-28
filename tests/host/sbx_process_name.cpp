// Pins the process-name rules that decide whether a process is an app the
// module may specialize, or a child zygote it must refuse.
//
// This is a pure header test: jni/sbx_process_name.hpp has no JNI, no Android
// headers and no globals, so it compiles on any host. The module and the
// companion both include it, which is the point - the two used to carry
// separate copies of the "_zygote" stripping, and the companion's copy is what
// made an AppZygote look like a target.
//
// Why this is worth a test rather than a comment: getting it wrong is not a
// cosmetic bug. An AppZygote that is specialized opens a companion socket, and
// AOSP's zygote::ForkCommon -> FileDescriptorTable::Restat then fstat()s every
// open fd on the AppZygote's next fork and aborts it through ZygoteFailure when
// one fails. Observed on device as
//
//   Abort message: 'JNI FatalError called: (zygote) Unable to stat 120'
//   Cmdline: com.shopee.id_zygote
//   FileDescriptorInfo::CreateFromFd <- RestatInternal <- Restat
//     <- zygote::ForkCommon <- zygote::forkApp
//
// with 120 being the fd connectCompanion() had just returned. Every AppZygote
// that was specialized this way crashed; every one that was skipped did not.

#include "sbx_process_name.hpp"

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(got, want)                                                \
    do {                                                                   \
        const std::string g_ = (got), w_ = (want);                         \
        if (g_ != w_) {                                                    \
            std::printf("  FAIL %s:%d  got '%s', want '%s'\n",             \
                        __FILE__, __LINE__, g_.c_str(), w_.c_str());       \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

int main() {
    using namespace sbxproc;

    std::printf("==> strip_subprocess\n");
    CHECK_EQ(strip_subprocess("com.example.app"), "com.example.app");
    CHECK_EQ(strip_subprocess("com.example.app:remote"), "com.example.app");
    CHECK_EQ(strip_subprocess("com.example.app:push"), "com.example.app");
    CHECK_EQ(strip_subprocess("com.example.app:isolated"), "com.example.app");
    // First colon wins: the framework never nests these, but taking the last
    // would silently invent a package name for a malformed one.
    CHECK_EQ(strip_subprocess("a:b:c"), "a");
    CHECK_EQ(strip_subprocess(""), "");
    CHECK_EQ(strip_subprocess(":leading"), "");

    std::printf("==> has_zygote_suffix\n");
    CHECK(has_zygote_suffix("com.shopee.id_zygote"));
    CHECK(has_zygote_suffix("com.android.chrome_zygote"));
    CHECK(has_zygote_suffix("webview_zygote"));
    CHECK(has_zygote_suffix("com.instagram.android_zygote"));
    // A sub-process of an AppZygote is still an AppZygote for our purposes.
    CHECK(has_zygote_suffix("com.shopee.id_zygote:something"));

    CHECK(!has_zygote_suffix("com.shopee.id"));
    CHECK(!has_zygote_suffix("com.example.app:remote"));
    // The suffix must not be matched as a bare prefix or infix, and a package
    // that merely mentions it is not a zygote.
    CHECK(!has_zygote_suffix("com.example.zygote"));
    CHECK(!has_zygote_suffix("com.example_zygoteX"));
    // A sub-process of an AppZygote is still refused: the sub-process marker is
    // stripped first, so the zygote suffix is seen. The framework does not name
    // app sub-processes this way, so refusing is the conservative read.
    CHECK(has_zygote_suffix("com.example_zygote:remote"));
    // No package name at all before the suffix: refuse to read "_zygote" as
    // some package's zygote.
    CHECK(!has_zygote_suffix("_zygote"));
    CHECK(!has_zygote_suffix(""));
    // A package genuinely named "...zygote" (no underscore) is an app.
    CHECK(!has_zygote_suffix("com.example.zygoteapp"));

    std::printf("==> canonical_pkg\n");
    CHECK_EQ(canonical_pkg("com.shopee.id"), "com.shopee.id");
    CHECK_EQ(canonical_pkg("com.shopee.id:remote"), "com.shopee.id");
    CHECK_EQ(canonical_pkg("com.shopee.id_zygote"), "com.shopee.id");
    CHECK_EQ(canonical_pkg("com.shopee.id_zygote:remote"), "com.shopee.id");
    // Unchanged when there is nothing to strip.
    CHECK_EQ(canonical_pkg("com.example.app"), "com.example.app");
    CHECK_EQ(canonical_pkg(""), "");

    std::printf("==> is_child_zygote: the framework flag is authoritative\n");
    // Flag present and true: refuse, whatever the name says. A provider that
    // reports this is telling us the process will fork apps.
    CHECK(is_child_zygote(true, true, "com.example.app"));
    CHECK(is_child_zygote(true, true, "com.shopee.id_zygote"));
    // Flag present and false: specialize, even under a zygote-looking name.
    // Trusting the name over a present flag would refuse a real app.
    CHECK(!is_child_zygote(true, false, "com.shopee.id_zygote"));
    CHECK(!is_child_zygote(true, false, "com.example.app"));

    std::printf("==> is_child_zygote: the name is the fallback\n");
    // Flag absent (a provider that leaves the optional field null): fall back
    // to the naming convention, which is what actually caught this on device.
    CHECK(is_child_zygote(false, false, "com.shopee.id_zygote"));
    CHECK(is_child_zygote(false, false, "com.android.chrome_zygote"));
    CHECK(!is_child_zygote(false, false, "com.shopee.id"));
    CHECK(!is_child_zygote(false, false, "com.example.app:remote"));
    // A false flag_value with no flag must not be read as "not a zygote".
    CHECK(is_child_zygote(false, true, "com.shopee.id_zygote"));

    std::printf("==> the exact device case\n");
    // The regression: com.shopee.id_zygote matched target com.shopee.id via the
    // suffix strip, was specialized, and aborted on its next fork.
    {
        const std::string pkg = "com.shopee.id_zygote";
        CHECK(has_zygote_suffix(pkg));
        // Both the module (flag absent on the provider that was live) and the
        // companion must now refuse it.
        CHECK(is_child_zygote(false, false, pkg));
        // ...while the app's own processes are still served.
        CHECK(!is_child_zygote(false, false, "com.shopee.id"));
        CHECK(!is_child_zygote(false, false, "com.shopee.id:push"));
        // And the canonical name a sub-process derives still matches the target
        // the companion loaded from target.txt.
        CHECK_EQ(canonical_pkg("com.shopee.id:push"), "com.shopee.id");
    }

    if (failures == 0) std::printf("all checks passed\n");
    else std::printf("%d check(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
