// ============================================================================
// SandboxID patch untuk Dobby — CodePatch() ramah W^X (Android 15 / API 35).
//
// DUA crash berurutan yang ditangani file ini:
//
// [CRASH #1 — WRITE fault di memcpy]  Dobby yang di-pin (LSPosed/Dobby @ edb2af1,
//   2021) menulis kode lewat mprotect(PROT_READ|PROT_WRITE|PROT_EXEC) + memcpy
//   TANPA memeriksa nilai balik mprotect. Di Android 15 kernel menegakkan W^X:
//   permintaan W+X SIMULTAN (RWX) ditolak, mprotect gagal (EACCES/EPERM), lalu
//   memcpy tetap menulis ke halaman R-X → SIGSEGV SEGV_ACCERR di dalam
//   __memcpy_aarch64_simd saat lsplant::Init memasang hook (OPPO/CPH2521, SDK 35).
//
// [CRASH #2 — EXECUTE fault]  Setelah tulisan dialihkan ke /proc/self/mem
//   (menembus write-fault #1), muncul SEGV_ACCERR EXECUTE (pc == fault addr) di
//   art::Runtime::SetRuntimeDebugState+0. Jalur /proc/self/mem TIDAK memanggil
//   mprotect sama sekali, jadi hilangnya exec pasti berasal dari COW: tulisan
//   men-dirty halaman .text libart (MAP_PRIVATE file-backed), dan kernel W^X
//   device ini MENCABUT PROT_EXEC dari halaman yang baru di-COW/dirty. LSPlant
//   MEMANGGIL (bukan meng-hook) SetRuntimeDebugState saat mendeteksi
//   debug_state_offset (art/runtime/runtime.hpp: probe pada fake_runtime); bila
//   fungsi itu se-halaman 4KB dengan salah satu fungsi yang di-hook (ClassLinker/
//   Instrumentation), halamannya ikut ter-COW → non-executable → execute-fault.
//   Gejala identik dilaporkan di bytedance/android-inline-hook#96 ("PROT_EXEC
//   asli suatu alamat terhapus → crash runtime").
//
// FIX:
//   (Jalur 1, utama) Tulis byte kode lewat /proc/self/mem (pwrite64). Kernel
//     melayani tulisan ini dengan FOLL_FORCE sehingga menembus halaman R-X tanpa
//     mengubahnya jadi W+X, dan TIDAK meminta izin SELinux execmem/execmod.
//     Lalu PULIHKAN exec: paksa PTE ditulis ulang dengan mprotect(R) → mprotect
//     (R|X). Perlu drop-lalu-restore karena mprotect(R|X) langsung akan no-op
//     saat flag VMA sudah == R|X (kernel mem-bypass PTE-walk bila
//     newflags==oldflags), sehingga bit exec PTE yang tercabut tak tertulis
//     ulang. Kita TAK PERNAH minta W+X (aman W^X) dan berakhir di R|X.
//   (Jalur 2, fallback) Dua-langkah mprotect(R|W) → memcpy → mprotect(R|X):
//     untuk kernel yang membatasi /proc/self/mem tapi mengizinkan penulisan via
//     mprotect. Tetap tak pernah minta W+X simultan (beda dari RWX Dobby lama).
//   (Jalur 3, fallback terakhir) mprotect(RWX) → memcpy → mprotect(R|X): paritas
//     Dobby upstream, hanya berhasil di kernel tanpa W^X / dengan sepolicy
//     execmem termuat. Juga jalur untuk POSIX non-Linux tanpa /proc/self/mem.
//   Bila SEMUA jalur gagal, fungsi mengembalikan kMemoryOperationError (bukan
//   menulis paksa) sehingga hook gagal rapi, bukan meng-crash proses. Nilai balik
//   mprotect + errno dicatat ke logcat (tag "SandboxID-Dobby") agar mekanisme
//   W^X device dapat dipastikan dari log berikutnya.
//
//   Ukuran halaman diambil dinamis dari sysconf(_SC_PAGESIZE) (aman untuk device
//   16KB seperti sebagian ColorOS A15) dan SEMUA halaman yang dilintasi patch
//   ditangani (paritas dengan perbaikan cross-page Dobby upstream).
//
//   CodePatch() adalah SATU-SATUNYA choke point tulisan ke memori eksekusi di
//   Dobby (InterceptRouting + AssemblyCodeBuilder + ClosureTrampoline), jadi
//   perbaikan di sini menutup semua jalur.
//
// KREDIT / REFERENSI:
//   - Dobby (upstream)      : https://github.com/jmpews/Dobby (fork LSPosed &
//                             JingMatrix; DobbyCodePatch = mprotect(RWX)+restore).
//   - Pola "jaga PROT_EXEC" : bytedance/android-inline-hook (ShadowHook) issue
//                             #96 — jangan drop PROT_EXEC saat re-protect.
//   - memfd dual-map (RW+RX): JingMatrix/LSPlant CreateDualMapping (referensi
//                             teknik W^X untuk memori milik-sendiri).
//   - Crash & akar masalah  : JingMatrix/Vector#559 & JingMatrix/LSPosed#560
//                             (SEGV_ACCERR memcpy, W^X/execmem); LSPosed/LSPlant
//                             #23 (SetJavaDebuggable→SetRuntimeDebugState A14+).
//   - Teknik /proc/self/mem : ShadowHook, YAHFA, HookZz.
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

