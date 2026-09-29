#include "wrgb_image.h"
#include "sd_host.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>

static constexpr size_t kWrgbHeaderSize = 8;

static int readPathAll(const char* path, uint8_t** outBuf, size_t maxBytes) {
  if (!path || !outBuf) return -1;
  *outBuf = nullptr;

  if (strncmp(path, "/sd/", 4) == 0) {
    if (!sdHostPathOk(path)) return -1;
    if (!sdHostReady() && !sdHostMount()) return -1;
    uint8_t* buf =
        (uint8_t*)heap_caps_malloc(maxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(maxBytes, MALLOC_CAP_8BIT);
    if (!buf) return -1;
    int n = sdHostRead(path, (char*)buf, maxBytes);
    if (n < 0) {
      heap_caps_free(buf);
      return -1;
    }
    *outBuf = buf;
    return n;
  }

  if (!LittleFS.exists(path)) return -1;
  File f = LittleFS.open(path, "r");
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return -1;
  }
  size_t sz = f.size();
  if (sz == 0 || sz > maxBytes) {
    f.close();
    return -1;
  }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if (!buf) {
    f.close();
    return -1;
  }
  size_t n = f.readBytes((char*)buf, sz);
  f.close();
  if (n != sz) {
    heap_caps_free(buf);
    return -1;
  }
  *outBuf = buf;
  return (int)n;
}

void wrgbImageFree(WrgbImage* img) {
  if (!img) return;
  if (img->pixels) {
    heap_caps_free(img->pixels);
    img->pixels = nullptr;
  }
  memset(&img->dsc, 0, sizeof(img->dsc));
  img->w = 0;
  img->h = 0;
}

bool wrgbImageLoad(const char* path, WrgbImage* out, uint16_t maxDim) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!path || !path[0]) return false;
  if (maxDim == 0) maxDim = 96;
  if (maxDim > 160) maxDim = 160;

  const size_t maxFile = kWrgbHeaderSize + (size_t)maxDim * (size_t)maxDim * 2u;
  uint8_t* fileBuf = nullptr;
  int n = readPathAll(path, &fileBuf, maxFile);
  if (n < (int)kWrgbHeaderSize || !fileBuf) {
    if (fileBuf) heap_caps_free(fileBuf);
    return false;
  }
  if (fileBuf[0] != 'W' || fileBuf[1] != 'R' || fileBuf[2] != 'G' || fileBuf[3] != 'B') {
    heap_caps_free(fileBuf);
    return false;
  }
  uint16_t w = (uint16_t)fileBuf[4] | ((uint16_t)fileBuf[5] << 8);
  uint16_t h = (uint16_t)fileBuf[6] | ((uint16_t)fileBuf[7] << 8);
  if (w == 0 || h == 0 || w > maxDim || h > maxDim) {
    heap_caps_free(fileBuf);
    return false;
  }
  size_t pixBytes = (size_t)w * (size_t)h * 2u;
  if ((size_t)n < kWrgbHeaderSize + pixBytes) {
    heap_caps_free(fileBuf);
    return false;
  }

  uint8_t* pix =
      (uint8_t*)heap_caps_malloc(pixBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pix) pix = (uint8_t*)heap_caps_malloc(pixBytes, MALLOC_CAP_8BIT);
  if (!pix) {
    heap_caps_free(fileBuf);
    return false;
  }
  memcpy(pix, fileBuf + kWrgbHeaderSize, pixBytes);
  heap_caps_free(fileBuf);

  out->pixels = pix;
  out->w = w;
  out->h = h;
  out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  out->dsc.header.w = w;
  out->dsc.header.h = h;
  out->dsc.header.stride = (uint32_t)w * 2u;
  out->dsc.data_size = (uint32_t)pixBytes;
  out->dsc.data = out->pixels;
  return true;
}
