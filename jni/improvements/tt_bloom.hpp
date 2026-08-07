#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace tt {
namespace bloom {

inline constexpr std::size_t BITS   = 8192;
inline constexpr std::size_t BYTES  = BITS / 8;
inline constexpr std::size_t K_HASH = 4;

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view s, std::uint64_t seed) noexcept {
    std::uint64_t h = seed ^ 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= (std::uint64_t)c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

struct Filter {
    std::uint8_t bits[BYTES];

    void clear() noexcept { std::memset(bits, 0, BYTES); }

    void add(std::string_view s) noexcept {
        for (std::size_t i = 0; i < K_HASH; ++i) {
            std::uint64_t h = fnv1a64(s, i * 0x9e3779b97f4a7c15ULL);
            std::size_t bit = (std::size_t)(h % BITS);
            bits[bit >> 3] |= (std::uint8_t)(1u << (bit & 7u));
        }
    }

    [[nodiscard]] bool might_contain(std::string_view s) const noexcept {
        for (std::size_t i = 0; i < K_HASH; ++i) {
            std::uint64_t h = fnv1a64(s, i * 0x9e3779b97f4a7c15ULL);
            std::size_t bit = (std::size_t)(h % BITS);
            if (!(bits[bit >> 3] & (std::uint8_t)(1u << (bit & 7u)))) return false;
        }
        return true;
    }
};

}
}
