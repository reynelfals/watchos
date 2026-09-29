#pragma once

#include <lvgl.h>
#include <stdint.h>

/** Loaded WRGB (RGB565 LE) ready for lv_image_set_src. */
struct WrgbImage {
  lv_image_dsc_t dsc;
  uint8_t* pixels;  // heap / PSRAM; free with wrgbImageFree
  uint16_t w;
  uint16_t h;
};

/**
 * Load a WRGB file from LittleFS or /sd/... into out.
 * Clamps accepted dimensions to maxDim (default 96). Returns false on error;
 * out is zeroed on failure.
 */
bool wrgbImageLoad(const char* path, WrgbImage* out, uint16_t maxDim = 96);

/** Free pixels and clear descriptor. Safe on empty / already-freed images. */
void wrgbImageFree(WrgbImage* img);
