#pragma once
//
// Process-name handling, shared by the module and the companion.
//
// The framework hands a process one of three names:
//
//   com.example.app              an ordinary app process
//   com.example.app:remote       a sub-process of that app
//   com.example.app_zygote       an AppZygote, which the app asked for with
//                                android:zygotePreloadName
//
// Only the first two are apps. An AppZygote is a *zygote*: the app's real
// process is forked FROM it later, and that fork goes through the same
// SpecializeCommon path, so it gets its own pre/postAppSpecialize carrying the
// plain package name. Specializing the AppZygote itself therefore buys nothing
// and costs a great deal - see is_child_zygote() below.
//
// This header is pure: no JNI, no Android headers, no globals. tests/host
// includes it directly so the rules below are pinned by a test rather than by
// a comment.
//
#include <string>

namespace sbxproc {

inline constexpr char kZygoteSuffix[] = "_zygote";
inline constexpr size_t kZygoteSuffixLen = sizeof(kZygoteSuffix) - 1;

// "com.example.app:remote" -> "com.example.app". Anything without a colon is
// returned unchanged. The colon split is unconditional and takes the first
// colon, matching how the framework builds sub-process names.
inline std::string strip_subprocess(const std::string& pkg) {
    const size_t colon = pkg.find(':');
    return colon == std::string::npos ? pkg : pkg.substr(0, colon);
}

// True for the AppZygote naming convention, "<pkg>_zygote". At least one byte
// of package name is required before the suffix, so the bare "_zygote" is not
// read as some package's zygote.
inline bool has_zygote_suffix(const std::string& pkg) {
    const std::string base = strip_subprocess(pkg);
    return base.size() > kZygoteSuffixLen &&
           base.compare(base.size() - kZygoteSuffixLen, kZygoteSuffixLen,
                        kZygoteSuffix) == 0;
}

// The canonical package a process belongs to, for identity and AppLog
// derivation: sub-process and AppZygote markers removed. g_pkg must hold this,
// so that L3, L9 and rotate_ids.sh all seed from the same string.
inline std::string canonical_pkg(const std::string& pkg) {
    std::string base = strip_subprocess(pkg);
    if (has_zygote_suffix(base)) base.resize(base.size() - kZygoteSuffixLen);
    return base;
}

// Is this process a zygote that will go on to fork app processes, rather than
// an app in its own right?
//
// When `have_flag` is true the framework's own is_child_zygote decides, and the
// name is not consulted - the flag is authoritative in both directions. It is
// an *optional* AppSpecializeArgs field, though, so a provider that leaves it
// null must not be read through; the naming convention is then the fallback.
//
// Why this must be refused rather than specialized:
//
//   An AppZygote is a zygote, so every fork it performs runs AOSP's
//   zygote::ForkCommon -> FileDescriptorTable::Restat -> CreateFromFd, which
//   fstat()s every open fd and calls fail_fn("Unable to stat %d") on failure.
//   fail_fn is ZygoteFailure -> env->FatalError() -> SIGABRT. The
//   fds_to_ignore exemption list is built per fork and only ever contains the
//   fds the *framework* knows about, so a companion socket this module opened
//   inside the AppZygote is not exempt from the AppZygote's own later forks.
//   The AppZygote then dies on its next fork. Observed on device as:
//
//     Abort message: 'JNI FatalError called: (zygote) Unable to stat 120'
//     Cmdline: com.shopee.id_zygote
//     #06 FileDescriptorInfo::CreateFromFd
//     #07 FileDescriptorTable::RestatInternal
//     #08 FileDescriptorTable::Restat
//     #09 zygote::ForkCommon
//     #10 zygote::forkApp
//
//   where 120 was the fd connectCompanion() had just returned in that pid.
//   Every AppZygote that was specialized this way crashed; every one that was
//   skipped did not.
inline bool is_child_zygote(bool have_flag, bool flag_value, const std::string& pkg) {
    if (have_flag) return flag_value;
    return has_zygote_suffix(pkg);
}

}  // namespace sbxproc
