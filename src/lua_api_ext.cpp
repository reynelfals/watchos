#include "lua_api_ext.h"
#include "sd_host.h"
#include "zip_install.h"
#include "audio_host.h"
#include "job_host.h"
#include "ble_mesh_host.h"
#include "wifi_ota.h"
#include "pcf85063.h"
#include "pin_config.h"

#include <Arduino.h>
#include <lvgl.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <FS.h>
#include <LittleFS.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static constexpr int kMaxSliders = 4;
static constexpr int kMaxProgress = 4;
static constexpr size_t kHttpMaxBytes = 32 * 1024;
static constexpr size_t kHttpHardCap = 64 * 1024;
static constexpr uint32_t kHttpTimeoutMs = 8000;
static constexpr int kLongPressMs = 600;
static constexpr int kSwipeMinPx = 80;
static constexpr int kSwipeMaxOrtho = 50;

/** Shared mic PCM scratch — avoid 2 KiB on loopTask stack under Lua. */
static int16_t gMicReadScratch[AUDIO_MIC_READ_MAX];
static float gMicSpectrumScratch[128];
static float gCanvasRowScratch[256];

/** Let LVGL flush "recording/playing..." before we block the UI task. */
static void luaAudioFlushUi(void) {
  lv_timer_handler();
}

static LuaExtCreateLabelFn gCreateLabel = nullptr;
static LuaExtCreatePanelFn gCreatePanel = nullptr;
static LuaExtCreateProgressFn gCreateProgress = nullptr;
static LuaExtSetProgressFn gSetProgress = nullptr;
static LuaExtCreateSliderFn gCreateSlider = nullptr;
static LuaExtBrightnessGetFn gBrightnessGet = nullptr;
static LuaExtBrightnessSetFn gBrightnessSet = nullptr;
static LuaExtIdleGetFn gIdleGet = nullptr;
static LuaExtIdleSetFn gIdleSet = nullptr;
static LuaExtNowFn gNowFn = nullptr;
static LuaExtShowSpriteFn gShowSprite = nullptr;
static LuaExtZRaiseFn gZRaise = nullptr;
static LuaExtCanvasCreateFn gCanvasCreate = nullptr;
static LuaExtCanvasClearFn gCanvasClear = nullptr;
static LuaExtCanvasScrollFn gCanvasScroll = nullptr;
static LuaExtCanvasRowHeatFn gCanvasRowHeat = nullptr;
static LuaExtCanvasPixelFn gCanvasPixel = nullptr;
static LuaExtNotifyFn gNotify = nullptr;

static lua_State* gExtL = nullptr;
static int gOnBackRef = LUA_NOREF;
static int gOnLongPressRef = LUA_NOREF;
static int gSliderRefs[kMaxSliders];
static int gSliderCount = 0;

static bool gLuaWifiOn = false;
static bool gLuaWifiConnecting = false;

// Gesture / long-press tracking
static bool gTouchDown = false;
static bool gLongFired = false;
static int gTouchStartX = 0, gTouchStartY = 0;
static uint32_t gTouchStartMs = 0;
static int gTouchCurX = 0, gTouchCurY = 0;
static char gLastGesture[24] = "";
static int gLastGDx = 0, gLastGDy = 0;
static bool gHaveGesture = false;
static bool gEdgeSwipeBack = false;
static int gGestureStartX = 0;

// GPIO whitelist (not used by AMOLED/QSPI/I2C/SD/motor/UART on this board)
static const int kGpioSafe[] = {10, 16, 19, 20};
static constexpr int kGpioSafeN = 4;

void luaApiExtSetCreateLabel(LuaExtCreateLabelFn fn) { gCreateLabel = fn; }
void luaApiExtSetCreatePanel(LuaExtCreatePanelFn fn) { gCreatePanel = fn; }
void luaApiExtSetCreateProgress(LuaExtCreateProgressFn fn) { gCreateProgress = fn; }
void luaApiExtSetSetProgress(LuaExtSetProgressFn fn) { gSetProgress = fn; }
void luaApiExtSetCreateSlider(LuaExtCreateSliderFn fn) { gCreateSlider = fn; }
void luaApiExtSetBrightnessGet(LuaExtBrightnessGetFn fn) { gBrightnessGet = fn; }
void luaApiExtSetBrightnessSet(LuaExtBrightnessSetFn fn) { gBrightnessSet = fn; }
void luaApiExtSetIdleGet(LuaExtIdleGetFn fn) { gIdleGet = fn; }
void luaApiExtSetIdleSet(LuaExtIdleSetFn fn) { gIdleSet = fn; }
void luaApiExtSetNow(LuaExtNowFn fn) { gNowFn = fn; }
void luaApiExtSetShowSprite(LuaExtShowSpriteFn fn) { gShowSprite = fn; }
void luaApiExtSetZRaise(LuaExtZRaiseFn fn) { gZRaise = fn; }
void luaApiExtSetCanvasCreate(LuaExtCanvasCreateFn fn) { gCanvasCreate = fn; }
void luaApiExtSetCanvasClear(LuaExtCanvasClearFn fn) { gCanvasClear = fn; }
void luaApiExtSetCanvasScroll(LuaExtCanvasScrollFn fn) { gCanvasScroll = fn; }
void luaApiExtSetCanvasRowHeat(LuaExtCanvasRowHeatFn fn) { gCanvasRowHeat = fn; }
void luaApiExtSetCanvasPixel(LuaExtCanvasPixelFn fn) { gCanvasPixel = fn; }
void luaApiExtSetNotify(LuaExtNotifyFn fn) { gNotify = fn; }

static char gLaunchAction[32] = {0};
static char gLaunchText[96] = {0};

void luaApiExtSetLaunchIntent(const char* action, const char* text) {
  if (!action) action = "";
  if (!text) text = "";
  strncpy(gLaunchAction, action, sizeof(gLaunchAction) - 1);
  gLaunchAction[sizeof(gLaunchAction) - 1] = '\0';
  strncpy(gLaunchText, text, sizeof(gLaunchText) - 1);
  gLaunchText[sizeof(gLaunchText) - 1] = '\0';
}


static bool gpioAllowed(int pin) {
  for (int i = 0; i < kGpioSafeN; ++i) if (kGpioSafe[i] == pin) return true;
  return false;
}

static bool prefsKeyOk(const char* key) {
  if (!key || !key[0] || strlen(key) > 15) return false;
  for (const char* p = key; *p; ++p) {
    if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
  }
  return true;
}

void luaApiExtSessionReset(void) {
  gOnBackRef = LUA_NOREF;
  gOnLongPressRef = LUA_NOREF;
  gSliderCount = 0;
  for (int i = 0; i < kMaxSliders; ++i) gSliderRefs[i] = LUA_NOREF;
  gHaveGesture = false;
  gEdgeSwipeBack = false;
  gLastGesture[0] = '\0';
  gTouchDown = false;
  gLongFired = false;
  gExtL = nullptr;
}

void luaApiExtUnref(lua_State* L) {
  if (!L) return;
  if (gOnBackRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, gOnBackRef); gOnBackRef = LUA_NOREF; }
  if (gOnLongPressRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, gOnLongPressRef); gOnLongPressRef = LUA_NOREF; }
  for (int i = 0; i < kMaxSliders; ++i) {
    if (gSliderRefs[i] != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, gSliderRefs[i]);
      gSliderRefs[i] = LUA_NOREF;
    }
  }
  gSliderCount = 0;
  gExtL = nullptr;
}

