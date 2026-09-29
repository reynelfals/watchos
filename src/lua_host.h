#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*LuaHostTitleFn)(const char* title);
typedef void (*LuaHostTextFn)(const char* text);
typedef void (*LuaHostAppendFn)(const char* text);
typedef void (*LuaHostClearFn)(void);

/** Get display size (pixels). Optional — host falls back to 410x502. */
typedef void (*LuaHostGetSizeFn)(int* width, int* height);
/** Set stub screen background RGB (0-255). */
typedef void (*LuaHostSetBgFn)(int r, int g, int b);
/** Remove all Lua-created widgets (buttons, shapes, labels). */
typedef void (*LuaHostClearWidgetsFn)(void);
/**
 * Create a button with label. buttonId is 0..(kMaxButtons-1) (host assigns).
 * On click, firmware must call luaHostButtonClicked(buttonId).
 * Return true on success.
 */
typedef bool (*LuaHostCreateButtonFn)(int buttonId, const char* label);
/**
 * Button layout: mode "column" | "grid2"; cols is columns for grid (default 2).
 * Return true on success.
 */
typedef bool (*LuaHostButtonLayoutFn)(const char* mode, int cols);
/**
 * Last / current touch sample. Return false if no press (Lua gets nil).
 * On true, fill x/y and set *pressed (typically true while finger down).
 */
typedef bool (*LuaHostTouchFn)(int* x, int* y, bool* pressed);
typedef void (*LuaHostCreateRectFn)(int x, int y, int w, int h, int r, int g, int b);
typedef void (*LuaHostCreateCircleFn)(int x, int y, int radius, int r, int g, int b);
typedef void (*LuaHostCreateLabelFn)(int x, int y, const char* text, int r, int g, int b);
/** Clear only luaLayer shapes (rects/circles/labels/ball). Keeps buttons + letter pad. */
typedef void (*LuaHostGfxClearFn)(void);
/**
 * Create or move a single persistent ball circle on luaLayer.
 * Center at (x,y). If no ball yet, create; else set_pos and update color/size.
 */
typedef void (*LuaHostBallFn)(int x, int y, int radius, int r, int g, int b);

/**
 * Battery percent 0..100, or negative if unavailable.
 * watch.battery() / watch.battery_pct() return that number, or nil when negative.
 */
typedef int (*LuaHostBatteryFn)(void);
/**
 * Battery voltage in millivolts from AXP2101 ADC, or negative if unavailable.
 * watch.battery_mv() returns that number, or nil when negative.
 */
typedef int (*LuaHostBatteryMvFn)(void);
/**
 * Charging status from AXP2101: 1=charging, 0=not, negative if PMIC unavailable.
 * watch.charging() returns bool, or nil when unavailable (never faked).
 */
typedef int (*LuaHostChargingFn)(void);
/**
 * USB/VBUS present from AXP2101: 1=yes, 0=no, negative if PMIC unavailable.
 * watch.usb_power() returns bool, or nil when unavailable.
 */
typedef int (*LuaHostUsbPowerFn)(void);
/**
 * Battery pack detected: 1=yes, 0=no, negative if PMIC unavailable.
 * watch.battery_connected() returns bool, or nil when unavailable.
 */
typedef int (*LuaHostBatteryConnectedFn)(void);
/** Request return to launcher (deferred — do not navigate inside Lua pcall). */
typedef void (*LuaHostBackFn)(void);
/** Haptic: host drives vibration motor (GPIO18); no-op if unset. */
typedef void (*LuaHostVibrateFn)(void);
/**
 * Wi‑Fi / OTA state string: "off" | "ota" | "on".
 * "ota" = Install/OTA SoftAP up; "on" reserved for future STA; "off" default.
 */
typedef const char* (*LuaHostWifiStateFn)(void);
/** Display brightness 0..100. Return actual clamped value, or -1 if unavailable. */
typedef int (*LuaHostBrightnessFn)(int percent);
/**
 * IMU sample stub. Return false until a driver is wired; true fills ax..gz.
 * Units are host-defined (typically g and deg/s).
 */
typedef bool (*LuaHostImuFn)(float* ax, float* ay, float* az,
                             float* gx, float* gy, float* gz);
