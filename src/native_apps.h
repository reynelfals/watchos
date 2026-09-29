#pragma once

#include <stddef.h>
#include <stdint.h>
#include <lvgl.h>

/**
 * Firmware-only native app registry (compile-in).
 * Lua/catalog packages stay on LittleFS/SD; native apps are never uploaded.
 */

struct NativeAppDef {
  const char* id;
  const char* name;
  const char* version;
  const char* iconPath;  // optional absolute WRGB path, or nullptr → letter tile
  bool is_protected;     // always true for built-ins
  void (*launch)();      // show native LVGL screen
};

/** Number of entries in the compile-in table. */
size_t nativeAppsCount();

/** Pointer to registry entry, or nullptr if out of range. */
const NativeAppDef* nativeAppsGet(size_t index);

/** True if id is reserved by a firmware native app. */
bool nativeAppsIsId(const char* id);

/** Index of native id, or -1. */
int nativeAppsIndexOf(const char* id);

/** Call registered launch callback. Returns false if bad index / null launch. */
bool nativeAppsLaunch(int index);

/**
 * Host hooks implemented by main.cpp so native screens can navigate
 * and read battery without pulling the whole shell.
 */
void nativeHostGoLauncher();
void nativeHostNoteActivity();
int nativeHostBatteryPercent();  // 0..100 or -1
int nativeHostBatteryMv();       // mV or -1

/** Soft-lock / lifecycle: true if active LVGL screen belongs to a native app. */
bool nativeAppsOwnsScreen(lv_obj_t* scr);

/** Build native screens once at boot (idempotent). */
void nativeAppsBuildScreens();

/** Sample: System Info (chip / heap / battery / uptime). */
void nativeSysInfoLaunch();
lv_obj_t* nativeSysInfoScreen();