bool luaApiExtInvokeOnBack(void) {
  if (!gExtL || gOnBackRef == LUA_NOREF) return false;
  lua_rawgeti(gExtL, LUA_REGISTRYINDEX, gOnBackRef);
  if (!lua_isfunction(gExtL, -1)) { lua_pop(gExtL, 1); return false; }
  int st = lua_pcall(gExtL, 0, 1, 0);
  if (st != LUA_OK) {
    const char* msg = lua_tostring(gExtL, -1);
    Serial.printf("lua on_back error: %s\n", msg ? msg : "?");
    lua_pop(gExtL, 1);
    return true; // handled (even if errored) — do not double-exit
  }
  bool handled = true;
  if (lua_isboolean(gExtL, -1) && !lua_toboolean(gExtL, -1)) handled = false;
  lua_pop(gExtL, 1);
  return handled;
}

void luaApiExtTouchSample(int x, int y, bool pressed) {
  uint32_t now = millis();
  if (pressed) {
    if (!gTouchDown) {
      gTouchDown = true;
      gLongFired = false;
      gTouchStartX = x;
      gTouchStartY = y;
      gTouchStartMs = now;
      gGestureStartX = x;
    }
    gTouchCurX = x;
    gTouchCurY = y;
  } else if (gTouchDown) {
    int dx = gTouchCurX - gTouchStartX;
    int dy = gTouchCurY - gTouchStartY;
    gTouchDown = false;
    if (!gLongFired) {
      if (abs(dx) >= kSwipeMinPx && abs(dy) <= kSwipeMaxOrtho) {
        strncpy(gLastGesture, dx < 0 ? "swipe_left" : "swipe_right", sizeof(gLastGesture) - 1);
        gLastGDx = dx; gLastGDy = dy; gHaveGesture = true;
        if (dx > 0 && gGestureStartX < 40) gEdgeSwipeBack = true;
      } else if (abs(dy) >= kSwipeMinPx && abs(dx) <= kSwipeMaxOrtho) {
        strncpy(gLastGesture, dy < 0 ? "swipe_up" : "swipe_down", sizeof(gLastGesture) - 1);
        gLastGDx = dx; gLastGDy = dy; gHaveGesture = true;
      }
    }
  }
}

bool luaApiExtTakeEdgeSwipeBack(void) {
  if (!gEdgeSwipeBack) return false;
  gEdgeSwipeBack = false;
  return true;
}

bool luaApiExtTakeSwipeDown(void) {
  if (!gHaveGesture || strcmp(gLastGesture, "swipe_down") != 0) return false;
  gHaveGesture = false;
  gLastGesture[0] = '\0';
  gLastGDx = 0;
  gLastGDy = 0;
  return true;
}

void luaApiExtPoll(void) {
  if (gTouchDown && !gLongFired && gExtL && gOnLongPressRef != LUA_NOREF) {
    if ((millis() - gTouchStartMs) >= (uint32_t)kLongPressMs) {
      gLongFired = true;
      lua_rawgeti(gExtL, LUA_REGISTRYINDEX, gOnLongPressRef);
      if (lua_isfunction(gExtL, -1)) {
        lua_pushinteger(gExtL, gTouchCurX);
        lua_pushinteger(gExtL, gTouchCurY);
        if (lua_pcall(gExtL, 2, 0, 0) != LUA_OK) {
          const char* msg = lua_tostring(gExtL, -1);
          Serial.printf("lua on_long_press error: %s\n", msg ? msg : "?");
          lua_pop(gExtL, 1);
        }
      } else {
        lua_pop(gExtL, 1);
      }
    }
  }
  // Edge swipe-back: swipe_right starting near left edge
  if (gHaveGesture && strcmp(gLastGesture, "swipe_right") == 0 && gTouchStartX < 40) {
    // Leave gesture for Lua too; native path handled in main via invoke
  }
}

void luaApiExtSliderChanged(int sliderId, int value) {
  if (!gExtL || sliderId < 0 || sliderId >= gSliderCount) return;
  int ref = gSliderRefs[sliderId];
  if (ref == LUA_NOREF) return;
  lua_rawgeti(gExtL, LUA_REGISTRYINDEX, ref);
  if (!lua_isfunction(gExtL, -1)) { lua_pop(gExtL, 1); return; }
  lua_pushinteger(gExtL, value);
  if (lua_pcall(gExtL, 1, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(gExtL, -1);
    Serial.printf("lua slider error: %s\n", msg ? msg : "?");
    lua_pop(gExtL, 1);
  }
}

// ---- Lua bindings ----

static int l_label(lua_State* L) {
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  const char* text = luaL_checkstring(L, 3);
  int r = 244, g = 247, b = 251;
  if (lua_gettop(L) >= 6) {
    r = (int)luaL_checkinteger(L, 4);
    g = (int)luaL_checkinteger(L, 5);
    b = (int)luaL_checkinteger(L, 6);
  }
  bool ok = gCreateLabel ? gCreateLabel(x, y, text, r, g, b) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_panel(lua_State* L) {
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int w = (int)luaL_checkinteger(L, 3);
  int h = (int)luaL_checkinteger(L, 4);
  int r = 20, g = 28, b = 40;
  if (lua_gettop(L) >= 7) {
    r = (int)luaL_checkinteger(L, 5);
    g = (int)luaL_checkinteger(L, 6);
    b = (int)luaL_checkinteger(L, 7);
  }
  bool ok = gCreatePanel ? gCreatePanel(x, y, w, h, r, g, b) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_progress(lua_State* L) {
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int w = (int)luaL_checkinteger(L, 3);
  int h = (int)luaL_checkinteger(L, 4);
  int value = (int)luaL_optinteger(L, 5, 0);
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  static int sProgId = 0;
  int id = sProgId % kMaxProgress;
  sProgId++;
  bool ok = gCreateProgress ? gCreateProgress(id, x, y, w, h, value) : false;
  if (ok) lua_pushinteger(L, id); else lua_pushnil(L);
  return 1;
}

static int l_progress_set(lua_State* L) {
  int id = (int)luaL_checkinteger(L, 1);
  int value = (int)luaL_checkinteger(L, 2);
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  bool ok = gSetProgress ? gSetProgress(id, value) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_slider(lua_State* L) {
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int w = (int)luaL_checkinteger(L, 3);
  int h = (int)luaL_checkinteger(L, 4);
  int minV = (int)luaL_optinteger(L, 5, 0);
  int maxV = (int)luaL_optinteger(L, 6, 100);
  int value = (int)luaL_optinteger(L, 7, minV);
  if (gSliderCount >= kMaxSliders) {
    lua_pushnil(L);
    return 1;
  }
  int id = gSliderCount;
  if (!gCreateSlider || !gCreateSlider(id, x, y, w, h, minV, maxV, value)) {
    lua_pushnil(L);
    return 1;
  }
  if (lua_gettop(L) >= 8 && lua_isfunction(L, 8)) {
    lua_pushvalue(L, 8);
    gSliderRefs[id] = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    gSliderRefs[id] = LUA_NOREF;
  }
  gSliderCount++;
  lua_pushinteger(L, id);
  return 1;
}

static int l_z_raise(lua_State* L) {
  const char* which = luaL_optstring(L, 1, "all");
  if (gZRaise) gZRaise(which);
  return 0;
}

static int l_on_back(lua_State* L) {
  if (lua_isnil(L, 1)) {
    if (gOnBackRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, gOnBackRef); gOnBackRef = LUA_NOREF; }
    return 0;
  }
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (gOnBackRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, gOnBackRef);
  lua_pushvalue(L, 1);
  gOnBackRef = luaL_ref(L, LUA_REGISTRYINDEX);
  gExtL = L;
  return 0;
}

static int l_on_long_press(lua_State* L) {
  if (lua_isnil(L, 1)) {
    if (gOnLongPressRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, gOnLongPressRef); gOnLongPressRef = LUA_NOREF; }
    return 0;
  }
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (gOnLongPressRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, gOnLongPressRef);
  lua_pushvalue(L, 1);
  gOnLongPressRef = luaL_ref(L, LUA_REGISTRYINDEX);
  gExtL = L;
  return 0;
}

static int l_gesture(lua_State* L) {
  if (!gHaveGesture) { lua_pushnil(L); return 1; }
  gHaveGesture = false;
  lua_createtable(L, 0, 3);
  lua_pushstring(L, gLastGesture); lua_setfield(L, -2, "type");
  lua_pushinteger(L, gLastGDx); lua_setfield(L, -2, "dx");
  lua_pushinteger(L, gLastGDy); lua_setfield(L, -2, "dy");
  return 1;
}

static int l_touch_delta(lua_State* L) {
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, gTouchDown ? (gTouchCurX - gTouchStartX) : 0); lua_setfield(L, -2, "dx");
  lua_pushinteger(L, gTouchDown ? (gTouchCurY - gTouchStartY) : 0); lua_setfield(L, -2, "dy");
  lua_pushboolean(L, gTouchDown ? 1 : 0); lua_setfield(L, -2, "pressed");
  lua_pushboolean(L, gLongFired ? 1 : 0); lua_setfield(L, -2, "long");
  return 1;
}

static int l_prefs_get(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);
  if (!prefsKeyOk(key)) { lua_pushnil(L); return 1; }
  Preferences prefs;
  if (!prefs.begin("wlua", true)) {
    if (lua_gettop(L) >= 2) { lua_pushvalue(L, 2); return 1; }
    lua_pushnil(L); return 1;
  }
  String v = prefs.getString(key, "");
  bool has = prefs.isKey(key);
  prefs.end();
  if (!has) {
    if (lua_gettop(L) >= 2) { lua_pushvalue(L, 2); return 1; }
    lua_pushnil(L); return 1;
  }
  lua_pushstring(L, v.c_str());
  return 1;
}

