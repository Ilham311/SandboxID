#pragma once
// pwrite64/off64_t butuh _GNU_SOURCE / _LARGEFILE64_SOURCE di glibc (dipakai oleh
// tools/validate.sh saat men-syntax-check L3 di host Linux). No-op di bionic
// (Android NDK) yang selalu mendeklarasikannya. Harus sebelum header sistem apa pun.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#ifndef SBX_LSP_TAG
#define SBX_LSP_TAG "SandboxID"
#endif
#define SBX_LSP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SBX_LSP_TAG, __VA_ARGS__)
#define SBX_LSP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, SBX_LSP_TAG, __VA_ARGS__)
#ifdef SBX_DEBUG
#define SBX_LSP_LOGD(...) __android_log_print(ANDROID_LOG_INFO, SBX_LSP_TAG, "[D] " __VA_ARGS__)
#else
#define SBX_LSP_LOGD(...) ((void)0)
#endif

#ifdef SBX_ENABLE_LSPLANT

#include <dobby.h>
#include <lsplant.hpp>
#include <lsparself.hpp>
#include "sbx_ident_synth.hpp"
#include "sbx_native_drm.hpp"
// Header POSIX untuk preflight kapabilitas code-patch (mmap/mprotect/pwrite64/
// sinyal). Hanya diperlukan pada jalur L3. Pakai bentuk C <signal.h>/<setjmp.h>
// karena sigaction/sigsetjmp/siglongjmp adalah POSIX (global namespace), bukan
// std:: — <csignal>/<csetjmp> tak menjaminnya.
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#if __has_include("hook_dex.h")
#include "hook_dex.h"
#define SBX_HAVE_HOOK_DEX 1
#endif
#endif

namespace sbxlsp {

struct HookValues {
    std::string android_id;
    std::string serial;
    std::string wifi_mac;
    std::string bt_addr;
    std::string op_num;
    std::string op_alpha;
    std::string op_iso;
    std::string carrier_id;
    std::string gaid;
    std::string app_set_id;
    std::string model;           // untuk BluetoothAdapter.getName()
    std::string self_pkg;        // paket aplikasi sendiri (target waktu instal)
    std::string build_time_utc;  // detik epoch build (jangkar waktu instal)
    uint64_t    seed = 0;
    bool gms_watch = false;

};

#ifndef SBX_ENABLE_LSPLANT

inline bool available()                              { return false; }
inline bool codepatch_capable()                      { return false; }
inline bool init(JNIEnv*)                            { return false; }
inline bool install_all(JNIEnv*, const HookValues&)  { return false; }

#else

inline bool available() { return true; }

inline void* sbx_inline_hooker(void* target, void* hooker) {
    void* origin = nullptr;
    if (DobbyHook(target, hooker, &origin) == 0) return origin;
    return nullptr;
}
inline bool sbx_inline_unhooker(void* func) { return DobbyDestroy(func) == 0; }

// ── Preflight kapabilitas inline-hook (W^X / SELinux) ───────────────────────
// Inline-hook L3 butuh DUA kapabilitas SELinux yang BERBEDA; keduanya harus ada
// sebelum lsplant::Init menyentuh libart hidup, atau proses crash. Riset 20 repo
// (Frida/YAHFA/ShadowHook/Dobby/LSPlant/LSPosed) mengkonfirmasi pemisahan ini:
//
//   (1) execmod  — menulis byte ke halaman .text libart file-backed r-x LALU
//                  halaman itu tetap executable (COW private page). Dipakai
//                  CodePatch untuk menanam jump ke fungsi target di libart.
//   (2) execmem  — memori ANONIM yang bisa ditulis-kode lalu dieksekusi. Dipakai
//                  DUA situs: arena trampolin Dobby (mmap RX anon + tulis) DAN
//                  trampolin per-metode LSPlant (mmap RWX anon, lsplant.cc:539).
//
// Perangkat W^X keras (untrusted_app A15/16) sering mengizinkan (1) tapi MENOLAK
// (2): `self:process execmem` tak diberikan → mmap/mprotect anon-exec = EACCES.
// Probe LAMA hanya menguji (1) di salinan libart file-backed → LOLOS PALSU →
// lsplant::Init lanjut → salah satu langkah InitNative menanam hook yang butuh
// trampolin anon yang tak bisa dialokasikan → cabang rusak → saat langkah
// TERAKHIR memanggil art::Runtime::SetRuntimeDebugState (runtime.hpp:85) →
// SIGSEGV SEGV_ACCERR EXECUTE di +0. Itulah crash berulang yang diadukan user.
//
// Perbaikan: uji KEDUA kelas. Semua uji di halaman throwaway (salinan libart
// MAP_PRIVATE untuk execmod; mmap anon untuk execmem) — TAK PERNAH menyentuh
// libart hidup, dan eksekusi 'ret' selalu di bawah guard SIGSEGV/SIGBUS/SIGILL
// sehingga probe sendiri tak akan meng-crash host. Verifikasi exec: cek 'x' di
// /proc/self/maps DAN (arm64/x86) benar-benar EKSEKUSI sebyte 'ret'. Hasil di-
// cache. lsplant::Init akhirnya dibungkus guard sinyal sebagai jaring terakhir.
// Ref: AOSP jit_memory_region.cc (memfd dual-map), ShadowHook #111 (mmap sukses
// tapi non-exec), offlinemark "obscure quirk of /proc", CVE-2022-50014.
#if defined(__aarch64__)
#  define SBX_LSP_RET_BYTES {0xC0, 0x03, 0x5F, 0xD6}   // ret
#  define SBX_LSP_HAVE_EXEC_PROBE 1
#elif defined(__x86_64__) || defined(__i386__)
#  define SBX_LSP_RET_BYTES {0xC3}                      // ret
#  define SBX_LSP_HAVE_EXEC_PROBE 1
#elif defined(__arm__)
#  define SBX_LSP_RET_BYTES {0x1E, 0xFF, 0x2F, 0xE1}    // bx lr (ARM)
#else
#  define SBX_LSP_RET_BYTES {0x00}                      // arch lain: cek maps saja
#endif

inline sigjmp_buf              sbx_probe_jmp;
inline volatile sig_atomic_t   sbx_probe_fault = 0;
inline void sbx_probe_sig_handler(int) { sbx_probe_fault = 1; siglongjmp(sbx_probe_jmp, 1); }

// Serialisasi akses ke handler sinyal proses-lebar dipakai sbx_can_execute —
// mencegah thread lain memasang/melepas handler SIGSEGV/SIGBUS/SIGILL secara
// bersamaan selagi probe berjalan (mis. crash handler pihak ketiga).
inline std::mutex sbx_probe_mutex;

// Coba eksekusi fn (leaf 'ret') di bawah guard sinyal; true bila kembali normal.
// Menjaga SIGSEGV, SIGBUS, DAN SIGILL — halaman throwaway hasil COW/mmap bisa
// memicu ketiganya (instruksi tak valid pada level-PTE juga observasinya SIGILL
// di sejumlah kernel/arch), bukan cuma SEGV/BUS. Akses ke handler proses-lebar
// diserialisasi lewat sbx_probe_mutex agar tidak balapan dengan thread lain.
inline bool sbx_can_execute(void* fn) {
    std::lock_guard<std::mutex> lock(sbx_probe_mutex);
    struct sigaction sa{}, old_segv{}, old_bus{}, old_ill{};
    sa.sa_handler = sbx_probe_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGSEGV, &sa, &old_segv) != 0) return false;
    if (sigaction(SIGBUS, &sa, &old_bus) != 0) {
        sigaction(SIGSEGV, &old_segv, nullptr);
        return false;
    }
    if (sigaction(SIGILL, &sa, &old_ill) != 0) {
        sigaction(SIGSEGV, &old_segv, nullptr);
        sigaction(SIGBUS, &old_bus, nullptr);
        return false;
    }
    sbx_probe_fault = 0;
    volatile bool ran = false;
    if (sigsetjmp(sbx_probe_jmp, 1) == 0) {
        reinterpret_cast<void (*)()>(fn)();
        ran = true;
    }
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    sigaction(SIGILL, &old_ill, nullptr);
    return ran && !sbx_probe_fault;
}
// true bila SELURUH rentang [addr, addr+len) tercakup region 'x' di maps.
inline bool sbx_perms_has_exec(uintptr_t addr, size_t len) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return false;
    char line[512];
    uintptr_t want_end = addr + len;
    bool covered = false;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t lo = 0, hi = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (addr >= lo && want_end <= hi) {
            covered = (perms[2] == 'x');
            break;
        }
    }
    fclose(f);
    return covered;
}