#if defined(__ANDROID__)
#include <android/log.h>
#define SBX_DOBBY_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "SandboxID-Dobby", __VA_ARGS__)
#define SBX_DOBBY_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SandboxID-Dobby", __VA_ARGS__)
#else
#define SBX_DOBBY_LOGE(...) ((void)0)
#define SBX_DOBBY_LOGI(...) ((void)0)
#endif

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
  size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  uintptr_t addr = (uintptr_t)address;
  uintptr_t start_page = ALIGN_FLOOR(addr, page_size);
  uintptr_t end_page = ALIGN_FLOOR(addr + buffer_size - 1, page_size);
  size_t range_len = (end_page - start_page) + page_size;

  bool patched = false;

#if defined(__ANDROID__) || defined(__linux__)
  // -- Jalur 1 (utama): tulis via /proc/self/mem, lalu PULIHKAN exec. --
  // Tulisan men-dirty (COW) halaman .text; di kernel W^X ketat halaman COW bisa
  // kehilangan PROT_EXEC → fungsi se-halaman gagal dieksekusi (SetRuntimeDebug-
  // State execute-fault). VMA tetap r-x sepanjang jalur ini (tak pernah kita
  // jadikan writable), jadi mprotect(R|X) di bawah BUKAN "exec gain on writable".
  // Paksa PTE ditulis ulang lewat R → R|X karena mprotect ke flag yang sama
  // (R|X == R|X) di-bypass kernel (newflags==oldflags) sehingga bit exec tak
  // dipulihkan.
  if (sbx_write_via_proc_self_mem(address, buffer, buffer_size)) {
    mprotect((void *)start_page, range_len, PROT_READ);
    if (mprotect((void *)start_page, range_len, PROT_READ | PROT_EXEC) == 0) {
      patched = true;
      static bool sbx_logged_once = false;
      if (!sbx_logged_once) {
        sbx_logged_once = true;
        SBX_DOBBY_LOGI("CodePatch: jalur /proc/self/mem + restore-exec aktif (page_size=%zu)",
                       page_size);
      }
    } else {
      SBX_DOBBY_LOGE("CodePatch: pulihkan exec gagal @%p len=%zu (errno=%d %s)",
                     (void *)start_page, range_len, errno, strerror(errno));
    }
  }

  // -- Jalur 2 (fallback): dua-langkah mprotect W^X-safe (R|W → R|X). --
  if (!patched) {
    if (mprotect((void *)start_page, range_len, PROT_READ | PROT_WRITE) == 0) {
      memcpy(address, buffer, buffer_size);
      if (mprotect((void *)start_page, range_len, PROT_READ | PROT_EXEC) == 0) {
        patched = true;
        SBX_DOBBY_LOGI("CodePatch: fallback dua-langkah (R|W→R|X) dipakai");
      } else {
        SBX_DOBBY_LOGE("CodePatch: R|X pasca-R|W gagal @%p (errno=%d %s)",
                       (void *)start_page, errno, strerror(errno));
      }
    } else {
      SBX_DOBBY_LOGE("CodePatch: mprotect R|W gagal @%p (errno=%d %s)",
                     (void *)start_page, errno, strerror(errno));
    }
  }
#endif
  // -- Jalur 3 (fallback terakhir): RWX → memcpy → R|X (kernel tanpa W^X). --
  // Paritas Dobby upstream; hanya berhasil bila W+X simultan diizinkan. Juga
  // satu-satunya jalur pada POSIX non-Linux/Android (tanpa /proc/self/mem).
  if (!patched) {
    if (mprotect((void *)start_page, range_len, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
      memcpy(address, buffer, buffer_size);
      mprotect((void *)start_page, range_len, PROT_READ | PROT_EXEC);
      patched = true;
    }
  }

  if (!patched) {
    SBX_DOBBY_LOGE("CodePatch: SEMUA jalur gagal @%p size=%u — hook dibatalkan",
                   address, buffer_size);
    return kMemoryOperationError;
  }

  ClearCache(address, (void *)(addr + buffer_size));
  return kMemoryOperationSuccess;
}

#endif
