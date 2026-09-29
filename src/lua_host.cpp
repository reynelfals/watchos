#include "lua_host.h"
#include "search.h"
#include "sd_host.h"
#include "usb_msc_host.h"
#include "uart_host.h"
#include "audio_host.h"
#include "job_host.h"
#include "display_geometry.h"
#include "lua_api_ext.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

static constexpr int kMaxButtons = 16;
static constexpr uint32_t kTickIntervalMs = 200;
static constexpr lua_Integer kDelayCapMs = 500;

static LuaHostTitleFn gTitleFn = nullptr;
static LuaHostTextFn gTextFn = nullptr;
static LuaHostAppendFn gAppendFn = nullptr;
static LuaHostClearFn gClearFn = nullptr;
static LuaHostGetSizeFn gGetSizeFn = nullptr;
static LuaHostSetBgFn gSetBgFn = nullptr;
static LuaHostClearWidgetsFn gClearWidgetsFn = nullptr;
static LuaHostCreateButtonFn gCreateButtonFn = nullptr;
static LuaHostCreateRectFn gCreateRectFn = nullptr;
static LuaHostCreateCircleFn gCreateCircleFn = nullptr;
static LuaHostCreateLabelFn gCreateLabelFn = nullptr;
static LuaHostGfxClearFn gGfxClearFn = nullptr;
static LuaHostBallFn gBallFn = nullptr;
static LuaHostBatteryFn gBatteryFn = nullptr;
static LuaHostBatteryMvFn gBatteryMvFn = nullptr;
static LuaHostChargingFn gChargingFn = nullptr;
static LuaHostUsbPowerFn gUsbPowerFn = nullptr;
static LuaHostBatteryConnectedFn gBatteryConnectedFn = nullptr;
static LuaHostBackFn gBackFn = nullptr;
static LuaHostVibrateFn gVibrateFn = nullptr;
static LuaHostWifiStateFn gWifiStateFn = nullptr;
static LuaHostBrightnessFn gBrightnessFn = nullptr;
static LuaHostImuFn gImuFn = nullptr;
static LuaHostCreateLetterPadFn gCreateLetterPadFn = nullptr;
static LuaHostButtonLayoutFn gButtonLayoutFn = nullptr;
static LuaHostTouchFn gTouchFn = nullptr;
static LuaHostShowImageFn gShowImageFn = nullptr;
static LuaHostClearImageFn gClearImageFn = nullptr;

static lua_State* gL = nullptr;
static int gOnTickRef = LUA_NOREF;
static int gLetterPadRef = LUA_NOREF;
static int gButtonRefs[kMaxButtons];
static int gButtonCount = 0;
static uint32_t gLastTickMs = 0;

void luaHostBegin(void) {
  for (int i = 0; i < kMaxButtons; ++i) gButtonRefs[i] = LUA_NOREF;
}

void luaHostSetTitleCallback(LuaHostTitleFn fn) { gTitleFn = fn; }
void luaHostSetTextCallback(LuaHostTextFn fn) { gTextFn = fn; }
void luaHostSetAppendCallback(LuaHostAppendFn fn) { gAppendFn = fn; }
void luaHostSetClearCallback(LuaHostClearFn fn) { gClearFn = fn; }
void luaHostSetGetSizeCallback(LuaHostGetSizeFn fn) { gGetSizeFn = fn; }
void luaHostSetSetBgCallback(LuaHostSetBgFn fn) { gSetBgFn = fn; }
void luaHostSetClearWidgetsCallback(LuaHostClearWidgetsFn fn) { gClearWidgetsFn = fn; }
void luaHostSetCreateButtonCallback(LuaHostCreateButtonFn fn) { gCreateButtonFn = fn; }
void luaHostSetCreateRectCallback(LuaHostCreateRectFn fn) { gCreateRectFn = fn; }
void luaHostSetCreateCircleCallback(LuaHostCreateCircleFn fn) { gCreateCircleFn = fn; }
void luaHostSetCreateLabelCallback(LuaHostCreateLabelFn fn) { gCreateLabelFn = fn; }
void luaHostSetGfxClearCallback(LuaHostGfxClearFn fn) { gGfxClearFn = fn; }
void luaHostSetBallCallback(LuaHostBallFn fn) { gBallFn = fn; }
void luaHostSetBatteryCallback(LuaHostBatteryFn fn) { gBatteryFn = fn; }
void luaHostSetBatteryMvCallback(LuaHostBatteryMvFn fn) { gBatteryMvFn = fn; }
void luaHostSetChargingCallback(LuaHostChargingFn fn) { gChargingFn = fn; }
void luaHostSetUsbPowerCallback(LuaHostUsbPowerFn fn) { gUsbPowerFn = fn; }
void luaHostSetBatteryConnectedCallback(LuaHostBatteryConnectedFn fn) {
  gBatteryConnectedFn = fn;
}
void luaHostSetBackCallback(LuaHostBackFn fn) { gBackFn = fn; }
void luaHostSetVibrateCallback(LuaHostVibrateFn fn) { gVibrateFn = fn; }
void luaHostSetWifiStateCallback(LuaHostWifiStateFn fn) { gWifiStateFn = fn; }
void luaHostSetBrightnessCallback(LuaHostBrightnessFn fn) { gBrightnessFn = fn; }
void luaHostSetImuCallback(LuaHostImuFn fn) { gImuFn = fn; }
void luaHostSetCreateLetterPadCallback(LuaHostCreateLetterPadFn fn) {
  gCreateLetterPadFn = fn;
}
void luaHostSetButtonLayoutCallback(LuaHostButtonLayoutFn fn) {
  gButtonLayoutFn = fn;
}
void luaHostSetTouchCallback(LuaHostTouchFn fn) { gTouchFn = fn; }
void luaHostSetShowImageCallback(LuaHostShowImageFn fn) { gShowImageFn = fn; }
void luaHostSetClearImageCallback(LuaHostClearImageFn fn) { gClearImageFn = fn; }

