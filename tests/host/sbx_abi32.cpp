// Compile-only check that jni/module_hooks.cpp builds for a 32-bit bionic target.
//
// The module ships four ABIs, but the host suites only ever compile it against
// the host's own 64-bit glibc headers. A syscall-number macro that exists there
// and not in bionic's 32-bit headers therefore compiles locally and breaks the
// release — which is what happened with __NR_newfstatat on armeabi-v7a.
//
// This TU is compiled twice by run.sh: once normally, and once with
// -include tests/host/abi32_bionic.h, which reproduces the 32-bit preprocessor
// state (no __NR_newfstatat, __NR_fstatat64 = 327). Both must compile.
//
// It is compiled with -fsyntax-only and never linked: the failure being guarded
// against is a compile error, and linking would drag in the whole Zygisk/JNI
// surface that the other suites stub out for reasons unrelated to ABI naming.
#include "module_hooks.cpp"

#include <cstdio>

// Touch the resolved macro so an empty or zero definition cannot slip through as
// "compiled fine". The value differs per ABI (79 on x86_64, 327 on ARM 32-bit),
// so the assertion is only that it is a positive syscall number.
static_assert(SBX_NR_FSTATAT > 0, "fstatat syscall number must be positive");

// Referencing the hooks proves their fallback branch was compiled, not skipped.
// A volatile sink keeps the reference from being optimised away; comparing the
// addresses to null would always be true and prove nothing.
static void* (*volatile sbx_abi32_sink)(void*) = nullptr;
static void sbx_abi32_keep_hooks() {
    sbx_abi32_sink = reinterpret_cast<void* (*)(void*)>(
            reinterpret_cast<void*>(&hook_fstatat));
}

// Never called — the check is that this TU compiles at all.
void sbx_abi32_report() {
    sbx_abi32_keep_hooks();
    std::printf("ok   SBX_NR_FSTATAT resolves to %d on this ABI\n", SBX_NR_FSTATAT);
}