/**
 * Create (or show) an A–Z letter pad on the Lua app screen.
 * On key press, firmware must call luaHostLetterPadKey(txt).
 * Return true on success.
 */
typedef bool (*LuaHostCreateLetterPadFn)(void);

/**
 * Show a WRGB image at (x,y). max_w/max_h clamp display size (0 => 160).
 * Path: /sd/... via sd_host, else LittleFS. Return true on success.
 */
typedef bool (*LuaHostShowImageFn)(const char* path, int x, int y, int max_w, int max_h);
/** Free previous image buffer and LVGL image object. */
typedef void (*LuaHostClearImageFn)(void);

/** Optional; currently a no-op reserved for future init. */
void luaHostBegin(void);

/** Register LVGL (or other) UI callbacks. Pass NULL to clear a hook. */
void luaHostSetTitleCallback(LuaHostTitleFn fn);
void luaHostSetTextCallback(LuaHostTextFn fn);
void luaHostSetAppendCallback(LuaHostAppendFn fn);
void luaHostSetClearCallback(LuaHostClearFn fn);
void luaHostSetGetSizeCallback(LuaHostGetSizeFn fn);
void luaHostSetSetBgCallback(LuaHostSetBgFn fn);
void luaHostSetClearWidgetsCallback(LuaHostClearWidgetsFn fn);
void luaHostSetCreateButtonCallback(LuaHostCreateButtonFn fn);
void luaHostSetCreateRectCallback(LuaHostCreateRectFn fn);
void luaHostSetCreateCircleCallback(LuaHostCreateCircleFn fn);
void luaHostSetCreateLabelCallback(LuaHostCreateLabelFn fn);
void luaHostSetGfxClearCallback(LuaHostGfxClearFn fn);
void luaHostSetBallCallback(LuaHostBallFn fn);
void luaHostSetBatteryCallback(LuaHostBatteryFn fn);
void luaHostSetBatteryMvCallback(LuaHostBatteryMvFn fn);
void luaHostSetChargingCallback(LuaHostChargingFn fn);
void luaHostSetUsbPowerCallback(LuaHostUsbPowerFn fn);
void luaHostSetBatteryConnectedCallback(LuaHostBatteryConnectedFn fn);
void luaHostSetBackCallback(LuaHostBackFn fn);
void luaHostSetVibrateCallback(LuaHostVibrateFn fn);
void luaHostSetWifiStateCallback(LuaHostWifiStateFn fn);
void luaHostSetBrightnessCallback(LuaHostBrightnessFn fn);
void luaHostSetImuCallback(LuaHostImuFn fn);
void luaHostSetCreateLetterPadCallback(LuaHostCreateLetterPadFn fn);
void luaHostSetButtonLayoutCallback(LuaHostButtonLayoutFn fn);
void luaHostSetTouchCallback(LuaHostTouchFn fn);
void luaHostSetShowImageCallback(LuaHostShowImageFn fn);
void luaHostSetClearImageCallback(LuaHostClearImageFn fn);

/**
 * Load and pcall a script from LittleFS or microSD (/sd/...), keeping a single
 * active lua_State open for the app session (buttons / on_tick). Closes any
 * previous session first. On failure writes a short message into errBuf (if
 * provided) and leaves no open state.
 */
bool luaHostOpen(const char* scriptPath, char* errBuf, size_t errLen);

/** Unref handlers, destroy widgets via callback, lua_close, clear state. */
void luaHostClose(void);

/** Call on_tick if set (throttled ~200ms). Safe when no app is open. */
void luaHostPoll(void);

bool luaHostIsOpen(void);

/** LVGL click bridge — invoke the Lua handler for buttonId (0..kMaxButtons-1). */
void luaHostButtonClicked(int buttonId);

/**
 * Letter-pad bridge — invoke the Lua letter_pad callback with key text
 * ("A".."Z", "BKSP", or "CLR").
 */
void luaHostLetterPadKey(const char* key);

/**
 * One-shot helper: open, run (session kept... actually for back-compat this
 * used to destroy). Prefer luaHostOpen for interactive apps.
 * Kept as alias that opens a session (same as luaHostOpen).
 */
bool luaHostRunFile(const char* scriptPath, char* errBuf, size_t errLen);

#ifdef __cplusplus
}
#endif
