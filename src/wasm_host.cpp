#include "wasm_host.h"

#include "audio_host.h"
#include "job_host.h"
#include "lua_api_ext.h"
#include "sd_host.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <string.h>
#include <esp_heap_caps.h>

#include "wasm3.h"

static WasmHostTitleFn gTitleFn = nullptr;
static WasmHostTextFn gTextFn = nullptr;
static WasmHostBackFn gBackFn = nullptr;
static WasmHostBatteryFn gBatteryFn = nullptr;

void wasmHostSetTitleCallback(WasmHostTitleFn fn) { gTitleFn = fn; }
void wasmHostSetTextCallback(WasmHostTextFn fn) { gTextFn = fn; }
void wasmHostSetBackCallback(WasmHostBackFn fn) { gBackFn = fn; }
void wasmHostSetBatteryCallback(WasmHostBatteryFn fn) { gBatteryFn = fn; }

static IM3Environment gEnv = nullptr;
static IM3Runtime gRuntime = nullptr;
static IM3Module gModule = nullptr;
static uint8_t* gWasmBytes = nullptr;
static size_t gWasmLen = 0;
static IM3Function gTickFn = nullptr;
static IM3Function gOnBackFn = nullptr;
static uint32_t gLastPollMs = 0;
static uint32_t gLastTickArgMs = 0;

static constexpr uint32_t kPollIntervalMs = 50;
static constexpr uint32_t kStackSlots = 8 * 1024;

static void setErr(char* errBuf, size_t errLen, const char* msg) {
  if (!errBuf || errLen == 0) return;
  if (!msg) msg = "error";
  strncpy(errBuf, msg, errLen - 1);
  errBuf[errLen - 1] = '\0';
}

static M3Result suppressMissing(M3Result r) {
  if (r == m3Err_functionLookupFailed) return m3Err_none;
  return r;
}

static void linkOne(IM3Module mod, const char* ns, const char* name, const char* sig,
                    M3RawCall fn) {
  (void)suppressMissing(m3_LinkRawFunction(mod, ns, name, sig, fn));
}

static void linkBoth(IM3Module mod, const char* name, const char* sig, M3RawCall fn) {
  linkOne(mod, "env", name, sig, fn);
  linkOne(mod, "watch", name, sig, fn);
}

static const char* cstrAt(void* _mem, int32_t ptr) {
  if (ptr <= 0) return "";
  size_t max = m3_GetMemorySizeAt(_mem);
  if ((size_t)ptr >= max) return "";
  const char* s = (const char*)((uint8_t*)_mem + (uint32_t)ptr);
  size_t remain = max - (size_t)ptr;
  if (remain > 512) remain = 512;
  for (size_t i = 0; i < remain; ++i) {
    if (s[i] == '\0') return s;
  }
  return "";
}

// ---- imports --------------------------------------------------------------

m3ApiRawFunction(w_log) {
  m3ApiGetArg(int32_t, ptr)
  m3ApiGetArg(int32_t, len)
  if (ptr > 0 && len > 0) {
    void* p = m3ApiOffsetToPtr(ptr);
    m3ApiCheckMem(p, (uint32_t)len);
    char tmp[161];
    int n = len > 160 ? 160 : len;
    memcpy(tmp, p, (size_t)n);
    tmp[n] = '\0';
    Serial.printf("[wasm] %s\n", tmp);
  }
  m3ApiSuccess();
}

m3ApiRawFunction(w_print) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, ptr)
  m3ApiGetArg(int32_t, len)
  if (ptr > 0 && len > 0) {
    void* p = m3ApiOffsetToPtr(ptr);
    m3ApiCheckMem(p, (uint32_t)len);
    char tmp[161];
    int n = len > 160 ? 160 : len;
    memcpy(tmp, p, (size_t)n);
    tmp[n] = '\0';
    Serial.printf("[wasm] %s\n", tmp);
    m3ApiReturn(n);
  }
  m3ApiReturn(0);
}

m3ApiRawFunction(w_set_text) {
  m3ApiGetArg(int32_t, ptr)
  if (gTextFn) gTextFn(cstrAt(_mem, ptr));
  m3ApiSuccess();
}

