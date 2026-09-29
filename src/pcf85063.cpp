#include "pcf85063.h"
#include "pin_config.h"
#include <Wire.h>

static constexpr uint8_t kAddr = PCF85063_I2C_ADDR;
static constexpr uint8_t kCtrl1 = 0x00;
static constexpr uint8_t kSec = 0x04;
static bool gOk = false;

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool writeRegs(uint8_t reg, const uint8_t* data, size_t n) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  for (size_t i = 0; i < n; ++i) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t reg, uint8_t* data, size_t n) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)kAddr, (int)n);
  if (got != n) return false;
  for (size_t i = 0; i < n; ++i) data[i] = Wire.read();
  return true;
}

bool pcf85063Begin() {
  gOk = false;
  uint8_t probe = 0;
  // Seconds register should respond if the chip is on the bus.
  if (!readRegs(kSec, &probe, 1)) return false;

  // Clear STOP bit (bit 5) in CTRL1 so the oscillator runs; keep 24h (bit1=0).
  uint8_t ctrl1 = 0;
  if (readRegs(kCtrl1, &ctrl1, 1)) {
    ctrl1 &= (uint8_t)~(1u << 5);  // clear STOP
    ctrl1 &= (uint8_t)~(1u << 1);  // 24-hour mode
    writeRegs(kCtrl1, &ctrl1, 1);
  }
  gOk = true;
  return true;
}

bool pcf85063Available() { return gOk; }

bool pcf85063GetTime(struct tm* out) {
  if (!gOk || !out) return false;
  uint8_t buf[7];
  if (!readRegs(kSec, buf, 7)) return false;
  // OS flag (bit7 of seconds) means oscillator stopped — still return values.
  out->tm_sec = bcd2dec(buf[0] & 0x7F);
  out->tm_min = bcd2dec(buf[1] & 0x7F);
  out->tm_hour = bcd2dec(buf[2] & 0x3F);
  out->tm_mday = bcd2dec(buf[3] & 0x3F);
  out->tm_wday = bcd2dec(buf[4] & 0x07);
  out->tm_mon = bcd2dec(buf[5] & 0x1F) - 1;
  out->tm_year = bcd2dec(buf[6]) + 100;  // years since 1900 → 20xx
  out->tm_isdst = -1;
  return true;
}

bool pcf85063SetTime(const struct tm* in) {
  if (!gOk || !in) return false;
  uint8_t buf[7];
  buf[0] = dec2bcd((uint8_t)constrain(in->tm_sec, 0, 59)) & 0x7F;
  buf[1] = dec2bcd((uint8_t)constrain(in->tm_min, 0, 59));
  buf[2] = dec2bcd((uint8_t)constrain(in->tm_hour, 0, 23));
  buf[3] = dec2bcd((uint8_t)constrain(in->tm_mday, 1, 31));
  buf[4] = dec2bcd((uint8_t)constrain(in->tm_wday, 0, 6));
  buf[5] = dec2bcd((uint8_t)constrain(in->tm_mon + 1, 1, 12));
  int y = in->tm_year % 100;
  if (y < 0) y += 100;
  buf[6] = dec2bcd((uint8_t)y);
  return writeRegs(kSec, buf, 7);
}