static int l_prefs_set(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);
  const char* val = luaL_checkstring(L, 2);
  if (!prefsKeyOk(key) || strlen(val) > 128) { lua_pushboolean(L, 0); return 1; }
  Preferences prefs;
  if (!prefs.begin("wlua", false)) { lua_pushboolean(L, 0); return 1; }
  bool ok = prefs.putString(key, val) > 0;
  prefs.end();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_sd_remove(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  if (!sdHostReady() || !sdHostPathOk(path)) { lua_pushboolean(L, 0); return 1; }
  bool ok = sdHostRemoveTree(path);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_sd_rename(lua_State* L) {
  const char* from = luaL_checkstring(L, 1);
  const char* to = luaL_checkstring(L, 2);
  if (!sdHostReady() || !sdHostPathOk(from) || !sdHostPathOk(to)) {
    lua_pushboolean(L, 0); return 1;
  }
  bool ok = sdHostRename(from, to);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_now(lua_State* L) {
  struct tm t = {};
  time_t epoch = 0;
  bool ok = false;
  if (gNowFn) ok = gNowFn(&t, &epoch);
  if (!ok) {
    epoch = time(nullptr);
    localtime_r(&epoch, &t);
    ok = true;
  }
  lua_createtable(L, 0, 9);
  lua_pushinteger(L, (lua_Integer)epoch); lua_setfield(L, -2, "epoch");
  lua_pushinteger(L, t.tm_year + 1900); lua_setfield(L, -2, "year");
  lua_pushinteger(L, t.tm_mon + 1); lua_setfield(L, -2, "month");
  lua_pushinteger(L, t.tm_mday); lua_setfield(L, -2, "day");
  lua_pushinteger(L, t.tm_hour); lua_setfield(L, -2, "hour");
  lua_pushinteger(L, t.tm_min); lua_setfield(L, -2, "min");
  lua_pushinteger(L, t.tm_sec); lua_setfield(L, -2, "sec");
  lua_pushinteger(L, t.tm_wday + 1); lua_setfield(L, -2, "wday");
  lua_pushboolean(L, pcf85063Available() ? 1 : 0); lua_setfield(L, -2, "rtc");
  return 1;
}

static int l_set_alarm(lua_State* L) {
  (void)L;
  lua_pushboolean(L, 0);
  lua_pushstring(L, "planned");
  return 2;
}

static const char* wifiStateExt(void) {
  if (gLuaWifiConnecting) return "connecting";
  if (gLuaWifiOn && WiFi.status() == WL_CONNECTED) return "on";
  if (wifiOtaIsRunning()) return "ota";
  return "off";
}

static int l_wifi_state(lua_State* L) {
  lua_pushstring(L, wifiStateExt());
  return 1;
}

static int l_wifi_connect(lua_State* L) {
  const char* ssid = luaL_checkstring(L, 1);
  const char* pass = luaL_optstring(L, 2, "");
  int timeoutMs = (int)luaL_optinteger(L, 3, 12000);
  if (timeoutMs < 1000) timeoutMs = 1000;
  if (timeoutMs > 30000) timeoutMs = 30000;
  if (wifiOtaIsRunning()) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "ota_busy");
    return 2;
  }
  if (!ssid || !ssid[0]) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no_ssid");
    return 2;
  }
  gLuaWifiConnecting = true;
  gLuaWifiOn = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < (uint32_t)timeoutMs) {
    delay(50);
  }
  gLuaWifiConnecting = false;
  if (WiFi.status() == WL_CONNECTED) {
    gLuaWifiOn = true;
    lua_pushboolean(L, 1);
    lua_pushstring(L, WiFi.localIP().toString().c_str());
    return 2;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  gLuaWifiOn = false;
  lua_pushboolean(L, 0);
  lua_pushstring(L, "timeout");
  return 2;
}