static void* luaPsramAlloc(void* /*ud*/, void* ptr, size_t osize, size_t nsize) {
  (void)osize;
  if (nsize == 0) {
    if (ptr) heap_caps_free(ptr);
    return nullptr;
  }
  void* p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
  }
  return p;
}

struct FsReader {
  File file;
};

static const char* littlefsReader(lua_State* /*L*/, void* ud, size_t* size) {
  FsReader* r = static_cast<FsReader*>(ud);
  static char buf[512];
  if (!r->file) {
    *size = 0;
    return nullptr;
  }
  size_t n = r->file.readBytes(buf, sizeof(buf));
  *size = n;
  return (n > 0) ? buf : nullptr;
}

static void setErr(char* errBuf, size_t errLen, const char* msg) {
  if (!errBuf || errLen == 0) return;
  strncpy(errBuf, msg ? msg : "error", errLen - 1);
  errBuf[errLen - 1] = '\0';
}

static int clampByte(lua_Integer v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (int)v;
}

// Pixel/geometry args: accept Lua floats (physics apps). luaL_checkinteger
// rejects non-integral numbers ("number has no integer representation") and
// aborts on_tick — that froze Tilt Maze's yellow ball after the first substep.
static int pixArg(lua_State* L, int arg) {
  lua_Number n = luaL_checknumber(L, arg);
  if (n >= 0) return (int)(n + 0.5);
  return (int)(n - 0.5);
}

static int l_watch_set_title(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  if (gTitleFn) gTitleFn(s);
  return 0;
}

static int l_watch_set_text(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  if (gTextFn) gTextFn(s);
  return 0;
}

static int l_watch_append_text(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  if (gAppendFn) gAppendFn(s);
  return 0;
}

static int l_watch_clear(lua_State* L) {
  (void)L;
  if (gClearFn) gClearFn();
  return 0;
}

static int l_watch_width(lua_State* L) {
  int w = 410, h = 502;
  if (gGetSizeFn) gGetSizeFn(&w, &h);
  lua_pushinteger(L, w);
  return 1;
}

static int l_watch_height(lua_State* L) {
  int w = 410, h = 502;
  if (gGetSizeFn) gGetSizeFn(&w, &h);
  lua_pushinteger(L, h);
  return 1;
}

static int l_watch_millis(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)millis());
  return 1;
}

static int l_watch_time(lua_State* L) {
  time_t now = time(nullptr);
  struct tm t = {};
  localtime_r(&now, &t);
  lua_createtable(L, 0, 7);
  lua_pushinteger(L, t.tm_hour);
  lua_setfield(L, -2, "hour");
  lua_pushinteger(L, t.tm_min);
  lua_setfield(L, -2, "min");
  lua_pushinteger(L, t.tm_sec);
  lua_setfield(L, -2, "sec");
  lua_pushinteger(L, t.tm_mday);
  lua_setfield(L, -2, "day");
  lua_pushinteger(L, t.tm_mon + 1);
  lua_setfield(L, -2, "month");
  lua_pushinteger(L, t.tm_year + 1900);
  lua_setfield(L, -2, "year");
  // Lua-style weekday: Sunday=1 .. Saturday=7 (tm_wday is 0..6)
  lua_pushinteger(L, t.tm_wday + 1);
  lua_setfield(L, -2, "wday");
  return 1;
}

static int l_watch_delay(lua_State* L) {
  lua_Integer ms = luaL_checkinteger(L, 1);
  if (ms < 0) ms = 0;
  if (ms > kDelayCapMs) ms = kDelayCapMs;
  delay((uint32_t)ms);
  return 0;
}

static int l_watch_button(lua_State* L) {
  const char* label = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (gButtonCount >= kMaxButtons) {
    char msg[64];
    snprintf(msg, sizeof(msg), "watch.button: max %d buttons (ignored)\n",
             kMaxButtons);
    Serial.print(msg);
    if (gAppendFn) gAppendFn(msg);
    lua_pushboolean(L, 0);
    return 1;
  }
  if (!gCreateButtonFn) {
    return luaL_error(L, "watch.button: UI not ready");
  }
  int id = gButtonCount;
  if (!gCreateButtonFn(id, label)) {
    return luaL_error(L, "watch.button: create failed");
  }
  lua_pushvalue(L, 2);
  gButtonRefs[id] = luaL_ref(L, LUA_REGISTRYINDEX);
  gButtonCount++;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_watch_on_tick(lua_State* L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (gOnTickRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, gOnTickRef);
    gOnTickRef = LUA_NOREF;
  }
  lua_pushvalue(L, 1);
  gOnTickRef = luaL_ref(L, LUA_REGISTRYINDEX);
  gLastTickMs = 0;  // fire soon
  return 0;
}

static int l_watch_fill(lua_State* L) {
  int r = clampByte(luaL_checkinteger(L, 1));
  int g = clampByte(luaL_checkinteger(L, 2));
  int b = clampByte(luaL_checkinteger(L, 3));
  if (gSetBgFn) gSetBgFn(r, g, b);
  return 0;
}

static int l_watch_rect(lua_State* L) {
  int x = pixArg(L, 1);
  int y = pixArg(L, 2);
  int w = pixArg(L, 3);
  int h = pixArg(L, 4);
  int r = clampByte(luaL_checkinteger(L, 5));
  int g = clampByte(luaL_checkinteger(L, 6));
  int b = clampByte(luaL_checkinteger(L, 7));
  if (gCreateRectFn) gCreateRectFn(x, y, w, h, r, g, b);
  return 0;
}

