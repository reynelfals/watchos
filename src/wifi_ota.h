#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Wi‑Fi / OTA radio policy: OFF unless user explicitly starts Install/OTA. */
typedef enum {
  WIFI_OTA_OFF = 0,       /**< Radio powered down (default / after stop). */
  WIFI_OTA_CONNECTING,    /**< STA joining home AP (not used for SoftAP). */
  WIFI_OTA_IDLE,          /**< SoftAP/STA up, waiting for a client / transfer. */
  WIFI_OTA_ACTIVE         /**< Transferring or recently wrote bytes. */
} WifiOtaState;

/** How the radio was started for this Install session. */
typedef enum {
  WIFI_OTA_MODE_NONE = 0,
  WIFI_OTA_MODE_SOFTAP,   /**< Phone joins WatchOS SoftAP. */
  WIFI_OTA_MODE_STA       /**< Watch joins home Wi‑Fi (LAN PC path). */
} WifiOtaMode;

#ifndef WATCHOS_WIFI_OTA_TIMEOUT_SEC
#define WATCHOS_WIFI_OTA_TIMEOUT_SEC 300  /* 5 minutes */
#endif

/** SoftAP SSID shown on the OTA screen (fixed; password is per-session). */
#ifndef WATCHOS_WIFI_OTA_SSID
#define WATCHOS_WIFI_OTA_SSID "WatchOS-OTA"
#endif

/** STA join timeout (seconds) before giving up and powering radio off. */
#ifndef WATCHOS_WIFI_STA_CONNECT_SEC
#define WATCHOS_WIFI_STA_CONNECT_SEC 25
#endif

/**
 * Start SoftAP + HTTP upload server (phone path).
 * Never call at boot — only from an explicit user action.
 */
bool wifiOtaStartSoftAp(void);

/**
 * Start STA join using per-device saved home Wi‑Fi (NVS) + HTTP upload server.
 * Optional compile-time WATCHOS_WIFI_STA_* is a developer fallback only.
 * Returns false if no creds or begin failed (radio left off).
 * Connection completes asynchronously; poll until IDLE or OFF.
 */
bool wifiOtaStartSta(void);

/** Alias for SoftAP start (legacy). */
bool wifiOtaStart(void);

/** Tear down HTTP server and power the radio fully off. Safe to call anytime. */
void wifiOtaStop(void);

/** Service WebServer + STA connect + timeout; call from loop(). No-op when off. */
void wifiOtaPoll(void);

WifiOtaState wifiOtaState(void);
WifiOtaMode wifiOtaMode(void);
bool wifiOtaIsRunning(void);

/**
 * True when home Wi‑Fi credentials are available (NVS saved, or optional
 * compile-time override). Normal users provision via SoftAP web form.
 */
bool wifiOtaStaConfigured(void);

/** Saved home SSID for UI (empty if none). Never exposes the password. */
const char* wifiOtaStaSavedSsid(void);

/** SoftAP SSID, or home STA SSID when in STA mode. */
const char* wifiOtaSsid(void);

/**
 * Session secret: SoftAP Wi‑Fi password and/or HTTP ?k= token (both modes).
 * Empty when off. Not the saved home Wi‑Fi password.
 */
const char* wifiOtaPassword(void);

const char* wifiOtaIp(void);

/** Seconds until auto-stop (0 if off). */
uint32_t wifiOtaRemainingSec(void);

/** Last status line for the OTA UI (never NULL). */
const char* wifiOtaStatusLine(void);

#ifdef __cplusplus
}
#endif