static int l_wifi_disconnect(lua_State* L) {
  if (wifiOtaIsRunning()) {
    lua_pushboolean(L, 0);
    return 1;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  gLuaWifiOn = false;
  gLuaWifiConnecting = false;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_http_get(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  size_t maxBytes = kHttpMaxBytes;
  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    lua_Integer mb = luaL_checkinteger(L, 2);
    if (mb < 0) mb = 0;
    if (mb > (lua_Integer)kHttpHardCap) mb = (lua_Integer)kHttpHardCap;
    maxBytes = (size_t)mb;
  }
  if (!url || (!gLuaWifiOn && WiFi.status() != WL_CONNECTED)) {
    lua_pushnil(L);
    lua_pushstring(L, "wifi_off");
    return 2;
  }
  if (maxBytes == 0) {
    lua_pushliteral(L, "");
    lua_pushinteger(L, 0);
    return 2;
  }
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  if (!http.begin(url)) {
    lua_pushnil(L);
    lua_pushstring(L, "begin_failed");
    return 2;
  }
  int code = http.GET();
  if (code <= 0) {
    http.end();
    lua_pushnil(L);
    lua_pushstring(L, "http_error");
    return 2;
  }
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    lua_pushnil(L);
    lua_pushstring(L, "no_stream");
    return 2;
  }
  char* buf = (char*)malloc(maxBytes + 1);
  if (!buf) {
    http.end();
    lua_pushnil(L);
    lua_pushstring(L, "oom");
    return 2;
  }
  size_t n = 0;
  uint32_t t0 = millis();
  while (http.connected() && n < maxBytes && (millis() - t0) < kHttpTimeoutMs) {
    size_t avail = stream->available();
    if (!avail) { delay(5); continue; }
    size_t chunk = avail;
    if (chunk > maxBytes - n) chunk = maxBytes - n;
    int r = stream->readBytes(buf + n, chunk);
    if (r <= 0) break;
    n += (size_t)r;
  }
  buf[n] = '\0';
  http.end();
  lua_pushlstring(L, buf, n);
  free(buf);
  lua_pushinteger(L, code);
  return 2;
}

static int l_audio_ready(lua_State* L) {
  lua_pushboolean(L, audioHostReady() ? 1 : 0);
  return 1;
}

static uint32_t micRateFromArg(lua_State* L, int idx) {
  uint32_t rate = 16000;
  if (lua_istable(L, idx)) {
    lua_getfield(L, idx, "rate");
    if (lua_isnumber(L, -1)) rate = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
  } else if (lua_isnumber(L, idx)) {
    rate = (uint32_t)lua_tointeger(L, idx);
  }
  return rate;
}

static int l_mic_start(lua_State* L) {
  uint32_t rate = 16000;
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) rate = micRateFromArg(L, 1);
  const char* err = nullptr;
  bool ok = audioHostMicStart(rate, &err);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, err ? err : audioHostLastError());
    return 2;
  }
  return 1;
}

static int l_mic_stop(lua_State* L) {
  lua_pushboolean(L, audioHostMicStop() ? 1 : 0);
  return 1;
}

static int l_mic_spectrum(lua_State* L) {
  int bins = (int)luaL_optinteger(L, 1, 64);
  if (bins < 8) bins = 8;
  if (bins > 128) bins = 128;
  float* mags = gMicSpectrumScratch;
  if (!audioHostMicSpectrum(mags, bins)) {
    lua_pushnil(L);
    lua_pushstring(L, audioHostLastError());
    return 2;
  }
  lua_createtable(L, bins, 0);
  for (int i = 0; i < bins; ++i) {
    lua_pushnumber(L, (lua_Number)mags[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_mic_fft(lua_State* L) { return l_mic_spectrum(L); }

static int micMaxSamplesArg(lua_State* L, int idx, int defVal) {
  int n = (int)luaL_optinteger(L, idx, defVal);
  if (n < 1) n = 1;
  if (n > AUDIO_MIC_READ_MAX) n = AUDIO_MIC_READ_MAX;
  return n;
}

/**
 * watch.mic_read([max_samples=256]) -> pcm_string, count | nil, err
 * pcm_string is raw little-endian int16 mono samples (#string == count*2).
 */
static int l_mic_read(lua_State* L) {
  int maxN = micMaxSamplesArg(L, 1, 256);
  int got = 0;
  if (!audioHostMicRead(gMicReadScratch, maxN, &got)) {
    lua_pushnil(L);
    lua_pushstring(L, audioHostLastError());
    return 2;
  }
  lua_pushlstring(L, (const char*)gMicReadScratch, (size_t)got * sizeof(int16_t));
  lua_pushinteger(L, got);
  return 2;
}

/**
 * watch.mic_read_table([max_samples=256]) -> {s0,s1,...}, count | nil, err
 * Optional convenience; prefer mic_read for speed.
 */
static int l_mic_read_table(lua_State* L) {
  int maxN = micMaxSamplesArg(L, 1, 256);
  int got = 0;
  if (!audioHostMicRead(gMicReadScratch, maxN, &got)) {
    lua_pushnil(L);
    lua_pushstring(L, audioHostLastError());
    return 2;
  }
  lua_createtable(L, got, 0);
  for (int i = 0; i < got; ++i) {
    lua_pushinteger(L, (lua_Integer)gMicReadScratch[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_pushinteger(L, got);
  return 2;
}

/** watch.mic_info() -> {sample_rate, bits, channels, running} */
static int l_mic_info(lua_State* L) {
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, (lua_Integer)audioHostMicSampleRate());
  lua_setfield(L, -2, "sample_rate");
  lua_pushinteger(L, audioHostMicBits());
  lua_setfield(L, -2, "bits");
  lua_pushinteger(L, audioHostMicChannels());
  lua_setfield(L, -2, "channels");
  lua_pushboolean(L, audioHostMicRunning() ? 1 : 0);
  lua_setfield(L, -2, "running");
  return 1;
}

/**
 * watch.mic_record_file(path, seconds?, rate?) -> true | false, err
 * Records mono int16 WAV to SD (creates parents). Caps at AUDIO_MIC_RECORD_MAX_SEC.
 */
static int l_mic_record_file(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  uint32_t seconds = (uint32_t)luaL_optinteger(L, 2, 3);
  uint32_t rate = (uint32_t)luaL_optinteger(L, 3, 16000);
  luaAudioFlushUi();
  const char* err = nullptr;
  bool ok = audioHostMicRecordFile(path, seconds, rate, &err);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, err ? err : audioHostLastError());
    return 2;
  }
  return 1;
}

/**
 * watch.mic_record(path, seconds?) — alias for mic_record_file when path given;
 * no-args keeps legacy redirect hint for spectrum/PCM apps.
 */
static int l_mic_record(lua_State* L) {
  if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
    return l_mic_record_file(L);
  }
  lua_pushnil(L);
  lua_pushstring(L, "use_mic_spectrum_or_mic_read");
  return 2;
}

/** watch.speaker_ready() -> bool (ES8311 probed) */
static int l_speaker_ready(lua_State* L) {
  lua_pushboolean(L, audioHostSpeakerReady() ? 1 : 0);
  return 1;
}

/**
 * watch.speaker_play(path) -> true | false, err
 * Plays WAV (mono/stereo int16) or raw mono .pcm/.raw from /sd through ES8311.
 */
static int l_speaker_play(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  luaAudioFlushUi();
  const char* err = nullptr;
  bool ok = audioHostSpeakerPlayFile(path, &err);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, err ? err : audioHostLastError());
    return 2;
  }
  return 1;
}

/** watch.speaker_start({rate=16000}|rate?) -> true | false, err */
static int l_speaker_start(lua_State* L) {
  uint32_t rate = 16000;
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) rate = micRateFromArg(L, 1);
  const char* err = nullptr;
  bool ok = audioHostSpeakerStart(rate, &err);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, err ? err : audioHostLastError());
    return 2;
  }
  return 1;
}

