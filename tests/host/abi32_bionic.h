/*
 * Forces the preprocessor state of a 32-bit bionic target onto a 64-bit host.
 *
 * Why this exists: every host suite compiles jni/module_hooks.cpp with the
 * host's own glibc headers, which define __NR_newfstatat. The module also ships
 * armeabi-v7a and x86, where bionic defines only __NR_fstatat64 — so a use of
 * __NR_newfstatat that is correct on the host is a hard compile error on two of
 * the four ABIs the release builds. That is exactly how it shipped once: host
 * tests passed, CI's armeabi-v7a step died with "use of undeclared identifier
 * '__NR_newfstatat'", and nothing in the local suite could have caught it.
 *
 * Included with -include so it runs BEFORE module_hooks.cpp's own
 * <sys/syscall.h>. Including that header here sets its include guard, so the
 * later include is a no-op and the undef below survives. 327 is ARM's
 * __NR_fstatat64, read from the device's own libc.so (fstatat64 at 0x847dc
 * does `movw r7, #0x147`).
 */
#include <sys/syscall.h>

#undef __NR_newfstatat
#define __NR_fstatat64 327