m3ApiRawFunction(w_set_title) {
  m3ApiGetArg(int32_t, ptr)
  if (gTitleFn) gTitleFn(cstrAt(_mem, ptr));
  m3ApiSuccess();
}

m3ApiRawFunction(w_label) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, x)
  m3ApiGetArg(int32_t, y)
  m3ApiGetArg(int32_t, ptr)
  m3ApiGetArg(int32_t, r)
  m3ApiGetArg(int32_t, g)
  m3ApiGetArg(int32_t, b)
  bool ok = hostUiLabel(x, y, cstrAt(_mem, ptr), r, g, b);
  m3ApiReturn(ok ? 1 : 0);
}

m3ApiRawFunction(w_label3) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, x)
  m3ApiGetArg(int32_t, y)
  m3ApiGetArg(int32_t, ptr)
  bool ok = hostUiLabel(x, y, cstrAt(_mem, ptr), 220, 220, 230);
  m3ApiReturn(ok ? 1 : 0);
}

m3ApiRawFunction(w_now) {
  m3ApiReturnType(int32_t)
  m3ApiReturn((int32_t)millis());
}

m3ApiRawFunction(w_battery) {
  m3ApiReturnType(int32_t)
  m3ApiReturn((int32_t)(gBatteryFn ? gBatteryFn() : -1));
}

m3ApiRawFunction(w_back) {
  if (gBackFn) gBackFn();
  m3ApiSuccess();
}

m3ApiRawFunction(w_audio_ready) {
  m3ApiReturnType(int32_t)
  m3ApiReturn(audioHostReady() ? 1 : 0);
}

m3ApiRawFunction(w_mic_start) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, rate)
  uint32_t r = (rate > 0) ? (uint32_t)rate : 16000u;
  const char* err = nullptr;
  bool ok = audioHostMicStart(r, &err);
  (void)err;
  m3ApiReturn(ok ? 1 : 0);
}

m3ApiRawFunction(w_mic_stop) {
  m3ApiReturnType(int32_t)
  m3ApiReturn(audioHostMicStop() ? 1 : 0);
}

m3ApiRawFunction(w_mic_info) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, ptr)
  if (ptr <= 0) {
    m3ApiReturn(0);
  }
  void* p = m3ApiOffsetToPtr(ptr);
  m3ApiCheckMem(p, 16);
  int32_t* out = (int32_t*)p;
  out[0] = (int32_t)audioHostMicSampleRate();
  out[1] = (int32_t)audioHostMicBits();
  out[2] = (int32_t)audioHostMicChannels();
  out[3] = audioHostMicRunning() ? 1 : 0;
  m3ApiReturn(1);
}

m3ApiRawFunction(w_mic_read) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, ptr)
  m3ApiGetArg(int32_t, maxSamples)
  if (ptr <= 0 || maxSamples < 1) {
    m3ApiReturn(0);
  }
  if (maxSamples > AUDIO_MIC_READ_MAX) maxSamples = AUDIO_MIC_READ_MAX;
  void* p = m3ApiOffsetToPtr(ptr);
  m3ApiCheckMem(p, (uint32_t)maxSamples * 2u);
  int got = 0;
  if (!audioHostMicRead((int16_t*)p, maxSamples, &got)) {
    m3ApiReturn(0);
  }
  m3ApiReturn(got);
}

m3ApiRawFunction(w_mic_spectrum) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, ptr)
  m3ApiGetArg(int32_t, bins)
  if (ptr <= 0) {
    m3ApiReturn(0);
  }
  if (bins < 8) bins = 8;
  if (bins > 128) bins = 128;
  void* p = m3ApiOffsetToPtr(ptr);
  m3ApiCheckMem(p, (uint32_t)bins * sizeof(float));
  if (!audioHostMicSpectrum((float*)p, bins)) {
    m3ApiReturn(0);
  }
  m3ApiReturn(bins);
}

m3ApiRawFunction(w_canvas) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, x)
  m3ApiGetArg(int32_t, y)
  m3ApiGetArg(int32_t, w)
  m3ApiGetArg(int32_t, h)
  m3ApiReturn(hostUiCanvasCreate(x, y, w, h) ? 1 : 0);
}