// Temukan region r-x libart.so hidup: path & offset file untuk mmap salinan.
inline bool sbx_find_libart_text(std::string& path_out, unsigned long& off_out) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t lo = 0, hi = 0;
        char perms[8] = {0}, path[400] = {0};
        unsigned long off = 0;
        int n = sscanf(line, "%lx-%lx %7s %lx %*x:%*x %*lu %399[^\n]",
                       &lo, &hi, perms, &off, path);
        if (n < 5) continue;
        if (perms[0] != 'r' || perms[2] != 'x') continue;
        size_t plen = strlen(path);
        static const char kSuffix[] = "/libart.so";
        size_t slen = sizeof(kSuffix) - 1;
        if (plen >= slen && strcmp(path + plen - slen, kSuffix) == 0) {
            path_out.assign(path);
            off_out = off;
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

// Ladder tulis identik CodePatch: (1) pwrite64 /proc/self/mem FOLL_FORCE, lalu
// (2) fallback mprotect(RWX)→memcpy→restore R|X. dst harus page-aligned & panjang
// ≤ page. Mengembalikan true bila byte akhirnya cocok di dst. Dipakai probe
// execmod (halaman libart file-backed) DAN execmem (halaman anon).
inline bool sbx_write_via_ladder(void* dst, const uint8_t* patch, size_t patch_len, size_t page) {
    bool wrote = false;
    int mem = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    if (mem >= 0) {
        size_t total = 0;
        bool ok = true;
        while (total < patch_len) {
            ssize_t w = pwrite64(mem, patch + total, patch_len - total,
                                 (off64_t)((uintptr_t)dst + total));
            if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
            if (w == 0) { ok = false; break; }
            total += (size_t)w;
        }
        close(mem);
        wrote = ok && total == patch_len;
    }
    if (!wrote) {
        if (mprotect(dst, page, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            memcpy(dst, patch, patch_len);
            mprotect(dst, page, PROT_READ | PROT_EXEC);
            wrote = true;
        }
    }
    return wrote && memcmp(dst, patch, patch_len) == 0;
}

// Probe execmod: tulis 'ret' ke SALINAN throwaway file-backed dari .text libart,
// lalu verifikasi exec dipertahankan. Menguji apakah proses boleh menulis-kode ke
// halaman file-backed r-x (COW private) lalu tetap mengeksekusinya — kapabilitas
// yang dibutuhkan CodePatch untuk menanam jump di libart. TAK menyentuh libart hidup.
inline bool sbx_run_execmod_probe() {
    std::string libart;
    unsigned long file_off = 0;
    if (!sbx_find_libart_text(libart, file_off)) {
        SBX_LSP_LOGE("probe execmod: region r-x libart.so tak ditemukan di maps — anggap TAK mampu");
        return false;
    }

    long ps = sysconf(_SC_PAGESIZE);
    size_t page = (ps > 0) ? (size_t)ps : 4096;
    unsigned long aligned_off = file_off & ~(unsigned long)(page - 1);

    int fd = open(libart.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        SBX_LSP_LOGE("probe execmod: open(%s) gagal (errno=%d %s) — anggap TAK mampu",
                     libart.c_str(), errno, strerror(errno));
        return false;
    }
    // Salinan privat file-backed: label SELinux & sifat COW = libart hidup.
    void* map = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, (off_t)aligned_off);
    close(fd);
    if (map == MAP_FAILED) {
        SBX_LSP_LOGE("probe execmod: mmap(PROT_EXEC libart, off=%lu) gagal (errno=%d %s) — anggap TAK mampu",
                     aligned_off, errno, strerror(errno));
        return false;
    }

    const uint8_t patch[] = SBX_LSP_RET_BYTES;
    const size_t patch_len = sizeof(patch);

    bool bytes_ok = sbx_write_via_ladder(map, patch, patch_len, page);
    bool maps_exec = bytes_ok && sbx_perms_has_exec((uintptr_t)map, patch_len);
    bool exec_ok = maps_exec;
#ifdef SBX_LSP_HAVE_EXEC_PROBE
    if (maps_exec) {
        __builtin___clear_cache((char*)map, (char*)map + page);
        exec_ok = sbx_can_execute(map);   // eksekusi 'ret' sungguhan di bawah guard
    }
#endif
    munmap(map, page);

    if (!exec_ok) {
        SBX_LSP_LOGE("probe execmod: TAK aman (bytes_ok=%d maps_exec=%d exec_ok=%d) "
                     "— perangkat mencabut exec pasca-tulis file-backed; L3 inline-hook DILEWATI",
                     bytes_ok, maps_exec, exec_ok);
    } else {
        SBX_LSP_LOGI("probe execmod: AMAN (file-backed .text writable & tetap executable)");
    }
    return exec_ok;
}

// Probe execmem: uji kapabilitas yang BENAR-BENAR runtuh di perangkat W^X keras —
// memori ANONIM yang bisa ditulis-kode lalu dieksekusi (`self:process execmem`).
// Inilah yang ditolak untrusted_app A15/16 dan yang tak pernah diuji probe lama.
// Dua sub-uji mencerminkan dua situs alokasi anon nyata yang dipakai lsplant::Init:
//   (A) arena trampolin Dobby     — mmap(RX anon) + tulis-kode via ladder CodePatch
//   (B) trampolin per-metode LSPlant — mmap(RWX anon) langsung (lsplant.cc:539)
// KEDUANYA harus lolos: sebagian ROM mengizinkan satu gaya alokasi tapi menolak
// yang lain (mmap RX sukses tapi mprotect+W ditolak, atau sebaliknya). Semua
// halaman throwaway & di-munmap; eksekusi 'ret' di bawah guard sinyal. Log bertag
// SandboxID + errno agar KONKLUSIF di logcat. Ref: AOSP jit_memory_region.cc.
inline bool sbx_run_execmem_probe() {
    long ps = sysconf(_SC_PAGESIZE);
    size_t page = (ps > 0) ? (size_t)ps : 4096;
    const uint8_t patch[] = SBX_LSP_RET_BYTES;
    const size_t patch_len = sizeof(patch);

    // (A) Gaya arena Dobby: mmap(PROT_READ|PROT_EXEC anon) lalu tulis via ladder.
    void* a = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED) {
        SBX_LSP_LOGE("probe execmem A: mmap(RX anon) DITOLAK (errno=%d %s) — execmem tak diberikan; L3 DILEWATI",
                     errno, strerror(errno));
        return false;
    }
    bool a_wrote = sbx_write_via_ladder(a, patch, patch_len, page);
    int a_errno = errno;   // tangkap sebelum panggilan lain menimpa (mprotect EACCES)
    bool a_exec = a_wrote && sbx_perms_has_exec((uintptr_t)a, patch_len);
#ifdef SBX_LSP_HAVE_EXEC_PROBE
    if (a_exec) { __builtin___clear_cache((char*)a, (char*)a + page); a_exec = sbx_can_execute(a); }
#endif
    munmap(a, page);
    if (!a_exec) {
        SBX_LSP_LOGE("probe execmem A (arena Dobby): TAK mampu (wrote=%d errno=%d %s) — "
                     "tulis/eksekusi memori anonim ditolak; L3 inline-hook DILEWATI",
                     a_wrote, a_errno, strerror(a_errno));
        return false;
    }

    // (B) Gaya trampolin LSPlant: mmap(PROT_READ|PROT_WRITE|PROT_EXEC anon) langsung.
    void* b = mmap(nullptr, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) {
        SBX_LSP_LOGE("probe execmem B: mmap(RWX anon) DITOLAK (errno=%d %s) — execmem tak diberikan; L3 DILEWATI",
                     errno, strerror(errno));
        return false;
    }
    memcpy(b, patch, patch_len);
    bool b_exec = sbx_perms_has_exec((uintptr_t)b, patch_len);
#ifdef SBX_LSP_HAVE_EXEC_PROBE
    if (b_exec) { __builtin___clear_cache((char*)b, (char*)b + page); b_exec = sbx_can_execute(b); }
#endif
    munmap(b, page);
    if (!b_exec) {
        SBX_LSP_LOGE("probe execmem B (trampolin LSPlant): TAK mampu — RWX anon tak executable; L3 DILEWATI");
        return false;
    }

    SBX_LSP_LOGI("probe execmem: AMAN (anon RX+RWX writable & executable) — L3 inline-hook diaktifkan");
    return true;
}

