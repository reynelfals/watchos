#include "usb_msc_host.h"
#include "sd_host.h"
#include "pin_config.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <string.h>

/*
 * USBMSC needs Native USB OTG (TinyUSB): ARDUINO_USB_MODE == 0.
 * HW CDC/JTAG mode (1) cannot expose MSC.
 */
#if SOC_USB_OTG_SUPPORTED && !ARDUINO_USB_MODE && CONFIG_TINYUSB_MSC_ENABLED

#include <USB.h>
#include <USBMSC.h>

// Global — constructor runs before app_main USB.begin() so MSC is in the
// composite descriptor alongside CDC-on-boot.
static USBMSC gMsc;
static bool gActive = false;
static bool gUsbBegun = false;
static char gStatus[96] = "USB SD idle";

static int32_t mscOnRead(uint32_t lba, uint32_t offset, void* buffer,
                        uint32_t bufsize) {
  (void)offset;
  uint32_t secSize = SD_MMC.sectorSize();
  if (!secSize || !buffer || bufsize == 0) return -1;
  // TinyUSB usually passes whole sectors (offset 0, bufsize % secSize == 0).
  if (offset != 0 || (bufsize % secSize) != 0) {
    // Slow path: byte-oriented RMW for partial/straddling requests.
    uint8_t* out = (uint8_t*)buffer;
    uint32_t done = 0;
    while (done < bufsize) {
      uint32_t absOff = (uint32_t)(lba * secSize + offset + done);
      uint32_t sec = absOff / secSize;
      uint32_t inSec = absOff % secSize;
      uint32_t chunk = secSize - inSec;
      if (chunk > bufsize - done) chunk = bufsize - done;
      uint8_t blk[512];
      if (secSize > sizeof(blk)) return -1;
      if (!SD_MMC.readRAW(blk, sec)) return -1;
      memcpy(out + done, blk + inSec, chunk);
      done += chunk;
    }
    return (int32_t)bufsize;
  }
  uint32_t nSec = bufsize / secSize;
  for (uint32_t i = 0; i < nSec; i++) {
    if (!SD_MMC.readRAW((uint8_t*)buffer + (i * secSize), lba + i)) {
      return -1;
    }
  }
  return (int32_t)bufsize;
}

static int32_t mscOnWrite(uint32_t lba, uint32_t offset, uint8_t* buffer,
                         uint32_t bufsize) {
  uint32_t secSize = SD_MMC.sectorSize();
  if (!secSize || !buffer || bufsize == 0) return -1;
  if (offset != 0 || (bufsize % secSize) != 0) {
    uint32_t done = 0;
    while (done < bufsize) {
      uint32_t absOff = (uint32_t)(lba * secSize + offset + done);
      uint32_t sec = absOff / secSize;
      uint32_t inSec = absOff % secSize;
      uint32_t chunk = secSize - inSec;
      if (chunk > bufsize - done) chunk = bufsize - done;
      uint8_t blk[512];
      if (secSize > sizeof(blk)) return -1;
      if (chunk != secSize) {
        if (!SD_MMC.readRAW(blk, sec)) return -1;
      }
      memcpy(blk + inSec, buffer + done, chunk);
      if (!SD_MMC.writeRAW(blk, sec)) return -1;
      done += chunk;
    }
    return (int32_t)bufsize;
  }
  uint32_t nSec = bufsize / secSize;
  for (uint32_t i = 0; i < nSec; i++) {
    if (!SD_MMC.writeRAW(buffer + (i * secSize), lba + i)) {
      return -1;
    }
  }
  return (int32_t)bufsize;
}

static bool mscOnStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  Serial.printf("usb_msc: start_stop start=%d eject=%d\n", (int)start,
                (int)load_eject);
  if (load_eject && !start) {
    // Host ejected — keep gActive until watch Exit so UI stays consistent;
    // mediaPresent(false) so PC stops using the LUN.
    gMsc.mediaPresent(false);
    snprintf(gStatus, sizeof(gStatus), "PC ejected — tap Exit USB SD");
  }
  return true;
}

bool usbMscActive(void) { return gActive; }

bool usbMscEnter(void) {
  if (gActive) {
    snprintf(gStatus, sizeof(gStatus), "USB SD already active");
    return true;
  }
  if (!sdHostMount()) {
    snprintf(gStatus, sizeof(gStatus), "No SD card");
    return false;
  }

  uint32_t secSize = (uint32_t)SD_MMC.sectorSize();
  uint32_t nSec = (uint32_t)SD_MMC.numSectors();
  if (secSize == 0 || nSec == 0) {
    snprintf(gStatus, sizeof(gStatus), "SD sector info failed");
    return false;
  }

  sdHostLockForMsc(true);

  gMsc.vendorID("WatchOS");
  gMsc.productID("USB_SD");
  gMsc.productRevision("1.0");
  gMsc.onRead(mscOnRead);
  gMsc.onWrite(mscOnWrite);
  gMsc.onStartStop(mscOnStartStop);
  gMsc.isWritable(true);
  gMsc.mediaPresent(true);
  if (!gMsc.begin(nSec, (uint16_t)secSize)) {
    sdHostLockForMsc(false);
    snprintf(gStatus, sizeof(gStatus), "MSC begin failed");
    return false;
  }

  // CDC-on-boot already called USB.begin(); call again is idempotent.
  if (!gUsbBegun) {
    USB.begin();
    gUsbBegun = true;
  }

  gActive = true;
  snprintf(gStatus, sizeof(gStatus),
           "USB SD active — PC should see drive (%lu MB)",
           (unsigned long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
  Serial.printf("usb_msc: enter sectors=%lu size=%lu\n",
                (unsigned long)nSec, (unsigned long)secSize);
  return true;
}

bool usbMscExit(void) {
  if (!gActive) {
    snprintf(gStatus, sizeof(gStatus), "USB SD idle");
    return true;
  }

  gMsc.mediaPresent(false);
  gMsc.end();
  gActive = false;
  sdHostLockForMsc(false);

  // Remount so FatFS sees whatever the PC wrote.
  sdHostUnmount();
  bool ok = sdHostMount();
  snprintf(gStatus, sizeof(gStatus),
           ok ? "USB SD exited — SD remounted" : "USB SD exited — remount failed");
  Serial.println(ok ? "usb_msc: exit + remount ok" : "usb_msc: exit remount fail");
  return true;
}

const char* usbMscStatusLine(void) { return gStatus; }

#else /* no TinyUSB MSC */

bool usbMscActive(void) { return false; }

bool usbMscEnter(void) {
  Serial.println("usb_msc: not available — flash waveshare-amoled-206-msc");
  return false;
}

bool usbMscExit(void) { return true; }

const char* usbMscStatusLine(void) {
  return "USB SD needs -msc firmware build";
}

#endif
