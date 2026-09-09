#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sbxhash {

inline uint32_t rotate_right(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

inline std::string sha256(std::string_view input) {
    static constexpr uint32_t constants[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::vector<uint8_t> bytes(input.begin(), input.end());
    const uint64_t bit_length = static_cast<uint64_t>(bytes.size()) * 8u;
    bytes.push_back(0x80u);
    while ((bytes.size() % 64u) != 56u) bytes.push_back(0u);
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<uint8_t>(bit_length >> shift));

    for (size_t offset = 0; offset < bytes.size(); offset += 64u) {
        uint32_t words[64]{};
        for (size_t i = 0; i < 16; ++i) {
            size_t position = offset + i * 4u;
            words[i] = (static_cast<uint32_t>(bytes[position]) << 24u) |
                       (static_cast<uint32_t>(bytes[position + 1]) << 16u) |
                       (static_cast<uint32_t>(bytes[position + 2]) << 8u) |
                       static_cast<uint32_t>(bytes[position + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            uint32_t s0 = rotate_right(words[i - 15], 7) ^
                          rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3u);
            uint32_t s1 = rotate_right(words[i - 2], 17) ^
                          rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10u);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (size_t i = 0; i < 64; ++i) {
            uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                            rotate_right(e, 25);
            uint32_t choose = (e & f) ^ (~e & g);
            uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
            uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                            rotate_right(a, 22);
            uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(64);
    for (uint32_t word : state)
        for (int shift = 28; shift >= 0; shift -= 4)
            digest.push_back(hex[(word >> shift) & 0x0fu]);
    return digest;
}

}  // namespace sbxhash