static int l_watch_circle(lua_State* L) {
  int x = pixArg(L, 1);
  int y = pixArg(L, 2);
  int radius = pixArg(L, 3);
  if (radius < 1) radius = 1;
  int r = clampByte(luaL_checkinteger(L, 4));
  int g = clampByte(luaL_checkinteger(L, 5));
  int b = clampByte(luaL_checkinteger(L, 6));
  if (gCreateCircleFn) gCreateCircleFn(x, y, radius, r, g, b);
  return 0;
}

static int l_watch_gfx_clear(lua_State* L) {
  (void)L;
  if (gGfxClearFn) gGfxClearFn();
  return 0;
}

static int l_watch_ball(lua_State* L) {
  int x = pixArg(L, 1);
  int y = pixArg(L, 2);
  int radius = pixArg(L, 3);
  if (radius < 1) radius = 1;
  int r = clampByte(luaL_checkinteger(L, 4));
  int g = clampByte(luaL_checkinteger(L, 5));
  int b = clampByte(luaL_checkinteger(L, 6));
  if (gBallFn) gBallFn(x, y, radius, r, g, b);
  return 0;
}

static int l_watch_text_at(lua_State* L) {
  int x = pixArg(L, 1);
  int y = pixArg(L, 2);
  const char* str = luaL_checkstring(L, 3);
  int r = 244, g = 247, b = 251;
  if (lua_gettop(L) >= 6) {
    r = clampByte(luaL_checkinteger(L, 4));
    g = clampByte(luaL_checkinteger(L, 5));
    b = clampByte(luaL_checkinteger(L, 6));
  } else if (lua_gettop(L) >= 4) {
    r = clampByte(luaL_checkinteger(L, 4));
    g = r;
    b = r;
  }
  if (gCreateLabelFn) gCreateLabelFn(x, y, str, r, g, b);
  return 0;
}

static int l_watch_battery(lua_State* L) {
  if (!gBatteryFn) {
    lua_pushnil(L);
    return 1;
  }
  int pct = gBatteryFn();
  if (pct < 0) {
    lua_pushnil(L);
    return 1;
  }
  if (pct > 100) pct = 100;
  lua_pushinteger(L, pct);
  return 1;
}

/** Alias for watch.battery() — percent 0..100 or nil. */
static int l_watch_battery_pct(lua_State* L) { return l_watch_battery(L); }

static int l_watch_battery_mv(lua_State* L) {
  if (!gBatteryMvFn) {
    lua_pushnil(L);
    return 1;
  }
  int mv = gBatteryMvFn();
  if (mv < 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, mv);
  return 1;
}

static int l_watch_tri_bool(lua_State* L, int (*fn)(void)) {
  if (!fn) {
    lua_pushnil(L);
    return 1;
  }
  int v = fn();
  if (v < 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushboolean(L, v ? 1 : 0);
  return 1;
}

static int l_watch_charging(lua_State* L) {
  return l_watch_tri_bool(L, gChargingFn);
}

static int l_watch_usb_power(lua_State* L) {
  return l_watch_tri_bool(L, gUsbPowerFn);
}

static int l_watch_battery_connected(lua_State* L) {
  return l_watch_tri_bool(L, gBatteryConnectedFn);
}

static int l_watch_back(lua_State* L) {
  (void)L;
  if (gBackFn) gBackFn();
  return 0;
}

static int l_watch_vibrate(lua_State* L) {
  (void)L;
  // Host callback drives GPIO18 motor (may be unset → silent).
  if (gVibrateFn) gVibrateFn();
  return 0;
}

// print() goes to USB CDC Serial (Arduino Serial).
// Hardware UART0 pins on this board are TX=GPIO43 RX=GPIO44 (not GPIO10/16).
static int l_print(lua_State* L) {
  int n = lua_gettop(L);
  lua_getglobal(L, "tostring");
  for (int i = 1; i <= n; i++) {
    lua_pushvalue(L, -1);
    lua_pushvalue(L, i);
    lua_call(L, 1, 1);
    const char* s = lua_tostring(L, -1);
    if (!s) s = "";
    if (i > 1) {
      Serial.print('\t');
      if (gAppendFn) gAppendFn("\t");
    }
    Serial.print(s);
    if (gAppendFn) gAppendFn(s);
    lua_pop(L, 1);
  }
  Serial.println();
  if (gAppendFn) gAppendFn("\n");
  return 0;
}


static int l_watch_wifi_state(lua_State* L) {
  const char* s = "off";
  if (gWifiStateFn) {
    const char* v = gWifiStateFn();
    if (v && v[0]) s = v;
  }
  lua_pushstring(L, s);
  return 1;
}

static int l_watch_brightness(lua_State* L) {
  lua_Integer pct = luaL_checkinteger(L, 1);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (!gBrightnessFn) {
    lua_pushnil(L);
    return 1;
  }
  int got = gBrightnessFn((int)pct);
  if (got < 0) {
    lua_pushnil(L);
  } else {
    lua_pushinteger(L, got);
  }
  return 1;
}

static int l_watch_imu(lua_State* L) {
  (void)L;
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  if (!gImuFn || !gImuFn(&ax, &ay, &az, &gx, &gy, &gz)) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 6);
  lua_pushnumber(L, ax); lua_setfield(L, -2, "ax");
  lua_pushnumber(L, ay); lua_setfield(L, -2, "ay");
  lua_pushnumber(L, az); lua_setfield(L, -2, "az");
  lua_pushnumber(L, gx); lua_setfield(L, -2, "gx");
  lua_pushnumber(L, gy); lua_setfield(L, -2, "gy");
  lua_pushnumber(L, gz); lua_setfield(L, -2, "gz");
  return 1;
}


static int l_watch_squish_count(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)catalogCount());
  return 1;
}

