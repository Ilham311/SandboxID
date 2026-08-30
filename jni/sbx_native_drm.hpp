#ifndef SBX_NATIVE_DRM_HPP
#define SBX_NATIVE_DRM_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <dobby.h>
#include "sbx_ident_synth.hpp"

namespace sbxdrm {

struct SbxByteArray { const uint8_t* ptr; size_t length; };

typedef int (*getpropbytearray_fn)(void* , const char*, SbxByteArray*);

inline getpropbytearray_fn g_orig = nullptr;
inline uint8_t             g_wv_bytes[32];
inline size_t              g_wv_len = 0;

inline int sbx_amediadrm_getpropbytearray(void* mObj, const char* prop, SbxByteArray* out) {
    if (prop && out && g_wv_len == sizeof(g_wv_bytes) &&
        std::strcmp(prop, "deviceUniqueId") == 0) {
        out->ptr    = g_wv_bytes;
        out->length = g_wv_len;
        return 0;
    }
    if (g_orig) return g_orig(mObj, prop, out);
    return -10000;
}

inline bool decode_hex32(const std::string& hex) {
    if (hex.size() < sizeof(g_wv_bytes) * 2) return false;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < sizeof(g_wv_bytes); ++i) {
        int hi = hv(hex[2 * i]), lo = hv(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        g_wv_bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

inline bool install(uint64_t seed) {
    static bool done = false;
    if (done) return g_orig != nullptr;
    done = true;

    if (!decode_hex32(sbxid::synth_widevine_hex(seed))) return false;
    g_wv_len = sizeof(g_wv_bytes);

    void* h = dlopen("libmediandk.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libmediandk.so", RTLD_NOW);
    if (!h) return false;

    void* addr = dlsym(h, "AMediaDrm_getPropertyByteArray");
    if (!addr) return false;

    if (DobbyHook(addr, reinterpret_cast<void*>(sbx_amediadrm_getpropbytearray),
                  reinterpret_cast<void**>(&g_orig)) != 0) {
        g_orig = nullptr;
        return false;
    }
    return true;
}

}
#endif
