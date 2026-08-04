// ternak_tt v2 — random_util.hpp
// Item #4: seed random from /dev/urandom directly (not std::random_device XOR steady_clock)
//
// USAGE (in ternak-tt.cpp):
//   #include "improvements/random_util.hpp"
//   static std::string random_hex(int bytes, bool upper) {
//       return tt::random_hex(bytes, upper);   // was: local implementation
//   }
//
// Rationale: bionic std::random_device is usually /dev/urandom-backed, but
// some vendor libcs fall back to deterministic mt19937. Reading /dev/urandom
// directly removes that ambiguity for identity-critical values (SERIAL,
// ANDROID_ID, GOOGLE_AID).

#pragma once

#include <cstdint>
#include <cstddef>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <random>
#include <chrono>

namespace tt {

inline uint64_t urandom_u64() {
    uint64_t v = 0;
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = ::read(fd, &v, sizeof(v));
        ::close(fd);
        if (n == (ssize_t)sizeof(v) && v != 0) return v;
    }
    // Fallback chain (only if /dev/urandom failed entirely):
    //   1) std::random_device
    //   2) high-res monotonic clock
    std::random_device rd;
    v = ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    v ^= (uint64_t)std::chrono::steady_clock::now()
             .time_since_epoch().count();
    return v ? v : 0xDEADBEEFCAFEBABEULL;
}

inline void urandom_fill(void* buf, size_t n) {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t got = 0;
        auto* p = static_cast<unsigned char*>(buf);
        while (got < n) {
            ssize_t r = ::read(fd, p + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        ::close(fd);
        if (got == n) return;
    }
    // Fallback: mt19937_64 seeded from urandom_u64
    std::mt19937_64 g(urandom_u64());
    auto* p = static_cast<unsigned char*>(buf);
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)(g() & 0xFF);
}

inline std::string random_hex(int bytes, bool upper) {
    const char* al = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    s.reserve((size_t)bytes * 2);
    unsigned char buf[64];
    if (bytes > (int)sizeof(buf)) bytes = (int)sizeof(buf);
    urandom_fill(buf, (size_t)bytes);
    for (int i = 0; i < bytes; ++i) {
        s.push_back(al[(buf[i] >> 4) & 0xF]);
        s.push_back(al[buf[i] & 0xF]);
    }
    return s;
}

inline std::string uuid_v4() {
    unsigned char b[16];
    urandom_fill(b, sizeof(b));
    // RFC 4122 v4 bits
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    static const char* hex = "0123456789abcdef";
    char out[37];
    int p = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
        out[p++] = hex[(b[i] >> 4) & 0xF];
        out[p++] = hex[b[i] & 0xF];
    }
    out[p] = 0;
    return std::string(out);
}

}  // namespace tt