static int l_watch_squish_search(lua_State* L) {
  const char* prefix = luaL_checkstring(L, 1);
  SquishRecord out[SEARCH_MAX_RESULTS];
  size_t n = searchPrefix(prefix, out, SEARCH_MAX_RESULTS);
  lua_createtable(L, (int)n, 0);
  for (size_t i = 0; i < n; ++i) {
    lua_createtable(L, 0, 7);
    lua_pushstring(L, out[i].name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, out[i].full_name);
    lua_setfield(L, -2, "full_name");
    lua_pushstring(L, out[i].animal);
    lua_setfield(L, -2, "animal");
    lua_pushstring(L, out[i].squad);
    lua_setfield(L, -2, "squad");
    lua_pushstring(L, out[i].size);
    lua_setfield(L, -2, "size");
    lua_pushstring(L, out[i].retail_price_usd);
    lua_setfield(L, -2, "retail_price_usd");
    lua_pushstring(L, out[i].bio);
    lua_setfield(L, -2, "bio");
    lua_rawseti(L, -2, (int)i + 1);
  }
  return 1;
}

static int l_watch_letter_pad(lua_State* L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (!gCreateLetterPadFn) {
    return luaL_error(L, "watch.letter_pad: UI not ready");
  }
  if (gLetterPadRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, gLetterPadRef);
    gLetterPadRef = LUA_NOREF;
  }
  if (!gCreateLetterPadFn()) {
    return luaL_error(L, "watch.letter_pad: create failed");
  }
  lua_pushvalue(L, 1);
  gLetterPadRef = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_pushboolean(L, 1);
  return 1;
}


static bool pathUnderApps(const char* path) {
  if (!path || path[0] != '/') return false;
  if (strncmp(path, "/apps/", 6) != 0) return false;
  // Reject ".." path segments (simple sandbox).
  const char* p = path;
  while ((p = strstr(p, "..")) != nullptr) {
    if ((p == path || p[-1] == '/') && (p[2] == '\0' || p[2] == '/')) {
      return false;
    }
    p += 2;
  }
  return true;
}

static void ensureParentDir(const char* path) {
  if (!path) return;
  char parent[96];
  strncpy(parent, path, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char* slash = strrchr(parent, '/');
  if (!slash || slash == parent) return;
  *slash = '\0';
  if (parent[0] && !LittleFS.exists(parent)) {
    LittleFS.mkdir(parent);
  }
}

static constexpr size_t kMaxFileBytes = 8192;

static int l_watch_read_file(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  if (!pathUnderApps(path)) {
    lua_pushnil(L);
    return 1;
  }
  if (!LittleFS.exists(path)) {
    lua_pushnil(L);
    return 1;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    lua_pushnil(L);
    return 1;
  }
  size_t sz = f.size();
  if (sz > kMaxFileBytes) sz = kMaxFileBytes;
  char* buf = (char*)malloc(sz + 1);
  if (!buf) {
    f.close();
    lua_pushnil(L);
    return 1;
  }
  size_t n = f.readBytes(buf, sz);
  f.close();
  buf[n] = '\0';
  lua_pushlstring(L, buf, n);
  free(buf);
  return 1;
}

static int l_watch_write_file(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);
  if (!pathUnderApps(path)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  if (len > kMaxFileBytes) {
    lua_pushboolean(L, 0);
    return 1;
  }
  ensureParentDir(path);
  File f = LittleFS.open(path, "w");
  if (!f) {
    lua_pushboolean(L, 0);
    return 1;
  }
  size_t wrote = f.write((const uint8_t*)data, len);
  f.close();
  lua_pushboolean(L, wrote == len);
  return 1;
}


static int l_watch_button_layout(lua_State* L) {
  const char* mode = luaL_checkstring(L, 1);
  int cols = 2;
  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    cols = (int)luaL_checkinteger(L, 2);
  }
  if (cols < 1) cols = 1;
  if (cols > 4) cols = 4;
  if (!gButtonLayoutFn) {
    lua_pushboolean(L, 0);
    return 1;
  }
  bool ok = gButtonLayoutFn(mode, cols);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_touch(lua_State* L) {
  if (!gTouchFn) {
    lua_pushnil(L);
    return 1;
  }
  int x = 0, y = 0;
  bool pressed = false;
  if (!gTouchFn(&x, &y, &pressed) || !pressed) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, x);
  lua_setfield(L, -2, "x");
  lua_pushinteger(L, y);
  lua_setfield(L, -2, "y");
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "pressed");
  return 1;
}

static int l_watch_clear_ui(lua_State* L) {
  // Tear down widgets + button/letter_pad refs so a new screen can rebuild
  // with a fresh set of up to kMaxButtons watch.button()s. Leaves on_tick
  // alone. Layout mode is NOT reset here (host keeps last mode until
  // watch.button_layout() or a new app open).
  if (gClearImageFn) gClearImageFn();
  if (gClearWidgetsFn) gClearWidgetsFn();
  if (gLetterPadRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, gLetterPadRef);
    gLetterPadRef = LUA_NOREF;
  }
  for (int i = 0; i < kMaxButtons; ++i) {
    if (gButtonRefs[i] != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, gButtonRefs[i]);
      gButtonRefs[i] = LUA_NOREF;
    }
  }
  gButtonCount = 0;
  if (gClearFn) gClearFn();
  return 0;
}


static int l_watch_safe_inset(lua_State* L) {
  lua_pushinteger(L, LCD_SAFE_INSET_PX);
  return 1;
}

static int l_watch_corner_inset(lua_State* L) {
  // Panel corner radius in px (AMOLED rounded corners).
  lua_pushinteger(L, LCD_CORNER_RADIUS_PX);
  return 1;
}

static constexpr size_t kMaxSdReadBytes = 8192;
static constexpr int kMaxSdListEntries = 64;


static int l_watch_usb_msc_active(lua_State* L) {
  lua_pushboolean(L, usbMscActive() ? 1 : 0);
  return 1;
}

