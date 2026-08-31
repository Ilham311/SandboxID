// ============================================================================
// SandboxID patch untuk Dobby — CodePatch() ramah W^X (Android / API 35+).
//
// Tulisan kode ke .text libart dilakukan lewat /proc/self/mem (pwrite64). Kernel
// memakai FOLL_FORCE sehingga byte tertulis menembus halaman r-x TANPA mengubah
// proteksi VMA: halaman tetap executable pasca-COW. Inilah kuncinya — mprotect
// TIDAK dipakai di jalur utama. Menambah PROT_WRITE/PROT_EXEC pada halaman
// file-backed libart yang sudah di-COW, dari domain SELinux aplikasi
// (untrusted_app), butuh izin execmem/execmod yang di banyak ROM Android 15/16
// DITOLAK; bila ditolak, halaman .text tertinggal non-exec dan fungsi di halaman
// itu — mis. art::Runtime::SetRuntimeDebugState yang DIPANGGIL lsplant::Init —
// crash SEGV_ACCERR saat dieksekusi. /proc/self/mem menghindari mprotect total.
// Teknik yang sama dipakai Frida/YAHFA/HookZz untuk kasus W^X ini.
//
// Fallback mprotect(RWX)->memcpy->restore HANYA dipakai bila /proc/self/mem tak
// tersedia (POSIX non-Linux) atau gagal; nilai balik mprotect diperiksa sehingga
// memcpy tak pernah menulis ke halaman yang gagal dijadikan writable (crash #1).
//
// Log memakai tag "SandboxID" (BUKAN sub-tag terpisah) agar ikut tertangkap oleh
// capture logcat modul yang sudah mem-filter "SandboxID".
//
// Disalin menimpa external/dobby/source/UserMode/ExecMemory/code-patch-tool-posix.cc
// oleh jni/fetch_lsplant_deps.sh (external/ di-gitignore; patch disuntik saat fetch).
// ============================================================================

#include "dobby_internal.h"

#include "core/arch/Cpu.h"

#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define SBX_DOBBY_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "SandboxID", __VA_ARGS__)
#define SBX_DOBBY_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SandboxID", __VA_ARGS__)
#else
#define SBX_DOBBY_LOGE(...) ((void)0)
#define SBX_DOBBY_LOGI(...) ((void)0)
#endif

#if !defined(__APPLE__)

#if defined(__ANDROID__) || defined(__linux__)
// Tulis buffer_size byte ke address lewat /proc/self/mem. Tidak butuh halaman
// writable dan TIDAK mengubah proteksi VMA: halaman tetap executable pasca-COW.
static bool sbx_write_via_proc_self_mem(void *address, uint8_t *buffer, uint32_t buffer_size) {
  int fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return false;

  size_t total = 0;
  bool ok = true;
  while (total < buffer_size) {
    ssize_t n = pwrite64(fd, buffer + total, (size_t)buffer_size - total,
                         (off64_t)((uintptr_t)address + total));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ok = false;
      break;
    }
    if (n == 0) {
      ok = false;
      break;
    }
    total += (size_t)n;
  }

  close(fd);
  return ok && total == (size_t)buffer_size;
}
#endif

PUBLIC MemoryOperationError CodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {
  bool patched = false;

#if defined(__ANDROID__) || defined(__linux__)
  // Jalur utama: /proc/self/mem, TANPA mprotect — proteksi halaman tak berubah,
  // .text tetap r-x. Ini yang menjaga SetRuntimeDebugState tetap executable.
  patched = sbx_write_via_proc_self_mem(address, buffer, buffer_size);
  if (patched) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      SBX_DOBBY_LOGI("CodePatch: jalur /proc/self/mem aktif (tanpa mprotect PROT_EXEC)");
    }
  } else {
    static bool logged_fail = false;
    if (!logged_fail) {
      logged_fail = true;
      SBX_DOBBY_LOGE("CodePatch: /proc/self/mem gagal @%p size=%u (errno=%d %s) — fallback mprotect",
                     address, buffer_size, errno, strerror(errno));
    }
  }
#endif

  // Fallback: mprotect(RWX) -> memcpy -> restore R|X. Hanya bila /proc/self/mem
  // gagal atau POSIX non-Linux. memcpy HANYA jalan bila mprotect sukses (nilai
  // balik diperiksa) agar tidak pernah menulis ke halaman non-writable (crash #1).
  // Semua halaman yang dilintasi buffer ditangani (patch bisa lintas-halaman).
  if (!patched) {
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t addr = (uintptr_t)address;
    uintptr_t start_page = ALIGN_FLOOR(addr, page_size);
    uintptr_t end_page = ALIGN_FLOOR(addr + (buffer_size > 0 ? buffer_size - 1 : 0), page_size);
    size_t range_len = (end_page - start_page) + page_size;

    if (mprotect((void *)start_page, range_len, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
      memcpy(address, buffer, buffer_size);
      // Restore best-effort; bila gagal halaman tetap RWX (masih executable).
      mprotect((void *)start_page, range_len, PROT_READ | PROT_EXEC);
      patched = true;
      static bool logged_fallback_ok = false;
      if (!logged_fallback_ok) {
        logged_fallback_ok = true;
        SBX_DOBBY_LOGI("CodePatch: jalur fallback mprotect(RWX) dipakai @%p len=%zu",
                       (void *)start_page, range_len);
      }
    } else {
      static bool logged_fallback_fail = false;
      if (!logged_fallback_fail) {
        logged_fallback_fail = true;
        SBX_DOBBY_LOGE("CodePatch: mprotect(RWX) gagal @%p len=%zu (errno=%d %s) — hook dibatalkan",
                       (void *)start_page, range_len, errno, strerror(errno));
      }
    }
  }

  if (!patched)
    return kMemoryOperationError;

  ClearCache(address, (void *)((uintptr_t)address + buffer_size));
  return kMemoryOperationSuccess;
}

#endif