m3ApiRawFunction(w_canvas_clear) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, r)
  m3ApiGetArg(int32_t, g)
  m3ApiGetArg(int32_t, b)
  m3ApiReturn(hostUiCanvasClear(r, g, b) ? 1 : 0);
}

m3ApiRawFunction(w_canvas_scroll) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, dy)
  m3ApiReturn(hostUiCanvasScroll(dy) ? 1 : 0);
}

m3ApiRawFunction(w_canvas_row) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, y)
  m3ApiGetArg(int32_t, ptr)
  m3ApiGetArg(int32_t, n)
  if (ptr <= 0 || n < 1) {
    m3ApiReturn(0);
  }
  if (n > 256) n = 256;
  void* p = m3ApiOffsetToPtr(ptr);
  m3ApiCheckMem(p, (uint32_t)n * sizeof(float));
  m3ApiReturn(hostUiCanvasRowHeat(y, (const float*)p, n) ? 1 : 0);
}

static void linkImports(IM3Module mod) {
  linkBoth(mod, "log", "v(ii)", &w_log);
  linkBoth(mod, "print", "i(ii)", &w_print);
  linkBoth(mod, "set_text", "v(i)", &w_set_text);
  linkBoth(mod, "set_title", "v(i)", &w_set_title);
  linkBoth(mod, "label", "i(iiiiii)", &w_label);
  linkBoth(mod, "label3", "i(iii)", &w_label3);
  linkBoth(mod, "now", "i()", &w_now);
  linkBoth(mod, "battery", "i()", &w_battery);
  linkBoth(mod, "back", "v()", &w_back);
  linkBoth(mod, "audio_ready", "i()", &w_audio_ready);
  linkBoth(mod, "mic_start", "i(i)", &w_mic_start);
  linkBoth(mod, "mic_stop", "i()", &w_mic_stop);
  linkBoth(mod, "mic_info", "i(i)", &w_mic_info);
  linkBoth(mod, "mic_read", "i(ii)", &w_mic_read);
  linkBoth(mod, "mic_spectrum", "i(ii)", &w_mic_spectrum);
  linkBoth(mod, "canvas", "i(iiii)", &w_canvas);
  linkBoth(mod, "canvas_clear", "i(iii)", &w_canvas_clear);
  linkBoth(mod, "canvas_scroll", "i(i)", &w_canvas_scroll);
  linkBoth(mod, "canvas_row", "i(iii)", &w_canvas_row);
}

static void freeWasmBytes() {
  if (gWasmBytes) {
    heap_caps_free(gWasmBytes);
    gWasmBytes = nullptr;
  }
  gWasmLen = 0;
}

void wasmHostClose(void) {
  jobHostReset();
  audioHostMicStop();
  gTickFn = nullptr;
  gOnBackFn = nullptr;
  gLastPollMs = 0;
  gLastTickArgMs = 0;
  if (gRuntime) {
    m3_FreeRuntime(gRuntime);
    gRuntime = nullptr;
    gModule = nullptr;
  }
  if (gEnv) {
    m3_FreeEnvironment(gEnv);
    gEnv = nullptr;
  }
  freeWasmBytes();
}

bool wasmHostIsOpen(void) { return gRuntime != nullptr; }