/**
 * watch.speaker_write(pcm_string) -> true | false, err
 * Stereo int16 LE frames (#string multiple of 4). Prefer speaker_play for files.
 */
static int l_speaker_write(lua_State* L) {
  size_t len = 0;
  const char* data = luaL_checklstring(L, 1, &len);
  if (len == 0 || (len & 3) != 0) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_pcm");
    return 2;
  }
  if (!audioHostSpeakerWrite(data, len)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, audioHostLastError());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

/** watch.speaker_stop() -> true */
static int l_speaker_stop(lua_State* L) {
  lua_pushboolean(L, audioHostSpeakerStop() ? 1 : 0);
  return 1;
}

/**
 * watch.speaker_volume() -> pct (0..100)
 * watch.speaker_volume(n) -> true | false, err
 * Linear map onto ES8311 reg 0x32 (0x00..0xBF). Session only; apps own UX.
 */
static int l_speaker_volume(lua_State* L) {
  if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
    lua_pushinteger(L, audioHostSpeakerVolumeGet());
    return 1;
  }
  int pct = (int)luaL_checkinteger(L, 1);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (!audioHostSpeakerVolumeSet(pct)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, audioHostLastError());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

/**
 * watch.speaker_volume_raw() -> reg (0..0xBF)
 * watch.speaker_volume_raw(n) -> true | false, err
 * Lab/raw access to ES8311 DAC volume register (clamped to 0xBF).
 */
static int l_speaker_volume_raw(lua_State* L) {
  if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
    lua_pushinteger(L, (lua_Integer)audioHostSpeakerVolumeRawGet());
    return 1;
  }
  int raw = (int)luaL_checkinteger(L, 1);
  if (raw < 0) raw = 0;
  if (raw > 0xBF) raw = 0xBF;
  if (!audioHostSpeakerVolumeRawSet((uint8_t)raw)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, audioHostLastError());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int l_canvas(lua_State* L) {
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int w = (int)luaL_checkinteger(L, 3);
  int h = (int)luaL_checkinteger(L, 4);
  bool ok = gCanvasCreate ? gCanvasCreate(x, y, w, h) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_canvas_clear(lua_State* L) {
  int r = (int)luaL_optinteger(L, 1, 0);
  int g = (int)luaL_optinteger(L, 2, 0);
  int b = (int)luaL_optinteger(L, 3, 0);
  bool ok = gCanvasClear ? gCanvasClear(r, g, b) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_canvas_scroll(lua_State* L) {
  int dy = (int)luaL_checkinteger(L, 1);
  bool ok = gCanvasScroll ? gCanvasScroll(dy) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_canvas_row(lua_State* L) {
  int y = (int)luaL_checkinteger(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  int n = (int)lua_rawlen(L, 2);
  if (n < 1) {
    lua_pushboolean(L, 0);
    return 1;
  }
  if (n > 256) n = 256;
  float* mags = gCanvasRowScratch;
  for (int i = 0; i < n; ++i) {
    lua_rawgeti(L, 2, i + 1);
    mags[i] = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (mags[i] < 0.0f) mags[i] = 0.0f;
    if (mags[i] > 1.0f) mags[i] = 1.0f;
  }
  bool ok = gCanvasRowHeat ? gCanvasRowHeat(y, mags, n) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_canvas_pixel(lua_State* L) {
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int r = (int)luaL_checkinteger(L, 3);
  int g = (int)luaL_checkinteger(L, 4);
  int b = (int)luaL_checkinteger(L, 5);
  bool ok = gCanvasPixel ? gCanvasPixel(x, y, r, g, b) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}


/** Extract JSON string value for key "text" or "transcript" (simple, no nest). */
static bool sttParseTranscript(const char* body, size_t n, char* out, size_t outLen) {
  if (!body || !out || outLen < 2) return false;
  out[0] = '\0';
  const char* keys[] = {"\"text\"", "\"transcript\"", "\"transcription\"", nullptr};
  for (int k = 0; keys[k]; ++k) {
    const char* p = strstr(body, keys[k]);
    if (!p || (size_t)(p - body) >= n) continue;
    p = strchr(p + strlen(keys[k]), ':');
    if (!p) continue;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') continue;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outLen) {
      if (*p == '\\' && p[1]) {
        p++;
        out[i++] = *p++;
      } else {
        out[i++] = *p++;
      }
    }
    out[i] = '\0';
    if (i > 0) return true;
  }
  return false;
}

/**
 * watch.speech_to_text(secs?) -> text | nil, err
 * Records short WAV via ES7210, POSTs to prefs stt_url when Wi-Fi is up.
 * No Whisper on-device. Honest errors when endpoint/Wi-Fi missing.
 */
static int l_speech_to_text(lua_State* L) {
  uint32_t secs = (uint32_t)luaL_optinteger(L, 1, 3);
  if (secs < 1) secs = 1;
  if (secs > 8) secs = 8;

  Preferences prefs;
  String url;
  if (prefs.begin("wlua", true)) {
    url = prefs.getString("stt_url", "");
    prefs.end();
  }
  if (url.length() < 8) {
    lua_pushnil(L);
    lua_pushstring(L, "set_stt_url");
    return 2;
  }

  bool wifiOk = (WiFi.status() == WL_CONNECTED) || gLuaWifiOn;
  if (!wifiOk || WiFi.status() != WL_CONNECTED) {
    lua_pushnil(L);
    lua_pushstring(L, "wifi_off");
    return 2;
  }

  if (!sdHostReady() && !sdHostMount()) {
    lua_pushnil(L);
    lua_pushstring(L, "sd_not_ready");
    return 2;
  }

  const char* path = "/sd/stt_tmp.wav";
  luaAudioFlushUi();
  const char* recErr = nullptr;
  if (!audioHostMicRecordFile(path, secs, 16000, &recErr)) {
    lua_pushnil(L);
    lua_pushstring(L, recErr ? recErr : "record_fail");
    return 2;
  }

  fs::File f;
  if (!sdHostOpenRead(path, &f) || !f) {
    lua_pushnil(L);
    lua_pushstring(L, "open_fail");
    return 2;
  }
  size_t fsz = f.size();
  if (fsz < 44 || fsz > 400000) {
    f.close();
    lua_pushnil(L);
    lua_pushstring(L, "bad_wav");
    return 2;
  }

  HTTPClient http;
  http.setTimeout(20000);
  http.setConnectTimeout(8000);
  if (!http.begin(url.c_str())) {
    f.close();
    lua_pushnil(L);
    lua_pushstring(L, "begin_failed");
    return 2;
  }
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("Accept", "application/json, text/plain");
  int code = http.sendRequest("POST", &f, fsz);
  f.close();

  if (code <= 0) {
    http.end();
    lua_pushnil(L);
    lua_pushstring(L, "http_error");
    return 2;
  }

  String body = http.getString();
  http.end();

  char text[256];
  if (sttParseTranscript(body.c_str(), body.length(), text, sizeof(text))) {
    lua_pushstring(L, text);
    return 1;
  }
  // Plain-text body fallback (trimmed)
  if (code >= 200 && code < 300 && body.length() > 0 && body.length() < 200 &&
      body.indexOf('{') < 0) {
    body.trim();
    if (body.length() > 0) {
      lua_pushstring(L, body.c_str());
      return 1;
    }
  }
  lua_pushnil(L);
  lua_pushstring(L, "no_transcript");
  return 2;
}

static int l_ble_ready(lua_State* L) {
  lua_pushboolean(L, bleMeshReady() ? 1 : 0);
  return 1;
}

static int l_ble_start(lua_State* L) {
  // Legacy: start a short scan (keeps old name working).
  (void)L;
  bool ok = bleMeshScanStart(4000);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, bleMeshStatus());
    return 2;
  }
  return 1;
}

static int l_ble_stop(lua_State* L) {
  (void)L;
  bleMeshDisconnect();
  lua_pushboolean(L, 1);
  return 1;
}

static int l_ble_status(lua_State* L) {
  lua_pushstring(L, bleMeshStatus());
  return 1;
}

static int l_ble_scan(lua_State* L) {
  uint32_t ms = 4000;
  if (lua_gettop(L) >= 1 && lua_isnumber(L, 1)) {
    ms = (uint32_t)lua_tointeger(L, 1);
  }
  if (!bleMeshScanStart(ms)) {
    lua_createtable(L, 0, 0);
    return 1;
  }
  // Block briefly so Lua gets results (scan is short; UI thread ok for v1).
  uint32_t t0 = millis();
  while ((millis() - t0) < ms + 200) {
    bleMeshTick();
    if (strcmp(bleMeshStatus(), "scanning") != 0) break;
    delay(20);
  }
  int n = bleMeshScanCount();
  lua_createtable(L, n, 0);
  char addr[24], name[28];
  int rssi = 0;
  for (int i = 0; i < n; ++i) {
    if (!bleMeshScanAt(i, addr, sizeof(addr), name, sizeof(name), &rssi)) continue;
    lua_createtable(L, 0, 3);
    lua_pushstring(L, addr);
    lua_setfield(L, -2, "addr");
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, rssi);
    lua_setfield(L, -2, "rssi");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_ble_connect(lua_State* L) {
  const char* addr = luaL_checkstring(L, 1);
  const char* pin = nullptr;
  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2) && lua_isstring(L, 2)) {
    pin = lua_tostring(L, 2);
    if (pin && !pin[0]) pin = nullptr;
  }
  bool ok = bleMeshConnect(addr, pin);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, bleMeshStatus());
    return 2;
  }
  return 1;
}

