#pragma once

#include <Arduino.h>

// Minimal FT3168 / FT3x68 driver (I2C 0x38).
// Register map matches Waveshare Arduino_DriveBus Arduino_FT3x68.

bool ft3168Begin();
bool ft3168Read(int16_t* x, int16_t* y);