static int l_watch_usb_msc_enter(lua_State* L) {
  bool ok = usbMscEnter();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_usb_msc_exit(lua_State* L) {
  bool ok = usbMscExit();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_usb_msc_status(lua_State* L) {
  lua_pushstring(L, usbMscStatusLine());
  return 1;
}

static int l_watch_sd_ready(lua_State* L) {
  lua_pushboolean(L, sdHostReady() ? 1 : 0);
  return 1;
}

static int l_watch_sd_mount(lua_State* L) {
  bool ok = sdHostMount();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_sd_unmount(lua_State* L) {
  bool ok = sdHostUnmount();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_sd_info(lua_State* L) {
  if (!sdHostReady()) {
    lua_pushnil(L);
    return 1;
  }
  uint64_t total = sdHostTotalBytes();
  uint64_t used = sdHostUsedBytes();
  lua_createtable(L, 0, 4);
  lua_pushnumber(L, (lua_Number)(total / (1024.0 * 1024.0)));
  lua_setfield(L, -2, "total_mb");
  lua_pushnumber(L, (lua_Number)(used / (1024.0 * 1024.0)));
  lua_setfield(L, -2, "used_mb");
  lua_pushinteger(L, (lua_Integer)total);
  lua_setfield(L, -2, "total_bytes");
  lua_pushinteger(L, (lua_Integer)used);
  lua_setfield(L, -2, "used_bytes");
  return 1;
}

static int l_watch_sd_list(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  if (!sdHostReady() || !sdHostPathOk(path)) {
    lua_pushnil(L);
    return 1;
  }
  char names[kMaxSdListEntries][64];
  uint32_t sizes[kMaxSdListEntries];
  bool isDir[kMaxSdListEntries];
  int n = sdHostList(path, names, sizes, isDir, kMaxSdListEntries);
  if (n < 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; ++i) {
    lua_createtable(L, 0, 3);
    lua_pushstring(L, names[i]);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)sizes[i]);
    lua_setfield(L, -2, "size");
    lua_pushboolean(L, isDir[i] ? 1 : 0);
    lua_setfield(L, -2, "is_dir");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_watch_sd_read(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  size_t maxBytes = kMaxSdReadBytes;
  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    lua_Integer mb = luaL_checkinteger(L, 2);
    if (mb < 0) mb = 0;
    if (mb > (lua_Integer)kMaxSdReadBytes) mb = (lua_Integer)kMaxSdReadBytes;
    maxBytes = (size_t)mb;
  }
  if (!sdHostReady() || !sdHostPathOk(path) || maxBytes == 0) {
    lua_pushnil(L);
    return 1;
  }
  char* buf = (char*)malloc(maxBytes + 1);
  if (!buf) {
    lua_pushnil(L);
    return 1;
  }
  int n = sdHostRead(path, buf, maxBytes);
  if (n < 0) {
    free(buf);
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, buf, (size_t)n);
  free(buf);
  return 1;
}

static int l_watch_sd_write(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);
  if (!sdHostReady() || !sdHostPathOk(path) || len > kMaxSdReadBytes) {
    lua_pushboolean(L, 0);
    return 1;
  }
  bool ok = sdHostWrite(path, data, len);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}


static bool pathLooksSd(const char* path) {
  return path && (strcmp(path, "/sd") == 0 || strncmp(path, "/sd/", 4) == 0);
}

static bool hostPathExists(const char* path) {
  if (!path || !path[0]) return false;
  if (pathLooksSd(path)) {
    if (!sdHostPathOk(path)) return false;
    if (!sdHostReady() && !sdHostMount()) return false;
    return sdHostExists(path);
  }
  // LittleFS paths (apps assets, etc.) — do not remount/format.
  return LittleFS.exists(path);
}

static void slugifyName(const char* name, char* out, size_t outSz) {
  if (!out || outSz == 0) return;
  size_t j = 0;
  if (!name) {
    out[0] = '\0';
    return;
  }
  for (size_t i = 0; name[i] && j + 1 < outSz; ++i) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else {
      out[j++] = '_';
    }
  }
  out[j] = '\0';
}

static int l_watch_image(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int max_w = 160;
  int max_h = 160;
  if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) max_w = (int)luaL_checkinteger(L, 4);
  if (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) max_h = (int)luaL_checkinteger(L, 5);
  if (!gShowImageFn) {
    lua_pushboolean(L, 0);
    return 1;
  }
  bool ok = gShowImageFn(path, x, y, max_w, max_h);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_image_clear(lua_State* L) {
  (void)L;
  if (gClearImageFn) gClearImageFn();
  return 0;
}

static int l_watch_image_exists(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  lua_pushboolean(L, hostPathExists(path) ? 1 : 0);
  return 1;
}

static int l_watch_squish_image_path(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  char slug[64];
  slugifyName(name, slug, sizeof(slug));
  if (!slug[0]) {
    lua_pushnil(L);
    return 1;
  }
  char sdPath[96];
  char lfsPath[96];
  snprintf(sdPath, sizeof(sdPath), "/sd/squish/img/%s.wrgb", slug);
  snprintf(lfsPath, sizeof(lfsPath), "/apps/squish_id/img/%s.wrgb", slug);
  // Prefer SD full catalog when present; else LittleFS samples.
  Serial.printf("squish_image_path: name='%s' slug='%s'\n", name, slug);
  Serial.printf("squish_image_path: try SD %s\n", sdPath);
  if (hostPathExists(sdPath)) {
    Serial.printf("squish_image_path: HIT SD %s\n", sdPath);
    lua_pushstring(L, sdPath);
    return 1;
  }
  Serial.printf("squish_image_path: miss SD, try LFS %s\n", lfsPath);
  if (hostPathExists(lfsPath)) {
    Serial.printf("squish_image_path: HIT LFS %s\n", lfsPath);
    lua_pushstring(L, lfsPath);
    return 1;
  }
  Serial.printf("squish_image_path: MISS both for slug='%s'\n", slug);
  lua_pushnil(L);
  return 1;
}



static bool pathHasDotDot(const char* path) {
  if (!path) return true;
  const char* p = path;
  while ((p = strstr(p, "..")) != nullptr) {
    if ((p == path || p[-1] == '/') && (p[2] == '\0' || p[2] == '/')) return true;
    p += 2;
  }
  return false;
}

static int l_watch_sd_mkdir(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  if (!sdHostReady() || !sdHostPathOk(path) || pathHasDotDot(path)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  bool ok = sdHostMkdir(path);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static constexpr size_t kMaxSdPutBytes = 256 * 1024;
static constexpr size_t kSdPutChunk = 4096;

static int l_watch_sd_put_lfs(lua_State* L) {
  const char* lfsSrc = luaL_checkstring(L, 1);
  const char* sdDst = luaL_checkstring(L, 2);
  if (!pathUnderApps(lfsSrc) || pathHasDotDot(lfsSrc) ||
      !sdHostPathOk(sdDst) || pathHasDotDot(sdDst) || !sdHostReady()) {
    lua_pushboolean(L, 0);
    return 1;
  }
  if (!LittleFS.exists(lfsSrc)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  File in = LittleFS.open(lfsSrc, "r");
  if (!in || in.isDirectory()) {
    if (in) in.close();
    lua_pushboolean(L, 0);
    return 1;
  }
  size_t sz = in.size();
  if (sz > kMaxSdPutBytes) {
    in.close();
    lua_pushboolean(L, 0);
    return 1;
  }
  // Stream via temp buffer into SD (binary-safe).
  // Prefer single-shot write when heap allows; else chunked open/write.
  uint8_t* buf = (uint8_t*)malloc(sz > 0 ? sz : 1);
  if (buf) {
    size_t n = in.read(buf, sz);
    in.close();
    bool ok = (n == sz) && sdHostWriteBytes(sdDst, buf, n);
    free(buf);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }
  // Low-heap fallback: chunked write using SD_MMC via repeated writeBytes rebuild.
  // Re-open and assemble is too heavy; fail soft.
  in.close();
  lua_pushboolean(L, 0);
  return 1;
}

static constexpr int kMaxLfsListEntries = 256;

static int l_watch_lfs_list(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  if (!pathUnderApps(path) || pathHasDotDot(path)) {
    lua_pushnil(L);
    return 1;
  }
  if (!LittleFS.exists(path)) {
    lua_pushnil(L);
    return 1;
  }
  File root = LittleFS.open(path);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  int idx = 0;
  File ent = root.openNextFile();
  while (ent && idx < kMaxLfsListEntries) {
    const char* full = ent.name();
    const char* base = full;
    const char* slash = strrchr(full, '/');
    if (slash && slash[1]) base = slash + 1;
    lua_createtable(L, 0, 3);
    lua_pushstring(L, base);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)ent.size());
    lua_setfield(L, -2, "size");
    lua_pushboolean(L, ent.isDirectory() ? 1 : 0);
    lua_setfield(L, -2, "is_dir");
    lua_rawseti(L, -2, ++idx);
    ent = root.openNextFile();
  }
  root.close();
  return 1;
}


static constexpr size_t kMaxUartReadBytes = 512;

static int l_watch_uart_ready(lua_State* L) {
  lua_pushboolean(L, uartHostReady() ? 1 : 0);
  return 1;
}

static int l_watch_uart_open(lua_State* L) {
  uint32_t baud = 115200;
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    lua_Integer b = luaL_checkinteger(L, 1);
    if (b < 300) b = 300;
    if (b > 921600) b = 921600;
    baud = (uint32_t)b;
  }
  bool ok = uartHostOpen(baud);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_uart_close(lua_State* L) {
  bool ok = uartHostClose();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int l_watch_uart_available(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)uartHostAvailable());
  return 1;
}

static int l_watch_uart_write(lua_State* L) {
  size_t len = 0;
  const char* data = luaL_checklstring(L, 1, &len);
  int n = uartHostWrite(data, len);
  if (n < 0) {
    lua_pushnil(L);
  } else {
    lua_pushinteger(L, (lua_Integer)n);
  }
  return 1;
}

static int l_watch_uart_read(lua_State* L) {
  size_t maxBytes = kMaxUartReadBytes;
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    lua_Integer mb = luaL_checkinteger(L, 1);
    if (mb < 0) mb = 0;
    if (mb > (lua_Integer)kMaxUartReadBytes) mb = (lua_Integer)kMaxUartReadBytes;
    maxBytes = (size_t)mb;
  }
  if (maxBytes == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  char* buf = (char*)malloc(maxBytes);
  if (!buf) {
    lua_pushnil(L);
    return 1;
  }
  int n = uartHostRead(buf, maxBytes);
  if (n < 0) {
    free(buf);
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, buf, (size_t)n);
  free(buf);
  return 1;
}

static void registerWatch(lua_State* L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_watch_set_title);
  lua_setfield(L, -2, "set_title");
  lua_pushcfunction(L, l_watch_set_text);
  lua_setfield(L, -2, "set_text");
  lua_pushcfunction(L, l_watch_append_text);
  lua_setfield(L, -2, "append_text");
  lua_pushcfunction(L, l_watch_clear);
  lua_setfield(L, -2, "clear");
  lua_pushcfunction(L, l_watch_width);
  lua_setfield(L, -2, "width");
  lua_pushcfunction(L, l_watch_height);
  lua_setfield(L, -2, "height");
  lua_pushcfunction(L, l_watch_millis);
  lua_setfield(L, -2, "millis");
  lua_pushcfunction(L, l_watch_time);
  lua_setfield(L, -2, "time");
  lua_pushcfunction(L, l_watch_delay);
  lua_setfield(L, -2, "delay");
  lua_pushcfunction(L, l_watch_button);
  lua_setfield(L, -2, "button");
  lua_pushcfunction(L, l_watch_on_tick);
  lua_setfield(L, -2, "on_tick");
  lua_pushcfunction(L, l_watch_fill);
  lua_setfield(L, -2, "fill");
  lua_pushcfunction(L, l_watch_rect);
  lua_setfield(L, -2, "rect");
  lua_pushcfunction(L, l_watch_circle);
  lua_setfield(L, -2, "circle");
  lua_pushcfunction(L, l_watch_gfx_clear);
  lua_setfield(L, -2, "gfx_clear");
  lua_pushcfunction(L, l_watch_ball);
  lua_setfield(L, -2, "ball");
  lua_pushcfunction(L, l_watch_text_at);
  lua_setfield(L, -2, "text_at");
  lua_pushcfunction(L, l_watch_battery);
  lua_setfield(L, -2, "battery");
  lua_pushcfunction(L, l_watch_battery_pct);
  lua_setfield(L, -2, "battery_pct");
  lua_pushcfunction(L, l_watch_battery_mv);
  lua_setfield(L, -2, "battery_mv");
  lua_pushcfunction(L, l_watch_charging);
  lua_setfield(L, -2, "charging");
  lua_pushcfunction(L, l_watch_usb_power);
  lua_setfield(L, -2, "usb_power");
  lua_pushcfunction(L, l_watch_battery_connected);
  lua_setfield(L, -2, "battery_connected");
  lua_pushcfunction(L, l_watch_back);
  lua_setfield(L, -2, "back");
  lua_pushcfunction(L, l_watch_vibrate);
  lua_setfield(L, -2, "vibrate");
  lua_pushcfunction(L, l_watch_wifi_state);
  lua_setfield(L, -2, "wifi_state");
  lua_pushcfunction(L, l_watch_brightness);
  lua_setfield(L, -2, "brightness");
  lua_pushcfunction(L, l_watch_imu);
  lua_setfield(L, -2, "imu");
  lua_pushcfunction(L, l_watch_squish_count);
  lua_setfield(L, -2, "squish_count");
  lua_pushcfunction(L, l_watch_squish_search);
  lua_setfield(L, -2, "squish_search");
  lua_pushcfunction(L, l_watch_letter_pad);
  lua_setfield(L, -2, "letter_pad");
  lua_pushcfunction(L, l_watch_read_file);
  lua_setfield(L, -2, "read_file");
  lua_pushcfunction(L, l_watch_write_file);
  lua_setfield(L, -2, "write_file");
  lua_pushcfunction(L, l_watch_clear_ui);
  lua_setfield(L, -2, "clear_ui");
  lua_pushcfunction(L, l_watch_button_layout);
  lua_setfield(L, -2, "button_layout");
  lua_pushcfunction(L, l_watch_touch);
  lua_setfield(L, -2, "touch");
  lua_pushcfunction(L, l_watch_safe_inset);
  lua_setfield(L, -2, "safe_inset");
  lua_pushcfunction(L, l_watch_corner_inset);
  lua_setfield(L, -2, "corner_inset");
  lua_pushcfunction(L, l_watch_sd_ready);
  lua_setfield(L, -2, "sd_ready");
  lua_pushcfunction(L, l_watch_sd_mount);
  lua_setfield(L, -2, "sd_mount");
  lua_pushcfunction(L, l_watch_sd_unmount);
  lua_setfield(L, -2, "sd_unmount");
  lua_pushcfunction(L, l_watch_sd_info);
  lua_setfield(L, -2, "sd_info");
  lua_pushcfunction(L, l_watch_sd_list);
  lua_setfield(L, -2, "sd_list");
  lua_pushcfunction(L, l_watch_sd_read);
  lua_setfield(L, -2, "sd_read");
  lua_pushcfunction(L, l_watch_sd_write);
  lua_setfield(L, -2, "sd_write");
  lua_pushcfunction(L, l_watch_sd_mkdir);
  lua_setfield(L, -2, "sd_mkdir");
  lua_pushcfunction(L, l_watch_sd_put_lfs);
  lua_setfield(L, -2, "sd_put_lfs");
  lua_pushcfunction(L, l_watch_usb_msc_active);
  lua_setfield(L, -2, "usb_msc_active");
  lua_pushcfunction(L, l_watch_usb_msc_enter);
  lua_setfield(L, -2, "usb_msc_enter");
  lua_pushcfunction(L, l_watch_usb_msc_exit);
  lua_setfield(L, -2, "usb_msc_exit");
  lua_pushcfunction(L, l_watch_usb_msc_status);
  lua_setfield(L, -2, "usb_msc_status");
  lua_pushcfunction(L, l_watch_lfs_list);
  lua_setfield(L, -2, "lfs_list");
  lua_pushcfunction(L, l_watch_uart_ready);
  lua_setfield(L, -2, "uart_ready");
  lua_pushcfunction(L, l_watch_uart_open);
  lua_setfield(L, -2, "uart_open");
  lua_pushcfunction(L, l_watch_uart_close);
  lua_setfield(L, -2, "uart_close");
  lua_pushcfunction(L, l_watch_uart_available);
  lua_setfield(L, -2, "uart_available");
  lua_pushcfunction(L, l_watch_uart_write);
  lua_setfield(L, -2, "uart_write");
  lua_pushcfunction(L, l_watch_uart_read);
  lua_setfield(L, -2, "uart_read");

  lua_pushcfunction(L, l_watch_image);
  lua_setfield(L, -2, "image");
  lua_pushcfunction(L, l_watch_image_clear);
  lua_setfield(L, -2, "image_clear");
  lua_pushcfunction(L, l_watch_image_exists);
  lua_setfield(L, -2, "image_exists");
  lua_pushcfunction(L, l_watch_squish_image_path);
  lua_setfield(L, -2, "squish_image_path");
  luaApiExtRegister(L);  // layout/input/prefs/net/audio/ble/zip/gpio/…
  lua_setglobal(L, "watch");

  lua_pushcfunction(L, l_print);
  lua_setglobal(L, "print");
}

