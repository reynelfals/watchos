#include "native_apps.h"

#include <string.h>

// Forward: sample app implemented in native_sysinfo.cpp
extern void nativeSysInfoLaunch();
extern lv_obj_t* nativeSysInfoScreen();

/**
 * Compile-in registry. Add a new .cpp with a launch fn, then append a row here
 * and rebuild firmware. Do not accept uploaded "native" packages as executable.
 */
static const NativeAppDef kNativeApps[] = {
    {"sysinfo", "System Info", "1.0.0", nullptr, true, nativeSysInfoLaunch},
};

static constexpr size_t kNativeAppsN =
    sizeof(kNativeApps) / sizeof(kNativeApps[0]);

size_t nativeAppsCount() { return kNativeAppsN; }

const NativeAppDef* nativeAppsGet(size_t index) {
  if (index >= kNativeAppsN) return nullptr;
  return &kNativeApps[index];
}

int nativeAppsIndexOf(const char* id) {
  if (!id || !id[0]) return -1;
  for (size_t i = 0; i < kNativeAppsN; ++i) {
    if (strcmp(kNativeApps[i].id, id) == 0) return (int)i;
  }
  return -1;
}

bool nativeAppsIsId(const char* id) { return nativeAppsIndexOf(id) >= 0; }

bool nativeAppsLaunch(int index) {
  if (index < 0 || (size_t)index >= kNativeAppsN) return false;
  if (!kNativeApps[index].launch) return false;
  kNativeApps[index].launch();
  return true;
}

bool nativeAppsOwnsScreen(lv_obj_t* scr) {
  if (!scr) return false;
  lv_obj_t* sys = nativeSysInfoScreen();
  return sys != nullptr && scr == sys;
}

void nativeAppsBuildScreens() {
  // Screens are built lazily on first launch; nothing required at boot.
  // Hook kept for future eager init / more apps.
}