static int l_ble_disconnect(lua_State* L) {
  (void)L;
  lua_pushboolean(L, bleMeshDisconnect() ? 1 : 0);
  return 1;
}

static int l_mesh_connected(lua_State* L) {
  lua_pushboolean(L, bleMeshConnected() ? 1 : 0);
  return 1;
}

static int l_mesh_send(lua_State* L) {
  const char* text = luaL_checkstring(L, 1);
  char err[48];
  err[0] = '\0';
  bool ok = bleMeshSendText(text, err, sizeof(err));
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, err[0] ? err : "send_fail");
    return 2;
  }
  return 1;
}

static int l_mesh_poll(lua_State* L) {
  bleMeshTick();
  lua_createtable(L, 0, 0);
  int idx = 1;
  BleMeshMsg msg;
  while (bleMeshPoll(&msg)) {
    lua_createtable(L, 0, 3);
    char fromHex[12];
    snprintf(fromHex, sizeof(fromHex), "!%08x", (unsigned)msg.from);
    lua_pushstring(L, fromHex);
    lua_setfield(L, -2, "from");
    lua_pushstring(L, msg.text);
    lua_setfield(L, -2, "text");
    lua_pushinteger(L, (lua_Integer)msg.rx_time);
    lua_setfield(L, -2, "time");
    lua_rawseti(L, -2, idx++);
  }
  return 1;
}

/** watch.mesh_channel() get / watch.mesh_channel(n) set — channel index 0..7 */
static int l_mesh_channel(lua_State* L) {
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    int n = (int)luaL_checkinteger(L, 1);
    if (n < 0) n = 0;
    if (n > 7) n = 7;
    bleMeshSetChannel((uint8_t)n);
  }
  lua_pushinteger(L, (lua_Integer)bleMeshGetChannel());
  return 1;
}

/** watch.mesh_alert_beep() get / watch.mesh_alert_beep(bool) set — default true */
static int l_mesh_alert_beep(lua_State* L) {
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    bleMeshAlertBeepSet(lua_toboolean(L, 1) != 0);
  }
  lua_pushboolean(L, bleMeshAlertBeepEnabled() ? 1 : 0);
  return 1;
}

/** watch.mesh_alert_vibrate() get / watch.mesh_alert_vibrate(bool) set — default true */
static int l_mesh_alert_vibrate(lua_State* L) {
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    bleMeshAlertVibrateSet(lua_toboolean(L, 1) != 0);
  }
  lua_pushboolean(L, bleMeshAlertVibrateEnabled() ? 1 : 0);
  return 1;
}

/**
 * watch.notify(app_id, text) — top banner associated with app_id.
 * Swipe-down on the banner opens that app (generic; any id in catalog).
 */

/** watch.launch_action() — one-shot; returns and clears pending action. */
static int l_launch_action(lua_State* L) {
  if (gLaunchAction[0]) {
    lua_pushstring(L, gLaunchAction);
    gLaunchAction[0] = '\0';
  } else {
    lua_pushnil(L);
  }
  return 1;
}

/** watch.launch_text() — one-shot; returns and clears pending text. */
static int l_launch_text(lua_State* L) {
  if (gLaunchText[0]) {
    lua_pushstring(L, gLaunchText);
    gLaunchText[0] = '\0';
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static int l_notify(lua_State* L) {
  const char* appId = luaL_checkstring(L, 1);
  const char* text = luaL_optstring(L, 2, "");
  if (!gNotify) {
    lua_pushboolean(L, 0);
    return 1;
  }
  gNotify(appId, text);
  lua_pushboolean(L, 1);
  return 1;
}

/** watch.beep([ms], [hz]) — short tone; defaults 100ms / 880Hz. */
static int l_beep(lua_State* L) {
  uint32_t ms = (uint32_t)luaL_optinteger(L, 1, 100);
  uint32_t hz = (uint32_t)luaL_optinteger(L, 2, 880);
  lua_pushboolean(L, audioHostBeep(ms, hz) ? 1 : 0);
  return 1;
}

/** watch.alert_uh_oh() — ICQ-style ascending-then-descending alert. */
static int l_alert_uh_oh(lua_State* L) {
  lua_pushboolean(L, audioHostAlertUhOh() ? 1 : 0);
  return 1;
}

static int l_install_zip(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  const char* appId = luaL_checkstring(L, 2);
  const char* dest = luaL_optstring(L, 3, "lfs");
  char err[64];
  bool ok = zipInstallFromPath(path, appId, dest, err, sizeof(err));
  lua_pushboolean(L, ok ? 1 : 0);
  lua_pushstring(L, err);
  return 2;
}

static int l_brightness(lua_State* L) {
  if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
    if (!gBrightnessGet) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, gBrightnessGet());
    return 1;
  }
  int pct = (int)luaL_checkinteger(L, 1);
  if (!gBrightnessSet) { lua_pushnil(L); return 1; }
  lua_pushinteger(L, gBrightnessSet(pct));
  return 1;
}

