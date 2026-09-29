#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lua_State;

/** Register extended watch.* fields onto the watch table currently at stack top-1. */
void luaApiExtRegister(struct lua_State* L);

/** Unref extended handlers (on_back, on_long_press, slider cbs). */
void luaApiExtUnref(struct lua_State* L);

/** Poll gestures / long-press; call from luaHostPoll. */
void luaApiExtPoll(void);

/** Reset per-session gesture/slider state (on open/close). */
void luaApiExtSessionReset(void);

/** Native Back / swipe-back: if on_back set, invoke it (return true=handled). */
bool luaApiExtInvokeOnBack(void);

/** Feed touch samples from main touchpad (pressed + coords). */
void luaApiExtTouchSample(int x, int y, bool pressed);

/** True once when a right-swipe from the left edge was detected (native Back). */
bool luaApiExtTakeEdgeSwipeBack(void);

/**
 * True once when a vertical swipe_down was detected. Consumes the gesture so
 * Lua watch.gesture() will not also see it. Used by notification banner
 * (swipe-down opens the notifying app).
 */
bool luaApiExtTakeSwipeDown(void);

/** Slider VALUE_CHANGED bridge from LVGL (sliderId). */
void luaApiExtSliderChanged(int sliderId, int value);

/* ---- Host callbacks filled by main.cpp ---- */

typedef bool (*LuaExtCreateLabelFn)(int x, int y, const char* text, int r, int g, int b);
typedef bool (*LuaExtCreatePanelFn)(int x, int y, int w, int h, int r, int g, int b);
typedef bool (*LuaExtCreateProgressFn)(int id, int x, int y, int w, int h, int value);
typedef bool (*LuaExtSetProgressFn)(int id, int value);
typedef bool (*LuaExtCreateSliderFn)(int id, int x, int y, int w, int h, int minV, int maxV,
                                     int value);
typedef int (*LuaExtBrightnessGetFn)(void);
typedef int (*LuaExtBrightnessSetFn)(int percent);
typedef uint32_t (*LuaExtIdleGetFn)(void);
typedef uint32_t (*LuaExtIdleSetFn)(uint32_t sec);
typedef bool (*LuaExtNowFn)(struct tm* out, time_t* epoch);
typedef bool (*LuaExtShowSpriteFn)(const char* path, int x, int y, int fw, int fh, int frame,
                                   int max_w, int max_h);
typedef void (*LuaExtZRaiseFn)(const char* which); /* "draw"|"buttons"|"back"|"all" */

/** Create/replace RGB565 canvas on luaLayer. */
typedef bool (*LuaExtCanvasCreateFn)(int x, int y, int w, int h);
typedef bool (*LuaExtCanvasClearFn)(int r, int g, int b);
/** Scroll canvas contents by dy pixels (negative = content moves up). */
typedef bool (*LuaExtCanvasScrollFn)(int dy);
/**
 * Write one row: mags[i] in 0..1, length = nMags (stretched across canvas width).
 * Applies a heat colormap in the host.
 */
typedef bool (*LuaExtCanvasRowHeatFn)(int y, const float* mags, int nMags);
/** Set one RGB pixel in the current canvas (no-op if OOB / no canvas). */
typedef bool (*LuaExtCanvasPixelFn)(int x, int y, int r, int g, int b);
/** Show a top notification banner associated with appId (generic open-on-swipe). */
typedef void (*LuaExtNotifyFn)(const char* appId, const char* text);

void luaApiExtSetCreateLabel(LuaExtCreateLabelFn fn);
void luaApiExtSetCreatePanel(LuaExtCreatePanelFn fn);
void luaApiExtSetCreateProgress(LuaExtCreateProgressFn fn);
void luaApiExtSetSetProgress(LuaExtSetProgressFn fn);
void luaApiExtSetCreateSlider(LuaExtCreateSliderFn fn);
void luaApiExtSetBrightnessGet(LuaExtBrightnessGetFn fn);
void luaApiExtSetBrightnessSet(LuaExtBrightnessSetFn fn);
void luaApiExtSetIdleGet(LuaExtIdleGetFn fn);
void luaApiExtSetIdleSet(LuaExtIdleSetFn fn);
void luaApiExtSetNow(LuaExtNowFn fn);
void luaApiExtSetShowSprite(LuaExtShowSpriteFn fn);
void luaApiExtSetZRaise(LuaExtZRaiseFn fn);
void luaApiExtSetCanvasCreate(LuaExtCanvasCreateFn fn);
void luaApiExtSetCanvasClear(LuaExtCanvasClearFn fn);
void luaApiExtSetCanvasScroll(LuaExtCanvasScrollFn fn);
void luaApiExtSetCanvasRowHeat(LuaExtCanvasRowHeatFn fn);
void luaApiExtSetCanvasPixel(LuaExtCanvasPixelFn fn);
void luaApiExtSetNotify(LuaExtNotifyFn fn);

/** One-shot launch intent for notif swipe-open (or other deep links). */
void luaApiExtSetLaunchIntent(const char* action, const char* text);

#ifdef __cplusplus
}
#endif

/** Thin C accessors so wasm_host can reuse the same LVGL callbacks as Lua. */
bool hostUiLabel(int x, int y, const char* text, int r, int g, int b);
bool hostUiCanvasCreate(int x, int y, int w, int h);
bool hostUiCanvasClear(int r, int g, int b);
bool hostUiCanvasScroll(int dy);
bool hostUiCanvasRowHeat(int y, const float* mags, int nMags);
bool hostUiCanvasPixel(int x, int y, int r, int g, int b);
bool hostUiNow(struct tm* out, time_t* epoch);
