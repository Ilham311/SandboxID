#pragma once

#include <cstdint>

namespace tt {
namespace proto {

inline constexpr std::uint8_t MAGIC0  = 'T';
inline constexpr std::uint8_t MAGIC1  = 'T';
inline constexpr std::uint8_t MAGIC2  = 0;
inline constexpr std::uint8_t VERSION = 1;

enum : std::uint8_t {
    CMD_INIT_APP = 4,
};

struct __attribute__((packed)) Header {
    std::uint8_t  magic[3];
    std::uint8_t  version;
    std::uint8_t  cmd;
    std::uint8_t  _pad[3];
    std::uint32_t payload_len;
};

struct __attribute__((packed)) InitAppRequestPayload {
    std::uint32_t pid;
    std::uint16_t pkg_len;
};

struct __attribute__((packed)) InitAppResponse {
    Header        hdr;
    std::uint16_t is_target;
    std::uint16_t mount_ok;
    std::uint32_t blob_len;
    std::uint16_t nkeys;
    std::uint16_t _pad;
};

struct __attribute__((packed)) BinaryEntry {
    std::uint16_t klen;
    std::uint16_t vlen;
};

inline void fill_header(Header& h, std::uint8_t cmd, std::uint32_t payload) noexcept {
    h.magic[0]    = MAGIC0;
    h.magic[1]    = MAGIC1;
    h.magic[2]    = MAGIC2;
    h.version     = VERSION;
    h.cmd         = cmd;
    h._pad[0]     = 0;
    h._pad[1]     = 0;
    h._pad[2]     = 0;
    h.payload_len = payload;
}

[[nodiscard]] inline bool check_header(const Header& h, std::uint8_t expect_cmd) noexcept {
    return h.magic[0] == MAGIC0 &&
           h.magic[1] == MAGIC1 &&
           h.magic[2] == MAGIC2 &&
           h.version  == VERSION &&
           h.cmd      == expect_cmd;
}

}
}