static bool loadFileToPsram(const char* path, char* errBuf, size_t errLen) {
  freeWasmBytes();
  const bool onSd = (strncmp(path, "/sd/", 4) == 0);
  File f;
  if (onSd) {
    if (!sdHostPathOk(path)) {
      setErr(errBuf, errLen, "bad sd path");
      return false;
    }
    if (!sdHostReady() && !sdHostMount()) {
      setErr(errBuf, errLen, "sd not mounted");
      return false;
    }
    if (!sdHostExists(path)) {
      setErr(errBuf, errLen, "wasm not found");
      return false;
    }
    if (!sdHostOpenRead(path, &f)) {
      setErr(errBuf, errLen, "open failed");
      return false;
    }
  } else {
    if (!LittleFS.exists(path)) {
      setErr(errBuf, errLen, "wasm not found");
      return false;
    }
    f = LittleFS.open(path, "r");
    if (!f) {
      setErr(errBuf, errLen, "open failed");
      return false;
    }
  }

  size_t sz = f.size();
  if (sz == 0 || sz > WASM_HOST_MAX_BYTES) {
    f.close();
    setErr(errBuf, errLen, sz == 0 ? "empty wasm" : "wasm too large");
    return false;
  }

  uint8_t* buf =
      (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if (!buf) {
    f.close();
    setErr(errBuf, errLen, "oom wasm buffer");
    return false;
  }

  size_t got = f.read(buf, sz);
  f.close();
  if (got != sz) {
    heap_caps_free(buf);
    setErr(errBuf, errLen, "short read");
    return false;
  }

  gWasmBytes = buf;
  gWasmLen = sz;
  return true;
}

static bool findFn(IM3Function* out, const char* name) {
  *out = nullptr;
  return m3_FindFunction(out, gRuntime, name) == m3Err_none && *out != nullptr;
}

bool wasmHostOpen(const char* wasmPath, char* errBuf, size_t errLen) {
  if (errBuf && errLen) errBuf[0] = '\0';
  wasmHostClose();

  if (!wasmPath || !wasmPath[0]) {
    setErr(errBuf, errLen, "no wasm path");
    return false;
  }
  if (!loadFileToPsram(wasmPath, errBuf, errLen)) return false;

  gEnv = m3_NewEnvironment();
  if (!gEnv) {
    setErr(errBuf, errLen, "m3_NewEnvironment failed");
    wasmHostClose();
    return false;
  }

  gRuntime = m3_NewRuntime(gEnv, kStackSlots, nullptr);
  if (!gRuntime) {
    setErr(errBuf, errLen, "m3_NewRuntime failed");
    wasmHostClose();
    return false;
  }

  M3Result result =
      m3_ParseModule(gEnv, &gModule, gWasmBytes, (uint32_t)gWasmLen);
  if (result) {
    setErr(errBuf, errLen, result);
    Serial.printf("wasm parse: %s\n", result);
    wasmHostClose();
    return false;
  }

  result = m3_LoadModule(gRuntime, gModule);
  if (result) {
    setErr(errBuf, errLen, result);
    Serial.printf("wasm load: %s\n", result);
    gModule = nullptr;
    wasmHostClose();
    return false;
  }

  linkImports(gModule);

  result = m3_RunStart(gModule);
  if (result) {
    Serial.printf("wasm start: %s\n", result);
  }

  IM3Function initFn = nullptr;
  if (!findFn(&initFn, "init")) {
    if (!findFn(&initFn, "_start")) {
      findFn(&initFn, "start");
    }
  }
  if (initFn) {
    result = m3_CallV(initFn);
    if (result) {
      setErr(errBuf, errLen, result);
      Serial.printf("wasm init: %s\n", result);
      wasmHostClose();
      return false;
    }
  }

  if (!findFn(&gTickFn, "tick")) {
    findFn(&gTickFn, "on_tick");
  }
  if (!findFn(&gOnBackFn, "on_back")) {
    findFn(&gOnBackFn, "onBack");
  }

  gLastPollMs = millis();
  gLastTickArgMs = gLastPollMs;
  Serial.printf("wasm: open ok (%u bytes) tick=%d on_back=%d\n",
                (unsigned)gWasmLen, gTickFn ? 1 : 0, gOnBackFn ? 1 : 0);
  return true;
}

void wasmHostPoll(void) {
  if (!gRuntime || !gTickFn) return;
  uint32_t now = millis();
  if (gLastPollMs != 0 && (now - gLastPollMs) < kPollIntervalMs) return;
  uint32_t dt =
      (gLastTickArgMs == 0) ? kPollIntervalMs : (now - gLastTickArgMs);
  gLastPollMs = now;
  gLastTickArgMs = now;

  M3Result r = m3_CallV(gTickFn, (uint32_t)dt);
  if (r) {
    Serial.printf("wasm tick error: %s\n", r);
  }
}

bool wasmHostInvokeOnBack(void) {
  if (!gRuntime || !gOnBackFn) return false;
  M3Result r = m3_CallV(gOnBackFn);
  if (r) {
    Serial.printf("wasm on_back error: %s\n", r);
    return false;
  }
  if (m3_GetRetCount(gOnBackFn) >= 1) {
    uint32_t out = 0;
    if (m3_GetResultsV(gOnBackFn, &out) == m3Err_none) {
      return out != 0;
    }
  }
  return false;
}
