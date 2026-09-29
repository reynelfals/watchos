#pragma once

#include <Arduino.h>
#include <time.h>

// Minimal PCF85063 driver (I2C 0x51) for Waveshare AMOLED 2.06.
// Shared bus with FT3168 (SDA=15 SCL=14). Optional — clock works without it.

bool pcf85063Begin();
bool pcf85063Available();
bool pcf85063GetTime(struct tm* out);
bool pcf85063SetTime(const struct tm* in);
