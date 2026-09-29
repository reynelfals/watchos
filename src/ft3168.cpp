#include "ft3168.h"
#include "pin_config.h"

#include <Wire.h>

#define FT3x68_FINGERNUM 0x02
#define FT3x68_X1POSH    0x03
#define FT3x68_X1POSL    0x04
#define FT3x68_Y1POSH    0x05
#define FT3x68_Y1POSL    0x06
#define FT3x68_POWER     0xA5
#define FT3x68_DEVICE_ID 0xA0

static bool write8(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(FT3168_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool readN(uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(FT3168_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)FT3168_I2C_ADDR, (int)n);
  if (got != n) return false;
  for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)Wire.read();
  return true;
}

bool ft3168Begin() {
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, HIGH);
  delay(1);
  digitalWrite(TP_RESET, LOW);
  delay(20);
  digitalWrite(TP_RESET, HIGH);
  delay(50);

  pinMode(TP_INT, INPUT_PULLUP);

  // Monitor / listen-for-touch power mode (Waveshare Arduino_FT3x68).
  write8(FT3x68_POWER, 0x01);
  delay(20);

  uint8_t id = 0;
  if (!readN(FT3x68_DEVICE_ID, &id, 1)) return false;
  // 0x03 = FT3168; other FT3x68 IDs are still compatible.
  (void)id;
  return true;
}

bool ft3168Read(int16_t* x, int16_t* y) {
  if (!x || !y) return false;
  uint8_t buf[5] = {0};
  if (!readN(FT3x68_FINGERNUM, buf, 5)) return false;
  uint8_t n = buf[0] & 0x0F;
  if (n == 0 || n > 2) return false;
  *x = (int16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
  *y = (int16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
  if (*x < 0 || *y < 0) return false;
  if (*x >= LCD_WIDTH) *x = LCD_WIDTH - 1;
  if (*y >= LCD_HEIGHT) *y = LCD_HEIGHT - 1;
  return true;
}
