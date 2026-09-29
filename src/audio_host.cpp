#include "audio_host.h"
#include "pin_config.h"
#include "sd_host.h"

#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <FS.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include <esp_task_wdt.h>

static constexpr int kFftN = 256;
static constexpr int kMaxBins = 128;
static constexpr uint32_t kDefaultRate = 16000;

static I2SClass gI2s;
static bool gProbedMic = false;
static bool gProbedSpk = false;
static bool gEs7210Ok = false;
static bool gEs8311Ok = false;
static bool gMicRunning = false;
static bool gSpkRunning = false;
static uint32_t gRate = 0;
static char gErr[48] = "";

static float gRe[kFftN];
static float gIm[kFftN];
static int16_t gPcmStereo[kFftN * 2];
static int16_t gPcmMono[kFftN];
static int16_t gReadStereo[AUDIO_MIC_READ_MAX * 2];
static int16_t gWriteStereo[512];
/** Scratch for record/play — must NOT live on the 8 KiB loopTask stack (Lua+LVGL). */
static int16_t gRecordMono[AUDIO_MIC_READ_MAX];
static uint8_t gPlayBuf[1024];
static float gFftMags[kFftN / 2];

/** Yield + feed TWDT during multi-second blocking record/play on loopTask. */
static void audioPumpWatchdog(void) {
  yield();
  // Safe if loop task is not subscribed (returns NOT_FOUND); ignore errors.
  (void)esp_task_wdt_reset();
}

static void setErr(const char* s) {
  strncpy(gErr, s ? s : "", sizeof(gErr) - 1);
  gErr[sizeof(gErr) - 1] = '\0';
}

/** Close mic + optional file, delete incomplete path, publish err. */
static bool failRecord(fs::File* f, const char* path, const char* err, const char** errOut) {
  if (f) f->close();
  audioHostMicStop();
  if (path && path[0]) sdHostRemove(path);
  setErr(err ? err : "record_failed");
  if (errOut) *errOut = gErr;
  return false;
}

/** Mono from stereo: pick louder of L/R each frame (ES7210 may only drive one slot). */
static inline int16_t stereoToMono(int16_t left, int16_t right) {
  int al = left < 0 ? -left : left;
  int ar = right < 0 ? -right : right;
  return (al >= ar) ? left : right;
}

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t* out) {
  if (!out) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  *out = (uint8_t)Wire.read();
  return true;
}

static bool i2cUpdateBits(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t data) {
  uint8_t v = 0;
  if (!i2cReadReg(addr, reg, &v)) return false;
  v = (uint8_t)((v & (uint8_t)~mask) | (data & mask));
  return i2cWriteReg(addr, reg, v);
}