// Guard sinyal untuk lsplant::Init: jaring TERAKHIR bila kedua probe lolos tapi
// Init tetap execute-fault di jalur tak teruji. Mengubah SIGSEGV/SIGBUS/SIGILL
// fatal menjadi skip bersih (return false) alih-alih crash proses. Tanpa lock_guard
// / objek stack ber-destructor di antara sigsetjmp & panggilan: siglongjmp TIDAK
// meng-unwind stack, jadi tak ada yang bocor. init() satu-shot & sekuensial setelah
// codepatch_capable() memoized, jadi tak ada probe lain yang balapan pasang handler.
inline sigjmp_buf              sbx_init_jmp;
inline volatile sig_atomic_t   sbx_init_fault = 0;
inline void sbx_init_sig_handler(int) { sbx_init_fault = 1; siglongjmp(sbx_init_jmp, 1); }

inline bool sbx_guarded_lsplant_init(JNIEnv* env, const lsplant::InitInfo& info) {
    struct sigaction sa {}, old_segv {}, old_bus {}, old_ill {};
    sa.sa_handler = sbx_init_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    bool h_segv = sigaction(SIGSEGV, &sa, &old_segv) == 0;
    bool h_bus  = h_segv && sigaction(SIGBUS, &sa, &old_bus) == 0;
    bool h_ill  = h_bus  && sigaction(SIGILL, &sa, &old_ill) == 0;
    if (!h_ill) {   // guard tak lengkap → jangan setengah-pasang; jalankan apa adanya
        if (h_bus)  sigaction(SIGBUS,  &old_bus,  nullptr);
        if (h_segv) sigaction(SIGSEGV, &old_segv, nullptr);
        return lsplant::Init(env, info);
    }
    sbx_init_fault = 0;
    bool r = false;
    if (sigsetjmp(sbx_init_jmp, 1) == 0) {
        r = lsplant::Init(env, info);
    } else {
        SBX_LSP_LOGE("lsplant::Init execute-fault (SIGSEGV/BUS/ILL ditangkap) — "
                     "L3 dinonaktifkan proses ini; L1/L2 tetap jalan");
        r = false;
    }
    sigaction(SIGILL,  &old_ill,  nullptr);
    sigaction(SIGBUS,  &old_bus,  nullptr);
    sigaction(SIGSEGV, &old_segv, nullptr);
    return r && !sbx_init_fault;
}

