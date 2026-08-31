// ============================================================================
// SandboxID patch untuk Dobby — CodePatch() ramah W^X (Android 15 / API 35).
//
// KENAPA: Dobby yang di-pin (LSPosed/Dobby @ edb2af1, 2021) menulis kode lewat
//   mprotect(PROT_READ|PROT_WRITE|PROT_EXEC) + memcpy TANPA memeriksa nilai
//   balik mprotect. Di Android 15 kernel menegakkan W^X: SELinux menolak
//   permintaan halaman jadi writable+executable (neverallow execmem untuk
//   proses app/isolated). mprotect gagal (EACCES), lalu memcpy tetap menulis
//   ke halaman R-X → SIGSEGV SEGV_ACCERR di dalam __memcpy_aarch64_simd saat
//   lsplant::Init menginstal hook (crash yang dilaporkan pada OPPO/CPH2521,
//   SDK_INT=35). Arena trampoline pun dialokasikan PROT_READ|PROT_EXEC
//   (MemoryArena::AllocateCodeChunk → kReadExecute), jadi tulisan ke arena
//   maupun ke target libart sama-sama kena.
//
// FIX: tulis byte kode lewat /proc/self/mem (pwrite64). Kernel melayani
//   tulisan ini dengan FOLL_FORCE sehingga menembus proteksi halaman R-X
//   tanpa perlu mengubahnya jadi RWX — untuk mapping file-backed (libart)
//   memicu COW, untuk halaman anonim (arena) di-fault-in lalu ditulis. Teknik
//   ini dipakai luas oleh framework hooking modern untuk melewati W^X.
//
//   CodePatch() adalah SATU-SATUNYA choke point tulisan ke memori eksekusi di
//   Dobby (InterceptRouting + AssemblyCodeBuilder + ClosureTrampoline), jadi
//   perbaikan di sini menutup semua jalur.
//
// KREDIT / REFERENSI:
//   - Dobby (upstream)      : https://github.com/jmpews/Dobby (LSPosed fork)
//   - Teknik /proc/self/mem : ShadowHook (bytedance/android-inline-hook),
//                             YAHFA, dan Dobby versi baru memakai pola yang sama
//                             untuk menembus proteksi halaman kode.
//
// File ini disalin menimpa
//   external/dobby/source/UserMode/ExecMemory/code-patch-tool-posix.cc
// oleh jni/fetch_lsplant_deps.sh setelah Dobby di-clone (external/ di-gitignore,
// jadi patch harus disuntik saat fetch agar sampai ke CI).
// ============================================================================

#include "dobby_internal.h"

#include "core/arch/Cpu.h"

#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#if !defined(__APPLE__)

#if defined(__ANDROID__) || defined(__linux__)
// Tulis 'buffer_size' byte dari 'buffer' ke 'address' lewat /proc/self/mem.
// Mengembalikan true bila seluruh byte tertulis. Tidak butuh halaman writable:
// kernel memakai FOLL_FORCE sehingga menembus halaman R-X (COW untuk mapping
// file-backed, fault-in untuk halaman anonim). Aman di kernel W^X Android 15.
static bool sbx_write_via_proc_self_mem(void *address, uint8_t *buffer, uint32_t buffer_size) {
  int fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return false;

  bool ok = true;
  size_t total = 0;
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
  int page_size = (int)sysconf(_SC_PAGESIZE);
  uintptr_t page_align_address = ALIGN_FLOOR(address, page_size);
  int offset = (uintptr_t)address - page_align_address;

#if defined(__ANDROID__) || defined(__linux__)
  bool patched = false;

  // Jalur utama: tulis lewat /proc/self/mem. Portabel lintas versi Android dan
  // tidak pernah butuh halaman writable — inilah yang membuatnya lolos W^X.
  patched = sbx_write_via_proc_self_mem(address, buffer, buffer_size);

  // Fallback: kernel lama / tanpa W^X, atau /proc/self/mem dibatasi kebijakan.
  // Ubah izin halaman jadi RWX, memcpy, lalu kembalikan ke R-X. Nilai balik
  // mprotect diperiksa agar tidak pernah lagi menulis ke halaman non-writable.
  if (!patched) {
    if (mprotect((void *)page_align_address, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
      memcpy((void *)((addr_t)page_align_address + offset), buffer, buffer_size);
      mprotect((void *)page_align_address, page_size, PROT_READ | PROT_EXEC);
      patched = true;
    }
  }

  if (!patched)
    return kMemoryOperationError;
#endif

  addr_t clear_start_ = (addr_t)page_align_address + offset;
  ClearCache((void *)clear_start_, (void *)(clear_start_ + buffer_size));
  return kMemoryOperationSuccess;
}

#endif
