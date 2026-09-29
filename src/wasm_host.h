#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Soft cap for loaded .wasm bytes (PSRAM preferred). */
#ifndef WASM_HOST_MAX_BYTES
#define WASM_HOST_MAX_BYTES (256u * 1024u)
#endif

typedef void (*WasmHostTitleFn)(const char* title);
typedef void (*WasmHostTextFn)(const char* text);
typedef void (*WasmHostBackFn)(void);
typedef int (*WasmHostBatteryFn)(void);  // 0..100 or -1

void wasmHostSetTitleCallback(WasmHostTitleFn fn);
void wasmHostSetTextCallback(WasmHostTextFn fn);
void wasmHostSetBackCallback(WasmHostBackFn fn);
void wasmHostSetBatteryCallback(WasmHostBatteryFn fn);

/**
 * Load a .wasm module from LittleFS or /sd/... into RAM/PSRAM, link host
 * imports, call init/_start. Closes any previous WASM session first.
 * Does not close Lua — caller should luaHostClose() when switching.
 */
bool wasmHostOpen(const char* wasmPath, char* errBuf, size_t errLen);

/** Tear down runtime, free module bytes, stop mic. Safe if nothing open. */
void wasmHostClose(void);

/** Call exported tick(dt_ms) when present (~every frame / ~50ms). */
void wasmHostPoll(void);

bool wasmHostIsOpen(void);

/**
 * If the module exports on_back / onBack, call it.
 * Returns true if the export returned non-zero (app handled back).
 * If no export, returns false (host should navigate).
 */
bool wasmHostInvokeOnBack(void);

#ifdef __cplusplus
}
#endif
