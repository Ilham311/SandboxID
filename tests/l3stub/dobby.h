#pragma once

extern "C" {
int DobbyHook(void* address, void* replace, void** origin);
int DobbyDestroy(void* address);
}