static int l_idle_timeout(lua_State* L) {
  if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
    uint32_t sec = gIdleGet ? gIdleGet() : 60;
    lua_pushinteger(L, (lua_Integer)sec);
    return 1;
  }
  uint32_t sec = (uint32_t)luaL_checkinteger(L, 1);
  if (sec > 3600) sec = 3600;
  uint32_t got = gIdleSet ? gIdleSet(sec) : sec;
  lua_pushinteger(L, (lua_Integer)got);
  return 1;
}

static int l_sprite(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int fw = (int)luaL_checkinteger(L, 4);
  int fh = (int)luaL_checkinteger(L, 5);
  int frame = (int)luaL_optinteger(L, 6, 0);
  int max_w = (int)luaL_optinteger(L, 7, fw);
  int max_h = (int)luaL_optinteger(L, 8, fh);
  bool ok = gShowSprite ? gShowSprite(path, x, y, fw, fh, frame, max_w, max_h) : false;
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_image_frame(lua_State* L) { return l_sprite(L); }

static int l_gpio_pwm(lua_State* L) {
  int pin = (int)luaL_checkinteger(L, 1);
  int duty = (int)luaL_checkinteger(L, 2); // 0..255
  int freq = (int)luaL_optinteger(L, 3, 5000);
  if (!gpioAllowed(pin)) { lua_pushboolean(L, 0); lua_pushstring(L, "pin_not_allowed"); return 2; }
  if (duty < 0) duty = 0;
  if (duty > 255) duty = 255;
  if (freq < 100) freq = 100;
  if (freq > 40000) freq = 40000;
  // Arduino-ESP32 3.x: pin-based LEDC API
  if (!ledcAttach((uint8_t)pin, (uint32_t)freq, 8)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "ledc_attach_failed");
    return 2;
  }
  ledcWrite((uint8_t)pin, (uint32_t)duty);
  lua_pushboolean(L, 1);
  return 1;
}

static int l_gpio_adc(lua_State* L) {
  int pin = (int)luaL_checkinteger(L, 1);
  if (!gpioAllowed(pin)) { lua_pushnil(L); lua_pushstring(L, "pin_not_allowed"); return 2; }
  analogReadResolution(12);
  int v = analogRead(pin);
  lua_pushinteger(L, v);
  return 1;
}

static int l_gpio_write(lua_State* L) {
  int pin = (int)luaL_checkinteger(L, 1);
  int val = (int)luaL_checkinteger(L, 2);
  if (!gpioAllowed(pin)) { lua_pushboolean(L, 0); return 1; }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, val ? HIGH : LOW);
  lua_pushboolean(L, 1);
  return 1;
}

static int l_gpio_read(lua_State* L) {
  int pin = (int)luaL_checkinteger(L, 1);
  if (!gpioAllowed(pin)) { lua_pushnil(L); return 1; }
  pinMode(pin, INPUT);
  lua_pushinteger(L, digitalRead(pin));
  return 1;
}


static int l_job_start(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  int bins = 64;
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "bins");
    if (lua_isnumber(L, -1)) bins = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
  } else if (lua_isnumber(L, 2)) {
    bins = (int)lua_tointeger(L, 2);
  }
  char err[64];
  uint32_t id = jobHostStart(name, bins, err, sizeof(err));
  if (id == 0) {
    lua_pushnil(L);
    lua_pushstring(L, err[0] ? err : "start_failed");
    return 2;
  }
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}

static int l_job_status(lua_State* L) {
  uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
  lua_pushstring(L, jobHostStatus(id));
  return 1;
}

static int l_job_result(lua_State* L) {
  uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
  const char* kind = jobHostKind(id);
  char err[64];
  if (!kind) {
    // May already be consumed or unknown — status check
    const char* st = jobHostStatus(id);
    lua_pushnil(L);
    lua_pushstring(L, (st && strcmp(st, "unknown") == 0) ? "unknown_id" : "not_ready");
    return 2;
  }
  if (strcmp(kind, "fft") == 0) {
    float* mags = gMicSpectrumScratch;
    int n = 0;
    if (!jobHostTakeSpectrum(id, mags, 128, &n, err, sizeof(err))) {
      lua_pushnil(L);
      lua_pushstring(L, err[0] ? err : "take_failed");
      return 2;
    }
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
      lua_pushnumber(L, (lua_Number)mags[i]);
      lua_rawseti(L, -2, i + 1);
    }
    return 1;
  }
  if (strcmp(kind, "rms") == 0) {
    float rms = 0.f;
    if (!jobHostTakeRms(id, &rms, err, sizeof(err))) {
      lua_pushnil(L);
      lua_pushstring(L, err[0] ? err : "take_failed");
      return 2;
    }
    lua_pushnumber(L, (lua_Number)rms);
    return 1;
  }
  lua_pushnil(L);
  lua_pushstring(L, "bad_kind");
  return 2;
}

static int l_job_cancel(lua_State* L) {
  uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
  lua_pushboolean(L, jobHostCancel(id) ? 1 : 0);
  return 1;
}

