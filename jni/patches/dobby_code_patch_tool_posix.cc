// ============================================================================
// SandboxID patch untuk Dobby — CodePatch() ramah W^X (Android 15 / API 35).
//
// TIGA crash berurutan yang ditangani file ini:
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
// [CRASH #3 — EXECUTE fault, SAMA dengan #2, TAPI fix #2 sudah ter-ship]  Pada
//   modul v2.1.19 (device google/tegu-spoof, kernel custom A15, proses usap64)
//   crash #2 MUNCUL LAGI di SetRuntimeDebugState+0 walau .so terbukti memuat fix
//   #2. Artinya "tulis /proc/self/mem lalu restore exec" TIDAK cukup: di kernel
//   ketat ini, halaman .text yang di-COW oleh FOLL_FORCE tampaknya tak bisa
//   dikembalikan executable. Dobby upstream (LSPosed & JingMatrix) justru TIDAK
//   memakai /proc/self/mem — DobbyCodePatch cukup mprotect(RWX)→memcpy→mprotect
//   (RX) dan itu jalan di mayoritas A15. Maka akar masalah #3 = jalur
//   /proc/self/mem yang kita dahulukan. PERBAIKAN #3: balik urutan jalur —
//   dahulukan mprotect(R|W)→memcpy→mprotect(R|X) (W^X-safe, halaman di-COW selagi
//   VMA writable → boleh balik R|X), /proc/self/mem hanya fallback.
//
// FIX (urutan jalur DIBALIK sejak crash #3 — mprotect diutamakan, /proc/self/mem
//      diturunkan jadi fallback karena JUSTRU memicu crash #3):
//   (Jalur 1, utama) mprotect(R|W) → memcpy → mprotect(R|X). Pola JIT standar,
//     sejalan Dobby upstream (DobbyCodePatch) TAPI tanpa W+X simultan → aman W^X.
//     Halaman di-COW selagi VMA-nya writable (R|W) sehingga kernel memperlakukan
//     sebagai halaman data biasa; transisi balik ke R|X dipatuhi — TIDAK
//     meninggalkan halaman COW non-exec seperti jalur /proc/self/mem.
//   (Jalur 2, fallback) Tulis via /proc/self/mem (pwrite64, FOLL_FORCE menembus
//     halaman R-X) untuk .text execute-only/tersegel yang menolak +W, lalu PAKSA
//     pulihkan exec mprotect(R) → mprotect(R|X). CATATAN: jalur inilah yang
//     memicu crash #3 (COW men-dirty selagi VMA r-x → kernel ketat mencabut
//     PROT_EXEC permanen), maka kini hanya dipakai bila Jalur 1 gagal total.
//   (Jalur 3, fallback terakhir) mprotect(RWX) → memcpy → mprotect(R|X): paritas
//     Dobby upstream, hanya berhasil di kernel tanpa W^X. Juga jalur POSIX
//     non-Linux tanpa /proc/self/mem.
//   Bila SEMUA jalur gagal, fungsi mengembalikan kMemoryOperationError (hook
//   gagal rapi, bukan meng-crash proses). Nilai balik mprotect + errno dicatat ke
//   logcat (tag "SandboxID-Dobby") — JANGAN filter tag ini saat menangkap logcat,
//   atau diagnosis jalur & W^X device tak akan terlihat di log.
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
  // -- Jalur 1 (utama): mprotect(R|W) → memcpy → mprotect(R|X). --
  // Pola JIT standar, sejalan dengan Dobby upstream tetapi TANPA meminta W+X
  // simultan (RWX), jadi aman di kernel W^X. Halaman di-COW selagi VMA-nya
  // writable (R|W) sehingga kernel memperlakukannya sebagai halaman data biasa;
  // transisi balik ke R|X dipatuhi. Berbeda dari tulis via /proc/self/mem
  // (FOLL_FORCE) yang men-dirty halaman selagi VMA masih r-x → sebagian kernel
  // ketat (custom A15) MENCABUT PROT_EXEC halaman COW itu permanen → fungsi
  // se-halaman yang DIPANGGIL langsung LSPlant (SetRuntimeDebugState) execute-
  // fault. Karena itu jalur mprotect kini DIDAHULUKAN (akar masalah crash #3).
  if (mprotect((void *)start_page, range_len, PROT_READ | PROT_WRITE) == 0) {
    memcpy(address, buffer, buffer_size);
    if (mprotect((void *)start_page, range_len, PROT_READ | PROT_EXEC) == 0) {
      patched = true;
      static bool sbx_logged_rw = false;
      if (!sbx_logged_rw) {
        sbx_logged_rw = true;
        SBX_DOBBY_LOGI("CodePatch: jalur mprotect(R|W→R|X) aktif (page_size=%zu)", page_size);
      }
    } else {
      SBX_DOBBY_LOGE("CodePatch: R|X pasca-R|W gagal @%p len=%zu (errno=%d %s)",
                     (void *)start_page, range_len, errno, strerror(errno));
    }
  } else {
    SBX_DOBBY_LOGE("CodePatch: mprotect R|W gagal @%p len=%zu (errno=%d %s) — coba /proc/self/mem",
                   (void *)start_page, range_len, errno, strerror(errno));
  }

  // -- Jalur 2 (fallback): /proc/self/mem (pwrite64) + PAKSA pulihkan exec. --
  // Untuk .text yang tak boleh ditambah PROT_WRITE (execute-only / tersegel) di
  // mana mprotect(R|W) di Jalur 1 ditolak. FOLL_FORCE menembus halaman R-X. Lalu
  // paksa PTE ditulis ulang R → R|X (mprotect ke flag sama di-bypass kernel bila
  // newflags==oldflags). CATATAN: jalur inilah yang memicu crash #3 di kernel
  // ketat (COW-strip-exec), jadi SENGAJA hanya dipakai bila Jalur 1 gagal total.
  if (!patched) {
    if (sbx_write_via_proc_self_mem(address, buffer, buffer_size)) {
      if (mprotect((void *)start_page, range_len, PROT_READ) == 0 &&
          mprotect((void *)start_page, range_len, PROT_READ | PROT_EXEC) == 0) {
        patched = true;
        SBX_DOBBY_LOGI("CodePatch: jalur /proc/self/mem + restore-exec dipakai @%p len=%zu",
                       (void *)start_page, range_len);
      } else {
        SBX_DOBBY_LOGE("CodePatch: pulihkan exec pasca-/proc/self/mem gagal @%p len=%zu (errno=%d %s)",
                       (void *)start_page, range_len, errno, strerror(errno));
        if (mprotect((void *)start_page, range_len, PROT_READ | PROT_EXEC) == 0) {
          patched = true;
        }
      }
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
