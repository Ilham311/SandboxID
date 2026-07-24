// ============================================================
// Ternak TT v1.0.1 - Zygisk companion (minimal reader)
//
// v1.0.0 punya UDS listener di sini yang cuma jalan setelah
// TikTok pertama kali di-launch. Bug: CLI dari Termux gak bisa
// connect kalo TT belum pernah dibuka.
//
// v1.0.1: semua logic freshen pindah ke ternak-tt.cpp (CLI).
// Companion cuma read identity.prop dan kirim ke Zygisk hook.
// ============================================================
#include <unistd.h>
#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <android/log.h>

#define LOG_TAG "TernakTTCompanion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

enum : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,
};

static const char* IDENTITY_FILE = "/data/adb/modules/ternak_tt/identity.prop";

static std::string read_file(const char* p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

extern "C" void ternak_tt_companion(int client) {
    while (true) {
        uint8_t cmd = 0;
        if (::read(client, &cmd, 1) != 1) break;
        if (cmd == CMD_GET_IDENTITY) {
            std::string d = read_file(IDENTITY_FILE);
            uint32_t l = (uint32_t)d.size();
            ::write(client, &l, sizeof(l));
            if (l) ::write(client, d.data(), l);
        } else {
            break;
        }
    }
    ::close(client);
}
