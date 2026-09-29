#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * USB Mass Storage Class (MSC) backed by microSD sector R/W.
 *
 * Requires TinyUSB: ARDUINO_USB_MODE=0 — only in env waveshare-amoled-206-msc.
 * Default env uses USB_MODE=1 (HW CDC/JTAG) for reliable ACM0 esptool; stub
 * returns "USB SD needs -msc firmware build" when MSC is not compiled in.
 * With CDC_ON_BOOT=1, CDC+MSC are composite; CDC may pause while MSC is busy.
 * The USBMSC object is constructed at static init so TinyUSB registers the
 * MSC interface before USB.begin() (CDC-on-boot path).
 */

/** True while media is presented to the PC. */
bool usbMscActive(void);

/**
 * Mount SD (if needed), present it as a USB drive.
 * Returns false if TinyUSB unavailable, no card, or already active.
 */
bool usbMscEnter(void);

/**
 * Eject media from USB view and remount FatFS so watch apps see PC writes.
 * Safe to call when inactive (no-op success).
 */
bool usbMscExit(void);

/** Human-readable status for UI (never NULL). */
const char* usbMscStatusLine(void);

#ifdef __cplusplus
}
#endif
