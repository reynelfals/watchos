/*
 * Watch OS — minimal native shell
 * Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502 capacitive AMOLED)
 *
 * Boot = discreet digital clock cover. Tap → PIN → app launcher.
 * Display: CO5300 QSPI (Arduino_GFX)  Touch: FT3168 I2C  RTC: PCF85063 (opt)
 * Power: AXP2101 (XPowersLib) — enable ALDO/BLDO rails before gfx->begin()
 * Haptic: vibration motor GPIO18  IMU: QMI8658 I2C 0x6B (SensorLib)
 * Apps: LittleFS /apps/ + SD /sd/apps/ (app.json); Lua 5.4 runs entry scripts
 * UI: LVGL 9
 */

#include <Arduino.h>

// Lua + LVGL + audio record/play need headroom beyond default 8 KiB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>
#include <Wire.h>
#include <string.h>
#ifndef XPOWERS_CHIP_AXP2101
#define XPOWERS_CHIP_AXP2101
#endif
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>

#include "pin_config.h"
#include "app_secrets.h"
#include "ft3168.h"
#include "pcf85063.h"
#include "apps_host.h"
#include "display_geometry.h"
#include "wrgb_image.h"
#include "search.h"
#include "lua_host.h"
#include "wasm_host.h"
#include "job_host.h"
#include "lua_api_ext.h"
#include "sd_host.h"
#include "usb_msc_host.h"
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include "wifi_ota.h"
#include "ble_mesh_host.h"
#include "audio_host.h"
#include "qrcode.h"
#include <esp_heap_caps.h>

static Arduino_DataBus* bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

// Official Arduino_GFX 1.6.0 constructor includes the IPS flag.
// Waveshare HelloWorld uses col_offset1 = 22 for this panel.
static Arduino_CO5300* amoled = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */, false /* IPS */,
    LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);
static Arduino_GFX* gfx = amoled;

// AXP2101 PMIC — required so AMOLED / peripheral rails stay enabled
static XPowersPMU power;
static bool gPmicOk = false;
static SensorQMI8658 qmi;
static bool gImuOk = false;
static uint32_t gVibrateUntilMs = 0;  // non-zero while motor pulse active
static lv_obj_t* gNotifBanner = nullptr;
static uint32_t gNotifUntilMs = 0;
static char gNotifAppId[APP_ID_MAX] = {0};
static char gNotifText[96] = {0};


static lv_display_t* disp = nullptr;
static uint32_t screenWidth = LCD_WIDTH;
static uint32_t screenHeight = LCD_HEIGHT;

static lv_obj_t* scrClock = nullptr;
static lv_obj_t* scrPin = nullptr;
static lv_obj_t* scrLauncher = nullptr;
static lv_obj_t* scrStub = nullptr;
static lv_obj_t* scrSetTime = nullptr;
static lv_obj_t* scrOta = nullptr;
static lv_obj_t* scrManage = nullptr;
static lv_obj_t* scrSettings = nullptr;

static lv_obj_t* lblClockTime = nullptr;
static lv_obj_t* lblClockDate = nullptr;
static lv_obj_t* lblClockSec = nullptr;
static lv_obj_t* lblClockBatt = nullptr;
static lv_obj_t* lblClockHint = nullptr;
static lv_obj_t* lblClockWifi = nullptr;
static lv_obj_t* lblClockWifiCap = nullptr;
static lv_obj_t* lblLauncherWifi = nullptr;

static lv_obj_t* lblOtaTitle = nullptr;
static lv_obj_t* lblOtaBody = nullptr;
static lv_obj_t* lblOtaStatus = nullptr;
static lv_obj_t* lblOtaHint = nullptr;
static lv_obj_t* canvasOtaQr = nullptr;
static lv_obj_t* contOtaQr = nullptr;  // clickable wrapper for toggle
static lv_obj_t* btnOtaUsbSd = nullptr;
static lv_obj_t* lblOtaUsbSd = nullptr;
static uint16_t* gOtaQrBuf = nullptr;
static uint16_t gOtaQrBufW = 0;
static uint16_t gOtaQrBufH = 0;
static bool gOtaQrShowWifi = true;  // true=Wi‑Fi join, false=install URL
static char gOtaQrWifiPayload[80] = {0};
static char gOtaQrUrlPayload[80] = {0};

static lv_obj_t* lblPinDots = nullptr;
static lv_obj_t* lblPinMsg = nullptr;
static lv_obj_t* pinPad = nullptr;

static lv_obj_t* lblLauncherStatus = nullptr;
static lv_obj_t* contApps = nullptr;  // scroll area: grid or list

// Launcher view mode (NVS namespace watchos / key launcher_view).
enum LauncherView : uint8_t { kLaunchGrid = 0, kLaunchList = 1 };
static LauncherView gLauncherView = kLaunchGrid;

// Cached WRGB icons for current launcher rebuild (freed on each refresh).
static constexpr uint16_t kLaunchIconMaxDim = 96;
static constexpr int kLaunchIconDisp = 72;
static constexpr int kLaunchIconList = 48;
static WrgbImage gLaunchIcons[APPS_MAX];
static bool gLaunchIconsValid[APPS_MAX];
static lv_obj_t* listManage = nullptr;
static lv_obj_t* lblManageStatus = nullptr;
static lv_obj_t* contManageConfirm = nullptr;
static lv_obj_t* lblManageConfirm = nullptr;
static char gPendingUninstallId[APP_ID_MAX] = {0};
static char gPendingUninstallName[APP_NAME_MAX] = {0};
static AppStorage gPendingUninstallStorage = APP_STORAGE_LFS;
static lv_obj_t* lblStubTitle = nullptr;
static lv_obj_t* lblStubBody = nullptr;
static lv_obj_t* luaLayer = nullptr;       // absolute-position overlay for Lua widgets
static lv_obj_t* luaBall = nullptr;        // one persistent ball (watch.ball)
static int luaBallRadius = 0;
static lv_obj_t* luaImage = nullptr;       // WRGB thumb on luaLayer
static lv_obj_t* luaCanvas = nullptr;      // RGB565 pixel canvas on luaLayer
static uint16_t* luaCanvasBuf = nullptr;
static int luaCanvasW = 0;
static int luaCanvasH = 0;
static int luaCanvasX = 0;
static int luaCanvasY = 0;
static uint8_t* luaImageBuf = nullptr;     // RGB565 pixel heap/PSRAM
static lv_image_dsc_t luaImageDsc;
static lv_obj_t* luaBtnColumn = nullptr;  // stacked Lua buttons
static lv_obj_t* luaLetterPad = nullptr;  // A–Z host letter pad (buttonmatrix)
static lv_obj_t* stubBackBtn = nullptr;   // native Back; hidden while letter pad up
static lv_obj_t* lblSetTimeVal = nullptr;

static String gPinEntry;
static uint8_t gPinFails = 0;
static uint32_t gPinLockUntil = 0;
static int gEditHour = 12;
static int gEditMin = 0;
static AppInfo gStubApp;

// Soft-lock: idle on launcher / shell covers returns to clock cover (not open apps).
static uint32_t gLastActivityMs = 0;
static bool gTouchWasPressed = false;
static int16_t gTouchLastX = 0;
static int16_t gTouchLastY = 0;
static bool gTouchPressedNow = false;

// Lua button container layout: "column" (default for old apps) or "grid2".
// clear_ui keeps this mode; only button_layout() / app open resets it.
enum LuaBtnLayout : uint8_t { kLuaBtnColumn = 0, kLuaBtnGrid2 = 1 };
static LuaBtnLayout gLuaBtnLayout = kLuaBtnColumn;
static int gLuaBtnCols = 2;
static bool gLuaBackRequested = false;
static uint32_t gSoftLockIdleSec = WATCHOS_SOFT_LOCK_IDLE_SEC;
static constexpr int kMaxLuaProgress = 4;
static constexpr int kMaxLuaSliders = 4;
static lv_obj_t* luaProgress[kMaxLuaProgress] = {};
static lv_obj_t* luaSliders[kMaxLuaSliders] = {};
static int gPendingSliderId = -1;
static int gPendingSliderVal = 0;


static const lv_color_t COL_BG = lv_color_hex(0x050508);
static const lv_color_t COL_PANEL = lv_color_hex(0x12141C);
static const lv_color_t COL_KEY = lv_color_hex(0x1C2740);
static const lv_color_t COL_ACCENT = lv_color_hex(0x3EE0F0);
static const lv_color_t COL_TEXT = lv_color_hex(0xF4F7FB);
static const lv_color_t COL_DIM = lv_color_hex(0x8B93A7);
static const lv_color_t COL_WARN = lv_color_hex(0xFF9F43);
static const lv_color_t COL_ERR = lv_color_hex(0xFF6B6B);

static uint32_t pinMaxLen() {
  size_t n = strlen(WATCHOS_UNLOCK_PIN);
  if (n == 0) n = 4;
  if (n > 8) n = 8;
  return (uint32_t)n;
}

static uint32_t millis_cb(void) { return millis(); }

static void my_disp_flush(lv_display_t* d, const lv_area_t* area, uint8_t* px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
  lv_display_flush_ready(d);
}

static void rounder_event_cb(lv_event_t* e) {
  lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
  area->x1 = (area->x1 >> 1) << 1;
  area->y1 = (area->y1 >> 1) << 1;
  area->x2 = ((area->x2 >> 1) << 1) + 1;
  area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void noteActivity() { gLastActivityMs = millis(); }

static void my_touchpad_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
  int16_t x = 0, y = 0;
  if (ft3168Read(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
    gTouchLastX = x;
    gTouchLastY = y;
    gTouchPressedNow = true;
    // Any touch resets soft-lock idle (including PIN entry — harmless there).
    noteActivity();
    gTouchWasPressed = true;
    luaApiExtTouchSample((int)x, (int)y, true);
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    if (gTouchPressedNow) {
      luaApiExtTouchSample((int)gTouchLastX, (int)gTouchLastY, false);
    }
    gTouchPressedNow = false;
    gTouchWasPressed = false;
  }
}

static void style_screen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(scr, COL_TEXT, 0);
  lv_obj_set_style_text_font(scr, &lv_font_montserrat_16, 0);
}

static void style_keypad(lv_obj_t* kb) {
  lv_obj_set_style_bg_color(kb, COL_BG, 0);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(kb, 0, 0);
  lv_obj_set_style_pad_all(kb, 2, 0);
  lv_obj_set_style_pad_row(kb, 6, 0);
  lv_obj_set_style_pad_column(kb, 6, 0);
  lv_obj_set_style_bg_color(kb, COL_KEY, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
}

// ---- system / RTC time ----------------------------------------------------

static int monthFromAbbrev(const char* m) {
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (int i = 0; i < 12; ++i) {
    if (strncmp(m, months + i * 3, 3) == 0) return i;
  }
  return 0;
}

static void seedTimeFromCompile() {
  struct tm t = {};
  char mon[4] = {0};
  int day = 1, year = 2026, hh = 12, mm = 0, ss = 0;
  if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3) {
    t.tm_mon = monthFromAbbrev(mon);
    t.tm_mday = day;
    t.tm_year = year - 1900;
  } else {
    t.tm_year = 126;
    t.tm_mon = 0;
    t.tm_mday = 1;
  }
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;
  t.tm_isdst = -1;
  time_t epoch = mktime(&t);
  if (epoch > 0) {
    struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    settimeofday(&tv, nullptr);
  }
}

static void syncFromRtcIfValid() {
  if (!pcf85063Available()) return;
  struct tm rtc = {};
  if (!pcf85063GetTime(&rtc)) return;
  if (rtc.tm_year + 1900 < 2020) return;
  time_t epoch = mktime(&rtc);
  if (epoch <= 0) return;
  struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&tv, nullptr);
}

static void applyWallTime(int hour, int minute) {
  time_t now = time(nullptr);
  struct tm t = {};
  localtime_r(&now, &t);
  t.tm_hour = constrain(hour, 0, 23);
  t.tm_min = constrain(minute, 0, 59);
  t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t epoch = mktime(&t);
  if (epoch > 0) {
    struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    settimeofday(&tv, nullptr);
  }
  if (pcf85063Available()) {
    pcf85063SetTime(&t);
  }
}

static void initWallClock() {
  syncFromRtcIfValid();
  time_t now = time(nullptr);
  struct tm t = {};
  localtime_r(&now, &t);
  if (t.tm_year + 1900 < 2020) {
    seedTimeFromCompile();
    now = time(nullptr);
    localtime_r(&now, &t);
    if (pcf85063Available() && t.tm_year + 1900 >= 2020) {
      pcf85063SetTime(&t);
    }
  }
}

static void getNowTm(struct tm* out) {
  time_t now = time(nullptr);
  localtime_r(&now, out);
}

// ---- forward decls --------------------------------------------------------

static void clearPinEntry();
static void updatePinDots();
static void updateSetTimeLabel();
static void refreshLauncher();
static void goClock();
static void goPin();
static void goLauncher();
static void goSetTime();
static void goOta();
static void goManage();
static void goSettings();
static void buildSettingsScreen();
static void loadLauncherViewPrefs();
static void saveLauncherViewPrefs();
static void freeLauncherIcons();
static void refreshManage();
static void onOpenManage(lv_event_t* e);
static void goStub(const AppInfo* app);
static void refreshWifiIcon();
static void refreshOtaScreen();
static void otaQrHide();
static int readBatteryPercent();
static uint8_t gDisplayBrightness = 0xC0;  // 0..255 panel brightness

// ---- battery --------------------------------------------------------------

/** Returns 0..100, or -1 if unknown / no battery. */
static int readBatteryPercent() {
  if (!gPmicOk) return -1;
  int pct = power.getBatteryPercent();
  if (pct >= 0 && pct <= 100) return pct;
  // Voltage-based estimate when fuel-gauge percent unavailable.
  uint16_t mv = power.getBattVoltage();
  if (mv < 2500) return -1;  // not connected / nonsense
  // Rough LiPo: 3.30V=0% .. 4.20V=100%
  if (mv <= 3300) return 0;
  if (mv >= 4200) return 100;
  return (int)((mv - 3300) * 100 / (4200 - 3300));
}

/** Battery millivolts from AXP2101 ADC, or -1 if unavailable. */
static int readBatteryMv() {
  if (!gPmicOk) return -1;
  if (!power.isBatteryConnect()) return -1;
  uint16_t mv = power.getBattVoltage();
  if (mv == 0) return -1;
  return (int)mv;
}

/** 1=charging, 0=not, -1 if PMIC unavailable (never invent status). */
static int readCharging() {
  if (!gPmicOk) return -1;
  return power.isCharging() ? 1 : 0;
}

/** 1=VBUS/USB present, 0=not, -1 if PMIC unavailable. */
static int readUsbPower() {
  if (!gPmicOk) return -1;
  return power.isVbusIn() ? 1 : 0;
}

/** 1=battery pack detected, 0=not, -1 if PMIC unavailable. */
static int readBatteryConnected() {
  if (!gPmicOk) return -1;
  return power.isBatteryConnect() ? 1 : 0;
}

static int luaUiBattery(void) { return readBatteryPercent(); }
static int luaUiBatteryMv(void) { return readBatteryMv(); }
static int luaUiCharging(void) { return readCharging(); }
static int luaUiUsbPower(void) { return readUsbPower(); }
static int luaUiBatteryConnected(void) { return readBatteryConnected(); }

static void luaUiRequestBack(void) { gLuaBackRequested = true; }

static void vibratePoll(void) {
  if (gVibrateUntilMs == 0) return;
  if ((int32_t)(millis() - gVibrateUntilMs) >= 0) {
    digitalWrite(VIBRATE_PIN, LOW);
    gVibrateUntilMs = 0;
  }
}

static void luaUiVibrate(void) {
  // Motor on GPIO18: ~60ms pulse; rapid re-trigger extends the window.
  digitalWrite(VIBRATE_PIN, HIGH);
  gVibrateUntilMs = millis() + 60;
}

/**
 * Generic notification banner: stores appId so swipe-down can open that app.
 * titlePrefix optional (e.g. "Mesh"); when null, uses appId.
 */
