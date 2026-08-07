// ternak_tt v2.2 — random_util.hpp
// P3-A: prefer getrandom(2) syscall (Linux 3.17+, Android API 28+), no fd churn.
//       Fallback ke /dev/urandom kalau kernel terlalu tua, terakhir mt19937_64.
//
// Refs:
//   - https://man7.org/linux/man-pages/man2/getrandom.2.html
//   - https://en.cppreference.com/w/cpp/numeric/random/mersenne_twister_engine
//
// Kenapa migrate: /dev/urandom open+read+close tiap panggilan mengalokasikan
// fd baru per invocation. urandom_fill dipanggil per generate identity/UUID
// (jarang), tapi getrandom syscall dua kali lebih cepat dan tidak membocorkan
// fd bila ada handler signal yang crash di antara open dan close.

#pragma once

#include <cstdint>
#include <cstddef>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <string>
#include <random>
#include <chrono>

namespace tt {

// Try getrandom(2) syscall (non-blocking pool for our use — kernel handles it).
inline bool try_getrandom(void* buf, std::size_t n) noexcept {
#ifdef __NR_getrandom
    auto* p = static_cast<unsigned char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        long r = ::syscall(__NR_getrandom, p + got, n - got, 0u);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;                 // ENOSYS di kernel < 3.17
        }
        if (r == 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
#else
    (void)buf; (void)n;
    return false;
#endif
}

// Fallback: read from /dev/urandom.
inline bool try_urandom(void* buf, std::size_t n) noexcept {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    auto* p = static_cast<unsigned char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (r == 0) break;
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    return got == n;
}

// Public API: fill n bytes with best-effort entropy. Never throws.
inline void urandom_fill(void* buf, std::size_t n) noexcept {
    if (try_getrandom(buf, n)) return;
    if (try_urandom(buf, n))   return;

    // Last-resort: seed a PRNG from time + pid. Non-cryptographic but stable.
    std::mt19937_64 g(
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())
        ^ static_cast<std::uint64_t>(::getpid()));
    auto* p = static_cast<unsigned char*>(buf);
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = static_cast<unsigned char>(g() & 0xFFu);
    }
}

[[nodiscard]] inline std::uint64_t urandom_u64() noexcept {
    std::uint64_t v = 0;
    urandom_fill(&v, sizeof(v));
    return v ? v : 0xDEADBEEFCAFEBABEULL;
}

[[nodiscard]] inline std::string random_hex(int bytes, bool upper) {
    const char* al = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (bytes < 1)  bytes = 1;
    if (bytes > 64) bytes = 64;
    unsigned char buf[64];
    urandom_fill(buf, static_cast<std::size_t>(bytes));
    std::string s;
    s.reserve(static_cast<std::size_t>(bytes) * 2);
    for (int i = 0; i < bytes; ++i) {
        s.push_back(al[(buf[i] >> 4) & 0xF]);
        s.push_back(al[buf[i]       & 0xF]);
    }
    return s;
}

[[nodiscard]] inline std::string uuid_v4() {
    unsigned char b[16];
    urandom_fill(b, sizeof(b));
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40); // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80); // variant 10x
    static constexpr char hex[] = "0123456789abcdef";
    char out[37];
    int p = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
        out[p++] = hex[(b[i] >> 4) & 0xF];
        out[p++] = hex[b[i]       & 0xF];
    }
    out[p] = '\0';
    return std::string(out, static_cast<std::size_t>(p));
}

}  // namespace tt
