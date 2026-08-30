#ifndef SBX_NATIVE_DRM_HPP
#define SBX_NATIVE_DRM_HPP
// NDK MediaDrm (libmediandk) Widevine-ID spoof — the native twin of the L3 Java
// hook on android.media.MediaDrm.getPropertyByteArray("deviceUniqueId").
//
// Some apps read the Widevine device-unique-id through the NDK C API
//   media_status_t AMediaDrm_getPropertyByteArray(AMediaDrm*, const char* name,
//                                                 AMediaDrmByteArray* out);
// instead of the Java MediaDrm class. That call is a direct export of
// libmediandk.so invoked by the app's own native code, so it never crosses the
// property layers (L2/L7/L9) and cannot be reached by a PLT hook on the app's
// libs (the function is DEFINED inside the very lib the app calls). It needs a
// Dobby INLINE hook on the resolved absolute address — the one place in this
// module Dobby patches a native C function directly (elsewhere Dobby is only
// LSPlant's ART inline_hooker).
//
// The spoofed 32 bytes come from sbxid::synth_widevine_hex(seed) — the SAME pure
// function and seed the L3 Java hook uses (sbx_lsplant.hpp install_all), so both
// surfaces return a byte-identical Widevine id that rotates with the persona.
//
// Fail-soft everywhere: if libmediandk is absent, the symbol is unresolved, or
// DobbyHook fails, we log and return — the app keeps working and the Java hook
// still covers the common path. Host-syntax-checked via tests/l3stub (DobbyHook
// is stubbed; the AMediaDrm types below are self-contained, so no NDK header is
// required and nothing new is linked — the real symbol is reached with dlopen).
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <dobby.h>
#include "sbx_ident_synth.hpp"   // sbxid::synth_widevine_hex

namespace sbxdrm {

// Layout-compatible mirror of NDK's AMediaDrmByteArray { const uint8_t* ptr;
// size_t length; }. We only ever fill it (repoint ptr/length at our own static
// buffer), so ABI layout is all that matters — using our own name avoids any
// redefinition clash if a real <media/NdkMediaDrm.h> is ever pulled in.
struct SbxByteArray { const uint8_t* ptr; size_t length; };

// media_status_t is a plain enum (underlying int); AMEDIA_OK == 0.
typedef int (*getpropbytearray_fn)(void* /*AMediaDrm* */, const char*, SbxByteArray*);

inline getpropbytearray_fn g_orig = nullptr;
inline uint8_t             g_wv_bytes[32];
inline size_t              g_wv_len = 0;

// Replacement for AMediaDrm_getPropertyByteArray. Only "deviceUniqueId" is
// substituted — every other property (and any call before our bytes are staged)
// passes straight through to the original. Reads no session state, so it is safe
// even when the real call would have failed (no crypto session).
inline int sbx_amediadrm_getpropbytearray(void* mObj, const char* prop, SbxByteArray* out) {
    if (prop && out && g_wv_len == sizeof(g_wv_bytes) &&
        std::strcmp(prop, "deviceUniqueId") == 0) {
        out->ptr    = g_wv_bytes;      // static storage: outlives the call, caller-safe
        out->length = g_wv_len;
        return 0;                      // AMEDIA_OK
    }
    if (g_orig) return g_orig(mObj, prop, out);
    return -10000;                     // AMEDIA_ERROR_UNKNOWN (only if hook installed w/o origin)
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

// Install once per process. `seed` is the persona seed (fnv1a(FP|SERIAL|ANDROID_ID)).
// Returns true only when the inline hook is actually armed.
inline bool install(uint64_t seed) {
    static bool done = false;
    if (done) return g_orig != nullptr;
    done = true;

    if (!decode_hex32(sbxid::synth_widevine_hex(seed))) return false;  // never: hex is 64 chars
    g_wv_len = sizeof(g_wv_bytes);

    // Force-load libmediandk: an app using NDK DRM usually loads it lazily on
    // first use, i.e. AFTER postAppSpecialize, so RTLD_NOLOAD alone would miss
    // the common case. It is a standard system lib present on every device.
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

} // namespace sbxdrm
#endif // SBX_NATIVE_DRM_HPP