static bool probeEs7210(void) {
  Wire.beginTransmission(ES7210_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

static bool probeEs8311(void) {
  Wire.beginTransmission(ES8311_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

static void paOff(void) {
  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, LOW);
}

static void paOn(void) {
  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
}

/** Re-assert PA HIGH and return sampled level (1=HIGH). */
static int paAssertOn(void) {
  paOn();
  return digitalRead(PA_CTRL) == HIGH ? 1 : 0;
}

/**
 * ES8311 DAC volume (reg 0x32): 0x00=-95.5dB … 0xBF=0dB … 0xFF=+32dB.
 * Apps own volume UX via audioHostSpeakerVolume*; max clamped at 0xBF (0 dB).
 * Default session level is 0 dB (audible). Not persisted globally.
 */
static constexpr uint8_t kEs8311DacVolMax = 0xBF;  // 0 dB — no boost on tiny speakers
static uint8_t gSpeakerVolRaw = kEs8311DacVolMax;  // session last-set (default 100%)

static uint8_t clampVolRaw(uint8_t raw) {
  return raw > kEs8311DacVolMax ? kEs8311DacVolMax : raw;
}

static uint8_t pctToRaw(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return (uint8_t)((percent * (int)kEs8311DacVolMax + 50) / 100);
}

static int rawToPct(uint8_t raw) {
  raw = clampVolRaw(raw);
  return (int)(((int)raw * 100 + (int)kEs8311DacVolMax / 2) / (int)kEs8311DacVolMax);
}

/** Store session volume; write reg 0x32 immediately if speaker is running. */
static bool es8311ApplyVolume(uint8_t raw) {
  raw = clampVolRaw(raw);
  gSpeakerVolRaw = raw;
  if (!gSpkRunning) return true;  // applied on next speaker start / play
  const uint8_t a = ES8311_I2C_ADDR;
  return i2cWriteReg(a, 0x32, raw);
}

/** Force unmute + current session volume and log reg readback. */
static bool es8311ForceAudible(void) {
  const uint8_t a = ES8311_I2C_ADDR;
  const uint8_t want = clampVolRaw(gSpeakerVolRaw);
  if (!i2cWriteReg(a, 0x31, 0x00)) return false;  // unmute DSM/DEM
  if (!i2cWriteReg(a, 0x32, want)) return false;
  // Power DAC / HP path again (idempotent; helps after mic shared-bus use)
  (void)i2cWriteReg(a, 0x12, 0x00);
  (void)i2cWriteReg(a, 0x13, 0x10);
  uint8_t r31 = 0xFF, r32 = 0xFF;
  bool ack31 = i2cReadReg(a, 0x31, &r31);
  bool ack32 = i2cReadReg(a, 0x32, &r32);
  Serial.printf("audio: ES8311 vol unmute ACK31=%d reg31=0x%02X ACK32=%d reg32=0x%02X (want 0x%02X pct=%d)\n",
                ack31 ? 1 : 0, (unsigned)r31, ack32 ? 1 : 0, (unsigned)r32,
                (unsigned)want, rawToPct(want));
  return ack31 && ack32 && r32 == want && (r31 & 0x60) == 0;
}

static uint32_t clampAudioRate(uint32_t rate) {
  if (rate < 8000) rate = 8000;
  if (rate > 48000) rate = 48000;
  if (rate != 8000 && rate != 16000 && rate != 48000) rate = 16000;
  return rate;
}

/** Best-effort ES7210 bring-up for 16-bit I2S slave @ rate with MCLK=256*Fs. */
static bool es7210Init(uint32_t rate) {
  const uint8_t a = ES7210_I2C_ADDR;
  if (!i2cWriteReg(a, 0x00, 0xff)) return false;
  delay(10);
  if (!i2cWriteReg(a, 0x00, 0x32)) return false;
  if (!i2cWriteReg(a, 0x01, 0x3f)) return false;
  if (!i2cWriteReg(a, 0x09, 0x30)) return false;
  if (!i2cWriteReg(a, 0x0A, 0x30)) return false;
  if (!i2cWriteReg(a, 0x23, 0x2a)) return false;
  if (!i2cWriteReg(a, 0x22, 0x0a)) return false;
  if (!i2cWriteReg(a, 0x20, 0x0a)) return false;
  if (!i2cWriteReg(a, 0x21, 0x2a)) return false;
  if (!i2cUpdateBits(a, 0x08, 0x01, 0x00)) return false;
  if (!i2cWriteReg(a, 0x40, 0xC3)) return false;
  if (!i2cWriteReg(a, 0x41, 0x70)) return false;
  if (!i2cWriteReg(a, 0x42, 0x70)) return false;
  if (!i2cWriteReg(a, 0x11, 0x60)) return false;
  if (!i2cWriteReg(a, 0x12, 0x00)) return false;

  uint8_t adc_div = 0x01, dll = 0x01, doubler = 0x01, osr = 0x20;
  uint8_t lrck_h = 0x01, lrck_l = 0x00;
  if (rate == 8000) {
    doubler = 0x00;
    lrck_h = 0x02;
    lrck_l = 0x00;
  }
  uint8_t mainclk = (uint8_t)((adc_div & 0x1f) | ((doubler & 1) << 6) | ((dll & 1) << 7));
  if (!i2cWriteReg(a, 0x02, mainclk)) return false;
  if (!i2cWriteReg(a, 0x07, osr)) return false;
  if (!i2cWriteReg(a, 0x04, lrck_h)) return false;
  if (!i2cWriteReg(a, 0x05, lrck_l)) return false;
  if (!i2cWriteReg(a, 0x03, 0x00)) return false;

  if (!i2cWriteReg(a, 0x47, 0x08)) return false;
  if (!i2cWriteReg(a, 0x48, 0x08)) return false;
  if (!i2cWriteReg(a, 0x49, 0x00)) return false;
  if (!i2cWriteReg(a, 0x4A, 0x00)) return false;
  if (!i2cWriteReg(a, 0x06, 0x04)) return false;
  if (!i2cWriteReg(a, 0x4B, 0x0F)) return false;
  if (!i2cWriteReg(a, 0x4C, 0x00)) return false;
  if (!i2cWriteReg(a, 0x43, 0x1A)) return false;
  if (!i2cWriteReg(a, 0x44, 0x1A)) return false;
  if (!i2cWriteReg(a, 0x01, 0x20)) return false;
  if (!i2cWriteReg(a, 0x00, 0x71)) return false;
  if (!i2cWriteReg(a, 0x00, 0x41)) return false;
  (void)rate;
  return true;
}

static void es7210Shutdown(void) {
  i2cWriteReg(ES7210_I2C_ADDR, 0x01, 0x3f);
  i2cWriteReg(ES7210_I2C_ADDR, 0x00, 0xff);
}

/**
 * Best-effort ES8311 DAC bring-up (slave, MCLK=256*Fs).
 * Register sequence adapted from Waveshare 08_ES8311 / community init +
 * Espressif coeff table for mclk = rate * 256.
 */
static bool es8311ConfigSample(uint32_t rate) {
  const uint8_t a = ES8311_I2C_ADDR;
  // Coeff for MCLK = 256 * Fs (Arduino I2S default):
  // pre_div, pre_multi, adc_div, dac_div, fs_mode, lrck_h, lrck_l, bclk_div, adc_osr, dac_osr
  uint8_t pre_div = 1, pre_multi = 1, adc_div = 1, dac_div = 1, fs_mode = 0;
  uint8_t lrck_h = 0x00, lrck_l = 0xff, bclk_div = 4, adc_osr = 0x10, dac_osr = 0x20;
  if (rate == 48000) {
    dac_osr = 0x10;
  } else if (rate == 8000) {
    // mclk=2048000 → pre_div=1; or mclk=4096000 → pre_div=2
    // Arduino uses 256*Fs → 2.048 MHz @ 8k
    pre_div = 1;
    dac_osr = 0x20;
  }

  uint8_t mult_code = 0;
  if (pre_multi == 2) mult_code = 1;
  else if (pre_multi == 4) mult_code = 2;
  else if (pre_multi == 8) mult_code = 3;

  uint8_t reg02 = 0;
  if (!i2cReadReg(a, 0x02, &reg02)) reg02 = 0;
  reg02 = (uint8_t)((reg02 & 0x07) | ((pre_div - 1) << 5) | (mult_code << 3));
  if (!i2cWriteReg(a, 0x02, reg02)) return false;
  if (!i2cWriteReg(a, 0x05, (uint8_t)(((adc_div - 1) << 4) | (dac_div - 1)))) return false;
  if (!i2cWriteReg(a, 0x03, (uint8_t)((fs_mode << 6) | adc_osr))) return false;
  if (!i2cWriteReg(a, 0x04, dac_osr)) return false;
  if (!i2cWriteReg(a, 0x07, lrck_h)) return false;
  if (!i2cWriteReg(a, 0x08, lrck_l)) return false;
  uint8_t bdiv = (bclk_div < 19) ? (uint8_t)(bclk_div - 1) : bclk_div;
  uint8_t reg06 = 0;
  if (!i2cReadReg(a, 0x06, &reg06)) reg06 = 0;
  reg06 = (uint8_t)((reg06 & 0xE0) | (bdiv & 0x1F));
  if (!i2cWriteReg(a, 0x06, reg06)) return false;
  return true;
}

static bool es8311Init(uint32_t rate) {
  const uint8_t a = ES8311_I2C_ADDR;
  // Waveshare-style reset
  if (!i2cWriteReg(a, 0x00, 0x1F)) return false;
  delay(5);
  if (!i2cWriteReg(a, 0x00, 0x00)) return false;
  delay(10);
  if (!i2cWriteReg(a, 0x00, 0x80)) return false;  // CSM on, slave
  delay(20);

  // Clocks from MCLK pad
  if (!i2cWriteReg(a, 0x01, 0x3F)) return false;
  if (!es8311ConfigSample(rate)) return false;

  // SDP 16-bit I2S (WL=3 → bits[4:2]=011 → 0x0C)
  if (!i2cWriteReg(a, 0x09, 0x0C)) return false;
  if (!i2cWriteReg(a, 0x0A, 0x0C)) return false;

  // Analog / DAC power (playback path)
  if (!i2cWriteReg(a, 0x0D, 0x01)) return false;
  if (!i2cWriteReg(a, 0x0E, 0x02)) return false;
  if (!i2cWriteReg(a, 0x12, 0x00)) return false;
  if (!i2cWriteReg(a, 0x13, 0x10)) return false;
  if (!i2cWriteReg(a, 0x14, 0x1A)) return false;
  if (!i2cWriteReg(a, 0x37, 0x08)) return false;
  // Unmute DAC (reg31 bits 6+5) + audible volume.
  // Datasheet reg32: 0x00 = -95.5 dB (near silent), 0xBF = 0 dB, 0xFF = +32 dB.
  // Prior bug set 0x00 thinking it was "max" — that is why Play returned ok but silent.
  if (!i2cWriteReg(a, 0x31, 0x00)) return false;  // unmute
  if (!i2cWriteReg(a, 0x32, clampVolRaw(gSpeakerVolRaw))) return false;  // session vol
  return true;
}

static void es8311Shutdown(void) {
  const uint8_t a = ES8311_I2C_ADDR;
  i2cWriteReg(a, 0x31, 0x60);  // mute
  i2cWriteReg(a, 0x32, 0x00);
  i2cWriteReg(a, 0x0E, 0xFF);
  i2cWriteReg(a, 0x0D, 0xFA);
  i2cWriteReg(a, 0x00, 0x1F);
  i2cWriteReg(a, 0x01, 0x00);
}

static bool i2sBeginStereo(uint32_t rate) {
  gI2s.setPins((int8_t)I2S_SCLK, (int8_t)I2S_LRCK, (int8_t)I2S_DSDIN,
               (int8_t)I2S_ASDOUT, (int8_t)I2S_MCLK);
  return gI2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

// ---- radix-2 iterative Cooley–Tukey (in-place) ----
static void fftRadix2(float* re, float* im, int n) {
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = re[i];
      re[i] = re[j];
      re[j] = tr;
      float ti = im[i];
      im[i] = im[j];
      im[j] = ti;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / (float)len;
    float wlenRe = cosf(ang);
    float wlenIm = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float wRe = 1.0f, wIm = 0.0f;
      for (int j = 0; j < len / 2; ++j) {
        int u = i + j;
        int v = i + j + len / 2;
        float tRe = wRe * re[v] - wIm * im[v];
        float tIm = wRe * im[v] + wIm * re[v];
        re[v] = re[u] - tRe;
        im[v] = im[u] - tIm;
        re[u] += tRe;
        im[u] += tIm;
        float nwRe = wRe * wlenRe - wIm * wlenIm;
        wIm = wRe * wlenIm + wIm * wlenRe;
        wRe = nwRe;
      }
    }
  }
}

bool audioHostReady(void) {
  if (!gProbedMic) {
    gProbedMic = true;
    gEs7210Ok = probeEs7210();
    if (!gEs7210Ok) {
      setErr("es7210_missing");
      Serial.println("audio: ES7210 not found on I2C 0x40");
    } else {
      setErr("");
      Serial.println("audio: ES7210 present (mic path)");
    }
  }
  return gEs7210Ok;
}

bool audioHostSpeakerReady(void) {
  if (!gProbedSpk) {
    gProbedSpk = true;
    gEs8311Ok = probeEs8311();
    if (!gEs8311Ok) {
      Serial.println("audio: ES8311 not found on I2C 0x18");
    } else {
      Serial.println("audio: ES8311 present (speaker path)");
    }
  }
  return gEs8311Ok;
}

bool audioHostSpeakerStop(void) {
  if (!gSpkRunning) {
    paOff();
    return true;
  }
  es8311Shutdown();
  paOff();
  gI2s.end();
  gSpkRunning = false;
  gRate = 0;
  Serial.println("audio: speaker stop");
  return true;
}

bool audioHostMicStop(void) {
  if (!gMicRunning) return true;
  es7210Shutdown();
  gI2s.end();
  gMicRunning = false;
  gRate = 0;
  Serial.println("audio: mic stop");
  return true;
}

bool audioHostMicStart(uint32_t rate, const char** errOut) {
  if (errOut) *errOut = nullptr;
  rate = clampAudioRate(rate);

  if (!audioHostReady()) {
    setErr("es7210_missing");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (gSpkRunning) audioHostSpeakerStop();
  if (gMicRunning && gRate == rate) {
    if (errOut) *errOut = nullptr;
    return true;
  }
  if (gMicRunning) audioHostMicStop();

  paOff();

  if (!i2sBeginStereo(rate)) {
    setErr("i2s_begin_failed");
    if (errOut) *errOut = gErr;
    Serial.println("audio: I2S begin failed");
    return false;
  }
  delay(40);

  if (!es7210Init(rate)) {
    gI2s.end();
    setErr("es7210_init_failed");
    if (errOut) *errOut = gErr;
    Serial.println("audio: ES7210 init failed");
    return false;
  }

  gMicRunning = true;
  gRate = rate;
  setErr("");
  Serial.printf("audio: mic start rate=%u MCLK=%d BCLK=%d WS=%d DIN=%d\n",
                (unsigned)rate, I2S_MCLK, I2S_SCLK, I2S_LRCK, I2S_ASDOUT);
  if (errOut) *errOut = nullptr;
  return true;
}

bool audioHostMicRunning(void) { return gMicRunning; }

uint32_t audioHostMicSampleRate(void) { return gMicRunning ? gRate : 0; }

int audioHostMicBits(void) { return 16; }

int audioHostMicChannels(void) { return 1; }

bool audioHostMicRead(int16_t* out, int maxSamples, int* gotOut) {
  if (gotOut) *gotOut = 0;
  if (!out || maxSamples <= 0) {
    setErr("bad_args");
    return false;
  }
  if (maxSamples > AUDIO_MIC_READ_MAX) maxSamples = AUDIO_MIC_READ_MAX;
  if (!gMicRunning) {
    setErr("mic_not_running");
    return false;
  }

  size_t want = (size_t)maxSamples * 2 * sizeof(int16_t);
  size_t got = gI2s.readBytes((char*)gReadStereo, want);
  int frames = (int)(got / (2 * sizeof(int16_t)));
  if (frames > maxSamples) frames = maxSamples;
  for (int i = 0; i < frames; ++i) {
    out[i] = stereoToMono(gReadStereo[i * 2], gReadStereo[i * 2 + 1]);
  }
  if (gotOut) *gotOut = frames;
  setErr("");
  return true;
}

bool audioHostMicSpectrum(float* outBins, int bins) {
  if (!outBins || bins < 8) {
    setErr("bad_bins");
    return false;
  }
  if (bins > kMaxBins) bins = kMaxBins;
  if (!gMicRunning) {
    setErr("mic_not_running");
    return false;
  }

  size_t want = (size_t)kFftN * 2 * sizeof(int16_t);
  size_t got = gI2s.readBytes((char*)gPcmStereo, want);
  if (got < want / 2) {
    memset(gPcmMono, 0, sizeof(gPcmMono));
  } else {
    int frames = (int)(got / (2 * sizeof(int16_t)));
    if (frames > kFftN) frames = kFftN;
    for (int i = 0; i < frames; ++i) {
      gPcmMono[i] = stereoToMono(gPcmStereo[i * 2], gPcmStereo[i * 2 + 1]);
    }
    for (int i = frames; i < kFftN; ++i) gPcmMono[i] = 0;
  }

  for (int i = 0; i < kFftN; ++i) {
    float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(kFftN - 1)));
    gRe[i] = ((float)gPcmMono[i] / 32768.0f) * w;
    gIm[i] = 0.0f;
  }
  fftRadix2(gRe, gIm, kFftN);

  const int half = kFftN / 2;
  float maxM = 1e-8f;
  for (int i = 0; i < half; ++i) {
    float m = sqrtf(gRe[i] * gRe[i] + gIm[i] * gIm[i]);
    gFftMags[i] = m;
    if (m > maxM) maxM = m;
  }
  for (int b = 0; b < bins; ++b) {
    int i0 = 1 + (b * (half - 1)) / bins;
    int i1 = 1 + ((b + 1) * (half - 1)) / bins;
    if (i1 <= i0) i1 = i0 + 1;
    if (i1 > half) i1 = half;
    float s = 0.0f;
    int n = 0;
    for (int i = i0; i < i1; ++i) {
      s += gFftMags[i];
      n++;
    }
    float v = (n > 0) ? (s / (float)n) / maxM : 0.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    v = sqrtf(v);
    outBins[b] = v;
  }
  setErr("");
  return true;
}

// ---- WAV helpers -----------------------------------------------------------

static void fillWavHeader(uint8_t* h, uint32_t dataBytes, uint32_t rate, uint16_t ch,
                          uint16_t bits) {
  uint32_t byteRate = rate * ch * (bits / 8);
  uint16_t blockAlign = (uint16_t)(ch * (bits / 8));
  uint32_t riffSize = 36 + dataBytes;
  memcpy(h + 0, "RIFF", 4);
  h[4] = (uint8_t)(riffSize);
  h[5] = (uint8_t)(riffSize >> 8);
  h[6] = (uint8_t)(riffSize >> 16);
  h[7] = (uint8_t)(riffSize >> 24);
  memcpy(h + 8, "WAVEfmt ", 8);
  h[16] = 16;
  h[17] = h[18] = h[19] = 0;  // fmt chunk size
  h[20] = 1;
  h[21] = 0;  // PCM
  h[22] = (uint8_t)ch;
  h[23] = 0;
  h[24] = (uint8_t)(rate);
  h[25] = (uint8_t)(rate >> 8);
  h[26] = (uint8_t)(rate >> 16);
  h[27] = (uint8_t)(rate >> 24);
  h[28] = (uint8_t)(byteRate);
  h[29] = (uint8_t)(byteRate >> 8);
  h[30] = (uint8_t)(byteRate >> 16);
  h[31] = (uint8_t)(byteRate >> 24);
  h[32] = (uint8_t)blockAlign;
  h[33] = 0;
  h[34] = (uint8_t)bits;
  h[35] = 0;
  memcpy(h + 36, "data", 4);
  h[40] = (uint8_t)(dataBytes);
  h[41] = (uint8_t)(dataBytes >> 8);
  h[42] = (uint8_t)(dataBytes >> 16);
  h[43] = (uint8_t)(dataBytes >> 24);
}

static bool pathIsWav(const char* path) {
  if (!path) return false;
  size_t n = strlen(path);
  if (n < 4) return false;
  const char* e = path + n - 4;
  return (e[0] == '.' && (e[1] == 'w' || e[1] == 'W') && (e[2] == 'a' || e[2] == 'A') &&
          (e[3] == 'v' || e[3] == 'V'));
}

bool audioHostMicRecordFile(const char* path, uint32_t seconds, uint32_t rate,
                            const char** errOut) {
  if (errOut) *errOut = nullptr;
  if (!path || !path[0]) {
    setErr("bad_path");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (seconds < 1) seconds = 1;
  if (seconds > AUDIO_MIC_RECORD_MAX_SEC) seconds = AUDIO_MIC_RECORD_MAX_SEC;
  rate = clampAudioRate(rate ? rate : kDefaultRate);

  if (sdHostMscLocked()) {
    setErr("sd_busy_usb");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (!sdHostReady() && !sdHostMount()) {
    setErr("sd_not_ready");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (!sdHostPathOk(path)) {
    setErr("bad_path");
    if (errOut) *errOut = gErr;
    return false;
  }

  const char* startErr = nullptr;
  if (!audioHostMicStart(rate, &startErr)) {
    if (errOut) *errOut = startErr ? startErr : audioHostLastError();
    return false;
  }

  fs::File f;
  if (!sdHostOpenWrite(path, &f)) {
    audioHostMicStop();
    setErr(sdHostMscLocked() ? "sd_busy_usb" : "sd_open_failed");
    if (errOut) *errOut = gErr;
    return false;
  }

  uint8_t hdr[44];
  fillWavHeader(hdr, 0, rate, 1, 16);
  if (f.write(hdr, sizeof(hdr)) != sizeof(hdr)) {
    return failRecord(&f, path, "sd_write_failed", errOut);
  }

  const uint32_t totalSamples = seconds * rate;
  uint32_t written = 0;
  uint32_t idleLoops = 0;
  int32_t peakAbs = 0;
  uint32_t pumpCounter = 0;
  while (written < totalSamples) {
    int got = 0;
    if (!audioHostMicRead(gRecordMono, AUDIO_MIC_READ_MAX, &got)) {
      const char* e = audioHostLastError();
      return failRecord(&f, path, (e && e[0]) ? e : "mic_read_failed", errOut);
    }
    if (got <= 0) {
      delay(2);
      audioPumpWatchdog();
      if (++idleLoops > 500) break;  // ~1s of empty reads
      continue;
    }
    idleLoops = 0;
    uint32_t remain = totalSamples - written;
    if ((uint32_t)got > remain) got = (int)remain;
    for (int i = 0; i < got; ++i) {
      int32_t a = gRecordMono[i] < 0 ? -(int32_t)gRecordMono[i] : (int32_t)gRecordMono[i];
      if (a > peakAbs) peakAbs = a;
    }
    size_t nbytes = (size_t)got * sizeof(int16_t);
    if (f.write((const uint8_t*)gRecordMono, nbytes) != nbytes) {
      return failRecord(&f, path, "sd_write_failed", errOut);
    }
    written += (uint32_t)got;
    // Feed TWDT / idle often: multi-second record runs on loopTask under Lua.
    if ((++pumpCounter & 3u) == 0u) {
      audioPumpWatchdog();
    } else {
      yield();
    }
  }

  if (written == 0) {
    return failRecord(&f, path, "mic_no_pcm", errOut);
  }

  // Digital silence / wrong I2S slot / muted path — do not leave a "valid" silent clip.
  if (peakAbs < 64) {
    Serial.printf("audio: near-silence peak=%d path=%s\n", (int)peakAbs, path);
    return failRecord(&f, path, "mic_near_silence", errOut);
  }

  uint32_t dataBytes = written * sizeof(int16_t);
  fillWavHeader(hdr, dataBytes, rate, 1, 16);
  f.seek(0);
  f.write(hdr, sizeof(hdr));
  f.flush();
  f.close();
  audioHostMicStop();

  setErr("");
  Serial.printf("audio: recorded %u samples (%u bytes) peak=%d -> %s\n", (unsigned)written,
                (unsigned)dataBytes, (int)peakAbs, path);
  if (errOut) *errOut = nullptr;
  return true;
}

bool audioHostSpeakerStart(uint32_t rate, const char** errOut) {
  if (errOut) *errOut = nullptr;
  rate = clampAudioRate(rate);

  if (!audioHostSpeakerReady()) {
    setErr("es8311_missing");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (gMicRunning) audioHostMicStop();
  if (gSpkRunning && gRate == rate) {
    (void)es8311ForceAudible();
    (void)paAssertOn();
    if (errOut) *errOut = nullptr;
    return true;
  }
  if (gSpkRunning) audioHostSpeakerStop();

  if (!i2sBeginStereo(rate)) {
    setErr("i2s_begin_failed");
    if (errOut) *errOut = gErr;
    return false;
  }
  delay(40);

  if (!es8311Init(rate)) {
    gI2s.end();
    setErr("es8311_init_failed");
    if (errOut) *errOut = gErr;
    Serial.println("audio: ES8311 init failed");
    return false;
  }

  // Re-apply unmute/volume after clocks settle (guards against stale silent 0x00).
  if (!es8311ForceAudible()) {
    Serial.println("audio: ES8311 force-audible warn (continuing)");
  }

  int paLvl = paAssertOn();
  delay(80);  // NS4150B settle — too short → mute/click on first ms
  paLvl = paAssertOn();  // hold HIGH for entire play; re-sample after settle
  gSpkRunning = true;
  gRate = rate;
  setErr("");
  Serial.printf("audio: speaker start rate=%u DOUT=%d PA_GPIO=%d PA_level=%d\n",
                (unsigned)rate, I2S_DSDIN, PA_CTRL, paLvl);
  if (errOut) *errOut = nullptr;
  return true;
}

bool audioHostSpeakerRunning(void) { return gSpkRunning; }

bool audioHostSpeakerWrite(const void* pcm, size_t bytes) {
  if (!gSpkRunning || !pcm || bytes == 0) {
    setErr("speaker_not_running");
    return false;
  }
  size_t n = gI2s.write((const uint8_t*)pcm, bytes);
  if (n != bytes) {
    setErr("i2s_write_short");
    return false;
  }
  setErr("");
  return true;
}

bool audioHostSpeakerWriteMono(const int16_t* mono, int samples) {
  if (!mono || samples <= 0) {
    setErr("bad_args");
    return false;
  }
  if (!gSpkRunning) {
    setErr("speaker_not_running");
    return false;
  }
  int off = 0;
  while (off < samples) {
    int n = samples - off;
    if (n > (int)(sizeof(gWriteStereo) / (2 * sizeof(int16_t)))) {
      n = (int)(sizeof(gWriteStereo) / (2 * sizeof(int16_t)));
    }
    for (int i = 0; i < n; ++i) {
      gWriteStereo[i * 2] = mono[off + i];
      gWriteStereo[i * 2 + 1] = mono[off + i];
    }
    if (!audioHostSpeakerWrite(gWriteStereo, (size_t)n * 2 * sizeof(int16_t))) return false;
    off += n;
  }
  return true;
}

static uint32_t read_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t read_le16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool audioHostSpeakerPlayFile(const char* path, const char** errOut) {
  if (errOut) *errOut = nullptr;
  if (!path || !path[0]) {
    setErr("bad_path");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (sdHostMscLocked()) {
    setErr("sd_busy_usb");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (!sdHostReady() && !sdHostMount()) {
    setErr("sd_not_ready");
    if (errOut) *errOut = gErr;
    return false;
  }
  if (!sdHostPathOk(path)) {
    setErr("bad_path");
    if (errOut) *errOut = gErr;
    return false;
  }

  fs::File f;
  if (!sdHostOpenRead(path, &f)) {
    setErr(sdHostMscLocked() ? "sd_busy_usb" : "sd_open_failed");
    if (errOut) *errOut = gErr;
    return false;
  }

  uint32_t rate = kDefaultRate;
  uint16_t channels = 1;
  uint16_t bits = 16;
  uint32_t dataBytes = 0;
  bool isWav = pathIsWav(path);

  if (isWav) {
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
      f.close();
      setErr("bad_wav");
      if (errOut) *errOut = gErr;
      return false;
    }
    bool gotFmt = false, gotData = false;
    while (f.available() >= 8) {
      uint8_t ch[8];
      if (f.read(ch, 8) != 8) break;
      uint32_t csz = read_le32(ch + 4);
      if (memcmp(ch, "fmt ", 4) == 0) {
        uint8_t fmt[16];
        size_t need = csz < 16 ? csz : 16;
        if (f.read(fmt, need) != (int)need) break;
        if (csz > need) f.seek(f.position() + (csz - need));
        // PCM only
        if (read_le16(fmt + 0) != 1) {
          f.close();
          setErr("wav_not_pcm");
          if (errOut) *errOut = gErr;
          return false;
        }
        channels = read_le16(fmt + 2);
        rate = read_le32(fmt + 4);
        bits = read_le16(fmt + 14);
        gotFmt = true;
      } else if (memcmp(ch, "data", 4) == 0) {
        dataBytes = csz;
        gotData = true;
        break;  // file position at start of PCM
      } else {
        f.seek(f.position() + csz + (csz & 1));
      }
    }
    if (!gotFmt || !gotData || bits != 16 || (channels != 1 && channels != 2)) {
      f.close();
      setErr("wav_unsupported");
      if (errOut) *errOut = gErr;
      return false;
    }
  } else {
    // Raw mono int16
    dataBytes = (uint32_t)f.size();
    channels = 1;
    bits = 16;
    rate = kDefaultRate;
  }

  if (dataBytes < (uint32_t)channels * sizeof(int16_t)) {
    f.close();
    setErr("wav_empty");
    if (errOut) *errOut = gErr;
    return false;
  }

  rate = clampAudioRate(rate);
  const char* startErr = nullptr;
  if (!audioHostSpeakerStart(rate, &startErr)) {
    f.close();
    if (errOut) *errOut = startErr ? startErr : audioHostLastError();
    return false;
  }

  uint32_t left = dataBytes;
  bool ok = true;
  uint32_t pumpCounter = 0;
  int32_t peakAbs = 0;
  uint32_t pcmFrames = 0;
  Serial.printf("audio: speaker play begin path=%s rate=%u ch=%u bytes=%u PA=%d\n", path,
                (unsigned)rate, (unsigned)channels, (unsigned)dataBytes, paAssertOn());

  while (left > 0 && f.available()) {
    size_t chunk = left > sizeof(gPlayBuf) ? sizeof(gPlayBuf) : left;
    // Align to frame
    size_t frameBytes = (size_t)channels * sizeof(int16_t);
    chunk = (chunk / frameBytes) * frameBytes;
    if (chunk == 0) break;
    int n = f.read(gPlayBuf, chunk);
    if (n <= 0) break;
    n = (int)(((size_t)n / frameBytes) * frameBytes);
    if (n <= 0) break;

    // Peak of WAV PCM (int16 LE) for silence diagnostics
    {
      const int16_t* s = (const int16_t*)gPlayBuf;
      int ns = n / (int)sizeof(int16_t);
      for (int i = 0; i < ns; ++i) {
        int32_t a = s[i] < 0 ? -(int32_t)s[i] : (int32_t)s[i];
        if (a > peakAbs) peakAbs = a;
      }
      pcmFrames += (uint32_t)(ns / (int)channels);
    }

    // Keep PA HIGH for the entire play (NS4150B enable)
    if ((pumpCounter & 7u) == 0u) {
      (void)paAssertOn();
    }

    if (channels == 2) {
      if (!audioHostSpeakerWrite(gPlayBuf, (size_t)n)) {
        ok = false;
        break;
      }
    } else {
      int samples = n / (int)sizeof(int16_t);
      if (!audioHostSpeakerWriteMono((const int16_t*)gPlayBuf, samples)) {
        ok = false;
        break;
      }
    }
    left -= (uint32_t)n;
    if ((++pumpCounter & 3u) == 0u) {
      audioPumpWatchdog();
    } else {
      yield();
    }
  }

  f.close();

  if (pcmFrames == 0 || peakAbs < 8) {
    Serial.printf("audio: play near-empty/silent peak=%d frames=%u path=%s\n", (int)peakAbs,
                  (unsigned)pcmFrames, path);
    // Drain + stop before failing so PA/I2S are released
    memset(gPlayBuf, 0, sizeof(gPlayBuf));
    (void)paAssertOn();
    audioHostSpeakerWrite(gPlayBuf, 512);
    audioHostSpeakerStop();
    setErr(pcmFrames == 0 ? "wav_empty" : "wav_near_silence");
    if (errOut) *errOut = gErr;
    return false;
  }

  // Drain a little silence so last samples leave the codec; hold PA until stop
  memset(gPlayBuf, 0, sizeof(gPlayBuf));
  (void)paAssertOn();
  audioHostSpeakerWrite(gPlayBuf, 512);
  delay(20);  // let last samples leave DAC before PA drops
  audioHostSpeakerStop();

  if (!ok) {
    if (errOut) *errOut = audioHostLastError();
    return false;
  }
  setErr("");
  if (errOut) *errOut = nullptr;
  Serial.printf("audio: played %s peak=%d frames=%u\n", path, (int)peakAbs, (unsigned)pcmFrames);
  return true;
}

int audioHostSpeakerVolumeGet(void) {
  return rawToPct(gSpeakerVolRaw);
}

bool audioHostSpeakerVolumeSet(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  uint8_t raw = pctToRaw(percent);
  if (!es8311ApplyVolume(raw)) {
    setErr("es8311_vol_failed");
    return false;
  }
  setErr("");
  Serial.printf("audio: speaker volume %d%% (reg32=0x%02X)\n", percent, (unsigned)raw);
  return true;
}

uint8_t audioHostSpeakerVolumeRawGet(void) {
  return clampVolRaw(gSpeakerVolRaw);
}

bool audioHostSpeakerVolumeRawSet(uint8_t raw) {
  raw = clampVolRaw(raw);
  if (!es8311ApplyVolume(raw)) {
    setErr("es8311_vol_failed");
    return false;
  }
  setErr("");
  Serial.printf("audio: speaker volume raw 0x%02X (%d%%)\n", (unsigned)raw, rawToPct(raw));
  return true;
}

static bool writeToneMs(uint32_t rate, uint32_t freq_hz, uint32_t duration_ms,
                        float amp) {
  if (freq_hz < 200) freq_hz = 200;
  if (freq_hz > 4000) freq_hz = 4000;
  if (duration_ms == 0) return true;
  const int total = (int)((rate * duration_ms) / 1000u);
  const float twoPiF = 6.2831853f * (float)freq_hz / (float)rate;
  const int chunk = 256;
  int16_t mono[256];
  int done = 0;
  while (done < total) {
    int n = total - done;
    if (n > chunk) n = chunk;
    for (int i = 0; i < n; ++i) {
      float s = sinf(twoPiF * (float)(done + i));
      float v = s * amp;
      if (v > 32767.0f) v = 32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      mono[i] = (int16_t)v;
    }
    if (!audioHostSpeakerWriteMono(mono, n)) return false;
    done += n;
  }
  return true;
}

static bool writeSilenceMs(uint32_t rate, uint32_t duration_ms) {
  if (duration_ms == 0) return true;
  const int total = (int)((rate * duration_ms) / 1000u);
  const int chunk = 256;
  int16_t mono[256];
  memset(mono, 0, sizeof(mono));
  int done = 0;
  while (done < total) {
    int n = total - done;
    if (n > chunk) n = chunk;
    if (!audioHostSpeakerWriteMono(mono, n)) return false;
    done += n;
  }
  return true;
}

bool audioHostBeep(uint32_t duration_ms, uint32_t freq_hz) {
  if (duration_ms < 20) duration_ms = 20;
  if (duration_ms > 400) duration_ms = 400;
  if (freq_hz == 0) freq_hz = 880;

  const uint32_t rate = 16000;
  const char* err = nullptr;
  const int prevVol = audioHostSpeakerVolumeGet();
  (void)audioHostSpeakerVolumeSet(100);
  if (!audioHostSpeakerStart(rate, &err)) {
    (void)audioHostSpeakerVolumeSet(prevVol);
    Serial.printf("audio: beep start fail (%s)\n", err ? err : audioHostLastError());
    return false;
  }

  // Louder than the old ~12000 peak — tiny watch speaker needs headroom.
  if (!writeToneMs(rate, freq_hz, duration_ms, 28000.0f)) {
    audioHostSpeakerStop();
    (void)audioHostSpeakerVolumeSet(prevVol);
    return false;
  }
  (void)writeSilenceMs(rate, 8);
  audioHostSpeakerStop();
  (void)audioHostSpeakerVolumeSet(prevVol);
  setErr("");
  return true;
}

bool audioHostAlertUhOh(void) {
  // Classic ICQ-ish "uh-oh": short rise then falling second tone.
  // Motif: 660Hz → 990Hz (asc) → 520Hz (desc "oh"), full volume.
  const uint32_t rate = 16000;
  const char* err = nullptr;
  const int prevVol = audioHostSpeakerVolumeGet();
  (void)audioHostSpeakerVolumeSet(100);
  if (!audioHostSpeakerStart(rate, &err)) {
    (void)audioHostSpeakerVolumeSet(prevVol);
    Serial.printf("audio: uh-oh start fail (%s)\n", err ? err : audioHostLastError());
    return false;
  }

  const float amp = 30000.0f;  // near-full scale; ES8311 DAC already at 0 dB
  bool ok = true;
  ok = ok && writeToneMs(rate, 660, 85, amp);
  ok = ok && writeSilenceMs(rate, 35);
  ok = ok && writeToneMs(rate, 990, 95, amp);   // ascending peak
  ok = ok && writeSilenceMs(rate, 45);
  ok = ok && writeToneMs(rate, 520, 180, amp);  // descending "oh"
  ok = ok && writeSilenceMs(rate, 20);

  audioHostSpeakerStop();
  (void)audioHostSpeakerVolumeSet(prevVol);
  if (!ok) {
    Serial.println("audio: uh-oh write fail");
    return false;
  }
  setErr("");
  Serial.println("audio: uh-oh alert played");
  return true;
}

const char* audioHostLastError(void) { return gErr; }
