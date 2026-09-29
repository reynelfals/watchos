#pragma once

// Panel geometry for Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502).
// pin_config.h may already define LCD_WIDTH/HEIGHT — keep those authoritative.
#ifndef LCD_WIDTH
#define LCD_WIDTH 410
#endif
#ifndef LCD_HEIGHT
#define LCD_HEIGHT 502
#endif
#define LCD_ACTIVE_W_MM 33.09f
#define LCD_ACTIVE_H_MM 40.51f
#define LCD_CORNER_RADIUS_MM 9.2f
#define LCD_CORNER_RADIUS_PX 114
#ifndef LCD_SAFE_INSET_PX
#define LCD_SAFE_INSET_PX 24
#endif
