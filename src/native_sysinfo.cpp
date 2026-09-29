#include "native_apps.h"
#include "display_geometry.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t* scrSysInfo = nullptr;
static lv_obj_t* lblSysBody = nullptr;
static lv_timer_t* sysInfoTimer = nullptr;

static const lv_color_t COL_BG = lv_color_hex(0x050508);
static const lv_color_t COL_ACCENT = lv_color_hex(0x3EE0F0);
static const lv_color_t COL_TEXT = lv_color_hex(0xF4F7FB);
static const lv_color_t COL_DIM = lv_color_hex(0x8B93A7);

static void formatUptime(char* out, size_t outSz, uint32_t ms) {
  if (!out || outSz == 0) return;
  uint32_t sec = ms / 1000u;
  uint32_t days = sec / 86400u;
  sec %= 86400u;
  uint32_t hrs = sec / 3600u;
  sec %= 3600u;
  uint32_t mins = sec / 60u;
  sec %= 60u;
  if (days > 0) {
    snprintf(out, outSz, "%lud %02lu:%02lu:%02lu", (unsigned long)days,
             (unsigned long)hrs, (unsigned long)mins, (unsigned long)sec);
  } else {
    snprintf(out, outSz, "%02lu:%02lu:%02lu", (unsigned long)hrs,
             (unsigned long)mins, (unsigned long)sec);
  }
}

static void refreshSysInfoBody() {
  if (!lblSysBody) return;

  char uptime[32];
  formatUptime(uptime, sizeof(uptime), millis());

  uint32_t heap = ESP.getFreeHeap();
  uint32_t psram = ESP.getFreePsram();
  uint32_t heapMin = ESP.getMinFreeHeap();
  const char* model = ESP.getChipModel();
  uint32_t rev = ESP.getChipRevision();
  uint32_t cores = ESP.getChipCores();
  uint32_t freq = ESP.getCpuFreqMHz();
  uint64_t mac = ESP.getEfuseMac();

  int battPct = nativeHostBatteryPercent();
  int battMv = nativeHostBatteryMv();
  char battLine[48];
  if (battPct >= 0 && battMv >= 0) {
    snprintf(battLine, sizeof(battLine), "%d%%  %d mV", battPct, battMv);
  } else if (battPct >= 0) {
    snprintf(battLine, sizeof(battLine), "%d%%", battPct);
  } else if (battMv >= 0) {
    snprintf(battLine, sizeof(battLine), "%d mV", battMv);
  } else {
    snprintf(battLine, sizeof(battLine), "n/a");
  }

  char body[512];
  snprintf(body, sizeof(body),
           "Chip: %s rev%u (%u core)\n"
           "CPU: %u MHz\n"
           "MAC: %02X:%02X:%02X:%02X:%02X:%02X\n"
           "\n"
           "Heap free: %u\n"
           "Heap min:  %u\n"
           "PSRAM free: %u\n"
           "\n"
           "Battery: %s\n"
           "Uptime: %s\n"
           "\n"
           "Firmware native app\n"
           "(not uninstallable)",
           model ? model : "?", (unsigned)rev, (unsigned)cores, (unsigned)freq,
           (unsigned)((mac >> 40) & 0xFF), (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 16) & 0xFF),
           (unsigned)((mac >> 8) & 0xFF), (unsigned)(mac & 0xFF),
           (unsigned)heap, (unsigned)heapMin, (unsigned)psram, battLine, uptime);
  lv_label_set_text(lblSysBody, body);
}

static void sysInfoTimerCb(lv_timer_t* /*t*/) {
  if (scrSysInfo && lv_screen_active() == scrSysInfo) {
    refreshSysInfoBody();
  }
}

static void onSysInfoBack(lv_event_t* /*e*/) {
  nativeHostNoteActivity();
  if (sysInfoTimer) {
    lv_timer_pause(sysInfoTimer);
  }
  nativeHostGoLauncher();
}

static void ensureSysInfoScreen() {
  if (scrSysInfo) return;

  scrSysInfo = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrSysInfo, COL_BG, 0);
  lv_obj_set_style_bg_opa(scrSysInfo, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(scrSysInfo, COL_TEXT, 0);
  lv_obj_set_style_pad_all(scrSysInfo, LCD_SAFE_INSET_PX, 0);
  lv_obj_clear_flag(scrSysInfo, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(scrSysInfo);
  lv_label_set_text(title, "System Info");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, COL_ACCENT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t* sub = lv_label_create(scrSysInfo);
  lv_label_set_text(sub, "sysinfo · native");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sub, COL_DIM, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 44);

  lblSysBody = lv_label_create(scrSysInfo);
  lv_obj_set_style_text_font(lblSysBody, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblSysBody, COL_TEXT, 0);
  lv_label_set_long_mode(lblSysBody, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lblSysBody, LCD_WIDTH - 2 * LCD_SAFE_INSET_PX - 8);
  lv_obj_align(lblSysBody, LV_ALIGN_TOP_MID, 0, 72);

  lv_obj_t* back = lv_button_create(scrSysInfo);
  lv_obj_set_size(back, 200, 52);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_color(back, COL_ACCENT, 0);
  lv_obj_set_style_radius(back, 14, 0);
  lv_obj_add_event_cb(back, onSysInfoBack, LV_EVENT_CLICKED, NULL);
  lv_obj_t* backLbl = lv_label_create(back);
  lv_label_set_text(backLbl, "Back");
  lv_obj_set_style_text_color(backLbl, lv_color_hex(0x051015), 0);
  lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_20, 0);
  lv_obj_center(backLbl);

  sysInfoTimer = lv_timer_create(sysInfoTimerCb, 1000, nullptr);
  lv_timer_pause(sysInfoTimer);
}

void nativeSysInfoLaunch() {
  ensureSysInfoScreen();
  refreshSysInfoBody();
  nativeHostNoteActivity();
  if (sysInfoTimer) lv_timer_resume(sysInfoTimer);
  lv_screen_load(scrSysInfo);
}

lv_obj_t* nativeSysInfoScreen() { return scrSysInfo; }