static void unrefAll(lua_State* L) {
  if (!L) return;
  luaApiExtUnref(L);
  if (gOnTickRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, gOnTickRef);
    gOnTickRef = LUA_NOREF;
  }
  if (gLetterPadRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, gLetterPadRef);
    gLetterPadRef = LUA_NOREF;
  }
  for (int i = 0; i < kMaxButtons; ++i) {
    if (gButtonRefs[i] != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, gButtonRefs[i]);
      gButtonRefs[i] = LUA_NOREF;
    }
  }
  gButtonCount = 0;
}

void luaHostClose(void) {
  jobHostReset();
  audioHostMicStop();
  audioHostSpeakerStop();
  if (gClearWidgetsFn) gClearWidgetsFn();
  if (gL) {
    unrefAll(gL);
    lua_close(gL);
    gL = nullptr;
  }
  gLastTickMs = 0;
}

bool luaHostIsOpen(void) { return gL != nullptr; }

void luaHostButtonClicked(int buttonId) {
  if (!gL) return;
  if (buttonId < 0 || buttonId >= gButtonCount) return;
  int ref = gButtonRefs[buttonId];
  if (ref == LUA_NOREF) return;
  lua_rawgeti(gL, LUA_REGISTRYINDEX, ref);
  if (!lua_isfunction(gL, -1)) {
    lua_pop(gL, 1);
    return;
  }
  int status = lua_pcall(gL, 0, 0, 0);
  if (status != LUA_OK) {
    const char* msg = lua_tostring(gL, -1);
    Serial.printf("lua button error: %s\n", msg ? msg : "?");
    lua_pop(gL, 1);
  }
}