// Kapabilitas inline-hook, dihitung sekali lalu di-cache (thread-safe). Perlu KEDUA:
// execmod (patch .text libart file-backed) DAN execmem (trampolin memori anonim).
inline bool codepatch_capable() {
    static std::mutex mu;
    static bool computed = false, capable = false;
    std::lock_guard<std::mutex> lk(mu);
    if (!computed) {
        capable = sbx_run_execmod_probe() && sbx_run_execmem_probe();
        computed = true;
    }
    return capable;
}

inline bool init(JNIEnv* env) {
    if (!env) { SBX_LSP_LOGE("L3 init: env NULL — dibatalkan"); return false; }
    static bool done = false, ok = false;
    if (done) { if (!ok) SBX_LSP_LOGE("L3 init: cached-fail (init sebelumnya gagal)"); return ok; }
    done = true;

    static lsparself::Elf art("/libart.so");

    static const std::string kCls = "androidx.core.os.HandlerCompatRef";
    static const std::string kSrc = "Hc";
    static const std::string kFld = "h";

    lsplant::InitInfo info{
        .inline_hooker   = sbx_inline_hooker,
        .inline_unhooker = sbx_inline_unhooker,
        // Android 15 (API 35): ART merename JitCodeCache::GarbageCollectCache ->
        // DoCollection. LSPlant v6.4 masih minta simbol lama & tanpa fallback, jadi
        // JitCodeCache::Init gagal -> lsplant::Init gagal -> semua hook L3 mati. Alias
        // di bawah setara fix upstream master handler(GarbageCollectCache_, DoCollection_):
        // hook fungsi yang sama, signature (JitCodeCache*, Thread*) identik.
        // Credit: LSPosed/LSPlant master jit_code_cache.cxx (Apache-2.0), Issue #97.
        .art_symbol_resolver =
            [](std::string_view s) -> void* {
                void* a = reinterpret_cast<void*>(art.getSymbAddress(s));
                if (!a && s == "_ZN3art3jit12JitCodeCache19GarbageCollectCacheEPNS_6ThreadE")
                    a = reinterpret_cast<void*>(art.getSymbAddress(
                        "_ZN3art3jit12JitCodeCache12DoCollectionEPNS_6ThreadE"));
                return a;
            },
        .art_symbol_prefix_resolver =
            [](std::string_view s) -> void* {
                return reinterpret_cast<void*>(art.getSymbPrefixFirstAddress(s));
            },
    };
    info.generated_class_name  = kCls;
    info.generated_source_name = kSrc;
    info.generated_field_name  = kFld;

    // Gate kapabilitas: L3 butuh execmod (patch .text libart) DAN execmem (trampolin
    // memori anonim untuk arena Dobby + trampolin per-metode LSPlant). Perangkat W^X
    // keras (untrusted_app A15/16) sering mengizinkan execmod tapi menolak execmem →
    // salah satu langkah InitNative gagal mengalokasikan trampolin anon → cabang rusak
    // → langkah TERAKHIR memanggil SetRuntimeDebugState pada alamat non-exec → SIGSEGV
    // SEGV_ACCERR EXECUTE +0. Probe throwaway menguji KEDUA kelas TANPA menyentuh
    // libart hidup; kalau tak mampu, lewati L3 & kembalikan false bersih (L1/L2 jalan).
    if (!codepatch_capable()) {
        SBX_LSP_LOGE("L3 init: perangkat menolak execmod/execmem (probe gagal) — "
                     "lsplant::Init DILEWATI demi mencegah crash SetRuntimeDebugState");
        ok = false;
        return ok;
    }

    // Diagnostik: apakah resolver ELF menemukan simbol libart? art.valid()==0
    // berarti /libart.so tak ketemu di /proc/self/maps atau symtab/.gnu_debugdata
    // gagal di-parse → lsplant::Init pasti gagal. Log ungated agar terlihat.
    SBX_LSP_LOGE("L3 init: art.valid=%d — memanggil lsplant::Init (di bawah guard sinyal)", art.valid() ? 1 : 0);
    ok = sbx_guarded_lsplant_init(env, info);
    if (!ok) SBX_LSP_LOGE("lsplant::Init failed — L3 disabled this process (L1/L2 tetap)");
    else     SBX_LSP_LOGD("lsplant::Init ok");
    return ok;
}