static void notifBannerShow(const char* appId, const char* text) {
  if (!appId) appId = "";
  if (!text) text = "";
  strncpy(gNotifAppId, appId, sizeof(gNotifAppId) - 1);
  gNotifAppId[sizeof(gNotifAppId) - 1] = '\0';
  strncpy(gNotifText, text, sizeof(gNotifText) - 1);
  gNotifText[sizeof(gNotifText) - 1] = '\0';

  char buf[112];
  const char* label = appId[0] ? appId : "App";
  // Prefer friendly catalog name when known.
  const AppInfo* info = appsHostFindById(appId);
  if (info && info->name[0]) label = info->name;
  snprintf(buf, sizeof(buf), "%s: %.60s", label, text);

  if (!gNotifBanner) {
    gNotifBanner = lv_label_create(lv_layer_top());
    lv_obj_set_width(gNotifBanner, LCD_WIDTH - 24);
    lv_obj_set_style_bg_color(gNotifBanner, lv_color_hex(0x1a2438), 0);
    lv_obj_set_style_bg_opa(gNotifBanner, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(gNotifBanner, lv_color_hex(0xF4F7FB), 0);
    lv_obj_set_style_pad_all(gNotifBanner, 8, 0);
    lv_obj_set_style_radius(gNotifBanner, 8, 0);
    lv_label_set_long_mode(gNotifBanner, LV_LABEL_LONG_CLIP);
    lv_obj_align(gNotifBanner, LV_ALIGN_TOP_MID, 0, 8);
  }
  lv_label_set_text(gNotifBanner, buf);
  lv_obj_clear_flag(gNotifBanner, LV_OBJ_FLAG_HIDDEN);
  gNotifUntilMs = millis() + 5000;  // slightly longer so swipe-open is usable
}

static void notifBannerHide(void) {
  if (gNotifBanner) lv_obj_add_flag(gNotifBanner, LV_OBJ_FLAG_HIDDEN);
  gNotifUntilMs = 0;
  gNotifAppId[0] = '\0';
  // Keep gNotifText until swipe-open copies it into launch intent.
}

static void notifBannerPoll(void) {
  if (gNotifUntilMs == 0 || !gNotifBanner) return;
  if ((int32_t)(millis() - gNotifUntilMs) >= 0) {
    notifBannerHide();
  }
}

/** Lua/host: raise a generic notification for any app id. */
static void luaUiNotify(const char* appId, const char* text) {
  notifBannerShow(appId, text);
}

/** Main-loop only: ICQ uh-oh beep (primary) + vibrate + banner → mesh_chat. */
static void serviceMeshBgAlert(void) {
  char text[96];
  if (!bleMeshTakeBgAlert(text, sizeof(text))) return;
  if (bleMeshAlertBeepEnabled()) {
    (void)audioHostAlertUhOh();
  }
  if (bleMeshAlertVibrateEnabled()) {
    luaUiVibrate();  // no-op if coin motor pads empty
  }
  notifBannerShow("mesh_chat", text);
}

static const char* luaUiWifiState(void) {
  WifiOtaState st = wifiOtaState();
  if (st == WIFI_OTA_OFF) return "off";
  if (st == WIFI_OTA_CONNECTING) return "ota";  /* joining home AP */
  return "ota";
}

static int luaUiBrightness(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  uint8_t v = (uint8_t)((percent * 255) / 100);
  if (percent > 0 && v < 8) v = 8;
  gDisplayBrightness = v;
  if (amoled) amoled->setBrightness(v);
  return percent;
}

static bool luaUiImu(float* ax, float* ay, float* az, float* gx, float* gy,
                     float* gz) {
  if (!gImuOk || !ax || !ay || !az || !gx || !gy || !gz) return false;
  // Units: accel = g, gyro = dps (SensorQMI8658 scales).
  // getDataReady() polls STATUS when INT1 is unused; still attempt reads.
  (void)qmi.getDataReady();
  float axx = 0, ayy = 0, azz = 0, gxx = 0, gyy = 0, gzz = 0;
  bool gotA = qmi.getAccelerometer(axx, ayy, azz);
  bool gotG = qmi.getGyroscope(gxx, gyy, gzz);
  if (!gotA && !gotG) return false;
  *ax = axx; *ay = ayy; *az = azz;
  *gx = gxx; *gy = gyy; *gz = gzz;
  return true;
}

// ---- navigation -----------------------------------------------------------

static void goClock() {
  // Always tear down any Lua/WASM session when returning to the cover (Lock /
  // soft-lock / Back-to-clock paths). Prevents session leak if we leave stub
  // without the stub Back button.
  luaHostClose();
  wasmHostClose();
  gLuaBackRequested = false;
  if (usbMscActive()) usbMscExit();
  if (wifiOtaIsRunning()) wifiOtaStop();
  otaQrHide();
  clearPinEntry();
  if (lblPinMsg) {
    lv_label_set_text(lblPinMsg, "Enter PIN");
    lv_obj_set_style_text_color(lblPinMsg, COL_DIM, 0);
  }
  lv_screen_load(scrClock);
}

static void goPin() {
  clearPinEntry();
  if (lblPinMsg) {
    lv_label_set_text(lblPinMsg, "Enter PIN");
    lv_obj_set_style_text_color(lblPinMsg, COL_DIM, 0);
  }
  noteActivity();
  lv_screen_load(scrPin);
}

static void goLauncher() {
  gLuaBackRequested = false;
  refreshLauncher();
  noteActivity();
  lv_screen_load(scrLauncher);
}

static void goSetTime() {
  struct tm t = {};
  getNowTm(&t);
  gEditHour = t.tm_hour;
  gEditMin = t.tm_min;
  updateSetTimeLabel();
  noteActivity();
  lv_screen_load(scrSetTime);
}


/** Keep stub body text above the Lua button strip so lines stay readable. */
static void luaUiLayoutBodyAboveButtons(void) {
  if (!lblStubBody || !luaBtnColumn) return;
  lv_obj_update_layout(scrStub);
  lv_coord_t btn_y = lv_obj_get_y(luaBtnColumn);
  lv_coord_t title_bottom = 72;
  if (lblStubTitle) {
    title_bottom = lv_obj_get_y(lblStubTitle) + lv_obj_get_height(lblStubTitle) + 8;
  }
  lv_coord_t max_h = btn_y - title_bottom - 8;
  if (max_h < 80) max_h = 80;
  if (max_h > 320) max_h = 320;
  lv_obj_set_width(lblStubBody, 360);
  lv_obj_set_height(lblStubBody, max_h);
  lv_obj_clear_flag(lblStubBody, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_long_mode(lblStubBody, LV_LABEL_LONG_CLIP);
  lv_obj_align(lblStubBody, LV_ALIGN_TOP_MID, 0, title_bottom);
}

static void luaUiSetTitle(const char* title) {
  if (lblStubTitle && title) lv_label_set_text(lblStubTitle, title);
}

static void luaUiSetText(const char* text) {
  if (!lblStubBody || !text) return;
  luaUiLayoutBodyAboveButtons();
  lv_label_set_text(lblStubBody, text);
}

static void luaUiAppendText(const char* text) {
  if (!lblStubBody || !text) return;
  const char* cur = lv_label_get_text(lblStubBody);
  if (!cur) cur = "";
  size_t need = strlen(cur) + strlen(text) + 1;
  char* buf = (char*)malloc(need);
  if (!buf) return;
  snprintf(buf, need, "%s%s", cur, text);
  lv_label_set_text(lblStubBody, buf);
  free(buf);
}

static void luaUiClear(void) {
  if (lblStubBody) lv_label_set_text(lblStubBody, "");
}

static void luaUiGetSize(int* width, int* height) {
  if (width) *width = (int)LCD_WIDTH;
  if (height) *height = (int)LCD_HEIGHT;
}

static void luaUiSetBg(int r, int g, int b) {
  if (!scrStub) return;
  lv_obj_set_style_bg_color(scrStub, lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b), 0);
  lv_obj_set_style_bg_opa(scrStub, LV_OPA_COVER, 0);
}

static void applyLuaBtnLayoutStyles(void) {
  if (!luaBtnColumn) return;
  if (gLuaBtnLayout == kLuaBtnGrid2) {
    lv_obj_set_size(luaBtnColumn, 390, 240);
    lv_obj_align(luaBtnColumn, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_flex_flow(luaBtnColumn, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(luaBtnColumn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(luaBtnColumn, 6, 0);
    lv_obj_set_style_pad_column(luaBtnColumn, 8, 0);
    lv_obj_set_style_pad_all(luaBtnColumn, 4, 0);
    // Allow scroll when many buttons (>~6–8).
    lv_obj_add_flag(luaBtnColumn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(luaBtnColumn, LV_DIR_VER);
  } else {
    lv_obj_set_size(luaBtnColumn, 300, 220);
    lv_obj_align(luaBtnColumn, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_flex_flow(luaBtnColumn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(luaBtnColumn, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(luaBtnColumn, 8, 0);
    lv_obj_set_style_pad_column(luaBtnColumn, 0, 0);
    lv_obj_set_style_pad_all(luaBtnColumn, 0, 0);
    lv_obj_clear_flag(luaBtnColumn, LV_OBJ_FLAG_SCROLLABLE);
  }
}

static void restoreLuaBtnColumnDefault(void) {
  // Re-apply current layout geometry after letter_pad teardown.
  applyLuaBtnLayoutStyles();
}

static void luaBtnSizeForLayout(int* w, int* h) {
  if (gLuaBtnLayout == kLuaBtnGrid2) {
    // ~2 columns in a ~390-wide container with pad/gap.
    int cols = gLuaBtnCols < 1 ? 2 : gLuaBtnCols;
    int avail = 390 - 8 - (cols - 1) * 8;
    if (avail < 80) avail = 80;
    *w = avail / cols;
    if (*w > 180) *w = 180;
    if (*w < 100) *w = 100;
    *h = 40;
  } else {
    *w = 280;
    *h = 44;
  }
}

static bool luaUiButtonLayout(const char* mode, int cols) {
  if (!mode || !mode[0]) return false;
  if (strcmp(mode, "grid2") == 0) {
    gLuaBtnLayout = kLuaBtnGrid2;
    gLuaBtnCols = (cols >= 1 && cols <= 4) ? cols : 2;
  } else if (strcmp(mode, "column") == 0) {
    gLuaBtnLayout = kLuaBtnColumn;
    gLuaBtnCols = 1;
  } else {
    return false;
  }
  applyLuaBtnLayoutStyles();
  // Resize any existing buttons to match the new layout.
  if (luaBtnColumn) {
    uint32_t n = lv_obj_get_child_cnt(luaBtnColumn);
    int bw = 280, bh = 44;
    luaBtnSizeForLayout(&bw, &bh);
    for (uint32_t i = 0; i < n; ++i) {
      lv_obj_t* child = lv_obj_get_child(luaBtnColumn, i);
      if (child) lv_obj_set_size(child, bw, bh);
    }
  }
  luaUiLayoutBodyAboveButtons();
  return true;
}

static bool luaUiTouch(int* x, int* y, bool* pressed) {
  if (!gTouchPressedNow) return false;
  if (x) *x = (int)gTouchLastX;
  if (y) *y = (int)gTouchLastY;
  if (pressed) *pressed = true;
  return true;
}

static void destroyLuaLetterPad(void) {
  if (luaLetterPad) {
    lv_obj_del(luaLetterPad);
    luaLetterPad = nullptr;
  }
  if (stubBackBtn) lv_obj_add_flag(stubBackBtn, LV_OBJ_FLAG_HIDDEN);
  restoreLuaBtnColumnDefault();
}

static void onLetterPadKey(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(obj);
  const char* txt = lv_buttonmatrix_get_button_text(obj, id);
  if (!txt || !txt[0]) return;
  // Map pad labels to Lua: letter, BKSP, CLR, SPC, SEND.
  if (strcmp(txt, "BKSP") == 0 || strcmp(txt, "CLR") == 0 ||
      strcmp(txt, "SPC") == 0 || strcmp(txt, "SEND") == 0) {
    luaHostLetterPadKey(txt);
    return;
  }
  // Single A–Z key
  if (txt[0] && txt[1] == '\0') {
    char key[2] = {txt[0], '\0'};
    // Normalize to uppercase
    if (key[0] >= 'a' && key[0] <= 'z') key[0] = (char)(key[0] - 'a' + 'A');
    luaHostLetterPadKey(key);
  }
}

static bool luaUiCreateLetterPad(void) {
  if (!scrStub) return false;
  if (luaLetterPad) return true;

  // Squish-proven 5-row A-Z + on-pad SPC/SEND so Compose never loses Send.
  static const char* letter_map[] = {
      "A", "B", "C", "D", "E", "F", "\n",
      "G", "H", "I", "J", "K", "L", "\n",
      "M", "N", "O", "P", "Q", "R", "\n",
      "S", "T", "U", "V", "W", "X", "\n",
      "Y", "Z", "BKSP", "CLR", "\n",
      "SPC", "SEND", ""};

  luaLetterPad = lv_buttonmatrix_create(scrStub);
  lv_buttonmatrix_set_map(luaLetterPad, letter_map);
  // 6 rows need ~270px; keep strip above with clear gap (no overlap).
  lv_obj_set_size(luaLetterPad, 390, 270);
  lv_obj_align(luaLetterPad, LV_ALIGN_BOTTOM_MID, 0, -4);
  style_keypad(luaLetterPad);
  lv_obj_set_style_text_font(luaLetterPad, &lv_font_montserrat_16, LV_PART_ITEMS);
  lv_obj_set_style_pad_row(luaLetterPad, 3, 0);
  lv_obj_set_style_pad_column(luaLetterPad, 3, 0);
  lv_obj_add_event_cb(luaLetterPad, onLetterPadKey, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_move_foreground(luaLetterPad);

  // Button strip above pad (Squish Open1/2/3 geometry); buttons stay on top for taps.
  if (luaBtnColumn) {
    lv_obj_set_size(luaBtnColumn, 390, 48);
    lv_obj_align(luaBtnColumn, LV_ALIGN_BOTTOM_MID, 0, -280);
    lv_obj_add_flag(luaBtnColumn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(luaBtnColumn);
  }
  if (stubBackBtn) lv_obj_add_flag(stubBackBtn, LV_OBJ_FLAG_HIDDEN);

  if (lblStubBody) {
    lv_obj_set_width(lblStubBody, 380);
    lv_obj_align(lblStubBody, LV_ALIGN_TOP_MID, 0, 64);
  }
  return true;
}

// Raise luaLayer (maze+ball) above title/body, then buttons/back/letter pad
// so Reset/Back stay clickable on top while the yellow ball stays visible.
static void raiseLuaDrawStack(void) {
  if (luaLayer) lv_obj_move_foreground(luaLayer);
  if (luaBall) lv_obj_move_foreground(luaBall);
  if (luaImage) lv_obj_move_foreground(luaImage);
  if (stubBackBtn) lv_obj_move_foreground(stubBackBtn);
  // Keypad visible; button strip (Spc/Send / Squish Open*) stays above for taps.
  if (luaLetterPad) lv_obj_move_foreground(luaLetterPad);
  if (luaBtnColumn) lv_obj_move_foreground(luaBtnColumn);
}

static constexpr int kWrgbMaxDim = 160;
static constexpr size_t kWrgbHeaderSize = 8;  // WRGB + w + h

static void freeLuaImage(bool deleteObj) {
  if (deleteObj && luaImage) {
    lv_obj_delete(luaImage);
  }
  luaImage = nullptr;
  if (luaImageBuf) {
    heap_caps_free(luaImageBuf);
    luaImageBuf = nullptr;
  }
  memset(&luaImageDsc, 0, sizeof(luaImageDsc));
}

static void luaUiClearImage(void) { freeLuaImage(true); }

/** Read entire file into *outBuf (heap_caps). Returns bytes or -1. Caller frees. */
static int readPathAll(const char* path, uint8_t** outBuf, size_t maxBytes) {
  if (!path || !outBuf) return -1;
  *outBuf = nullptr;

  if (strncmp(path, "/sd/", 4) == 0) {
    if (!sdHostPathOk(path)) return -1;
    if (!sdHostReady() && !sdHostMount()) return -1;
    // Probe size via open through SD_MMC helpers: read up to maxBytes.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(maxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(maxBytes, MALLOC_CAP_8BIT);
    if (!buf) return -1;
    int n = sdHostRead(path, (char*)buf, maxBytes);
    if (n < 0) {
      heap_caps_free(buf);
      return -1;
    }
    *outBuf = buf;
    return n;
  }

  // LittleFS — never remount/format.
  if (!LittleFS.exists(path)) return -1;
  File f = LittleFS.open(path, "r");
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return -1;
  }
  size_t sz = f.size();
  if (sz == 0 || sz > maxBytes) {
    f.close();
    return -1;
  }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if (!buf) {
    f.close();
    return -1;
  }
  size_t n = f.readBytes((char*)buf, sz);
  f.close();
  if (n != sz) {
    heap_caps_free(buf);
    return -1;
  }
  *outBuf = buf;
  return (int)n;
}

static bool luaUiShowImage(const char* path, int x, int y, int max_w, int max_h) {
  if (!luaLayer || !path || !path[0]) return false;
  if (max_w <= 0) max_w = kWrgbMaxDim;
  if (max_h <= 0) max_h = kWrgbMaxDim;
  if (max_w > kWrgbMaxDim) max_w = kWrgbMaxDim;
  if (max_h > kWrgbMaxDim) max_h = kWrgbMaxDim;

  // Max file: header + 160*160*2
  const size_t maxFile = kWrgbHeaderSize + (size_t)kWrgbMaxDim * (size_t)kWrgbMaxDim * 2u;
  uint8_t* fileBuf = nullptr;
  int n = readPathAll(path, &fileBuf, maxFile);
  if (n < (int)kWrgbHeaderSize || !fileBuf) {
    if (fileBuf) heap_caps_free(fileBuf);
    return false;
  }
  if (fileBuf[0] != 'W' || fileBuf[1] != 'R' || fileBuf[2] != 'G' || fileBuf[3] != 'B') {
    heap_caps_free(fileBuf);
    return false;
  }
  uint16_t w = (uint16_t)fileBuf[4] | ((uint16_t)fileBuf[5] << 8);
  uint16_t h = (uint16_t)fileBuf[6] | ((uint16_t)fileBuf[7] << 8);
  if (w == 0 || h == 0 || w > (uint16_t)kWrgbMaxDim || h > (uint16_t)kWrgbMaxDim) {
    heap_caps_free(fileBuf);
    return false;
  }
  size_t pixBytes = (size_t)w * (size_t)h * 2u;
  if ((size_t)n < kWrgbHeaderSize + pixBytes) {
    heap_caps_free(fileBuf);
    return false;
  }

  // Replace any previous image.
  freeLuaImage(true);

  // Keep only pixel payload in luaImageBuf (descriptor points at it).
  uint8_t* pix = (uint8_t*)heap_caps_malloc(pixBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pix) pix = (uint8_t*)heap_caps_malloc(pixBytes, MALLOC_CAP_8BIT);
  if (!pix) {
    heap_caps_free(fileBuf);
    return false;
  }
  memcpy(pix, fileBuf + kWrgbHeaderSize, pixBytes);
  heap_caps_free(fileBuf);
  luaImageBuf = pix;

  memset(&luaImageDsc, 0, sizeof(luaImageDsc));
  luaImageDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  luaImageDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  luaImageDsc.header.w = w;
  luaImageDsc.header.h = h;
  luaImageDsc.header.stride = (uint32_t)w * 2u;
  luaImageDsc.data_size = (uint32_t)pixBytes;
  luaImageDsc.data = luaImageBuf;

  luaImage = lv_image_create(luaLayer);
  if (!luaImage) {
    freeLuaImage(false);
    return false;
  }
  lv_image_set_src(luaImage, &luaImageDsc);
  lv_obj_clear_flag(luaImage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(luaImage, LV_OBJ_FLAG_SCROLLABLE);

  int disp_w = (int)w;
  int disp_h = (int)h;
  if (disp_w > max_w || disp_h > max_h) {
    lv_obj_set_size(luaImage, max_w, max_h);
    lv_image_set_inner_align(luaImage, LV_IMAGE_ALIGN_CONTAIN);
    disp_w = max_w;
    disp_h = max_h;
  } else {
    lv_obj_set_size(luaImage, disp_w, disp_h);
  }
  lv_obj_set_pos(luaImage, x, y);
  raiseLuaDrawStack();
  Serial.printf("wrgb: show %s %ux%u at %d,%d\n", path, (unsigned)w, (unsigned)h, x, y);
  return true;
}



static void freeLuaCanvas(void) {
  if (luaCanvasBuf) {
    free(luaCanvasBuf);
    luaCanvasBuf = nullptr;
  }
  luaCanvas = nullptr;
  luaCanvasW = 0;
  luaCanvasH = 0;
}

static uint16_t rgb565pack(int r, int g, int b) {
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** Heat map: cold blue → cyan → green → yellow → red. v in 0..1 */
static void heatRgb(float v, int* r, int* g, int* b) {
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  float x = v * 4.0f;
  int seg = (int)x;
  float t = x - (float)seg;
  int r0, g0, b0, r1, g1, b1;
  switch (seg) {
    case 0: r0=0;g0=0;b0=40; r1=0;g1=80;b1=255; break;
    case 1: r0=0;g0=80;b0=255; r1=0;g1=220;b1=180; break;
    case 2: r0=0;g0=220;b0=180; r1=180;g1=255;b1=40; break;
    case 3: r0=180;g0=255;b0=40; r1=255;g1=120;b1=0; break;
    default: r0=255;g0=120;b0=0; r1=255;g1=40;b1=40; t=1.0f; break;
  }
  *r = (int)(r0 + (r1 - r0) * t);
  *g = (int)(g0 + (g1 - g0) * t);
  *b = (int)(b0 + (b1 - b0) * t);
}

static bool luaExtCanvasCreate(int x, int y, int w, int h) {
  if (!luaLayer) return false;
  if (w < 8) w = 8;
  if (h < 8) h = 8;
  if (w > LCD_WIDTH) w = LCD_WIDTH;
  if (h > LCD_HEIGHT) h = LCD_HEIGHT;
  freeLuaCanvas();
  size_t n = (size_t)w * (size_t)h;
  luaCanvasBuf = (uint16_t*)heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!luaCanvasBuf) {
    luaCanvasBuf = (uint16_t*)malloc(n * sizeof(uint16_t));
  }
  if (!luaCanvasBuf) return false;
  memset(luaCanvasBuf, 0, n * sizeof(uint16_t));
  luaCanvas = lv_canvas_create(luaLayer);
  if (!luaCanvas) {
    free(luaCanvasBuf);
    luaCanvasBuf = nullptr;
    return false;
  }
  lv_canvas_set_buffer(luaCanvas, luaCanvasBuf, (uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(luaCanvas, x, y);
  lv_obj_set_size(luaCanvas, w, h);
  lv_obj_clear_flag(luaCanvas, LV_OBJ_FLAG_CLICKABLE);
  luaCanvasW = w;
  luaCanvasH = h;
  luaCanvasX = x;
  luaCanvasY = y;
  return true;
}

static bool luaExtCanvasClear(int r, int g, int b) {
  if (!luaCanvas || !luaCanvasBuf || luaCanvasW < 1 || luaCanvasH < 1) return false;
  uint16_t c = rgb565pack(r, g, b);
  size_t n = (size_t)luaCanvasW * (size_t)luaCanvasH;
  for (size_t i = 0; i < n; ++i) luaCanvasBuf[i] = c;
  lv_obj_invalidate(luaCanvas);
  return true;
}

static bool luaExtCanvasScroll(int dy) {
  if (!luaCanvas || !luaCanvasBuf || luaCanvasW < 1 || luaCanvasH < 1) return false;
  if (dy == 0) return true;
  int w = luaCanvasW, h = luaCanvasH;
  if (dy < 0) {
    int shift = -dy;
    if (shift >= h) {
      memset(luaCanvasBuf, 0, (size_t)w * h * sizeof(uint16_t));
    } else {
      memmove(luaCanvasBuf, luaCanvasBuf + (size_t)shift * w,
              (size_t)(h - shift) * w * sizeof(uint16_t));
      memset(luaCanvasBuf + (size_t)(h - shift) * w, 0,
             (size_t)shift * w * sizeof(uint16_t));
    }
  } else {
    int shift = dy;
    if (shift >= h) {
      memset(luaCanvasBuf, 0, (size_t)w * h * sizeof(uint16_t));
    } else {
      memmove(luaCanvasBuf + (size_t)shift * w, luaCanvasBuf,
              (size_t)(h - shift) * w * sizeof(uint16_t));
      memset(luaCanvasBuf, 0, (size_t)shift * w * sizeof(uint16_t));
    }
  }
  lv_obj_invalidate(luaCanvas);
  return true;
}

static bool luaExtCanvasRowHeat(int y, const float* mags, int nMags) {
  if (!luaCanvas || !luaCanvasBuf || !mags || nMags < 1) return false;
  if (y < 0 || y >= luaCanvasH) return false;
  int w = luaCanvasW;
  uint16_t* row = luaCanvasBuf + (size_t)y * w;
  for (int x = 0; x < w; ++x) {
    int i0 = (x * nMags) / w;
    if (i0 >= nMags) i0 = nMags - 1;
    int r, g, b;
    heatRgb(mags[i0], &r, &g, &b);
    row[x] = rgb565pack(r, g, b);
  }
  lv_obj_invalidate(luaCanvas);
  return true;
}

static bool luaExtCanvasPixel(int x, int y, int r, int g, int b) {
  if (!luaCanvas || !luaCanvasBuf || luaCanvasW < 1 || luaCanvasH < 1) return false;
  if (x < 0 || y < 0 || x >= luaCanvasW || y >= luaCanvasH) return false;
  luaCanvasBuf[(size_t)y * (size_t)luaCanvasW + (size_t)x] = rgb565pack(r, g, b);
  lv_obj_invalidate(luaCanvas);
  return true;
}

static void luaUiGfxClear(void) {
  // Shapes only — do not touch buttons, letter pad, or button refs.
  // Free WRGB buffer before lv_obj_clean deletes the image widget.
  freeLuaImage(false);
  freeLuaCanvas();
  if (luaLayer) lv_obj_clean(luaLayer);
  luaBall = nullptr;
  luaBallRadius = 0;
}

static void luaUiBall(int x, int y, int radius, int r, int g, int b) {
  if (!luaLayer) return;
  if (radius < 1) radius = 1;
  int d = radius * 2;
  lv_color_t col = lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
  if (!luaBall) {
    luaBall = lv_obj_create(luaLayer);
    lv_obj_set_style_border_width(luaBall, 0, 0);
    lv_obj_set_style_radius(luaBall, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(luaBall, 0, 0);
    lv_obj_set_style_bg_opa(luaBall, LV_OPA_COVER, 0);
    lv_obj_clear_flag(luaBall, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(luaBall, LV_OBJ_FLAG_CLICKABLE);
    luaBallRadius = 0;
  }
  if (luaBallRadius != radius) {
    lv_obj_set_size(luaBall, d, d);
    luaBallRadius = radius;
  }
  lv_obj_set_pos(luaBall, x - radius, y - radius);
  lv_obj_set_style_bg_color(luaBall, col, 0);
  // Keep ball above walls inside luaLayer, then raise stack vs title/buttons.
  lv_obj_move_foreground(luaBall);
  raiseLuaDrawStack();
}

static void luaUiClearWidgets(void) {
  destroyLuaLetterPad();
  freeLuaImage(false);
  freeLuaCanvas();
  if (luaLayer) lv_obj_clean(luaLayer);
  luaBall = nullptr;
  luaBallRadius = 0;
  for (int i = 0; i < kMaxLuaProgress; ++i) luaProgress[i] = nullptr;
  for (int i = 0; i < kMaxLuaSliders; ++i) luaSliders[i] = nullptr;
  if (luaBtnColumn) lv_obj_clean(luaBtnColumn);
}

// Queue Lua button clicks so handlers can watch.clear_ui() / rebuild safely
// outside the LVGL event (deleting the clicked widget mid-callback is unsafe).
static int gPendingLuaButton = -1;

static void onLuaButtonClicked(lv_event_t* e) {
  gPendingLuaButton = (int)(intptr_t)lv_event_get_user_data(e);
}

static void servicePendingLuaButton() {
  if (gPendingLuaButton < 0) return;
  int id = gPendingLuaButton;
  gPendingLuaButton = -1;
  luaHostButtonClicked(id);
}

static bool luaUiCreateButton(int buttonId, const char* label) {
  if (!luaBtnColumn || !label) return false;
  lv_obj_t* btn = lv_button_create(luaBtnColumn);
  int bw = 280, bh = 44;
  luaBtnSizeForLayout(&bw, &bh);
  lv_obj_set_size(btn, bw, bh);
  lv_obj_set_style_bg_color(btn, COL_KEY, 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_add_event_cb(btn, onLuaButtonClicked, LV_EVENT_CLICKED,
                      (void*)(intptr_t)buttonId);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl);
  return true;
}

static void luaUiCreateRect(int x, int y, int w, int h, int r, int g, int b) {
  if (!luaLayer) return;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  lv_obj_t* o = lv_obj_create(luaLayer);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static void luaUiCreateCircle(int x, int y, int radius, int r, int g, int b) {
  if (!luaLayer) return;
  if (radius < 1) radius = 1;
  int d = radius * 2;
  lv_obj_t* o = lv_obj_create(luaLayer);
  lv_obj_set_pos(o, x - radius, y - radius);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_bg_color(o, lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static void luaUiCreateLabel(int x, int y, const char* text, int r, int g, int b) {
  if (!luaLayer || !text) return;
  lv_obj_t* lbl = lv_label_create(luaLayer);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(lbl, x, y);
  lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}


// ---- Lua layout widgets (label/panel/progress/slider) + power/sprite ----

static bool luaExtCreateLabel(int x, int y, const char* text, int r, int g, int b) {
  if (!luaLayer || !text) return false;
  lv_obj_t* lbl = lv_label_create(luaLayer);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(lbl, x, y);
  lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
  // Labels are draw-layer; buttons stay above via raiseLuaDrawStack contract:
  // draw (luaLayer) under buttons/back. Callers use z_raise("buttons") if needed.
  return true;
}

static bool luaExtCreatePanel(int x, int y, int w, int h, int r, int g, int b) {
  if (!luaLayer) return false;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  lv_obj_t* o = lv_obj_create(luaLayer);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 12, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return true;
}

static bool luaExtCreateProgress(int id, int x, int y, int w, int h, int value) {
  if (!luaLayer || id < 0 || id >= kMaxLuaProgress) return false;
  if (luaProgress[id]) {
    lv_obj_delete(luaProgress[id]);
    luaProgress[id] = nullptr;
  }
  lv_obj_t* bar = lv_bar_create(luaLayer);
  lv_obj_set_pos(bar, x, y);
  lv_obj_set_size(bar, w < 8 ? 8 : w, h < 6 ? 6 : h);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, value, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x1a2430), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x3DDC97), LV_PART_INDICATOR);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  luaProgress[id] = bar;
  return true;
}

static bool luaExtSetProgress(int id, int value) {
  if (id < 0 || id >= kMaxLuaProgress || !luaProgress[id]) return false;
  lv_bar_set_value(luaProgress[id], value, LV_ANIM_OFF);
  return true;
}

static void onLuaSliderChanged(lv_event_t* e) {
  int id = (int)(intptr_t)lv_event_get_user_data(e);
  lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
  gPendingSliderId = id;
  gPendingSliderVal = (int)lv_slider_get_value(s);
}

static bool luaExtCreateSlider(int id, int x, int y, int w, int h, int minV, int maxV,
                               int value) {
  if (!scrStub || id < 0 || id >= kMaxLuaSliders) return false;
  if (luaSliders[id]) {
    lv_obj_delete(luaSliders[id]);
    luaSliders[id] = nullptr;
  }
  // Sliders live on scrStub (clickable) above draw layer — same stack as buttons.
  lv_obj_t* s = lv_slider_create(scrStub);
  lv_obj_set_pos(s, x, y);
  lv_obj_set_size(s, w < 40 ? 40 : w, h < 12 ? 12 : h);
  if (maxV <= minV) maxV = minV + 1;
  lv_slider_set_range(s, minV, maxV);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  lv_obj_add_event_cb(s, onLuaSliderChanged, LV_EVENT_VALUE_CHANGED,
                      (void*)(intptr_t)id);
  luaSliders[id] = s;
  if (stubBackBtn) lv_obj_move_foreground(stubBackBtn);
  return true;
}

static int luaExtBrightnessGet(void) {
  // Invert 0..255 panel → 0..100
  return (int)((gDisplayBrightness * 100u) / 255u);
}

static int luaExtBrightnessSet(int percent) { return luaUiBrightness(percent); }

static uint32_t luaExtIdleGet(void) { return gSoftLockIdleSec; }

static uint32_t luaExtIdleSet(uint32_t sec) {
  gSoftLockIdleSec = sec;
  // Persist small key in NVS via Preferences (best-effort).
  // Kept in soft-lock path only; PIN unchanged.
  return gSoftLockIdleSec;
}

static bool luaExtNow(struct tm* out, time_t* epoch) {
  if (!out) return false;
  if (pcf85063Available() && pcf85063GetTime(out)) {
    // Build epoch from broken-down time (local).
    time_t e = mktime(out);
    if (epoch) *epoch = e;
    return true;
  }
  time_t now = time(nullptr);
  localtime_r(&now, out);
  if (epoch) *epoch = now;
  return true;
}

static void luaExtZRaise(const char* which) {
  if (!which) which = "all";
  if (strcmp(which, "draw") == 0) {
    if (luaLayer) lv_obj_move_foreground(luaLayer);
    return;
  }
  if (strcmp(which, "buttons") == 0) {
    if (luaBtnColumn) lv_obj_move_foreground(luaBtnColumn);
    return;
  }
  if (strcmp(which, "back") == 0) {
    if (stubBackBtn) lv_obj_move_foreground(stubBackBtn);
    return;
  }
  raiseLuaDrawStack();
}

/** Blit one frame from a WRGB sheet (row-major frames of fw x fh). */
static bool luaExtShowSprite(const char* path, int x, int y, int fw, int fh, int frame,
                             int max_w, int max_h) {
  if (!luaLayer || !path || fw < 1 || fh < 1 || frame < 0) return false;
  if (max_w <= 0) max_w = fw;
  if (max_h <= 0) max_h = fh;
  if (max_w > kWrgbMaxDim) max_w = kWrgbMaxDim;
  if (max_h > kWrgbMaxDim) max_h = kWrgbMaxDim;
  // Allow sheets up to 320x320 for sprite atlases.
  const int kSheetMax = 320;
  const size_t maxFile = kWrgbHeaderSize + (size_t)kSheetMax * (size_t)kSheetMax * 2u;
  uint8_t* fileBuf = nullptr;
  int n = readPathAll(path, &fileBuf, maxFile);
  if (n < (int)kWrgbHeaderSize || !fileBuf) {
    if (fileBuf) heap_caps_free(fileBuf);
    return false;
  }
  if (fileBuf[0] != 'W' || fileBuf[1] != 'R' || fileBuf[2] != 'G' || fileBuf[3] != 'B') {
    heap_caps_free(fileBuf);
    return false;
  }
  uint16_t sw = (uint16_t)fileBuf[4] | ((uint16_t)fileBuf[5] << 8);
  uint16_t sh = (uint16_t)fileBuf[6] | ((uint16_t)fileBuf[7] << 8);
  if (sw == 0 || sh == 0 || sw > kSheetMax || sh > kSheetMax) {
    heap_caps_free(fileBuf);
    return false;
  }
  int cols = (int)sw / fw;
  int rows = (int)sh / fh;
  if (cols < 1 || rows < 1) {
    heap_caps_free(fileBuf);
    return false;
  }
  int maxFrames = cols * rows;
  if (frame >= maxFrames) {
    heap_caps_free(fileBuf);
    return false;
  }
  int col = frame % cols;
  int row = frame / cols;
  int srcX = col * fw;
  int srcY = row * fh;
  size_t pixBytes = (size_t)fw * (size_t)fh * 2u;
  uint8_t* pix = (uint8_t*)heap_caps_malloc(pixBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pix) pix = (uint8_t*)heap_caps_malloc(pixBytes, MALLOC_CAP_8BIT);
  if (!pix) {
    heap_caps_free(fileBuf);
    return false;
  }
  const uint8_t* src = fileBuf + kWrgbHeaderSize;
  for (int yy = 0; yy < fh; ++yy) {
    const uint8_t* rowp = src + ((size_t)(srcY + yy) * (size_t)sw + (size_t)srcX) * 2u;
    memcpy(pix + (size_t)yy * (size_t)fw * 2u, rowp, (size_t)fw * 2u);
  }
  heap_caps_free(fileBuf);

  freeLuaImage(true);
  luaImageBuf = pix;
  memset(&luaImageDsc, 0, sizeof(luaImageDsc));
  luaImageDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  luaImageDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  luaImageDsc.header.w = fw;
  luaImageDsc.header.h = fh;
  luaImageDsc.header.stride = (uint32_t)fw * 2u;
  luaImageDsc.data_size = (uint32_t)pixBytes;
  luaImageDsc.data = luaImageBuf;
  luaImage = lv_image_create(luaLayer);
  if (!luaImage) {
    freeLuaImage(false);
    return false;
  }
  lv_image_set_src(luaImage, &luaImageDsc);
  lv_obj_set_pos(luaImage, x, y);
  if (fw > max_w || fh > max_h) {
    lv_obj_set_size(luaImage, max_w, max_h);
    lv_image_set_inner_align(luaImage, LV_IMAGE_ALIGN_CONTAIN);
  }
  lv_obj_clear_flag(luaImage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(luaImage, LV_OBJ_FLAG_SCROLLABLE);
  raiseLuaDrawStack();
  return true;
}

static void servicePendingLuaSlider() {
  if (gPendingSliderId < 0) return;
  int id = gPendingSliderId;
  int val = gPendingSliderVal;
  gPendingSliderId = -1;
  luaApiExtSliderChanged(id, val);
}


static void goStub(const AppInfo* app) {
  luaHostClose();
  wasmHostClose();
  // Fresh app session: default column (old apps). KidCoder sets grid2 itself.
  gLuaBtnLayout = kLuaBtnColumn;
  gLuaBtnCols = 2;
  applyLuaBtnLayoutStyles();

  if (app) {
    gStubApp = *app;
  } else {
    memset(&gStubApp, 0, sizeof(gStubApp));
    strncpy(gStubApp.id, "hello", sizeof(gStubApp.id) - 1);
    strncpy(gStubApp.name, "Hello", sizeof(gStubApp.name) - 1);
    strncpy(gStubApp.version, "0.1.0", sizeof(gStubApp.version) - 1);
  }
  if (lblStubTitle) {
    lv_label_set_text(lblStubTitle, gStubApp.name[0] ? gStubApp.name : "App");
  }
  if (lblStubBody) {
    lv_label_set_text(lblStubBody, "");
  }
  luaUiClearWidgets();
  // Restore default stub bg
  if (scrStub) {
    lv_obj_set_style_bg_color(scrStub, COL_BG, 0);
  }
  noteActivity();
  lv_screen_load(scrStub);

  // Prefer catalog script path (LittleFS or /sd/apps/...); else native stub copy.
  char scriptPath[128];
  scriptPath[0] = '\0';
  if (gStubApp.script[0]) {
    strncpy(scriptPath, gStubApp.script, sizeof(scriptPath) - 1);
    scriptPath[sizeof(scriptPath) - 1] = '\0';
  } else if (gStubApp.folder[0] && gStubApp.entry[0]) {
    snprintf(scriptPath, sizeof(scriptPath), "%s/%s", gStubApp.folder, gStubApp.entry);
  } else if (gStubApp.folder[0]) {
    snprintf(scriptPath, sizeof(scriptPath), "%s/main.lua", gStubApp.folder);
  }

  bool scriptOk = false;
  if (scriptPath[0]) {
    if (gStubApp.storage == APP_STORAGE_SD || strncmp(scriptPath, "/sd/", 4) == 0) {
      if (!sdHostReady()) sdHostMount();
      scriptOk = sdHostExists(scriptPath);
    } else {
      scriptOk = LittleFS.exists(scriptPath);
    }
  }

  // Close any previous WASM as well (luaHostOpen already closes Lua).
  wasmHostClose();

  const bool isWasm = (gStubApp.kind == APP_KIND_WASM) ||
                      (scriptPath[0] && strstr(scriptPath, ".wasm") != nullptr);

  if (scriptPath[0] && scriptOk) {
    char err[192];
    if (isWasm) {
      if (!wasmHostOpen(scriptPath, err, sizeof(err))) {
        char body[256];
        snprintf(body, sizeof(body), "WASM error:\n%s", err[0] ? err : "unknown");
        if (lblStubBody) lv_label_set_text(lblStubBody, body);
      }
    } else if (!luaHostOpen(scriptPath, err, sizeof(err))) {
      char body[256];
      snprintf(body, sizeof(body), "Lua error:\n%s", err[0] ? err : "unknown");
      if (lblStubBody) lv_label_set_text(lblStubBody, body);
    }
  } else if (lblStubBody) {
    char body[160];
    snprintf(body, sizeof(body),
             "%s app\n(no script)\n\nid: %s\nentry: %s",
             gStubApp.name[0] ? gStubApp.name : "Hello",
             gStubApp.id[0] ? gStubApp.id : "-",
             gStubApp.entry[0] ? gStubApp.entry : (isWasm ? "main.wasm" : "main.lua"));
    lv_label_set_text(lblStubBody, body);
  }
}

// ---- clock screen ---------------------------------------------------------

static void refreshClockLabels() {
  if (!lblClockTime) return;
  struct tm t = {};
  getNowTm(&t);

  static const char* wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  bool blink = ((millis() / 500) % 2) == 0;
  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d%c%02d", t.tm_hour, blink ? ':' : ' ',
           t.tm_min);
  lv_label_set_text(lblClockTime, timeBuf);

  char dateBuf[32];
  int wd = t.tm_wday;
  if (wd < 0 || wd > 6) wd = 0;
  int mo = t.tm_mon;
  if (mo < 0 || mo > 11) mo = 0;
  snprintf(dateBuf, sizeof(dateBuf), "%s %s %02d", wdays[wd], months[mo],
           t.tm_mday);
  lv_label_set_text(lblClockDate, dateBuf);

  char secBuf[8];
  snprintf(secBuf, sizeof(secBuf), "%02d", t.tm_sec);
  lv_label_set_text(lblClockSec, secBuf);

  if (lblClockBatt) {
    int pct = readBatteryPercent();
    if (pct >= 0) {
      char battBuf[16];
      snprintf(battBuf, sizeof(battBuf), "%d%%", pct);
      lv_label_set_text(lblClockBatt, battBuf);
      lv_obj_remove_flag(lblClockBatt, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(lblClockBatt, "");
      lv_obj_add_flag(lblClockBatt, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (lblClockHint) {
    size_t n = appsHostCount();
    char hintBuf[24];
    if (n > 0) {
      snprintf(hintBuf, sizeof(hintBuf), "Lua · %u", (unsigned)n);
    } else {
      snprintf(hintBuf, sizeof(hintBuf), "Lua");
    }
    lv_label_set_text(lblClockHint, hintBuf);
  }

  refreshWifiIcon();
}

static void clockTimerCb(lv_timer_t* /*t*/) {
  if (lv_screen_active() == scrClock) {
    refreshClockLabels();
  }
}

static void refreshWifiIcon() {
  WifiOtaState st = wifiOtaState();
  const char* icon = "Wi/";
  const char* cap = "off";
  lv_color_t col = COL_DIM;
  lv_opa_t opa = LV_OPA_40;

  if (st == WIFI_OTA_CONNECTING) {
    icon = "Wi~";
    cap = "join";
    col = COL_WARN;
    opa = LV_OPA_80;
  } else if (st == WIFI_OTA_IDLE) {
    icon = "WiFi";
    cap = "OTA";
    col = COL_ACCENT;
    opa = LV_OPA_80;
  } else if (st == WIFI_OTA_ACTIVE) {
    static const char* frames[] = {"Wi.", "Wi..", "Wi...", "Wi(((("};
    uint32_t frame = (millis() / 300) % 4;
    icon = frames[frame];
    cap = "...";
    col = COL_ACCENT;
    opa = LV_OPA_COVER;
  }

  if (lblClockWifi) {
    lv_label_set_text(lblClockWifi, icon);
    lv_obj_set_style_text_color(lblClockWifi, col, 0);
    lv_obj_set_style_text_opa(lblClockWifi, opa, 0);
  }
  if (lblClockWifiCap) {
    lv_label_set_text(lblClockWifiCap, cap);
    lv_obj_set_style_text_color(lblClockWifiCap, col, 0);
    lv_obj_set_style_text_opa(lblClockWifiCap, opa, 0);
  }
  if (lblLauncherWifi) {
    char buf[24];
    if (st == WIFI_OTA_OFF) {
      snprintf(buf, sizeof(buf), "WiFi off");
      lv_obj_set_style_text_color(lblLauncherWifi, COL_DIM, 0);
      lv_obj_set_style_text_opa(lblLauncherWifi, LV_OPA_50, 0);
    } else if (st == WIFI_OTA_CONNECTING) {
      snprintf(buf, sizeof(buf), "WiFi join");
      lv_obj_set_style_text_color(lblLauncherWifi, COL_WARN, 0);
      lv_obj_set_style_text_opa(lblLauncherWifi, LV_OPA_80, 0);
    } else if (st == WIFI_OTA_ACTIVE) {
      snprintf(buf, sizeof(buf), "WiFi ...");
      lv_obj_set_style_text_color(lblLauncherWifi, COL_ACCENT, 0);
      lv_obj_set_style_text_opa(lblLauncherWifi, LV_OPA_COVER, 0);
    } else {
      snprintf(buf, sizeof(buf), "WiFi OTA");
      lv_obj_set_style_text_color(lblLauncherWifi, COL_ACCENT, 0);
      lv_obj_set_style_text_opa(lblLauncherWifi, LV_OPA_80, 0);
    }
    lv_label_set_text(lblLauncherWifi, buf);
  }
}


// SoftAP Install QR — Wi‑Fi join + install URL (tap toggles).
#ifndef WATCHOS_OTA_QR_SCALE
#define WATCHOS_OTA_QR_SCALE 3
#endif
#ifndef WATCHOS_OTA_QR_QUIET
#define WATCHOS_OTA_QR_QUIET 2
#endif
#ifndef WATCHOS_OTA_QR_MAX_VERSION
#define WATCHOS_OTA_QR_MAX_VERSION 6
#endif

static void otaQrEnsureBuffer(uint16_t w, uint16_t h) {
  size_t need = (size_t)w * (size_t)h * sizeof(uint16_t);
  if (gOtaQrBuf && gOtaQrBufW == w && gOtaQrBufH == h) return;
  if (gOtaQrBuf) {
    free(gOtaQrBuf);
    gOtaQrBuf = nullptr;
  }
  gOtaQrBuf = (uint16_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!gOtaQrBuf) {
    gOtaQrBuf = (uint16_t*)malloc(need);
  }
  gOtaQrBufW = gOtaQrBuf ? w : 0;
  gOtaQrBufH = gOtaQrBuf ? h : 0;
}

static bool otaQrEncodeDraw(const char* text) {
  if (!text || !text[0] || !canvasOtaQr) return false;

  QRCode qr;
  // Version auto: pick smallest that fits (~80-char strings → v3–v5 typical).
  uint8_t version = 0;
  uint8_t* modules = nullptr;
  for (uint8_t v = 3; v <= WATCHOS_OTA_QR_MAX_VERSION; ++v) {
    uint16_t bufSz = qrcode_getBufferSize(v);
    uint8_t* tryBuf = (uint8_t*)malloc(bufSz);
    if (!tryBuf) continue;
    if (qrcode_initText(&qr, tryBuf, v, ECC_MEDIUM, text) == 0) {
      version = v;
      modules = tryBuf;
      break;
    }
    free(tryBuf);
  }
  if (!modules || version == 0) {
    Serial.println("ota qr: encode failed");
    return false;
  }

  const int quiet = WATCHOS_OTA_QR_QUIET;
  const int scale = WATCHOS_OTA_QR_SCALE;
  const int modulesSide = (int)qr.size + quiet * 2;
  const uint16_t pix = (uint16_t)(modulesSide * scale);
  otaQrEnsureBuffer(pix, pix);
  if (!gOtaQrBuf) {
    free(modules);
    Serial.println("ota qr: no pixel buffer");
    return false;
  }

  const uint16_t qrWhite = 0xFFFF;
  const uint16_t qrBlack = 0x0000;
  for (uint16_t y = 0; y < pix; ++y) {
    for (uint16_t x = 0; x < pix; ++x) {
      gOtaQrBuf[(size_t)y * pix + x] = qrWhite;
    }
  }
  for (uint8_t my = 0; my < qr.size; ++my) {
    for (uint8_t mx = 0; mx < qr.size; ++mx) {
      if (!qrcode_getModule(&qr, mx, my)) continue;
      int x0 = (mx + quiet) * scale;
      int y0 = (my + quiet) * scale;
      for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
          gOtaQrBuf[(size_t)(y0 + dy) * pix + (x0 + dx)] = qrBlack;
        }
      }
    }
  }
  free(modules);

  lv_canvas_set_buffer(canvasOtaQr, gOtaQrBuf, pix, pix, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_size(canvasOtaQr, pix, pix);
  if (contOtaQr) lv_obj_set_size(contOtaQr, pix, pix);
  lv_obj_remove_flag(canvasOtaQr, LV_OBJ_FLAG_HIDDEN);
  if (contOtaQr) lv_obj_remove_flag(contOtaQr, LV_OBJ_FLAG_HIDDEN);
  return true;
}

static void otaQrHide() {
  if (canvasOtaQr) lv_obj_add_flag(canvasOtaQr, LV_OBJ_FLAG_HIDDEN);
  if (contOtaQr) lv_obj_add_flag(contOtaQr, LV_OBJ_FLAG_HIDDEN);
  gOtaQrWifiPayload[0] = '\0';
  gOtaQrUrlPayload[0] = '\0';
}

static void otaQrBuildPayloads() {
  // WIFI:T:WPA;S:<ssid>;P:<password>;;
  snprintf(gOtaQrWifiPayload, sizeof(gOtaQrWifiPayload),
           "WIFI:T:WPA;S:%s;P:%s;;", wifiOtaSsid(), wifiOtaPassword());
  // http://<ip>/?k=<password>
  snprintf(gOtaQrUrlPayload, sizeof(gOtaQrUrlPayload),
           "http://%s/?k=%s", wifiOtaIp(), wifiOtaPassword());
}

static void otaQrShowCurrent() {
  if (!wifiOtaIsRunning() || wifiOtaState() == WIFI_OTA_CONNECTING) {
    otaQrHide();
    if (lblOtaHint) {
      lv_label_set_text(lblOtaHint,
                        wifiOtaState() == WIFI_OTA_CONNECTING ? "Joining home Wi-Fi…"
                                                              : "");
    }
    return;
  }
  otaQrBuildPayloads();
  // SoftAP: Wi‑Fi join QR + tap for page. STA/LAN: page QR only (PC already on LAN).
  const bool softap = (wifiOtaMode() == WIFI_OTA_MODE_SOFTAP);
  if (!softap) gOtaQrShowWifi = false;
  const char* payload = (softap && gOtaQrShowWifi) ? gOtaQrWifiPayload
                                                   : gOtaQrUrlPayload;
  if (!otaQrEncodeDraw(payload)) {
    otaQrHide();
    if (lblOtaHint) lv_label_set_text(lblOtaHint, "QR encode failed");
    return;
  }
  if (lblOtaHint) {
    if (!softap) {
      lv_label_set_text(lblOtaHint, "Scan page on LAN PC/phone");
    } else {
      lv_label_set_text(lblOtaHint,
                        gOtaQrShowWifi ? "Scan Wi-Fi · tap for page"
                                       : "Scan page · tap for Wi-Fi");
    }
  }
}

static void onOtaQrTap(lv_event_t* /*e*/) {
  if (!wifiOtaIsRunning() || wifiOtaMode() != WIFI_OTA_MODE_SOFTAP) return;
  noteActivity();
  gOtaQrShowWifi = !gOtaQrShowWifi;
  otaQrShowCurrent();
}

static void refreshOtaScreen() {
  if (!lblOtaBody || lv_screen_active() != scrOta) return;
  char body[300];
  static bool wasReady = false;
  WifiOtaState s = wifiOtaState();
  bool running = wifiOtaIsRunning();
  bool ready = (s == WIFI_OTA_IDLE || s == WIFI_OTA_ACTIVE);

  if (!running) {
    otaQrHide();
    if (lblOtaHint) lv_label_set_text(lblOtaHint, "");
    if (wifiOtaStaConfigured()) {
      snprintf(body, sizeof(body),
               "Radio off.\nPhone SoftAP = join watch AP.\n"
               "Home Wi-Fi → %s\n(pass hidden)",
               wifiOtaStaSavedSsid());
    } else {
      snprintf(body, sizeof(body),
               "Radio off.\nPhone SoftAP ready.\n"
               "Home Wi-Fi: save SSID from\nSoftAP page first.");
    }
    wasReady = false;
  } else if (s == WIFI_OTA_CONNECTING) {
    otaQrHide();
    if (lblOtaHint) lv_label_set_text(lblOtaHint, "Joining home Wi-Fi…");
    snprintf(body, sizeof(body),
             "Joining %s…\nSession k=%s\n%us left",
             wifiOtaSsid(), wifiOtaPassword(),
             (unsigned)wifiOtaRemainingSec());
    wasReady = false;
  } else {
    if (!wasReady) {
      gOtaQrShowWifi = (wifiOtaMode() == WIFI_OTA_MODE_SOFTAP);
      otaQrShowCurrent();
    }
    wasReady = true;
    if (wifiOtaMode() == WIFI_OTA_MODE_SOFTAP) {
      snprintf(body, sizeof(body),
               "SoftAP %s\nPass %s\nIP %s · %us\nhttp://%s/?k=…",
               wifiOtaSsid(), wifiOtaPassword(), wifiOtaIp(),
               (unsigned)wifiOtaRemainingSec(), wifiOtaIp());
    } else {
      snprintf(body, sizeof(body),
               "LAN %s\nIP %s · k=%s\n%us left · watchos.local",
               wifiOtaSsid(), wifiOtaIp(), wifiOtaPassword(),
               (unsigned)wifiOtaRemainingSec());
    }
  }
  if (usbMscActive()) {
    snprintf(body, sizeof(body),
             "USB SD active — PC should see drive.\n"
             "Tap Exit before unplug.\n"
             "Serial/CDC may pause; exit MSC before flash.\n"
             "%s",
             usbMscStatusLine());
  }
  lv_label_set_text(lblOtaBody, body);
  if (lblOtaStatus) {
    char stbuf[96];
    if (usbMscActive()) {
      snprintf(stbuf, sizeof(stbuf), "msc · %s", usbMscStatusLine());
      lv_label_set_text(lblOtaStatus, stbuf);
      lv_obj_set_style_text_color(lblOtaStatus, COL_ACCENT, 0);
    } else {
      const char* tag = (s == WIFI_OTA_OFF) ? "off"
                        : (s == WIFI_OTA_CONNECTING) ? "joining"
                        : (s == WIFI_OTA_ACTIVE) ? "transferring" : "waiting";
      snprintf(stbuf, sizeof(stbuf), "%s · %s", tag, wifiOtaStatusLine());
      lv_label_set_text(lblOtaStatus, stbuf);
      lv_color_t c = COL_DIM;
      if (s == WIFI_OTA_ACTIVE) c = COL_ACCENT;
      else if (s == WIFI_OTA_CONNECTING) c = COL_WARN;
      lv_obj_set_style_text_color(lblOtaStatus, c, 0);
    }
  }
  if (lblOtaUsbSd) {
    lv_label_set_text(lblOtaUsbSd, usbMscActive() ? "Exit USB SD" : "USB SD");
  }
  refreshWifiIcon();
}

static void onOtaStartSoftAp(lv_event_t* /*e*/) {
  noteActivity();
  if (!wifiOtaStartSoftAp()) {
    otaQrHide();
    if (lblOtaStatus) {
      lv_label_set_text(lblOtaStatus, wifiOtaStatusLine());
      lv_obj_set_style_text_color(lblOtaStatus, COL_ERR, 0);
    }
  } else {
    gOtaQrShowWifi = true;
    otaQrShowCurrent();
  }
  refreshOtaScreen();
}

static void onOtaStartSta(lv_event_t* /*e*/) {
  noteActivity();
  if (!wifiOtaStaConfigured()) {
    otaQrHide();
    if (lblOtaStatus) {
      lv_label_set_text(lblOtaStatus, "Save home Wi-Fi via SoftAP");
      lv_obj_set_style_text_color(lblOtaStatus, COL_ERR, 0);
    }
    if (lblOtaBody) {
      lv_label_set_text(
          lblOtaBody,
          "No home Wi-Fi saved.\n"
          "1) Phone SoftAP → join\n"
          "2) Open page → Save home Wi-Fi\n"
          "3) Then tap Home Wi-Fi");
    }
    return;
  }
  if (!wifiOtaStartSta()) {
    otaQrHide();
    if (lblOtaStatus) {
      lv_label_set_text(lblOtaStatus, wifiOtaStatusLine());
      lv_obj_set_style_text_color(lblOtaStatus, COL_ERR, 0);
    }
  } else {
    otaQrHide();  // show page QR after connect
  }
  refreshOtaScreen();
}


static void onOtaUsbSd(lv_event_t* /*e*/) {
  noteActivity();
  if (usbMscActive()) {
    usbMscExit();
    refreshOtaScreen();
    return;
  }
  // Stop SoftAP/STA so radio + MSC do not fight for attention.
  if (wifiOtaIsRunning()) {
    wifiOtaStop();
    otaQrHide();
  }
  bool ok = usbMscEnter();
  refreshOtaScreen();
  // Surface stub/error AFTER refresh so status/body are not overwritten.
  if (!ok) {
    if (lblOtaStatus) {
      lv_label_set_text(lblOtaStatus, usbMscStatusLine());
      lv_obj_set_style_text_color(lblOtaStatus, COL_ERR, 0);
    }
    if (lblOtaBody) {
      lv_label_set_text(
          lblOtaBody,
          "USB SD not in this firmware.\n"
          "Needs waveshare-amoled-206-msc\n"
          "(TinyUSB USB_MODE=0).\n"
          "Default build keeps ACM0 flash.\n"
          "See docs/USB_FLASH.md");
    }
  }
}

static void onOtaCancel(lv_event_t* /*e*/) {
  if (usbMscActive()) usbMscExit();
  wifiOtaStop();
  otaQrHide();
  refreshWifiIcon();
  goLauncher();
}

static void goOta() {
  noteActivity();
  refreshOtaScreen();
  lv_screen_load(scrOta);
}

static void onClockTap(lv_event_t* /*e*/) { goPin(); }

static void buildClockScreen() {
  scrClock = lv_obj_create(NULL);
  style_screen(scrClock);
  lv_obj_add_flag(scrClock, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scrClock, onClockTap, LV_EVENT_CLICKED, NULL);

  lblClockTime = lv_label_create(scrClock);
  lv_obj_set_style_text_font(lblClockTime, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lblClockTime, COL_TEXT, 0);
  lv_label_set_text(lblClockTime, "12:00");
  lv_obj_align(lblClockTime, LV_ALIGN_CENTER, 0, -36);

  lblClockDate = lv_label_create(scrClock);
  lv_obj_set_style_text_font(lblClockDate, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblClockDate, COL_DIM, 0);
  lv_label_set_text(lblClockDate, "Mon Jan 01");
  lv_obj_align(lblClockDate, LV_ALIGN_CENTER, 0, 28);

  lblClockSec = lv_label_create(scrClock);
  lv_obj_set_style_text_font(lblClockSec, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblClockSec, COL_ACCENT, 0);
  lv_obj_set_style_text_opa(lblClockSec, LV_OPA_60, 0);
  lv_label_set_text(lblClockSec, "00");
  lv_obj_align(lblClockSec, LV_ALIGN_CENTER, 0, 58);

  lblClockBatt = lv_label_create(scrClock);
  lv_obj_set_style_text_font(lblClockBatt, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblClockBatt, COL_DIM, 0);
  lv_label_set_text(lblClockBatt, "");
  lv_obj_align(lblClockBatt, LV_ALIGN_TOP_RIGHT, -16, 14);
  lv_obj_add_flag(lblClockBatt, LV_OBJ_FLAG_HIDDEN);

  lblClockHint = lv_label_create(scrClock);
  lv_obj_set_style_text_font(lblClockHint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblClockHint, COL_ACCENT, 0);
  lv_obj_set_style_text_opa(lblClockHint, LV_OPA_50, 0);
  lv_label_set_text(lblClockHint, "Lua");
  lv_obj_align(lblClockHint, LV_ALIGN_TOP_LEFT, 16, 14);

  lblClockWifi = lv_label_create(scrClock);
  lv_obj_set_style_text_font(lblClockWifi, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblClockWifi, COL_DIM, 0);
  lv_obj_set_style_text_opa(lblClockWifi, LV_OPA_40, 0);
  lv_label_set_text(lblClockWifi, "Wi/");
  lv_obj_align(lblClockWifi, LV_ALIGN_TOP_LEFT, 16, 34);

  lblClockWifiCap = lv_label_create(scrClock);
  lv_obj_set_style_text_font(lblClockWifiCap, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblClockWifiCap, COL_DIM, 0);
  lv_obj_set_style_text_opa(lblClockWifiCap, LV_OPA_40, 0);
  lv_label_set_text(lblClockWifiCap, "off");
  lv_obj_align(lblClockWifiCap, LV_ALIGN_TOP_LEFT, 56, 34);

  lv_obj_t* hint = lv_label_create(scrClock);
  lv_label_set_text(hint, "·");
  lv_obj_set_style_text_color(hint, COL_ACCENT, 0);
  lv_obj_set_style_text_opa(hint, LV_OPA_40, 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_24, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_add_flag(hint, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hint, onClockTap, LV_EVENT_CLICKED, NULL);

  refreshClockLabels();
  lv_timer_create(clockTimerCb, 250, NULL);
}

// ---- PIN screen -----------------------------------------------------------

static void clearPinEntry() {
  gPinEntry = "";
  updatePinDots();
}

static void updatePinDots() {
  if (!lblPinDots) return;
  size_t n = gPinEntry.length();
  if (n > 8) n = 8;
  if (n == 0) {
    lv_label_set_text(lblPinDots, "-");
    lv_obj_set_style_text_color(lblPinDots, COL_DIM, 0);
    return;
  }
  char pretty[16];
  for (size_t i = 0; i < n; ++i) pretty[i] = '*';
  pretty[n] = '\0';
  lv_label_set_text(lblPinDots, pretty);
  lv_obj_set_style_text_color(lblPinDots, COL_ACCENT, 0);
}

static void shakePinExec(void* obj, int32_t v) {
  lv_obj_set_x((lv_obj_t*)obj, v - 6);
}

static void shakePinDots() {
  if (!lblPinDots) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, lblPinDots);
  lv_anim_set_values(&a, 0, 12);
  lv_anim_set_duration(&a, 40);
  lv_anim_set_playback_duration(&a, 40);
  lv_anim_set_repeat_count(&a, 3);
  lv_anim_set_exec_cb(&a, shakePinExec);
  lv_anim_start(&a);
}

static void onPinOk() {
  if (millis() < gPinLockUntil) {
    lv_label_set_text(lblPinMsg, "Wait...");
    lv_obj_set_style_text_color(lblPinMsg, COL_WARN, 0);
    return;
  }
  const char* expect = WATCHOS_UNLOCK_PIN;
  if (gPinEntry == expect) {
    gPinFails = 0;
    clearPinEntry();
    goLauncher();
    return;
  }
  gPinFails++;
  shakePinDots();
  lv_label_set_text(lblPinMsg, "Wrong PIN");
  lv_obj_set_style_text_color(lblPinMsg, COL_ERR, 0);
  clearPinEntry();
  if (gPinFails >= 5) {
    gPinFails = 0;
    gPinLockUntil = millis() + 3000;
    lv_label_set_text(lblPinMsg, "Locked 3s");
    lv_obj_set_style_text_color(lblPinMsg, COL_WARN, 0);
  }
}

static void onPinKey(lv_event_t* e) {
  if (millis() < gPinLockUntil) {
    lv_label_set_text(lblPinMsg, "Wait...");
    lv_obj_set_style_text_color(lblPinMsg, COL_WARN, 0);
    return;
  }
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(obj);
  const char* txt = lv_buttonmatrix_get_button_text(obj, id);
  if (!txt) return;

  if (strcmp(txt, "DEL") == 0) {
    if (gPinEntry.length() > 0) gPinEntry.remove(gPinEntry.length() - 1);
  } else if (strcmp(txt, "OK") == 0) {
    onPinOk();
    return;
  } else if (txt[0] && txt[1] == '\0' && isdigit((unsigned char)txt[0])) {
    if (gPinEntry.length() < pinMaxLen()) gPinEntry += txt[0];
  }
  if (lblPinMsg) {
    lv_label_set_text(lblPinMsg, "Enter PIN");
    lv_obj_set_style_text_color(lblPinMsg, COL_DIM, 0);
  }
  updatePinDots();
}

static void onPinBack(lv_event_t* /*e*/) { goClock(); }

static void buildPinScreen() {
  scrPin = lv_obj_create(NULL);
  style_screen(scrPin);
  lv_obj_set_style_pad_all(scrPin, 10, 0);

  lv_obj_t* back = lv_button_create(scrPin);
  lv_obj_set_size(back, 72, 36);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, COL_PANEL, 0);
  lv_obj_set_style_radius(back, 10, 0);
  lv_obj_add_event_cb(back, onPinBack, LV_EVENT_CLICKED, NULL);
  lv_obj_t* backLbl = lv_label_create(back);
  lv_label_set_text(backLbl, "Back");
  lv_obj_set_style_text_color(backLbl, COL_DIM, 0);
  lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(backLbl);

  lblPinMsg = lv_label_create(scrPin);
  lv_label_set_text(lblPinMsg, "Enter PIN");
  lv_obj_set_style_text_color(lblPinMsg, COL_DIM, 0);
  lv_obj_set_style_text_font(lblPinMsg, &lv_font_montserrat_16, 0);
  lv_obj_align(lblPinMsg, LV_ALIGN_TOP_MID, 0, 14);

  lv_obj_t* dotsBox = lv_obj_create(scrPin);
  lv_obj_set_size(dotsBox, 280, 52);
  lv_obj_align(dotsBox, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_bg_color(dotsBox, COL_PANEL, 0);
  lv_obj_set_style_border_color(dotsBox, COL_ACCENT, 0);
  lv_obj_set_style_border_width(dotsBox, 1, 0);
  lv_obj_set_style_radius(dotsBox, 12, 0);
  lv_obj_set_style_pad_all(dotsBox, 8, 0);
  lv_obj_remove_flag(dotsBox, LV_OBJ_FLAG_SCROLLABLE);

  lblPinDots = lv_label_create(dotsBox);
  lv_obj_set_style_text_font(lblPinDots, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lblPinDots, COL_ACCENT, 0);
  lv_obj_center(lblPinDots);
  updatePinDots();

  static const char* pin_map[] = {
      "1", "2", "3", "\n",
      "4", "5", "6", "\n",
      "7", "8", "9", "\n",
      "DEL", "0", "OK", ""};

  pinPad = lv_buttonmatrix_create(scrPin);
  lv_buttonmatrix_set_map(pinPad, pin_map);
  lv_obj_set_size(pinPad, 360, 320);
  lv_obj_align(pinPad, LV_ALIGN_BOTTOM_MID, 0, -10);
  style_keypad(pinPad);
  lv_obj_add_event_cb(pinPad, onPinKey, LV_EVENT_VALUE_CHANGED, NULL);
}

// ---- launcher -------------------------------------------------------------

static void freeLauncherIcons() {
  for (size_t i = 0; i < APPS_MAX; ++i) {
    wrgbImageFree(&gLaunchIcons[i]);
    gLaunchIconsValid[i] = false;
  }
}

static void loadLauncherViewPrefs() {
  Preferences prefs;
  if (!prefs.begin("watchos", true)) {
    gLauncherView = kLaunchGrid;
    return;
  }
  String v = prefs.getString("launcher_view", "grid");
  prefs.end();
  if (v == "list") {
    gLauncherView = kLaunchList;
  } else {
    gLauncherView = kLaunchGrid;
  }
}

static void saveLauncherViewPrefs() {
  Preferences prefs;
  if (!prefs.begin("watchos", false)) return;
  prefs.putString("launcher_view", gLauncherView == kLaunchList ? "list" : "grid");
  prefs.end();
}

static void onLock(lv_event_t* /*e*/) { goClock(); }
static void onOpenSetTime(lv_event_t* /*e*/) { goSetTime(); }
static void onOpenOta(lv_event_t* /*e*/) { goOta(); }
static void onOpenSettings(lv_event_t* /*e*/) { goSettings(); }

static void onAppClicked(lv_event_t* e) {
  uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
  const AppInfo* info = appsHostGet(idx);
  if (info) goStub(info);
}

static void onHelloStub(lv_event_t* /*e*/) { goStub(nullptr); }

/** Stable-ish color from app id for letter-tile fallback. */
static lv_color_t letterTileColor(const char* id) {
  uint32_t h = 2166136261u;
  if (id) {
    for (const char* p = id; *p; ++p) {
      h ^= (uint8_t)*p;
      h *= 16777619u;
    }
  }
  uint8_t r = (uint8_t)(64 + (h & 0x7F));
  uint8_t g = (uint8_t)(64 + ((h >> 8) & 0x7F));
  uint8_t b = (uint8_t)(64 + ((h >> 16) & 0x7F));
  return lv_color_make(r, g, b);
}

static void loadOneLauncherIcon(size_t idx, const AppInfo* a) {
  gLaunchIconsValid[idx] = false;
  wrgbImageFree(&gLaunchIcons[idx]);
  if (!a || !a->icon[0]) return;
  char path[APP_PATH_MAX + APP_ICON_MAX];
  int n = snprintf(path, sizeof(path), "%s/%s", a->folder, a->icon);
  if (n <= 0 || (size_t)n >= sizeof(path)) return;
  if (wrgbImageLoad(path, &gLaunchIcons[idx], kLaunchIconMaxDim)) {
    gLaunchIconsValid[idx] = true;
  }
}

/** Icon widget: WRGB image or colored letter tile. size = display edge px. */
static lv_obj_t* makeIconWidget(lv_obj_t* parent, size_t idx, const AppInfo* a, int size) {
  if (idx < APPS_MAX && gLaunchIconsValid[idx]) {
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, &gLaunchIcons[idx].dsc);
    lv_obj_set_size(img, size, size);
    if ((int)gLaunchIcons[idx].w > size || (int)gLaunchIcons[idx].h > size) {
      lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    return img;
  }
  lv_obj_t* tile = lv_obj_create(parent);
  lv_obj_set_size(tile, size, size);
  lv_obj_set_style_radius(tile, 14, 0);
  lv_obj_set_style_border_width(tile, 0, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tile, letterTileColor(a ? a->id : "x"), 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* letter = lv_label_create(tile);
  char ch[2] = {'?', '\0'};
  if (a && a->name[0]) {
    ch[0] = (char)toupper((unsigned char)a->name[0]);
  } else if (a && a->id[0]) {
    ch[0] = (char)toupper((unsigned char)a->id[0]);
  }
  lv_label_set_text(letter, ch);
  lv_obj_set_style_text_font(letter, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(letter, COL_TEXT, 0);
  lv_obj_center(letter);
  return tile;
}

static void refreshLauncher() {
  if (!contApps) return;
  freeLauncherIcons();
  lv_obj_clean(contApps);

  appsHostRescan();
  size_t n = appsHostCount();

  if (lblLauncherStatus) {
    char buf[96];
    size_t nSd = 0;
    for (size_t i = 0; i < n; ++i) {
      const AppInfo* a = appsHostGet(i);
      if (a && a->storage == APP_STORAGE_SD) ++nSd;
    }
    size_t nLfs = n - nSd;
    if (!appsHostFsOk() && n == 0) {
      snprintf(buf, sizeof(buf), "LittleFS missing — pio run -t uploadfs");
      lv_label_set_text(lblLauncherStatus, buf);
      lv_obj_set_style_text_color(lblLauncherStatus, COL_WARN, 0);
    } else if (n == 0) {
      snprintf(buf, sizeof(buf), "No apps — showing Hello stub");
      lv_label_set_text(lblLauncherStatus, buf);
      lv_obj_set_style_text_color(lblLauncherStatus, COL_DIM, 0);
    } else if (nSd > 0) {
      snprintf(buf, sizeof(buf), "%u app%s (%u flash, %u SD)", (unsigned)n,
               n == 1 ? "" : "s", (unsigned)nLfs, (unsigned)nSd);
      lv_label_set_text(lblLauncherStatus, buf);
      lv_obj_set_style_text_color(lblLauncherStatus, COL_DIM, 0);
    } else {
      snprintf(buf, sizeof(buf), "%u app%s on LittleFS", (unsigned)n,
               n == 1 ? "" : "s");
      lv_label_set_text(lblLauncherStatus, buf);
      lv_obj_set_style_text_color(lblLauncherStatus, COL_DIM, 0);
    }
  }

  if (n == 0) {
    lv_obj_t* btn = lv_button_create(contApps);
    lv_obj_set_size(btn, lv_pct(100), 48);
    lv_obj_set_style_bg_color(btn, COL_PANEL, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, onHelloStub, LV_EVENT_CLICKED, NULL);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Hello");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_center(lbl);

    lv_obj_t* hint = lv_label_create(contApps);
    lv_label_set_text(hint, "Package apps under data/apps/*/app.json\nthen: pio run -t uploadfs");
    lv_obj_set_style_text_color(hint, COL_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(92));
    return;
  }

  for (size_t i = 0; i < n && i < APPS_MAX; ++i) {
    loadOneLauncherIcon(i, appsHostGet(i));
  }

  const int inset = LCD_SAFE_INSET_PX;
  const int contW = LCD_WIDTH - 2 * inset;
  // contApps is already inset-sized; use inner width for tile math.
  int innerW = (int)lv_obj_get_content_width(contApps);
  if (innerW < 80) innerW = contW - 16;

  if (gLauncherView == kLaunchList) {
    lv_obj_set_flex_flow(contApps, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(contApps, 6, 0);
    lv_obj_set_style_pad_column(contApps, 0, 0);
    lv_obj_set_flex_align(contApps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    for (size_t i = 0; i < n; ++i) {
      const AppInfo* a = appsHostGet(i);
      if (!a) continue;

      lv_obj_t* row = lv_button_create(contApps);
      lv_obj_set_width(row, lv_pct(100));
      lv_obj_set_height(row, 64);
      lv_obj_set_style_bg_color(row, COL_PANEL, 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(row, 12, 0);
      lv_obj_set_style_pad_left(row, 8, 0);
      lv_obj_set_style_pad_right(row, 8, 0);
      lv_obj_set_style_pad_top(row, 6, 0);
      lv_obj_set_style_pad_bottom(row, 6, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 10, 0);
      lv_obj_add_event_cb(row, onAppClicked, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

      makeIconWidget(row, i, a, kLaunchIconList);

      lv_obj_t* textCol = lv_obj_create(row);
      lv_obj_set_style_bg_opa(textCol, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(textCol, 0, 0);
      lv_obj_set_style_pad_all(textCol, 0, 0);
      lv_obj_set_flex_grow(textCol, 1);
      lv_obj_set_height(textCol, LV_SIZE_CONTENT);
      lv_obj_clear_flag(textCol, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(textCol, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_flex_flow(textCol, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(textCol, 2, 0);

      lv_obj_t* nameLbl = lv_label_create(textCol);
      lv_label_set_text(nameLbl, a->name);
      lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_color(nameLbl, COL_TEXT, 0);
      lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
      lv_obj_set_width(nameLbl, lv_pct(100));

      char meta[64];
      if (a->storage == APP_STORAGE_SD && a->version[0]) {
        snprintf(meta, sizeof(meta), "v%s  [SD]", a->version);
      } else if (a->storage == APP_STORAGE_SD) {
        snprintf(meta, sizeof(meta), "[SD]");
      } else if (a->version[0]) {
        snprintf(meta, sizeof(meta), "v%s", a->version);
      } else {
        meta[0] = '\0';
      }
      if (meta[0]) {
        lv_obj_t* verLbl = lv_label_create(textCol);
        lv_label_set_text(verLbl, meta);
        lv_obj_set_style_text_font(verLbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(verLbl, COL_DIM, 0);
      }
    }
    return;
  }

  // ---- grid (default): 3 columns, icon + truncated name ----
  lv_obj_set_flex_flow(contApps, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(contApps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(contApps, 10, 0);
  lv_obj_set_style_pad_column(contApps, 8, 0);

  const int cols = 3;
  const int gap = 8;
  int tileW = (innerW - gap * (cols - 1)) / cols;
  if (tileW < 90) tileW = 90;
  int tileH = kLaunchIconDisp + 28;

  for (size_t i = 0; i < n; ++i) {
    const AppInfo* a = appsHostGet(i);
    if (!a) continue;

    lv_obj_t* cell = lv_button_create(contApps);
    lv_obj_set_size(cell, tileW, tileH);
    lv_obj_set_style_bg_color(cell, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cell, 12, 0);
    lv_obj_set_style_pad_all(cell, 4, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 4, 0);
    lv_obj_add_event_cb(cell, onAppClicked, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

    makeIconWidget(cell, i, a, kLaunchIconDisp);

    lv_obj_t* nameLbl = lv_label_create(cell);
    lv_label_set_text(nameLbl, a->name);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nameLbl, COL_TEXT, 0);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nameLbl, tileW - 8);
    lv_obj_set_style_text_align(nameLbl, LV_TEXT_ALIGN_CENTER, 0);
  }
}

static void buildLauncherScreen() {
  scrLauncher = lv_obj_create(NULL);
  style_screen(scrLauncher);
  lv_obj_set_style_pad_all(scrLauncher, 0, 0);
  lv_obj_clear_flag(scrLauncher, LV_OBJ_FLAG_SCROLLABLE);

  const int inset = LCD_SAFE_INSET_PX;

  lv_obj_t* title = lv_label_create(scrLauncher);
  lv_label_set_text(title, "Apps");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, COL_ACCENT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, inset - 8);

  lv_obj_t* lockBtn = lv_button_create(scrLauncher);
  lv_obj_set_size(lockBtn, 64, 32);
  lv_obj_align(lockBtn, LV_ALIGN_TOP_RIGHT, -inset, inset - 12);
  lv_obj_set_style_bg_color(lockBtn, COL_PANEL, 0);
  lv_obj_set_style_radius(lockBtn, 10, 0);
  lv_obj_add_event_cb(lockBtn, onLock, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lockLbl = lv_label_create(lockBtn);
  lv_label_set_text(lockLbl, "Lock");
  lv_obj_set_style_text_color(lockLbl, COL_DIM, 0);
  lv_obj_set_style_text_font(lockLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(lockLbl);

  lv_obj_t* setBtn = lv_button_create(scrLauncher);
  lv_obj_set_size(setBtn, 64, 32);
  lv_obj_align(setBtn, LV_ALIGN_TOP_RIGHT, -(inset + 68), inset - 12);
  lv_obj_set_style_bg_color(setBtn, COL_PANEL, 0);
  lv_obj_set_style_radius(setBtn, 10, 0);
  lv_obj_add_event_cb(setBtn, onOpenSettings, LV_EVENT_CLICKED, NULL);
  lv_obj_t* setLbl = lv_label_create(setBtn);
  lv_label_set_text(setLbl, "Set");
  lv_obj_set_style_text_color(setLbl, COL_ACCENT, 0);
  lv_obj_set_style_text_font(setLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(setLbl);

  lv_obj_t* timeBtn = lv_button_create(scrLauncher);
  lv_obj_set_size(timeBtn, 64, 32);
  lv_obj_align(timeBtn, LV_ALIGN_TOP_LEFT, inset, inset - 12);
  lv_obj_set_style_bg_color(timeBtn, COL_PANEL, 0);
  lv_obj_set_style_radius(timeBtn, 10, 0);
  lv_obj_add_event_cb(timeBtn, onOpenSetTime, LV_EVENT_CLICKED, NULL);
  lv_obj_t* timeLbl = lv_label_create(timeBtn);
  lv_label_set_text(timeLbl, "Time");
  lv_obj_set_style_text_color(timeLbl, COL_DIM, 0);
  lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(timeLbl);

  lv_obj_t* otaBtn = lv_button_create(scrLauncher);
  lv_obj_set_size(otaBtn, 72, 32);
  lv_obj_align(otaBtn, LV_ALIGN_TOP_LEFT, inset + 68, inset - 12);
  lv_obj_set_style_bg_color(otaBtn, COL_PANEL, 0);
  lv_obj_set_style_radius(otaBtn, 10, 0);
  lv_obj_add_event_cb(otaBtn, onOpenOta, LV_EVENT_CLICKED, NULL);
  lv_obj_t* otaLbl = lv_label_create(otaBtn);
  lv_label_set_text(otaLbl, "Install");
  lv_obj_set_style_text_color(otaLbl, COL_ACCENT, 0);
  lv_obj_set_style_text_font(otaLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(otaLbl);

  lv_obj_t* manageBtn = lv_button_create(scrLauncher);
  lv_obj_set_size(manageBtn, 80, 32);
  lv_obj_align(manageBtn, LV_ALIGN_TOP_LEFT, inset + 144, inset - 12);
  lv_obj_set_style_bg_color(manageBtn, COL_PANEL, 0);
  lv_obj_set_style_radius(manageBtn, 10, 0);
  lv_obj_add_event_cb(manageBtn, onOpenManage, LV_EVENT_CLICKED, NULL);
  lv_obj_t* manageLbl = lv_label_create(manageBtn);
  lv_label_set_text(manageLbl, "Manage");
  lv_obj_set_style_text_color(manageLbl, COL_WARN, 0);
  lv_obj_set_style_text_font(manageLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(manageLbl);

  lblLauncherWifi = lv_label_create(scrLauncher);
  lv_obj_set_style_text_font(lblLauncherWifi, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblLauncherWifi, COL_DIM, 0);
  lv_obj_set_style_text_opa(lblLauncherWifi, LV_OPA_50, 0);
  lv_label_set_text(lblLauncherWifi, "WiFi off");
  lv_obj_align(lblLauncherWifi, LV_ALIGN_TOP_MID, 0, inset + 24);

  lblLauncherStatus = lv_label_create(scrLauncher);
  lv_obj_set_style_text_font(lblLauncherStatus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblLauncherStatus, COL_DIM, 0);
  lv_label_set_text(lblLauncherStatus, "");
  lv_obj_align(lblLauncherStatus, LV_ALIGN_TOP_MID, 0, inset + 42);

  contApps = lv_obj_create(scrLauncher);
  int topY = inset + 64;
  int bottomPad = inset;
  int h = LCD_HEIGHT - topY - bottomPad;
  int w = LCD_WIDTH - 2 * inset;
  lv_obj_set_size(contApps, w, h);
  lv_obj_set_pos(contApps, inset, topY);
  lv_obj_set_style_bg_color(contApps, COL_BG, 0);
  lv_obj_set_style_bg_opa(contApps, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(contApps, 0, 0);
  lv_obj_set_style_radius(contApps, 12, 0);
  lv_obj_set_style_pad_all(contApps, 4, 0);
  lv_obj_set_style_text_color(contApps, COL_TEXT, 0);
  lv_obj_set_scroll_dir(contApps, LV_DIR_VER);
  lv_obj_add_flag(contApps, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(contApps, LV_FLEX_FLOW_ROW_WRAP);
}

// ---- settings (launcher view) ---------------------------------------------

static lv_obj_t* btnSettingsGrid = nullptr;
static lv_obj_t* btnSettingsList = nullptr;
static lv_obj_t* lblSettingsView = nullptr;

static void refreshSettingsViewButtons() {
  if (lblSettingsView) {
    lv_label_set_text(lblSettingsView,
                      gLauncherView == kLaunchList ? "Current: List" : "Current: Grid");
  }
  if (btnSettingsGrid) {
    lv_obj_set_style_bg_color(btnSettingsGrid,
                              gLauncherView == kLaunchGrid ? COL_ACCENT : COL_PANEL, 0);
  }
  if (btnSettingsList) {
    lv_obj_set_style_bg_color(btnSettingsList,
                              gLauncherView == kLaunchList ? COL_ACCENT : COL_PANEL, 0);
  }
}

static void onSettingsGrid(lv_event_t* /*e*/) {
  noteActivity();
  gLauncherView = kLaunchGrid;
  saveLauncherViewPrefs();
  refreshSettingsViewButtons();
  refreshLauncher();
}

static void onSettingsList(lv_event_t* /*e*/) {
  noteActivity();
  gLauncherView = kLaunchList;
  saveLauncherViewPrefs();
  refreshSettingsViewButtons();
  refreshLauncher();
}

static void onSettingsBack(lv_event_t* /*e*/) { goLauncher(); }

static void goSettings() {
  refreshSettingsViewButtons();
  noteActivity();
  lv_screen_load(scrSettings);
}

static void buildSettingsScreen() {
  scrSettings = lv_obj_create(NULL);
  style_screen(scrSettings);
  lv_obj_set_style_pad_all(scrSettings, LCD_SAFE_INSET_PX, 0);

  lv_obj_t* back = lv_button_create(scrSettings);
  lv_obj_set_size(back, 72, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(back, COL_PANEL, 0);
  lv_obj_set_style_radius(back, 10, 0);
  lv_obj_add_event_cb(back, onSettingsBack, LV_EVENT_CLICKED, NULL);
  lv_obj_t* backLbl = lv_label_create(back);
  lv_label_set_text(backLbl, "Back");
  lv_obj_set_style_text_color(backLbl, COL_DIM, 0);
  lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(backLbl);

  lv_obj_t* title = lv_label_create(scrSettings);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, COL_ACCENT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

  lv_obj_t* section = lv_label_create(scrSettings);
  lv_label_set_text(section, "Launcher view");
  lv_obj_set_style_text_font(section, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(section, COL_TEXT, 0);
  lv_obj_align(section, LV_ALIGN_TOP_MID, 0, 56);

  lblSettingsView = lv_label_create(scrSettings);
  lv_obj_set_style_text_font(lblSettingsView, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblSettingsView, COL_DIM, 0);
  lv_label_set_text(lblSettingsView, "Current: Grid");
  lv_obj_align(lblSettingsView, LV_ALIGN_TOP_MID, 0, 84);

  btnSettingsGrid = lv_button_create(scrSettings);
  lv_obj_set_size(btnSettingsGrid, 140, 48);
  lv_obj_align(btnSettingsGrid, LV_ALIGN_TOP_MID, -80, 120);
  lv_obj_set_style_radius(btnSettingsGrid, 12, 0);
  lv_obj_add_event_cb(btnSettingsGrid, onSettingsGrid, LV_EVENT_CLICKED, NULL);
  lv_obj_t* gLbl = lv_label_create(btnSettingsGrid);
  lv_label_set_text(gLbl, "Grid");
  lv_obj_set_style_text_font(gLbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(gLbl, COL_TEXT, 0);
  lv_obj_center(gLbl);

  btnSettingsList = lv_button_create(scrSettings);
  lv_obj_set_size(btnSettingsList, 140, 48);
  lv_obj_align(btnSettingsList, LV_ALIGN_TOP_MID, 80, 120);
  lv_obj_set_style_radius(btnSettingsList, 12, 0);
  lv_obj_add_event_cb(btnSettingsList, onSettingsList, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lLbl = lv_label_create(btnSettingsList);
  lv_label_set_text(lLbl, "List");
  lv_obj_set_style_text_font(lLbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lLbl, COL_TEXT, 0);
  lv_obj_center(lLbl);

  lv_obj_t* hint = lv_label_create(scrSettings);
  lv_label_set_text(hint,
                    "Saved in NVS (watchos / launcher_view).\n"
                    "Grid = 3 columns with icons.\n"
                    "List = icon + name + version.");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, COL_DIM, 0);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(hint, LCD_WIDTH - 2 * LCD_SAFE_INSET_PX);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 190);
}

// ---- stub app screen ------------------------------------------------------

static void onStubBack(lv_event_t* /*e*/) {
  // WASM on_back export first (non-zero = handled), then Lua watch.on_back.
  if (wasmHostInvokeOnBack()) return;
  if (luaApiExtInvokeOnBack()) return;
  luaHostClose();
  wasmHostClose();
  goLauncher();
}

static void buildStubScreen() {
  scrStub = lv_obj_create(NULL);
  style_screen(scrStub);
  lv_obj_set_style_pad_all(scrStub, 0, 0);
  lv_obj_clear_flag(scrStub, LV_OBJ_FLAG_SCROLLABLE);

  // Full-screen overlay for watch.rect / circle / text_at (absolute coords).
  luaLayer = lv_obj_create(scrStub);
  lv_obj_set_size(luaLayer, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_pos(luaLayer, 0, 0);
  lv_obj_set_style_bg_opa(luaLayer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(luaLayer, 0, 0);
  lv_obj_set_style_pad_all(luaLayer, 0, 0);
  lv_obj_clear_flag(luaLayer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(luaLayer, LV_OBJ_FLAG_CLICKABLE);  // let touches pass through empty overlay

  lblStubTitle = lv_label_create(scrStub);
  lv_obj_set_style_text_font(lblStubTitle, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lblStubTitle, COL_ACCENT, 0);
  lv_label_set_text(lblStubTitle, "Hello");
  lv_obj_align(lblStubTitle, LV_ALIGN_TOP_MID, 0, 28);

  lblStubBody = lv_label_create(scrStub);
  lv_obj_set_style_text_font(lblStubBody, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblStubBody, COL_TEXT, 0);
  lv_label_set_long_mode(lblStubBody, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lblStubBody, 360);
  lv_label_set_text(lblStubBody, "Hello app");
  lv_obj_align(lblStubBody, LV_ALIGN_TOP_MID, 0, 72);

  luaBtnColumn = lv_obj_create(scrStub);
  lv_obj_set_size(luaBtnColumn, 300, 220);
  lv_obj_align(luaBtnColumn, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_set_style_bg_opa(luaBtnColumn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(luaBtnColumn, 0, 0);
  lv_obj_set_style_pad_all(luaBtnColumn, 0, 0);
  lv_obj_set_flex_flow(luaBtnColumn, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(luaBtnColumn, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(luaBtnColumn, 8, 0);
  lv_obj_clear_flag(luaBtnColumn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(luaBtnColumn, LV_OBJ_FLAG_CLICKABLE);

  stubBackBtn = lv_button_create(scrStub);
  lv_obj_set_size(stubBackBtn, 200, 52);
  lv_obj_align(stubBackBtn, LV_ALIGN_BOTTOM_MID, 0, -24);
  lv_obj_set_style_bg_color(stubBackBtn, COL_ACCENT, 0);
  lv_obj_set_style_radius(stubBackBtn, 14, 0);
  lv_obj_add_event_cb(stubBackBtn, onStubBack, LV_EVENT_CLICKED, NULL);
  lv_obj_t* backLbl = lv_label_create(stubBackBtn);
  lv_label_set_text(backLbl, "Back");
  // Hidden: reclaim bottom space; edge-swipe / on_back still exit apps.
  lv_obj_add_flag(stubBackBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_color(backLbl, lv_color_hex(0x051015), 0);
  lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_20, 0);
  lv_obj_center(backLbl);
}

// ---- set time screen ------------------------------------------------------

static void updateSetTimeLabel() {
  if (!lblSetTimeVal) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", gEditHour, gEditMin);
  lv_label_set_text(lblSetTimeVal, buf);
}

static void onHourMinus(lv_event_t* /*e*/) {
  gEditHour = (gEditHour + 23) % 24;
  updateSetTimeLabel();
}
static void onHourPlus(lv_event_t* /*e*/) {
  gEditHour = (gEditHour + 1) % 24;
  updateSetTimeLabel();
}
static void onMinMinus(lv_event_t* /*e*/) {
  gEditMin = (gEditMin + 59) % 60;
  updateSetTimeLabel();
}
static void onMinPlus(lv_event_t* /*e*/) {
  gEditMin = (gEditMin + 1) % 60;
  updateSetTimeLabel();
}
static void onSetTimeSave(lv_event_t* /*e*/) {
  applyWallTime(gEditHour, gEditMin);
  goLauncher();
}
static void onSetTimeCancel(lv_event_t* /*e*/) { goLauncher(); }

static lv_obj_t* makeAdjBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb,
                            int x, int y) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, 72, 52);
  lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
  lv_obj_set_style_bg_color(b, COL_KEY, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(l, COL_TEXT, 0);
  lv_obj_center(l);
  return b;
}

static void buildSetTimeScreen() {
  scrSetTime = lv_obj_create(NULL);
  style_screen(scrSetTime);

  lv_obj_t* title = lv_label_create(scrSetTime);
  lv_label_set_text(title, "Set time");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, COL_ACCENT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lblSetTimeVal = lv_label_create(scrSetTime);
  lv_obj_set_style_text_font(lblSetTimeVal, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lblSetTimeVal, COL_TEXT, 0);
  lv_obj_align(lblSetTimeVal, LV_ALIGN_TOP_MID, 0, 80);
  updateSetTimeLabel();

  lv_obj_t* hLbl = lv_label_create(scrSetTime);
  lv_label_set_text(hLbl, "Hour");
  lv_obj_set_style_text_color(hLbl, COL_DIM, 0);
  lv_obj_align(hLbl, LV_ALIGN_TOP_MID, -90, 160);

  lv_obj_t* mLbl = lv_label_create(scrSetTime);
  lv_label_set_text(mLbl, "Min");
  lv_obj_set_style_text_color(mLbl, COL_DIM, 0);
  lv_obj_align(mLbl, LV_ALIGN_TOP_MID, 90, 160);

  makeAdjBtn(scrSetTime, "+", onHourPlus, -90, 190);
  makeAdjBtn(scrSetTime, "-", onHourMinus, -90, 250);
  makeAdjBtn(scrSetTime, "+", onMinPlus, 90, 190);
  makeAdjBtn(scrSetTime, "-", onMinMinus, 90, 250);

  lv_obj_t* save = lv_button_create(scrSetTime);
  lv_obj_set_size(save, 200, 52);
  lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, -70);
  lv_obj_set_style_bg_color(save, COL_ACCENT, 0);
  lv_obj_set_style_radius(save, 14, 0);
  lv_obj_add_event_cb(save, onSetTimeSave, LV_EVENT_CLICKED, NULL);
  lv_obj_t* saveLbl = lv_label_create(save);
  lv_label_set_text(saveLbl, "Save");
  lv_obj_set_style_text_color(saveLbl, lv_color_hex(0x051015), 0);
  lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_20, 0);
  lv_obj_center(saveLbl);

  lv_obj_t* cancel = lv_button_create(scrSetTime);
  lv_obj_set_size(cancel, 200, 44);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_style_bg_color(cancel, COL_PANEL, 0);
  lv_obj_set_style_radius(cancel, 12, 0);
  lv_obj_add_event_cb(cancel, onSetTimeCancel, LV_EVENT_CLICKED, NULL);
  lv_obj_t* cancelLbl = lv_label_create(cancel);
  lv_label_set_text(cancelLbl, "Cancel");
  lv_obj_set_style_text_color(cancelLbl, COL_DIM, 0);
  lv_obj_set_style_text_font(cancelLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(cancelLbl);
}


// ---- Manage Apps / uninstall ---------------------------------------------

static void onManageAppClicked(lv_event_t* e);

static void hideManageConfirm() {
  gPendingUninstallId[0] = '\0';
  gPendingUninstallName[0] = '\0';
  gPendingUninstallStorage = APP_STORAGE_LFS;
  if (contManageConfirm) {
    lv_obj_add_flag(contManageConfirm, LV_OBJ_FLAG_HIDDEN);
  }
}

static void refreshManage() {
  if (!listManage) return;
  lv_obj_clean(listManage);
  hideManageConfirm();

  appsHostRescan();
  size_t n = appsHostCount();

  if (lblManageStatus) {
    char buf[96];
    if (!appsHostFsOk()) {
      snprintf(buf, sizeof(buf), "LittleFS missing");
      lv_label_set_text(lblManageStatus, buf);
      lv_obj_set_style_text_color(lblManageStatus, COL_WARN, 0);
    } else if (n == 0) {
      snprintf(buf, sizeof(buf), "No apps installed");
      lv_label_set_text(lblManageStatus, buf);
      lv_obj_set_style_text_color(lblManageStatus, COL_DIM, 0);
    } else {
      snprintf(buf, sizeof(buf), "Tap an app to uninstall (%u)", (unsigned)n);
      lv_label_set_text(lblManageStatus, buf);
      lv_obj_set_style_text_color(lblManageStatus, COL_DIM, 0);
    }
  }

  if (n == 0) {
    lv_obj_t* hint = lv_label_create(listManage);
    lv_label_set_text(hint, "Install apps via SoftAP or uploadfs.");
    lv_obj_set_style_text_color(hint, COL_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    return;
  }

  for (size_t i = 0; i < n; ++i) {
    const AppInfo* a = appsHostGet(i);
    if (!a) continue;
    char line[128];
    const char* sdTag = (a->storage == APP_STORAGE_SD) ? " [SD]" : "";
    if (a->is_protected) {
      snprintf(line, sizeof(line), "%s  [protected]%s", a->name, sdTag);
    } else if (a->version[0]) {
      snprintf(line, sizeof(line), "%s  ·  v%s%s", a->name, a->version, sdTag);
    } else {
      snprintf(line, sizeof(line), "%s%s", a->name, sdTag);
    }
    lv_obj_t* btn = lv_list_add_button(listManage, NULL, line);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(btn, a->is_protected ? COL_DIM : COL_TEXT, 0);
    lv_obj_set_style_bg_color(btn, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(btn, onManageAppClicked, LV_EVENT_CLICKED,
                        (void*)(uintptr_t)i);
  }
}

static void onManageAppClicked(lv_event_t* e) {
  noteActivity();
  uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
  const AppInfo* a = appsHostGet(idx);
  if (!a) return;

  if (a->is_protected) {
    if (lblManageStatus) {
      lv_label_set_text(lblManageStatus, "Protected — cannot uninstall");
      lv_obj_set_style_text_color(lblManageStatus, COL_WARN, 0);
    }
    return;
  }

  strncpy(gPendingUninstallId, a->id, sizeof(gPendingUninstallId) - 1);
  gPendingUninstallId[sizeof(gPendingUninstallId) - 1] = '\0';
  strncpy(gPendingUninstallName, a->name, sizeof(gPendingUninstallName) - 1);
  gPendingUninstallName[sizeof(gPendingUninstallName) - 1] = '\0';
  gPendingUninstallStorage = a->storage;

  if (lblManageConfirm) {
    char buf[160];
    if (gPendingUninstallStorage == APP_STORAGE_SD) {
      snprintf(buf, sizeof(buf),
               "Uninstall\n%s\n(%s)?\n\nDeletes /sd/apps/%s/",
               gPendingUninstallName, gPendingUninstallId, gPendingUninstallId);
    } else {
      snprintf(buf, sizeof(buf),
               "Uninstall\n%s\n(%s)?\n\nDeletes /apps/%s/",
               gPendingUninstallName, gPendingUninstallId, gPendingUninstallId);
    }
    lv_label_set_text(lblManageConfirm, buf);
  }
  if (contManageConfirm) {
    lv_obj_clear_flag(contManageConfirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(contManageConfirm);
  }
}

static void onManageConfirmYes(lv_event_t* /*e*/) {
  noteActivity();
  if (!gPendingUninstallId[0]) {
    hideManageConfirm();
    return;
  }
  char id[APP_ID_MAX];
  strncpy(id, gPendingUninstallId, sizeof(id) - 1);
  id[sizeof(id) - 1] = '\0';
  hideManageConfirm();

  bool ok = appsHostRemove(id);
  if (lblManageStatus) {
    if (ok) {
      char buf[80];
      snprintf(buf, sizeof(buf), "Removed %s", id);
      lv_label_set_text(lblManageStatus, buf);
      lv_obj_set_style_text_color(lblManageStatus, COL_ACCENT, 0);
    } else {
      lv_label_set_text(lblManageStatus, "Uninstall failed");
      lv_obj_set_style_text_color(lblManageStatus, COL_ERR, 0);
    }
  }
  refreshManage();
  refreshLauncher();
}

static void onManageConfirmNo(lv_event_t* /*e*/) {
  noteActivity();
  hideManageConfirm();
}

static void onManageBack(lv_event_t* /*e*/) {
  hideManageConfirm();
  goLauncher();
}

static void goManage() {
  refreshManage();
  lv_screen_load(scrManage);
  noteActivity();
}

static void onOpenManage(lv_event_t* /*e*/) { goManage(); }

static void buildManageScreen() {
  scrManage = lv_obj_create(NULL);
  style_screen(scrManage);
  lv_obj_set_style_pad_all(scrManage, 10, 0);

  lv_obj_t* title = lv_label_create(scrManage);
  lv_label_set_text(title, "Manage Apps");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, COL_ACCENT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t* back = lv_button_create(scrManage);
  lv_obj_set_size(back, 72, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, COL_PANEL, 0);
  lv_obj_set_style_radius(back, 10, 0);
  lv_obj_add_event_cb(back, onManageBack, LV_EVENT_CLICKED, NULL);
  lv_obj_t* backLbl = lv_label_create(back);
  lv_label_set_text(backLbl, "Back");
  lv_obj_set_style_text_color(backLbl, COL_DIM, 0);
  lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(backLbl);

  lblManageStatus = lv_label_create(scrManage);
  lv_obj_set_style_text_font(lblManageStatus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblManageStatus, COL_DIM, 0);
  lv_label_set_text(lblManageStatus, "");
  lv_obj_align(lblManageStatus, LV_ALIGN_TOP_MID, 0, 44);

  listManage = lv_list_create(scrManage);
  lv_obj_set_size(listManage, 390, 400);
  lv_obj_align(listManage, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_color(listManage, COL_PANEL, 0);
  lv_obj_set_style_border_width(listManage, 0, 0);
  lv_obj_set_style_radius(listManage, 12, 0);
  lv_obj_set_style_pad_all(listManage, 8, 0);
  lv_obj_set_style_text_color(listManage, COL_TEXT, 0);

  // Confirm overlay (full-screen dim + panel)
  contManageConfirm = lv_obj_create(scrManage);
  lv_obj_set_size(contManageConfirm, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_pos(contManageConfirm, 0, 0);
  lv_obj_set_style_bg_color(contManageConfirm, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(contManageConfirm, LV_OPA_70, 0);
  lv_obj_set_style_border_width(contManageConfirm, 0, 0);
  lv_obj_set_style_pad_all(contManageConfirm, 0, 0);
  lv_obj_clear_flag(contManageConfirm, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(contManageConfirm, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* panel = lv_obj_create(contManageConfirm);
  lv_obj_set_size(panel, 340, 260);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
  lv_obj_set_style_radius(panel, 16, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 16, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lblManageConfirm = lv_label_create(panel);
  lv_obj_set_style_text_font(lblManageConfirm, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblManageConfirm, COL_TEXT, 0);
  lv_obj_set_style_text_align(lblManageConfirm, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lblManageConfirm, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lblManageConfirm, 300);
  lv_label_set_text(lblManageConfirm, "");
  lv_obj_align(lblManageConfirm, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t* yes = lv_button_create(panel);
  lv_obj_set_size(yes, 140, 48);
  lv_obj_align(yes, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  lv_obj_set_style_bg_color(yes, COL_ERR, 0);
  lv_obj_set_style_radius(yes, 12, 0);
  lv_obj_add_event_cb(yes, onManageConfirmYes, LV_EVENT_CLICKED, NULL);
  lv_obj_t* yesLbl = lv_label_create(yes);
  lv_label_set_text(yesLbl, "Uninstall");
  lv_obj_set_style_text_color(yesLbl, COL_TEXT, 0);
  lv_obj_set_style_text_font(yesLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(yesLbl);

  lv_obj_t* no = lv_button_create(panel);
  lv_obj_set_size(no, 140, 48);
  lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
  lv_obj_set_style_bg_color(no, COL_KEY, 0);
  lv_obj_set_style_radius(no, 12, 0);
  lv_obj_add_event_cb(no, onManageConfirmNo, LV_EVENT_CLICKED, NULL);
  lv_obj_t* noLbl = lv_label_create(no);
  lv_label_set_text(noLbl, "Cancel");
  lv_obj_set_style_text_color(noLbl, COL_TEXT, 0);
  lv_obj_set_style_text_font(noLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(noLbl);
}

// ---- Install / OTA screen -------------------------------------------------

static void buildOtaScreen() {
  scrOta = lv_obj_create(NULL);
  style_screen(scrOta);
  lv_obj_set_style_pad_all(scrOta, 6, 0);

  lblOtaTitle = lv_label_create(scrOta);
  lv_label_set_text(lblOtaTitle, "Install / OTA");
  lv_obj_set_style_text_font(lblOtaTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblOtaTitle, COL_ACCENT, 0);
  lv_obj_align(lblOtaTitle, LV_ALIGN_TOP_MID, 0, 2);

  // Clickable QR (SoftAP: tap toggles Wi‑Fi join ↔ page URL).
  contOtaQr = lv_obj_create(scrOta);
  lv_obj_set_size(contOtaQr, 132, 132);
  lv_obj_align(contOtaQr, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_bg_color(contOtaQr, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(contOtaQr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(contOtaQr, 0, 0);
  lv_obj_set_style_pad_all(contOtaQr, 0, 0);
  lv_obj_set_style_radius(contOtaQr, 6, 0);
  lv_obj_add_flag(contOtaQr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(contOtaQr, onOtaQrTap, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(contOtaQr, LV_OBJ_FLAG_HIDDEN);

  canvasOtaQr = lv_canvas_create(contOtaQr);
  lv_obj_center(canvasOtaQr);
  lv_obj_add_flag(canvasOtaQr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(canvasOtaQr, LV_OBJ_FLAG_CLICKABLE);

  lblOtaHint = lv_label_create(scrOta);
  lv_obj_set_style_text_font(lblOtaHint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblOtaHint, COL_DIM, 0);
  lv_label_set_text(lblOtaHint, "");
  lv_obj_align(lblOtaHint, LV_ALIGN_TOP_MID, 0, 162);

  lblOtaBody = lv_label_create(scrOta);
  lv_obj_set_style_text_font(lblOtaBody, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblOtaBody, COL_TEXT, 0);
  lv_label_set_long_mode(lblOtaBody, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lblOtaBody, 390);
  lv_obj_set_style_text_align(lblOtaBody, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lblOtaBody, "");
  lv_obj_align(lblOtaBody, LV_ALIGN_TOP_MID, 0, 182);

  lblOtaStatus = lv_label_create(scrOta);
  lv_obj_set_style_text_font(lblOtaStatus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblOtaStatus, COL_DIM, 0);
  lv_label_set_text(lblOtaStatus, "off");
  lv_obj_align(lblOtaStatus, LV_ALIGN_BOTTOM_MID, 0, -194);

  // USB SD — expose microSD as MSC (TinyUSB). Toggle enter/exit.
  btnOtaUsbSd = lv_button_create(scrOta);
  lv_obj_set_size(btnOtaUsbSd, 386, 32);
  lv_obj_align(btnOtaUsbSd, LV_ALIGN_BOTTOM_MID, 0, -158);
  lv_obj_set_style_bg_color(btnOtaUsbSd, COL_WARN, 0);
  lv_obj_set_style_radius(btnOtaUsbSd, 12, 0);
  lv_obj_add_event_cb(btnOtaUsbSd, onOtaUsbSd, LV_EVENT_CLICKED, NULL);
  lblOtaUsbSd = lv_label_create(btnOtaUsbSd);
  lv_label_set_text(lblOtaUsbSd, "USB SD");
  lv_obj_set_style_text_color(lblOtaUsbSd, lv_color_hex(0x051015), 0);
  lv_obj_set_style_text_font(lblOtaUsbSd, &lv_font_montserrat_14, 0);
  lv_obj_center(lblOtaUsbSd);


  lv_obj_t* otaManage = lv_button_create(scrOta);
  lv_obj_set_size(otaManage, 386, 32);
  lv_obj_align(otaManage, LV_ALIGN_BOTTOM_MID, 0, -122);
  lv_obj_set_style_bg_color(otaManage, COL_KEY, 0);
  lv_obj_set_style_radius(otaManage, 12, 0);
  lv_obj_add_event_cb(otaManage, onOpenManage, LV_EVENT_CLICKED, NULL);
  lv_obj_t* otaManageLbl = lv_label_create(otaManage);
  lv_label_set_text(otaManageLbl, "Manage Apps");
  lv_obj_set_style_text_color(otaManageLbl, COL_WARN, 0);
  lv_obj_set_style_text_font(otaManageLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(otaManageLbl);

  // Phone SoftAP — join watch hotspot (QR Wi‑Fi + page).
  lv_obj_t* soft = lv_button_create(scrOta);
  lv_obj_set_size(soft, 188, 40);
  lv_obj_align(soft, LV_ALIGN_BOTTOM_LEFT, 12, -78);
  lv_obj_set_style_bg_color(soft, COL_ACCENT, 0);
  lv_obj_set_style_radius(soft, 12, 0);
  lv_obj_add_event_cb(soft, onOtaStartSoftAp, LV_EVENT_CLICKED, NULL);
  lv_obj_t* softLbl = lv_label_create(soft);
  lv_label_set_text(softLbl, "Phone SoftAP");
  lv_obj_set_style_text_color(softLbl, lv_color_hex(0x051015), 0);
  lv_obj_set_style_text_font(softLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(softLbl);

  // Home Wi‑Fi STA — watch joins LAN for Ethernet PC.
  lv_obj_t* home = lv_button_create(scrOta);
  lv_obj_set_size(home, 188, 40);
  lv_obj_align(home, LV_ALIGN_BOTTOM_RIGHT, -12, -78);
  lv_obj_set_style_bg_color(home, COL_KEY, 0);
  lv_obj_set_style_radius(home, 12, 0);
  lv_obj_add_event_cb(home, onOtaStartSta, LV_EVENT_CLICKED, NULL);
  lv_obj_t* homeLbl = lv_label_create(home);
  lv_label_set_text(homeLbl, "Home Wi-Fi");
  lv_obj_set_style_text_color(homeLbl, COL_TEXT, 0);
  lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(homeLbl);

  lv_obj_t* cancel = lv_button_create(scrOta);
  lv_obj_set_size(cancel, 220, 38);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_bg_color(cancel, COL_PANEL, 0);
  lv_obj_set_style_radius(cancel, 12, 0);
  lv_obj_add_event_cb(cancel, onOtaCancel, LV_EVENT_CLICKED, NULL);
  lv_obj_t* cancelLbl = lv_label_create(cancel);
  lv_label_set_text(cancelLbl, "Cancel / Off");
  lv_obj_set_style_text_color(cancelLbl, COL_DIM, 0);
  lv_obj_set_style_text_font(cancelLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(cancelLbl);
}

// ---- bring-up -------------------------------------------------------------

static void initVibrate(void) {
  pinMode(VIBRATE_PIN, OUTPUT);
  digitalWrite(VIBRATE_PIN, LOW);
  gVibrateUntilMs = 0;
}

static bool initImu(void) {
  // Shared Wire already started by initPmic / fallback. Addr 0x6B (SA0 high).
  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("QMI8658 begin failed");
    return false;
  }
  qmi.configAccelerometer(
      SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz,
      SensorQMI8658::LPF_MODE_0);
  qmi.configGyroscope(
      SensorQMI8658::GYR_RANGE_512DPS, SensorQMI8658::GYR_ODR_112_1Hz,
      SensorQMI8658::LPF_MODE_0);
  qmi.enableAccelerometer();
  qmi.enableGyroscope();
  Serial.printf("QMI8658 ok  chipId=0x%02x\n", (unsigned)qmi.getChipID());
  return true;
}

static bool initPmic() {
  Wire.begin(IIC_SDA, IIC_SCL);
  if (!power.begin(Wire, AXP2101_I2C_ADDR, IIC_SDA, IIC_SCL)) {
    Serial.println("AXP2101 begin failed");
    return false;
  }

  power.setALDO1Voltage(3300);
  power.enableALDO1();
  power.setALDO2Voltage(3300);
  power.enableALDO2();
  power.setALDO3Voltage(3300);
  power.enableALDO3();
  power.setALDO4Voltage(1800);
  power.enableALDO4();
  power.setBLDO2Voltage(2800);
  power.enableBLDO2();

  power.disableTSPinMeasure();
  power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power.clearIrqStatus();
  power.enableBattDetection();
  power.enableBattVoltageMeasure();
  power.enableVbusVoltageMeasure();

  Serial.printf(
      "AXP2101 ok  ALDO1=%u ALDO2=%u ALDO3=%u ALDO4=%u BLDO2=%u\n",
      (unsigned)power.getALDO1Voltage(), (unsigned)power.getALDO2Voltage(),
      (unsigned)power.getALDO3Voltage(), (unsigned)power.getALDO4Voltage(),
      (unsigned)power.getBLDO2Voltage());
  int bp = power.getBatteryPercent();
  Serial.printf(
      "battery percent=%d voltage=%umV connected=%d charging=%d vbus=%d\n", bp,
      (unsigned)power.getBattVoltage(), (int)power.isBatteryConnect(),
      (int)power.isCharging(), (int)power.isVbusIn());
  return true;
}

static bool initDisplay() {
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed");
    return false;
  }
  gDisplayBrightness = 0xC0;
  amoled->setBrightness(gDisplayBrightness);
  gfx->fillScreen(RGB565_BLACK);
  return true;
}

static void drawBootBanner(const char* line2) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(24, 40);
  gfx->println("Watch OS");
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(24, 80);
  gfx->println(line2 ? line2 : "Booting...");
}

static bool initLvgl() {
  lv_init();
  lv_tick_set_cb(millis_cb);

  screenWidth = gfx->width();
  screenHeight = gfx->height();

  const uint32_t lines = 40;
  const uint32_t bufSize = screenWidth * lines;
  lv_color_t* buf = (lv_color_t*)heap_caps_malloc(
      bufSize * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) {
    buf = (lv_color_t*)heap_caps_malloc(bufSize * sizeof(lv_color_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!buf) {
    Serial.println("LVGL draw buffer alloc failed");
    return false;
  }

  disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, buf, NULL, bufSize * sizeof(lv_color_t),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Watch OS  AMOLED 2.06");
  Serial.printf("heap=%u psram=%u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getPsramSize());

  // EMF: radio must stay off at boot. OTA SoftAP is user-triggered only.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  Serial.println("Wi-Fi: off (on-demand OTA only)");

  gPmicOk = initPmic();
  if (!gPmicOk) {
    Serial.println("WARNING: continuing without AXP2101 — panel may stay black");
    Wire.begin(IIC_SDA, IIC_SCL);
  }

  initVibrate();
  gImuOk = initImu();
  Serial.println(gImuOk ? "IMU ready" : "IMU unavailable (shake rules will no-op)");

  if (!initDisplay()) {
    Serial.println("Display init failed — halt");
    return;
  }
  drawBootBanner("Booting...");

  bool touchOk = false;
  for (int i = 0; i < 5 && !touchOk; ++i) {
    touchOk = ft3168Begin();
    if (!touchOk) delay(200);
  }
  Serial.println(touchOk ? "FT3168 ok" : "FT3168 init failed (UI still runs)");

  bool rtcOk = pcf85063Begin();
  Serial.println(rtcOk ? "PCF85063 ok" : "PCF85063 not found (using system time)");
  initWallClock();

  drawBootBanner("Mounting apps...");
  bool fsOk = appsHostBegin();
  jobHostBegin();
  // Squish ID catalog (LittleFS CSV → PSRAM). Offline; no Wi‑Fi.
  bool catalogOk = catalogInit();
  Serial.printf("catalog init=%d count=%u source=%d\n", (int)catalogOk,
                (unsigned)catalogCount(), (int)catalogSource());
  bleMeshInit();
  luaHostBegin();
  luaHostSetTitleCallback(luaUiSetTitle);
  luaHostSetTextCallback(luaUiSetText);
  luaHostSetAppendCallback(luaUiAppendText);
  luaHostSetClearCallback(luaUiClear);
  luaHostSetGetSizeCallback(luaUiGetSize);
  luaHostSetSetBgCallback(luaUiSetBg);
  luaHostSetClearWidgetsCallback(luaUiClearWidgets);
  luaHostSetCreateButtonCallback(luaUiCreateButton);
  luaHostSetCreateRectCallback(luaUiCreateRect);
  luaHostSetCreateCircleCallback(luaUiCreateCircle);
  luaHostSetCreateLabelCallback(luaUiCreateLabel);
  luaHostSetGfxClearCallback(luaUiGfxClear);
  luaHostSetBallCallback(luaUiBall);
  luaHostSetBatteryCallback(luaUiBattery);
  wasmHostSetTitleCallback(luaUiSetTitle);
  wasmHostSetTextCallback(luaUiSetText);
  wasmHostSetBackCallback(luaUiRequestBack);
  wasmHostSetBatteryCallback(luaUiBattery);
  luaHostSetBatteryMvCallback(luaUiBatteryMv);
  luaHostSetChargingCallback(luaUiCharging);
  luaHostSetUsbPowerCallback(luaUiUsbPower);
  luaHostSetBatteryConnectedCallback(luaUiBatteryConnected);
  luaHostSetBackCallback(luaUiRequestBack);
  luaHostSetVibrateCallback(luaUiVibrate);
  luaHostSetWifiStateCallback(luaUiWifiState);
  luaHostSetBrightnessCallback(luaUiBrightness);
  luaHostSetImuCallback(luaUiImu);
  luaHostSetCreateLetterPadCallback(luaUiCreateLetterPad);
  luaHostSetButtonLayoutCallback(luaUiButtonLayout);
  luaHostSetTouchCallback(luaUiTouch);
  luaHostSetShowImageCallback(luaUiShowImage);
  luaHostSetClearImageCallback(luaUiClearImage);
  luaApiExtSetCreateLabel(luaExtCreateLabel);
  luaApiExtSetCreatePanel(luaExtCreatePanel);
  luaApiExtSetCreateProgress(luaExtCreateProgress);
  luaApiExtSetSetProgress(luaExtSetProgress);
  luaApiExtSetCreateSlider(luaExtCreateSlider);
  luaApiExtSetBrightnessGet(luaExtBrightnessGet);
  luaApiExtSetBrightnessSet(luaExtBrightnessSet);
  luaApiExtSetIdleGet(luaExtIdleGet);
  luaApiExtSetIdleSet(luaExtIdleSet);
  luaApiExtSetNow(luaExtNow);
  luaApiExtSetShowSprite(luaExtShowSprite);
  luaApiExtSetZRaise(luaExtZRaise);
  luaApiExtSetCanvasCreate(luaExtCanvasCreate);
  luaApiExtSetCanvasClear(luaExtCanvasClear);
  luaApiExtSetCanvasScroll(luaExtCanvasScroll);
  luaApiExtSetCanvasRowHeat(luaExtCanvasRowHeat);
  luaApiExtSetCanvasPixel(luaExtCanvasPixel);
  luaApiExtSetNotify(luaUiNotify);
  Serial.printf("apps fs=%d count=%u\n", (int)fsOk, (unsigned)appsHostCount());

  drawBootBanner("Starting UI...");
  if (!initLvgl()) {
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(20, 40);
    gfx->setTextColor(RGB565_ORANGE);
    gfx->setTextSize(2);
    gfx->println("LVGL init failed");
    return;
  }

  buildClockScreen();
  buildPinScreen();
  buildLauncherScreen();
  buildStubScreen();
  buildSetTimeScreen();
  buildOtaScreen();
  buildManageScreen();
  buildSettingsScreen();
  loadLauncherViewPrefs();
  refreshWifiIcon();
  lv_screen_load(scrClock);
  noteActivity();
  Serial.printf("UI ready (clock cover); soft-lock=%us; wifi=off\n",
                (unsigned)gSoftLockIdleSec);
}

static void softLockTick() {
  // Never soft-lock on clock cover, PIN, Install/OTA, or while USB MSC owns SD.
  lv_obj_t* active = lv_screen_active();
  if (usbMscActive()) return;
  if (active == scrClock || active == scrPin || active == scrOta) return;
  // Open Lua/WASM app host (scrStub): do not soft-lock. IMU-driven apps
  // (e.g. Portal Look) may have no touch for minutes; idle lock belongs on
  // the launcher / shell covers only. Also skip if a host session is open.
  if (active == scrStub || luaHostIsOpen() || wasmHostIsOpen()) return;
  // Soft-lock applies to launcher (+ Set time / Manage / Settings) so an
  // abandoned shell screen cannot stay unlocked forever.
  if (active != scrLauncher && active != scrSetTime &&
      active != scrManage && active != scrSettings) return;

  uint32_t idleSec = gSoftLockIdleSec;
  if (idleSec == 0) return;  // 0 disables soft-lock
  if (gLastActivityMs == 0) {
    noteActivity();
    return;
  }
  uint32_t now = millis();
  if ((now - gLastActivityMs) < idleSec * 1000u) return;

  Serial.println("soft-lock: idle → clock cover");
  goClock();
}

static void serviceEdgeSwipeBack() {
  if (!luaApiExtTakeEdgeSwipeBack()) return;
  if (lv_screen_active() != scrStub) return;
  if (wasmHostInvokeOnBack()) return;
  if (luaApiExtInvokeOnBack()) return;
  luaHostClose();
  wasmHostClose();
  goLauncher();
}

/** Swipe-down while a notification is showing → open the notifying app. */
static void serviceNotifSwipeOpen() {
  if (gNotifUntilMs == 0 || !gNotifAppId[0]) return;
  if (!luaApiExtTakeSwipeDown()) return;

  char id[APP_ID_MAX];
  char text[96];
  strncpy(id, gNotifAppId, sizeof(id) - 1);
  id[sizeof(id) - 1] = '\0';
  strncpy(text, gNotifText, sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  notifBannerHide();
  gNotifText[0] = '\0';
  noteActivity();

  const AppInfo* app = appsHostFindById(id);
  if (!app) {
    Serial.printf("notif: swipe-open unknown app id=%s\n", id);
    return;
  }
  // Deep-link: app can open reply UI via watch.launch_action/text (one-shot).
  luaApiExtSetLaunchIntent("reply", text);
  Serial.printf("notif: swipe-open app=%s action=reply\n", id);
  goStub(app);
}

static void serviceLuaBackRequest() {
  if (!gLuaBackRequested) return;
  gLuaBackRequested = false;
  if (lv_screen_active() != scrStub) return;
  luaHostClose();
  wasmHostClose();
  goLauncher();
}

void loop() {
  lv_timer_handler();
  vibratePoll();
  serviceLuaBackRequest();
  serviceEdgeSwipeBack();
  serviceNotifSwipeOpen();
  servicePendingLuaButton();
  servicePendingLuaSlider();
  luaHostPoll();
  wasmHostPoll();
  bleMeshTick();
  serviceMeshBgAlert();
  notifBannerPoll();

  bool otaWas = wifiOtaIsRunning();
  wifiOtaPoll();
  if (lv_screen_active() == scrOta) {
    refreshOtaScreen();
    if (otaWas && !wifiOtaIsRunning()) {
      refreshWifiIcon();
      goLauncher();
    }
  } else if (lv_screen_active() == scrClock ||
             lv_screen_active() == scrLauncher) {
    static uint32_t lastIconMs = 0;
    if (millis() - lastIconMs > 400) {
      lastIconMs = millis();
      refreshWifiIcon();
    }
  }

  softLockTick();
  delay(5);
}