void luaHostLetterPadKey(const char* key) {
  if (!gL || !key) return;
  if (gLetterPadRef == LUA_NOREF) return;
  lua_rawgeti(gL, LUA_REGISTRYINDEX, gLetterPadRef);
  if (!lua_isfunction(gL, -1)) {
    lua_pop(gL, 1);
    return;
  }
  lua_pushstring(gL, key);
  int status = lua_pcall(gL, 1, 0, 0);
  if (status != LUA_OK) {
    const char* msg = lua_tostring(gL, -1);
    Serial.printf("lua letter_pad error: %s\n", msg ? msg : "?");
    lua_pop(gL, 1);
  }
}

void luaHostPoll(void) {
  luaApiExtPoll();
  if (!gL || gOnTickRef == LUA_NOREF) return;
  uint32_t now = millis();
  if (gLastTickMs != 0 && (now - gLastTickMs) < kTickIntervalMs) return;
  gLastTickMs = now;

  lua_rawgeti(gL, LUA_REGISTRYINDEX, gOnTickRef);
  if (!lua_isfunction(gL, -1)) {
    lua_pop(gL, 1);
    return;
  }
  int status = lua_pcall(gL, 0, 0, 0);
  if (status != LUA_OK) {
    const char* msg = lua_tostring(gL, -1);
    Serial.printf("lua on_tick error: %s\n", msg ? msg : "?");
    lua_pop(gL, 1);
  }
}

