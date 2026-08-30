#pragma once
// Minimal Dobby stub for host syntax-checking the L3 layer. Never linked.
// Mirrors the two entry points jni/sbx_lsplant.hpp uses.
extern "C" {
int DobbyHook(void* address, void* replace, void** origin);
int DobbyDestroy(void* address);
}
