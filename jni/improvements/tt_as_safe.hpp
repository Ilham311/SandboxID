#pragma once

#include <cstddef>
#include <cstdint>

namespace tt {
namespace as_safe {

[[nodiscard]] inline std::size_t write_str(char* buf, std::size_t cap, const char* s) noexcept {
    if (!s || cap == 0) return 0;
    std::size_t n = 0;
    while (s[n] && n < cap) { buf[n] = s[n]; ++n; }
    return n;
}

[[nodiscard]] inline std::size_t write_int_dec(char* buf, std::size_t cap, long v) noexcept {
    if (cap == 0) return 0;
    bool neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
    char tmp[24];
    std::size_t t = 0;
    do { tmp[t++] = char('0' + (u % 10)); u /= 10; } while (u);
    std::size_t n = 0;
    if (neg && n < cap) buf[n++] = '-';
    while (t && n < cap) buf[n++] = tmp[--t];
    return n;
}

[[nodiscard]] inline std::size_t write_hex_ptr(char* buf, std::size_t cap, const void* p) noexcept {
    if (cap == 0) return 0;
    std::uintptr_t u = reinterpret_cast<std::uintptr_t>(p);
    static const char HEX[] = "0123456789abcdef";
    char tmp[24];
    std::size_t t = 0;
    if (u == 0) tmp[t++] = '0';
    else while (u) { tmp[t++] = HEX[u & 0xF]; u >>= 4; }
    std::size_t n = 0;
    if (cap >= 2) { buf[n++] = '0'; buf[n++] = 'x'; }
    while (t && n < cap) buf[n++] = tmp[--t];
    return n;
}

}
}