bool luaHostOpen(const char* scriptPath, char* errBuf, size_t errLen) {
  if (errBuf && errLen) errBuf[0] = '\0';
  luaHostClose();

  if (!scriptPath || !scriptPath[0]) {
    setErr(errBuf, errLen, "no script path");
    return false;
  }

  const bool onSd = (strncmp(scriptPath, "/sd/", 4) == 0);
  File f;
  if (onSd) {
    if (!sdHostPathOk(scriptPath)) {
      setErr(errBuf, errLen, "bad sd path");
      return false;
    }
    if (!sdHostReady() && !sdHostMount()) {
      setErr(errBuf, errLen, "sd not mounted");
      return false;
    }
    if (!sdHostExists(scriptPath)) {
      setErr(errBuf, errLen, "script not found");
      return false;
    }
    if (!sdHostOpenRead(scriptPath, &f)) {
      setErr(errBuf, errLen, "open failed");
      return false;
    }
  } else {
    if (!LittleFS.exists(scriptPath)) {
      setErr(errBuf, errLen, "script not found");
      return false;
    }
    f = LittleFS.open(scriptPath, "r");
    if (!f) {
      setErr(errBuf, errLen, "open failed");
      return false;
    }
  }

  lua_State* L = lua_newstate(luaPsramAlloc, nullptr);
  if (!L) {
    f.close();
    setErr(errBuf, errLen, "lua_newstate failed");
    return false;
  }

  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(L, 1);

  registerWatch(L);

  FsReader reader;
  reader.file = f;

  int loadStatus = lua_load(L, littlefsReader, &reader, scriptPath, "t");
  reader.file.close();

  if (loadStatus != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    setErr(errBuf, errLen, msg ? msg : "load error");
    Serial.printf("lua load error: %s\n", msg ? msg : "?");
    lua_close(L);
    return false;
  }

  // State must be visible during pcall so watch.button / on_tick can register.
  gL = L;
  luaApiExtSessionReset();
  gOnTickRef = LUA_NOREF;
  gLetterPadRef = LUA_NOREF;
  gButtonCount = 0;
  for (int i = 0; i < kMaxButtons; ++i) gButtonRefs[i] = LUA_NOREF;
  gLastTickMs = 0;

  int callStatus = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (callStatus != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    setErr(errBuf, errLen, msg ? msg : "runtime error");
    Serial.printf("lua runtime error: %s\n", msg ? msg : "?");
    luaHostClose();
    return false;
  }

  // Leave leftovers from MULTRET cleaned up.
  lua_settop(L, 0);
  return true;
}

bool luaHostRunFile(const char* scriptPath, char* errBuf, size_t errLen) {
  return luaHostOpen(scriptPath, errBuf, errLen);
}
