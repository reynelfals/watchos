#pragma once

// Unlock PIN for Watch OS (clock cover).
// PUBLIC DEFAULT: change this before any real deploy, rebuild, and
// re-flash firmware (pio run -t upload). Treat like a device passcode.
#define WATCHOS_UNLOCK_PIN "1234"

// Soft-lock: after this many idle seconds on the launcher / shell covers
// (Set time, Manage, Settings), return to the clock cover (PIN required again).
// Touch resets the timer. Soft-lock does NOT run while a Lua/WASM app is open
// (scrStub), while entering the PIN, or while Install/OTA is open.
#ifndef WATCHOS_SOFT_LOCK_IDLE_SEC
#define WATCHOS_SOFT_LOCK_IDLE_SEC 60
#endif

// On-demand Wi‑Fi OTA window (seconds). Radio turns off on cancel/timeout.
#ifndef WATCHOS_WIFI_OTA_TIMEOUT_SEC
#define WATCHOS_WIFI_OTA_TIMEOUT_SEC 300
#endif

// SoftAP name for Install/OTA mode (password is random per session, on-screen).
#ifndef WATCHOS_WIFI_OTA_SSID
#define WATCHOS_WIFI_OTA_SSID "WatchOS-OTA"
#endif

// Optional developer STA override/fallback only. Normal users save home Wi‑Fi
// from the SoftAP Install page (stored in NVS on the device). Leave empty.
#ifndef WATCHOS_WIFI_STA_SSID
#define WATCHOS_WIFI_STA_SSID ""
#endif
#ifndef WATCHOS_WIFI_STA_PASS
#define WATCHOS_WIFI_STA_PASS ""
#endif