void luaApiExtRegister(lua_State* L) {
  gExtL = L;
  // Layout
  lua_pushcfunction(L, l_label); lua_setfield(L, -2, "label");
  lua_pushcfunction(L, l_panel); lua_setfield(L, -2, "panel");
  lua_pushcfunction(L, l_progress); lua_setfield(L, -2, "progress");
  lua_pushcfunction(L, l_progress_set); lua_setfield(L, -2, "progress_set");
  lua_pushcfunction(L, l_slider); lua_setfield(L, -2, "slider");
  lua_pushcfunction(L, l_z_raise); lua_setfield(L, -2, "z_raise");
  // Input
  lua_pushcfunction(L, l_on_back); lua_setfield(L, -2, "on_back");
  lua_pushcfunction(L, l_on_long_press); lua_setfield(L, -2, "on_long_press");
  lua_pushcfunction(L, l_gesture); lua_setfield(L, -2, "gesture");
  lua_pushcfunction(L, l_touch_delta); lua_setfield(L, -2, "touch_delta");
  // Storage
  lua_pushcfunction(L, l_prefs_get); lua_setfield(L, -2, "prefs_get");
  lua_pushcfunction(L, l_prefs_set); lua_setfield(L, -2, "prefs_set");
  lua_pushcfunction(L, l_sd_remove); lua_setfield(L, -2, "sd_remove");
  lua_pushcfunction(L, l_sd_rename); lua_setfield(L, -2, "sd_rename");
  // Time
  lua_pushcfunction(L, l_now); lua_setfield(L, -2, "now");
  lua_pushcfunction(L, l_set_alarm); lua_setfield(L, -2, "set_alarm");
  // Net (override wifi_state with extended)
  lua_pushcfunction(L, l_wifi_state); lua_setfield(L, -2, "wifi_state");
  lua_pushcfunction(L, l_wifi_connect); lua_setfield(L, -2, "wifi_connect");
  lua_pushcfunction(L, l_wifi_disconnect); lua_setfield(L, -2, "wifi_disconnect");
  lua_pushcfunction(L, l_http_get); lua_setfield(L, -2, "http_get");
  // Audio (ES7210 mic: fine PCM + coarse native FFT spectrum)
  lua_pushcfunction(L, l_audio_ready); lua_setfield(L, -2, "audio_ready");
  lua_pushcfunction(L, l_mic_start); lua_setfield(L, -2, "mic_start");
  lua_pushcfunction(L, l_mic_stop); lua_setfield(L, -2, "mic_stop");
  lua_pushcfunction(L, l_mic_info); lua_setfield(L, -2, "mic_info");
  lua_pushcfunction(L, l_mic_read); lua_setfield(L, -2, "mic_read");
  lua_pushcfunction(L, l_mic_read_table); lua_setfield(L, -2, "mic_read_table");
  lua_pushcfunction(L, l_mic_spectrum); lua_setfield(L, -2, "mic_spectrum");
  lua_pushcfunction(L, l_mic_fft); lua_setfield(L, -2, "mic_fft");
  lua_pushcfunction(L, l_mic_record); lua_setfield(L, -2, "mic_record");
  lua_pushcfunction(L, l_mic_record_file); lua_setfield(L, -2, "mic_record_file");
  lua_pushcfunction(L, l_speech_to_text); lua_setfield(L, -2, "speech_to_text");
  lua_pushcfunction(L, l_speaker_ready); lua_setfield(L, -2, "speaker_ready");
  lua_pushcfunction(L, l_speaker_play); lua_setfield(L, -2, "speaker_play");
  lua_pushcfunction(L, l_speaker_start); lua_setfield(L, -2, "speaker_start");
  lua_pushcfunction(L, l_speaker_write); lua_setfield(L, -2, "speaker_write");
  lua_pushcfunction(L, l_speaker_stop); lua_setfield(L, -2, "speaker_stop");
  lua_pushcfunction(L, l_speaker_volume); lua_setfield(L, -2, "speaker_volume");
  lua_pushcfunction(L, l_speaker_volume_raw); lua_setfield(L, -2, "speaker_volume_raw");
  // Canvas (RGB565 buffer for waterfall / pixel drawing)
  lua_pushcfunction(L, l_canvas); lua_setfield(L, -2, "canvas");
  lua_pushcfunction(L, l_canvas_clear); lua_setfield(L, -2, "canvas_clear");
  lua_pushcfunction(L, l_canvas_scroll); lua_setfield(L, -2, "canvas_scroll");
  lua_pushcfunction(L, l_canvas_row); lua_setfield(L, -2, "canvas_row");
  lua_pushcfunction(L, l_canvas_pixel); lua_setfield(L, -2, "canvas_pixel");
  // BLE / Meshtastic mesh chat
  lua_pushcfunction(L, l_ble_ready); lua_setfield(L, -2, "ble_ready");
  lua_pushcfunction(L, l_ble_start); lua_setfield(L, -2, "ble_start");
  lua_pushcfunction(L, l_ble_stop); lua_setfield(L, -2, "ble_stop");
  lua_pushcfunction(L, l_ble_status); lua_setfield(L, -2, "ble_status");
  lua_pushcfunction(L, l_ble_scan); lua_setfield(L, -2, "ble_scan");
  lua_pushcfunction(L, l_ble_connect); lua_setfield(L, -2, "ble_connect");
  lua_pushcfunction(L, l_ble_disconnect); lua_setfield(L, -2, "ble_disconnect");
  lua_pushcfunction(L, l_mesh_connected); lua_setfield(L, -2, "mesh_connected");
  lua_pushcfunction(L, l_mesh_send); lua_setfield(L, -2, "mesh_send");
  lua_pushcfunction(L, l_mesh_poll); lua_setfield(L, -2, "mesh_poll");
  lua_pushcfunction(L, l_mesh_channel); lua_setfield(L, -2, "mesh_channel");
  lua_pushcfunction(L, l_mesh_alert_beep); lua_setfield(L, -2, "mesh_alert_beep");
  lua_pushcfunction(L, l_mesh_alert_vibrate); lua_setfield(L, -2, "mesh_alert_vibrate");
  lua_pushcfunction(L, l_notify); lua_setfield(L, -2, "notify");
  lua_pushcfunction(L, l_launch_action); lua_setfield(L, -2, "launch_action");
  lua_pushcfunction(L, l_launch_text); lua_setfield(L, -2, "launch_text");
  lua_pushcfunction(L, l_beep); lua_setfield(L, -2, "beep");
  lua_pushcfunction(L, l_alert_uh_oh); lua_setfield(L, -2, "alert_uh_oh");
  // Zip
  lua_pushcfunction(L, l_install_zip); lua_setfield(L, -2, "install_zip");
  // Power
  lua_pushcfunction(L, l_brightness); lua_setfield(L, -2, "brightness");
  lua_pushcfunction(L, l_idle_timeout); lua_setfield(L, -2, "idle_timeout");
  // Sprites
  lua_pushcfunction(L, l_sprite); lua_setfield(L, -2, "sprite");
  lua_pushcfunction(L, l_image_frame); lua_setfield(L, -2, "image_frame");
  // GPIO
  lua_pushcfunction(L, l_gpio_pwm); lua_setfield(L, -2, "gpio_pwm");
  lua_pushcfunction(L, l_gpio_adc); lua_setfield(L, -2, "gpio_adc");
  lua_pushcfunction(L, l_gpio_write); lua_setfield(L, -2, "gpio_write");
  lua_pushcfunction(L, l_gpio_read); lua_setfield(L, -2, "gpio_read");
  // Background jobs (non-UI core; see docs/WASM_APPS.md / CAPABILITIES)
  lua_pushcfunction(L, l_job_start); lua_setfield(L, -2, "job_start");
  lua_pushcfunction(L, l_job_status); lua_setfield(L, -2, "job_status");
  lua_pushcfunction(L, l_job_result); lua_setfield(L, -2, "job_result");
  lua_pushcfunction(L, l_job_cancel); lua_setfield(L, -2, "job_cancel");
}

bool hostUiLabel(int x, int y, const char* text, int r, int g, int b) {
  return gCreateLabel ? gCreateLabel(x, y, text, r, g, b) : false;
}
bool hostUiCanvasCreate(int x, int y, int w, int h) {
  return gCanvasCreate ? gCanvasCreate(x, y, w, h) : false;
}
bool hostUiCanvasClear(int r, int g, int b) {
  return gCanvasClear ? gCanvasClear(r, g, b) : false;
}
bool hostUiCanvasScroll(int dy) {
  return gCanvasScroll ? gCanvasScroll(dy) : false;
}
bool hostUiCanvasRowHeat(int y, const float* mags, int nMags) {
  return gCanvasRowHeat ? gCanvasRowHeat(y, mags, nMags) : false;
}
bool hostUiCanvasPixel(int x, int y, int r, int g, int b) {
  return gCanvasPixel ? gCanvasPixel(x, y, r, g, b) : false;
}
bool hostUiNow(struct tm* out, time_t* epoch) {
  return gNowFn ? gNowFn(out, epoch) : false;
}
