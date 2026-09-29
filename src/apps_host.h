#pragma once

#include <Arduino.h>
#include <stdint.h>

// App catalog: LittleFS /apps/*/app.json + optional /sd/apps/*/app.json
// + firmware-only native apps from the compile-in registry (native_apps.h).

static constexpr size_t APPS_MAX = 24;
static constexpr size_t APP_ID_MAX = 32;
static constexpr size_t APP_NAME_MAX = 48;
static constexpr size_t APP_VER_MAX = 16;
static constexpr size_t APP_ENTRY_MAX = 48;
static constexpr size_t APP_PATH_MAX = 80;
static constexpr size_t APP_SCRIPT_MAX = 96;
static constexpr size_t APP_ICON_MAX = 48;

/** Where the package lives. On id clash, LittleFS wins (SD copy ignored). */
enum AppStorage : uint8_t {
  APP_STORAGE_LFS = 0,
  APP_STORAGE_SD = 1,
};

/** LUA / WASM = LittleFS/SD package; NATIVE = compile-in firmware registry. */
enum AppKind : uint8_t {
  APP_KIND_LUA = 0,
  APP_KIND_NATIVE = 1,
  APP_KIND_WASM = 2,
};

struct AppInfo {
  char id[APP_ID_MAX];
  char name[APP_NAME_MAX];
  char version[APP_VER_MAX];
  char entry[APP_ENTRY_MAX];       // relative, e.g. main.lua / main.wasm
  char folder[APP_PATH_MAX];       // /apps/<id> or /sd/apps/<id>
  char script[APP_SCRIPT_MAX];     // full entry path folder/entry
  AppStorage storage;
  bool is_protected;               // app.json "protected": true — uninstall refused
  char icon[APP_ICON_MAX];         // relative WRGB, or absolute path for native
  AppKind kind;                    // LUA (default), WASM, or NATIVE
  int16_t native_index;            // registry index when kind==NATIVE; else -1
};

bool appsHostBegin();          // LittleFS.begin(false); then rescan
size_t appsHostCount();
const AppInfo* appsHostGet(size_t index);
/** Lookup by app id (catalog scan). nullptr if unknown. */
const AppInfo* appsHostFindById(const char* id);
void appsHostRescan();
bool appsHostFsOk();

/**
 * Sanitize folderOrId to a safe app id (alnum / _ / - only).
 * Accepts bare id or path-like "/apps/My_app" (uses last segment).
 * Rejects empty, ".", "..", path escape, and any other characters.
 */
bool appsHostSanitizeId(const char* folderOrId, char* outId, size_t outSz);

/**
 * Recursively delete the installed package for this id, then appsHostRescan().
 * Uses catalog storage when known (LittleFS /apps/<id>/ or SD /sd/apps/<id>/).
 * Safe path checks (must resolve under the chosen apps root, no "..").
 * Returns false if bad id, missing, protected, native/built-in, or delete failed.
 */
bool appsHostRemove(const char* folderOrId);
