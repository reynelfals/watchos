#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init NimBLE central stack (idempotent). Safe with WiFi off. */
bool bleMeshInit(void);

/**
 * Tear down NimBLE so the ESP32 radio controller can run SoftAP/STA.
 * Disconnects, stops scan, clears ready flags, NimBLEDevice::deinit(true).
 * Call before WiFi.mode(WIFI_AP|WIFI_STA). Idempotent.
 */
bool bleMeshSuspendForWifi(void);

/**
 * Re-init NimBLE after Wi‑Fi OTA radio is powered off.
 * No-op if already ready. Mesh Chat works again after this.
 */
bool bleMeshResumeAfterWifi(void);

/** True once NimBLE init succeeded. */
bool bleMeshReady(void);

/** Non-blocking pump: drains FromRadio after notify, advances scan/connect. */
void bleMeshTick(void);

/** idle | scanning | connecting | connected | error */
const char* bleMeshStatus(void);

/**
 * Start scan for Meshtastic GATT service advertisers.
 * timeout_ms 0 → default 4000. Results available via bleMeshScanCount/At
 * after status returns to idle (or while scanning for partials).
 */
bool bleMeshScanStart(uint32_t timeout_ms);

int bleMeshScanCount(void);

/** Copy scan hit i into buffers. Returns false if i out of range. */
bool bleMeshScanAt(int i, char* addrOut, size_t addrLen, char* nameOut,
                   size_t nameLen, int* rssiOut);

/**
 * Connect to BLE address string ("AA:BB:..." or "aabb...").
 * pin: nullptr / empty → NO_PIN open connect; else 4–6 digit passkey.
 * Non-blocking: status goes connecting → connected|error.
 */
bool bleMeshConnect(const char* addr, const char* pin);

bool bleMeshDisconnect(void);

bool bleMeshConnected(void);

/** Encode TEXT_MESSAGE_APP broadcast on current channel and write ToRadio. */
bool bleMeshSendText(const char* utf8, char* err, size_t errLen);

/** Mesh channel index 0..7 (Meshtastic); default 1 (FamilyCh secondary). Persisted in NVS. */
void bleMeshSetChannel(uint8_t ch);
uint8_t bleMeshGetChannel(void);

typedef struct {
  uint32_t from;
  char text[200];
  uint32_t rx_time;
} BleMeshMsg;

/** Pop one decoded text message; returns false if queue empty. */
bool bleMeshPoll(BleMeshMsg* out);

/**
 * Take one pending background inbound-text alert (vibrate/banner path).
 * Set from NimBLE/decode via flag — never call LVGL from BLE callbacks.
 * Returns false if none pending. Clears the pending flag.
 */
bool bleMeshTakeBgAlert(char* out, size_t outLen);

/** Background alert prefs (NVS ble_mesh). Defaults: beep=true, vibrate=true. */
bool bleMeshAlertBeepEnabled(void);
void bleMeshAlertBeepSet(bool on);
bool bleMeshAlertVibrateEnabled(void);
void bleMeshAlertVibrateSet(bool on);

#ifdef __cplusplus
}
#endif