inline jclass    g_cb_class = nullptr;
inline jmethodID g_cb_ctor  = nullptr;
inline jmethodID g_cb_handle= nullptr;
inline jobject   g_cb_reflected = nullptr;
inline jfieldID  f_isStatic=nullptr, f_keyIdx=nullptr, f_keyMatch=nullptr,
                 f_retType=nullptr, f_sval=nullptr, f_bval=nullptr, f_backup=nullptr;
inline std::vector<jobject> g_keep;

inline std::string g_gms_gaid;
inline std::string g_gms_appset;
inline bool        g_gms_adv_done   = false;
inline bool        g_gms_appset_done = false;
inline std::mutex  g_gms_mu;

inline jclass load_callback_class(JNIEnv* env) {
#ifndef SBX_HAVE_HOOK_DEX
    (void)env;
    SBX_LSP_LOGE("L3: callback DEX (hook_dex.h) tak ada di build ini — hook dilewati");
    return nullptr;
#else
    if (env->PushLocalFrame(16) != 0) { env->ExceptionClear(); return nullptr; }
    jobject cls = [&]() -> jobject {
        jobject bb = env->NewDirectByteBuffer((void*)hook_dex, (jlong)hook_dex_len);
        if (!bb || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jclass loaderCls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (!loaderCls || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jmethodID ctor = env->GetMethodID(loaderCls, "<init>",
            "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (!ctor || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jclass clCls = env->FindClass("java/lang/ClassLoader");
        if (!clCls || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader",
            "()Ljava/lang/ClassLoader;");
        if (!getSys || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jobject parent = env->CallStaticObjectMethod(clCls, getSys);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jobject loader = env->NewObject(loaderCls, ctor, bb, parent);
        if (!loader || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jmethodID loadClass = env->GetMethodID(clCls, "loadClass",
            "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!loadClass || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jstring name = env->NewStringUTF("androidx.core.os.EnvCompatState");
        jobject c = env->CallObjectMethod(loader, loadClass, name);
        if (!c || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return c;
    }();
    jclass g = cls ? (jclass)env->NewGlobalRef(cls) : nullptr;
    env->PopLocalFrame(nullptr);
    return g;
#endif
}

inline bool resolve_callback_members(JNIEnv* env) {
    if (!g_cb_class) return false;
    g_cb_ctor   = env->GetMethodID(g_cb_class, "<init>", "()V");
    g_cb_handle = env->GetMethodID(g_cb_class, "handle",
                                   "([Ljava/lang/Object;)Ljava/lang/Object;");
    if (!g_cb_ctor || !g_cb_handle || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject refl = env->ToReflectedMethod(g_cb_class, g_cb_handle, JNI_FALSE);
    if (!refl || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    g_cb_reflected = env->NewGlobalRef(refl);

    f_isStatic = env->GetFieldID(g_cb_class, "isStatic",    "Z");
    f_keyIdx   = env->GetFieldID(g_cb_class, "keyArgIndex", "I");
    f_keyMatch = env->GetFieldID(g_cb_class, "keyMatch",    "Ljava/lang/String;");
    f_retType  = env->GetFieldID(g_cb_class, "retType",     "I");
    f_sval     = env->GetFieldID(g_cb_class, "sval",        "Ljava/lang/String;");
    f_bval     = env->GetFieldID(g_cb_class, "bval",        "[B");
    f_backup   = env->GetFieldID(g_cb_class, "backup",      "Ljava/lang/reflect/Method;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return f_isStatic && f_keyIdx && f_keyMatch && f_retType && f_sval && f_bval && f_backup;
}

enum ValId {
    V_NONE = 0, V_ANDROID_ID, V_SERIAL, V_IMEI, V_MEID, V_IMSI, V_ICCID,
    V_OP_NUM, V_OP_ALPHA, V_OP_ISO, V_WIFI_MAC, V_BT_ADDR, V_WIDEVINE,

    V_SIM_STATE, V_PHONE_TYPE, V_ROAMING, V_MODEM_COUNT, V_CARRIER_ID,

    V_GAID, V_APP_SET_ID, V_LAT,

    V_MCC_STR, V_MNC_STR,

    V_WIFI_SSID, V_WIFI_BSSID,

    V_EMPTY_LIST,

    V_BT_NAME,

    V_GSERVICES
};

struct HookSpec {
    const char* cls;
    const char* name;
    const char* sig;
    bool        is_static;
    int         key_index;
    const char* key_match;
    int         ret_type;
    int         val_id;
    bool        no_deopt = false;
};

inline const HookSpec* hook_specs(size_t& n) {
    static const HookSpec S[] = {

        { "android/provider/Settings$Secure", "getString",
          "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
          true, 1, "android_id", 0, V_ANDROID_ID },

        { "android/provider/Settings$Secure", "getString",
          "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
          true, 1, "bluetooth_address", 0, V_BT_ADDR },
        { "android/os/Build", "getSerial", "()Ljava/lang/String;",
          true, -1, nullptr, 0, V_SERIAL },
        { "android/media/MediaDrm", "getPropertyByteArray", "(Ljava/lang/String;)[B",
          false, 1, "deviceUniqueId", 1, V_WIDEVINE },

        { "android/telephony/TelephonyManager", "getDeviceId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getDeviceId", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getImei", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getImei", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getMeid", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_MEID },
        { "android/telephony/TelephonyManager", "getMeid", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_MEID },
        { "android/telephony/TelephonyManager", "getSubscriberId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMSI },
        { "android/telephony/TelephonyManager", "getSimSerialNumber", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_ICCID },
        { "android/telephony/TelephonyManager", "getNetworkOperator", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_NUM },
        { "android/telephony/TelephonyManager", "getSimOperator", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_NUM },
        { "android/telephony/TelephonyManager", "getNetworkOperatorName", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ALPHA },
        { "android/telephony/TelephonyManager", "getSimOperatorName", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ALPHA },
        { "android/telephony/TelephonyManager", "getSimCountryIso", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },
        { "android/telephony/TelephonyManager", "getNetworkCountryIso", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },

        { "android/telephony/TelephonyManager", "getSubscriberId", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMSI },
        { "android/telephony/TelephonyManager", "getSimSerialNumber", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_ICCID },
        { "android/telephony/TelephonyManager", "getSimOperator", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_NUM },
        { "android/telephony/TelephonyManager", "getNetworkCountryIso", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },

        { "android/telephony/TelephonyManager", "getSimState", "()I",
          false, -1, nullptr, 2, V_SIM_STATE },
        { "android/telephony/TelephonyManager", "getSimState", "(I)I",
          false, -1, nullptr, 2, V_SIM_STATE },
        { "android/telephony/TelephonyManager", "getPhoneType", "()I",
          false, -1, nullptr, 2, V_PHONE_TYPE },
        { "android/telephony/TelephonyManager", "isNetworkRoaming", "()Z",
          false, -1, nullptr, 4, V_ROAMING },
        { "android/telephony/TelephonyManager", "getPhoneCount", "()I",
          false, -1, nullptr, 2, V_MODEM_COUNT },
        { "android/telephony/TelephonyManager", "getActiveModemCount", "()I",
          false, -1, nullptr, 2, V_MODEM_COUNT },
        { "android/telephony/TelephonyManager", "getSimCarrierId", "()I",
          false, -1, nullptr, 2, V_CARRIER_ID },

        { "android/telephony/TelephonyManager", "getSimCarrierIdName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },
        { "android/telephony/TelephonyManager", "getSimSpecificCarrierId", "()I",
          false, -1, nullptr, 2, V_CARRIER_ID },
        { "android/telephony/TelephonyManager", "getSimSpecificCarrierIdName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },

        { "android/telephony/SubscriptionInfo", "getMccString", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_MCC_STR },
        { "android/telephony/SubscriptionInfo", "getMncString", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_MNC_STR },
        { "android/telephony/SubscriptionInfo", "getMcc", "()I",
          false, -1, nullptr, 2, V_MCC_STR },
        { "android/telephony/SubscriptionInfo", "getMnc", "()I",
          false, -1, nullptr, 2, V_MNC_STR },
        { "android/telephony/SubscriptionInfo", "getCountryIso", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },
        { "android/telephony/SubscriptionInfo", "getCarrierName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },
        { "android/telephony/SubscriptionInfo", "getDisplayName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },
        { "android/telephony/SubscriptionInfo", "getCarrierId", "()I",
          false, -1, nullptr, 2, V_CARRIER_ID },
        { "android/telephony/SubscriptionInfo", "getIccId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_ICCID },

        { "android/net/wifi/WifiInfo", "getMacAddress", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_WIFI_MAC },
        { "android/bluetooth/BluetoothAdapter", "getAddress", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_BT_ADDR },
        { "android/bluetooth/BluetoothAdapter", "getName", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_BT_NAME },
        { "android/bluetooth/BluetoothAdapter", "getBondedDevices", "()Ljava/util/Set;",
          false, -1, nullptr, 10, V_NONE },

        { "android/telephony/TelephonyManager", "getLine1Number", "()Ljava/lang/String;",
          false, -1, nullptr, 11, V_NONE },
        { "android/telephony/TelephonyManager", "getLine1Number", "(I)Ljava/lang/String;",
          false, -1, nullptr, 11, V_NONE },
        { "android/telephony/TelephonyManager", "getVoiceMailNumber", "()Ljava/lang/String;",
          false, -1, nullptr, 11, V_NONE },
        { "android/telephony/SubscriptionInfo", "getNumber", "()Ljava/lang/String;",
          false, -1, nullptr, 11, V_NONE },

        { "android/net/wifi/WifiInfo", "getSSID", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_WIFI_SSID },
        { "android/net/wifi/WifiInfo", "getBSSID", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_WIFI_BSSID },

        { "android/net/wifi/WifiManager", "getConfiguredNetworks", "()Ljava/util/List;",
          false, -1, nullptr, 6, V_EMPTY_LIST },

        { "android/adservices/adid/AdId", "getAdId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_GAID },
        { "android/adservices/adid/AdId", "isLimitAdTrackingEnabled", "()Z",
          false, -1, nullptr, 4, V_LAT },
        { "android/adservices/appsetid/AppSetId", "getId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_APP_SET_ID },

        { "android/content/ContentResolver", "query",
          "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
          false, -1, nullptr, 7, V_GSERVICES, true },
        { "android/content/ContentResolver", "query",
          "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Landroid/os/CancellationSignal;)Landroid/database/Cursor;",
          false, -1, nullptr, 7, V_GSERVICES, true },
    };
    n = sizeof(S) / sizeof(S[0]);
    return S;
}

inline std::string sbx_mac_upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    return s;
}
inline jbyteArray sbx_hex_to_jbytes(JNIEnv* env, const std::string& hex) {
    size_t n = hex.size() / 2;
    jbyteArray a = env->NewByteArray((jsize)n);
    if (!a) return nullptr;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    std::vector<jbyte> buf(n);
    for (size_t i = 0; i < n; ++i)
        buf[i] = (jbyte)((hv(hex[2 * i]) << 4) | hv(hex[2 * i + 1]));
    env->SetByteArrayRegion(a, 0, (jsize)n, buf.data());
    return a;
}

inline bool hook_one_on_class(JNIEnv* env, jclass cls, const HookSpec& sp,
                              const std::string& sval, const std::string& wvbytes) {
    if (!cls) return false;
    if (env->PushLocalFrame(24) != 0) { env->ExceptionClear(); return false; }
    bool ok = [&]() -> bool {
        jmethodID mid = sp.is_static ? env->GetStaticMethodID(cls, sp.name, sp.sig)
                                     : env->GetMethodID(cls, sp.name, sp.sig);
        if (!mid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jobject target = env->ToReflectedMethod(cls, mid, sp.is_static ? JNI_TRUE : JNI_FALSE);
        if (!target || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        jobject hooker = env->NewObject(g_cb_class, g_cb_ctor);
        if (!hooker || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetBooleanField(hooker, f_isStatic, sp.is_static ? JNI_TRUE : JNI_FALSE);
        env->SetIntField(hooker, f_keyIdx, sp.key_index);
        env->SetIntField(hooker, f_retType, sp.ret_type);
        if (sp.key_match) {
            jstring km = env->NewStringUTF(sp.key_match);
            env->SetObjectField(hooker, f_keyMatch, km);
        }
        if (sp.ret_type == 1) {
            if (wvbytes.size() >= 2) {
                jbyteArray b = sbx_hex_to_jbytes(env, wvbytes);
                if (b) env->SetObjectField(hooker, f_bval, b);
            }
        } else if (!sval.empty()) {
            jstring sv = env->NewStringUTF(sval.c_str());
            env->SetObjectField(hooker, f_sval, sv);
        }

        jobject backup = lsplant::Hook(env, target, hooker, g_cb_reflected);
        if (!backup || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetObjectField(hooker, f_backup, backup);

        g_keep.push_back(env->NewGlobalRef(hooker));
        g_keep.push_back(env->NewGlobalRef(backup));

        if (!sp.no_deopt) {
            (void) lsplant::Deoptimize(env, target);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        return true;
    }();
    env->PopLocalFrame(nullptr);
    if (ok) SBX_LSP_LOGD("L3 hooked %s.%s%s", sp.cls, sp.name, sp.sig);
    return ok;
}

inline bool hook_one(JNIEnv* env, const HookSpec& sp,
                     const std::string& sval, const std::string& wvbytes) {
    if (env->PushLocalFrame(4) != 0) { env->ExceptionClear(); return false; }
    jclass cls = env->FindClass(sp.cls);
    if (!cls || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->PopLocalFrame(nullptr);
        return false;
    }
    bool ok = hook_one_on_class(env, cls, sp, sval, wvbytes);
    env->PopLocalFrame(nullptr);
    return ok;
}

inline void hook_gms_getters(JNIEnv* env, jclass cls, bool is_advertising) {
    if (!cls) return;
    if (is_advertising) {
        HookSpec gid{ "com/google/android/gms/ads/identifier/AdvertisingIdClient$Info",
                      "getId", "()Ljava/lang/String;", false, -1, nullptr, 0, V_NONE, true };
        hook_one_on_class(env, cls, gid, g_gms_gaid, std::string());
        HookSpec lat{ "com/google/android/gms/ads/identifier/AdvertisingIdClient$Info",
                      "isLimitAdTrackingEnabled", "()Z", false, -1, nullptr, 4, V_NONE, true };
        hook_one_on_class(env, cls, lat, "false", std::string());
    } else {
        HookSpec sid{ "com/google/android/gms/appset/AppSetIdInfo",
                      "getId", "()Ljava/lang/String;", false, -1, nullptr, 0, V_NONE, true };
        hook_one_on_class(env, cls, sid, g_gms_appset, std::string());
    }
}

inline void sbx_on_class_loaded(JNIEnv* env, jclass , jstring jname, jobject clsObj) {
    if (!env || !jname || !clsObj) return;
    const char* nm = env->GetStringUTFChars(jname, nullptr);
    if (!nm) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
    std::string name(nm);
    env->ReleaseStringUTFChars(jname, nm);
    std::lock_guard<std::mutex> lk(g_gms_mu);
    if (!g_gms_adv_done &&
        name == "com.google.android.gms.ads.identifier.AdvertisingIdClient$Info") {
        g_gms_adv_done = true;
        hook_gms_getters(env, static_cast<jclass>(clsObj), true);
    } else if (!g_gms_appset_done &&
               name == "com.google.android.gms.appset.AppSetIdInfo") {
        g_gms_appset_done = true;
        hook_gms_getters(env, static_cast<jclass>(clsObj), false);
    }
}

inline bool register_class_watch_native(JNIEnv* env) {
    if (!g_cb_class) return false;
    JNINativeMethod m{ "onClassLoaded", "(Ljava/lang/String;Ljava/lang/Object;)V",
                       reinterpret_cast<void*>(&sbx_on_class_loaded) };
    if (env->RegisterNatives(g_cb_class, &m, 1) != 0) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    return true;
}

inline std::string sbx_value_for(int val_id, const HookValues& v,
                                 const sbxid::SynthIds& ids,
                                 const std::string& wifi, const std::string& bt,
                                 const std::string& gaid, const std::string& appset,
                                 const std::string& gsf) {
    switch (val_id) {
        case V_ANDROID_ID: return v.android_id;
        case V_SERIAL:     return v.serial;
        case V_IMEI:       return ids.imei;
        case V_MEID:       return ids.meid;
        case V_IMSI:       return ids.imsi;
        case V_ICCID:      return ids.iccid;
        case V_OP_NUM:     return v.op_num;

        case V_MCC_STR:    return v.op_num.size() >= 3 ? v.op_num.substr(0, 3) : std::string();
        case V_MNC_STR:    return v.op_num.size() >  3 ? v.op_num.substr(3)    : std::string();
        case V_OP_ALPHA:   return v.op_alpha;
        case V_OP_ISO:     return v.op_iso;
        case V_WIFI_MAC:   return wifi;
        case V_BT_ADDR:    return bt;
        case V_BT_NAME:    return v.model;

        case V_WIFI_SSID:  return "<unknown ssid>";
        case V_WIFI_BSSID: return "02:00:00:00:00:00";
        case V_EMPTY_LIST: return std::string();

        case V_SIM_STATE:  return "5";
        case V_PHONE_TYPE: return "1";
        case V_ROAMING:    return "false";
        case V_MODEM_COUNT:return "1";

        case V_CARRIER_ID: return v.carrier_id.empty() ? std::string("-1") : v.carrier_id;

        case V_GAID:       return gaid;
        case V_APP_SET_ID: return appset;
        case V_LAT:        return "false";

        case V_GSERVICES:  return gsf;
        default:           return std::string();
    }
}

inline bool install_all(JNIEnv* env, const HookValues& v) {
    // Diagnostik masuk (ungated): jika baris ini TIDAK muncul di logcat padahal
    // main.cpp mencetak "L3 hooks not installed", berarti binary terpasang ≠
    // source (mismatch build), bukan bug logika di sini.
    SBX_LSP_LOGE("L3 install_all: masuk (env=%p, have_hook_dex=%d)",
                 (void*)env,
#ifdef SBX_HAVE_HOOK_DEX
                 1
#else
                 0
#endif
    );
    if (!env) { SBX_LSP_LOGE("L3 install_all: env NULL — dibatalkan"); return false; }
    if (!init(env)) return false;

    if (!g_cb_class) {
        g_cb_class = load_callback_class(env);
        if (!g_cb_class || !resolve_callback_members(env)) {
            SBX_LSP_LOGE("L3: callback class/members unavailable — hooks skipped");
            return false;
        }
    }

    sbxid::SynthIds ids = sbxid::synth_all(v.seed, v.op_num);

    std::string wifi = !v.wifi_mac.empty() ? v.wifi_mac
                                           : sbxnr::mac_from_seed(v.seed ^ 0x9E3779B97F4A7C15ULL);
    std::string bt   = !v.bt_addr.empty() ? sbx_mac_upper(v.bt_addr)
                                          : sbx_mac_upper(sbxnr::mac_from_seed(v.seed ^ 0x424C554554ULL));

    std::string gaid   = !v.gaid.empty() ? v.gaid
                                         : sbxnr::uuid_from_seed(v.seed ^ 0x47414944ULL);
    std::string appset = !v.app_set_id.empty() ? v.app_set_id
                                               : sbxnr::uuid_from_seed(v.seed ^ 0x4150534554ULL);

    std::string gsf = sbxid::synth_gsf_id(v.seed);

    const bool have_sim = !v.op_num.empty();
    const bool have_android_id = !v.android_id.empty();
    size_t n = 0;
    const HookSpec* specs = hook_specs(n);
    int good = 0;
    for (size_t i = 0; i < n; ++i) {
        const int vid = specs[i].val_id;

        if (!have_sim && (vid == V_SIM_STATE || vid == V_MODEM_COUNT || vid == V_CARRIER_ID))
            continue;
        if (!have_android_id && vid == V_GSERVICES)
            continue;
        std::string sval = sbx_value_for(vid, v, ids, wifi, bt, gaid, appset, gsf);
        if (hook_one(env, specs[i], sval, ids.widevine_hex)) ++good;
    }

    // Widevine provisioningUniqueId (byte[]): dibangun terpisah dari tabel agar
    // memakai byte yang BERBEDA dari deviceUniqueId (nilai asli keduanya memang
    // beda; menyamakannya bisa jadi tell saat app membaca keduanya).
    {
        std::string prov_hex = sbxid::synth_widevine_prov_hex(v.seed);
        HookSpec pv{ "android/media/MediaDrm", "getPropertyByteArray",
                     "(Ljava/lang/String;)[B",
                     false, 1, "provisioningUniqueId", 1, V_WIDEVINE };
        if (hook_one(env, pv, std::string(), prov_hex)) ++good;
    }

    // Waktu instal/update aplikasi (retType 8): hanya paket sendiri, dijangkar
    // ke tanggal build agar stabil lintas restart dan plausibel (instal setelah
    // perangkat dibuat). firstInstallTime == lastUpdateTime → tampak belum
    // pernah di-update, pola yang lazim dan aman.
    if (!v.self_pkg.empty() && !v.build_time_utc.empty()) {
        long long bt_s = atoll(v.build_time_utc.c_str());
        if (bt_s > 0) {
            uint64_t off_days = 1 + (v.seed % 120);
            long long first_ms = (bt_s + (long long)off_days * 86400LL) * 1000LL;
            std::string install_ms = std::to_string(first_ms);
            static const char* const pm_sigs[] = {
                "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
                "(Ljava/lang/String;Landroid/content/pm/PackageManager$PackageInfoFlags;)"
                    "Landroid/content/pm/PackageInfo;",
            };
            for (const char* psig : pm_sigs) {
                HookSpec ps{ "android/app/ApplicationPackageManager", "getPackageInfo", psig,
                             false, 1, v.self_pkg.c_str(), 8, V_NONE };
                if (hook_one(env, ps, install_ms, std::string())) ++good;
            }
        }
    }

    if (v.gms_watch) {
        g_gms_gaid   = gaid;
        g_gms_appset = appset;
        if (register_class_watch_native(env)) {
            HookSpec fc{ "dalvik/system/BaseDexClassLoader", "findClass",
                         "(Ljava/lang/String;)Ljava/lang/Class;",
                         false, -1, nullptr, 9, V_NONE, true };
            if (hook_one(env, fc, std::string(), std::string())) {
                ++good;
                SBX_LSP_LOGD("L3 GMS class-load watch armed");
            }
        }
    }

    SBX_LSP_LOGD("L3 install_all: %d/%zu targets hooked", good, n);
    if (good == 0) SBX_LSP_LOGE("L3: no targets hooked (continuing with L1/L2/L9)");
    return good > 0;
}
#endif

}
