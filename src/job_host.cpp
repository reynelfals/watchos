#include "job_host.h"
#include "audio_host.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

// Arduino / LVGL UI loop runs on core 1. Worker is pinned to core 0.
static constexpr BaseType_t kWorkerCore = 0;
static constexpr uint32_t kStackWords = 6144;
static constexpr UBaseType_t kPrio = 1;

enum Kind : uint8_t { KindNone = 0, KindFft = 1, KindRms = 2 };
enum State : uint8_t {
  StIdle = 0,
  StQueued,
  StRunning,
  StDone,
  StError,
  StCancelled
};

struct Slot {
  uint32_t id;
  Kind kind;
  State state;
  int bins;
  float spectrum[128];
  int spectrumN;
  float rms;
  char err[48];
  bool cancel;
};

struct Msg {
  uint32_t id;
};

static Slot gSlots[JOB_HOST_MAX_JOBS];
static SemaphoreHandle_t gMu = nullptr;
static QueueHandle_t gQ = nullptr;
static TaskHandle_t gTask = nullptr;
static uint32_t gNextId = 1;
static bool gBegun = false;

static void setErr(char* buf, size_t len, const char* msg) {
  if (!buf || len == 0) return;
  strncpy(buf, msg ? msg : "error", len - 1);
  buf[len - 1] = '\0';
}

static Slot* findLocked(uint32_t id) {
  if (!id) return nullptr;
  for (int i = 0; i < JOB_HOST_MAX_JOBS; ++i) {
    if (gSlots[i].id == id && gSlots[i].state != StIdle) return &gSlots[i];
  }
  return nullptr;
}

static Slot* allocLocked() {
  for (int i = 0; i < JOB_HOST_MAX_JOBS; ++i) {
    if (gSlots[i].state == StIdle) return &gSlots[i];
  }
  for (int i = 0; i < JOB_HOST_MAX_JOBS; ++i) {
    if (gSlots[i].state == StDone || gSlots[i].state == StError ||
        gSlots[i].state == StCancelled) {
      memset(&gSlots[i], 0, sizeof(gSlots[i]));
      return &gSlots[i];
    }
  }
  return nullptr;
}

static State runFft(int bins, float* outSpec, int* outN, char* err, size_t errLen,
                    volatile bool* cancelFlag) {
  if (bins < 8) bins = 8;
  if (bins > 128) bins = 128;
  if (!audioHostMicRunning()) {
    const char* e = nullptr;
    if (!audioHostMicStart(16000, &e)) {
      setErr(err, errLen, e ? e : "mic_start_failed");
      return StError;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  if (cancelFlag && *cancelFlag) return StCancelled;
  if (!audioHostMicSpectrum(outSpec, bins)) {
    const char* e = audioHostLastError();
    setErr(err, errLen, (e && e[0]) ? e : "spectrum_failed");
    return StError;
  }
  *outN = bins;
  return StDone;
}

static State runRms(float* outRms, char* err, size_t errLen, volatile bool* cancelFlag) {
  if (!audioHostMicRunning()) {
    const char* e = nullptr;
    if (!audioHostMicStart(16000, &e)) {
      setErr(err, errLen, e ? e : "mic_start_failed");
      return StError;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  if (cancelFlag && *cancelFlag) return StCancelled;
  int16_t pcm[512];
  int got = 0;
  if (!audioHostMicRead(pcm, 512, &got) || got < 1) {
    const char* e = audioHostLastError();
    setErr(err, errLen, (e && e[0]) ? e : "mic_read_failed");
    return StError;
  }
  double acc = 0.0;
  for (int i = 0; i < got; ++i) {
    double v = (double)pcm[i];
    acc += v * v;
  }
  *outRms = (float)sqrt(acc / (double)got);
  return StDone;
}


static bool slotCancelled(uint32_t id) {
  bool c = false;
  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(5)) == pdTRUE) {
    Slot* p = findLocked(id);
    c = p && p->cancel;
    xSemaphoreGive(gMu);
  }
  return c;
}

static void worker(void* /*arg*/) {
  Msg msg;
  for (;;) {
    if (xQueueReceive(gQ, &msg, portMAX_DELAY) != pdTRUE) continue;

    Kind kind = KindNone;
    int bins = 64;
    if (xSemaphoreTake(gMu, portMAX_DELAY) != pdTRUE) continue;
    Slot* s = findLocked(msg.id);
    if (!s || s->state != StQueued) {
      xSemaphoreGive(gMu);
      continue;
    }
    if (s->cancel) {
      s->state = StCancelled;
      xSemaphoreGive(gMu);
      continue;
    }
    s->state = StRunning;
    kind = s->kind;
    bins = s->bins;
    xSemaphoreGive(gMu);

    float spectrum[128];
    int spectrumN = 0;
    float rms = 0.f;
    char err[48] = {0};
    State result = StDone;

    if (kind == KindFft) {
      if (slotCancelled(msg.id)) {
        result = StCancelled;
      } else {
        result = runFft(bins, spectrum, &spectrumN, err, sizeof(err), nullptr);
        if (slotCancelled(msg.id)) result = StCancelled;
      }
    } else if (kind == KindRms) {
      if (slotCancelled(msg.id)) {
        result = StCancelled;
      } else {
        result = runRms(&rms, err, sizeof(err), nullptr);
        if (slotCancelled(msg.id)) result = StCancelled;
      }
    } else {
      result = StError;
      setErr(err, sizeof(err), "bad_kind");
    }

    if (xSemaphoreTake(gMu, portMAX_DELAY) != pdTRUE) continue;
    s = findLocked(msg.id);
    if (!s) {
      xSemaphoreGive(gMu);
      continue;
    }
    if (s->cancel || result == StCancelled) {
      s->state = StCancelled;
    } else {
      s->state = result;
      if (result == StDone && kind == KindFft) {
        s->spectrumN = spectrumN;
        if (spectrumN > 0) {
          memcpy(s->spectrum, spectrum, (size_t)spectrumN * sizeof(float));
        }
      } else if (result == StDone && kind == KindRms) {
        s->rms = rms;
      }
      if (err[0]) strncpy(s->err, err, sizeof(s->err) - 1);
    }
    xSemaphoreGive(gMu);
  }
}

void jobHostBegin(void) {
  if (gBegun) return;
  memset(gSlots, 0, sizeof(gSlots));
  gMu = xSemaphoreCreateMutex();
  gQ = xQueueCreate(JOB_HOST_QUEUE_DEPTH, sizeof(Msg));
  if (!gMu || !gQ) {
    Serial.println("jobHost: alloc failed");
    return;
  }
  BaseType_t ok =
      xTaskCreatePinnedToCore(worker, "jobHost", kStackWords, nullptr, kPrio, &gTask,
                              kWorkerCore);
  if (ok != pdPASS) {
    Serial.println("jobHost: task create failed");
    return;
  }
  gBegun = true;
  Serial.printf("jobHost: worker on core %d queue=%d\n", (int)kWorkerCore,
                JOB_HOST_QUEUE_DEPTH);
}

int jobHostWorkerCore(void) { return (int)kWorkerCore; }

void jobHostReset(void) {
  if (!gBegun) return;
  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(200)) != pdTRUE) return;
  for (int i = 0; i < JOB_HOST_MAX_JOBS; ++i) {
    if (gSlots[i].state == StQueued || gSlots[i].state == StRunning) {
      gSlots[i].cancel = true;
      gSlots[i].state = StCancelled;
    } else {
      memset(&gSlots[i], 0, sizeof(gSlots[i]));
    }
  }
  Msg dump;
  while (xQueueReceive(gQ, &dump, 0) == pdTRUE) {
  }
  xSemaphoreGive(gMu);
}

uint32_t jobHostStart(const char* name, int optsBins, char* errBuf, size_t errLen) {
  if (errBuf && errLen) errBuf[0] = '\0';
  if (!gBegun) jobHostBegin();
  if (!gBegun || !name || !name[0]) {
    setErr(errBuf, errLen, "job_host_not_ready");
    return 0;
  }

  Kind kind = KindNone;
  if (strcmp(name, "fft") == 0 || strcmp(name, "mic_fft") == 0) {
    kind = KindFft;
  } else if (strcmp(name, "rms") == 0) {
    kind = KindRms;
  } else {
    setErr(errBuf, errLen, "unknown_job");
    return 0;
  }

  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(100)) != pdTRUE) {
    setErr(errBuf, errLen, "busy");
    return 0;
  }
  Slot* s = allocLocked();
  if (!s) {
    xSemaphoreGive(gMu);
    setErr(errBuf, errLen, "no_slot");
    return 0;
  }
  uint32_t id = gNextId++;
  if (id == 0) id = gNextId++;
  memset(s, 0, sizeof(*s));
  s->id = id;
  s->kind = kind;
  s->state = StQueued;
  s->bins = (optsBins > 0) ? optsBins : 64;
  s->cancel = false;

  Msg msg = {id};
  if (xQueueSend(gQ, &msg, 0) != pdTRUE) {
    memset(s, 0, sizeof(*s));
    xSemaphoreGive(gMu);
    setErr(errBuf, errLen, "queue_full");
    return 0;
  }
  xSemaphoreGive(gMu);
  return id;
}

const char* jobHostStatus(uint32_t id) {
  if (!gBegun || id == 0) return "unknown";
  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(50)) != pdTRUE) return "unknown";
  Slot* s = findLocked(id);
  if (!s) {
    xSemaphoreGive(gMu);
    return "unknown";
  }
  const char* st = "unknown";
  switch (s->state) {
    case StQueued: st = "queued"; break;
    case StRunning: st = "running"; break;
    case StDone: st = "done"; break;
    case StError: st = "error"; break;
    case StCancelled: st = "cancelled"; break;
    default: break;
  }
  xSemaphoreGive(gMu);
  return st;
}

bool jobHostTakeSpectrum(uint32_t id, float* out, int maxBins, int* nOut, char* errBuf,
                         size_t errLen) {
  if (nOut) *nOut = 0;
  if (!out || maxBins < 1) {
    setErr(errBuf, errLen, "bad_args");
    return false;
  }
  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(50)) != pdTRUE) {
    setErr(errBuf, errLen, "busy");
    return false;
  }
  Slot* s = findLocked(id);
  if (!s) {
    xSemaphoreGive(gMu);
    setErr(errBuf, errLen, "unknown_id");
    return false;
  }
  if (s->kind != KindFft) {
    xSemaphoreGive(gMu);
    setErr(errBuf, errLen, "not_fft");
    return false;
  }
  if (s->state == StError) {
    setErr(errBuf, errLen, s->err[0] ? s->err : "error");
    memset(s, 0, sizeof(*s));
    xSemaphoreGive(gMu);
    return false;
  }
  if (s->state != StDone) {
    setErr(errBuf, errLen, s->state == StCancelled ? "cancelled" : "not_ready");
    xSemaphoreGive(gMu);
    return false;
  }
  int n = s->spectrumN;
  if (n > maxBins) n = maxBins;
  memcpy(out, s->spectrum, (size_t)n * sizeof(float));
  if (nOut) *nOut = n;
  memset(s, 0, sizeof(*s));
  xSemaphoreGive(gMu);
  return true;
}

bool jobHostTakeRms(uint32_t id, float* outRms, char* errBuf, size_t errLen) {
  if (!outRms) {
    setErr(errBuf, errLen, "bad_args");
    return false;
  }
  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(50)) != pdTRUE) {
    setErr(errBuf, errLen, "busy");
    return false;
  }
  Slot* s = findLocked(id);
  if (!s) {
    xSemaphoreGive(gMu);
    setErr(errBuf, errLen, "unknown_id");
    return false;
  }
  if (s->kind != KindRms) {
    xSemaphoreGive(gMu);
    setErr(errBuf, errLen, "not_rms");
    return false;
  }
  if (s->state == StError) {
    setErr(errBuf, errLen, s->err[0] ? s->err : "error");
    memset(s, 0, sizeof(*s));
    xSemaphoreGive(gMu);
    return false;
  }
  if (s->state != StDone) {
    setErr(errBuf, errLen, s->state == StCancelled ? "cancelled" : "not_ready");
    xSemaphoreGive(gMu);
    return false;
  }
  *outRms = s->rms;
  memset(s, 0, sizeof(*s));
  xSemaphoreGive(gMu);
  return true;
}

bool jobHostCancel(uint32_t id) {
  if (!gBegun || id == 0) return false;
  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  Slot* s = findLocked(id);
  if (!s) {
    xSemaphoreGive(gMu);
    return false;
  }
  s->cancel = true;
  if (s->state == StQueued || s->state == StRunning) s->state = StCancelled;
  xSemaphoreGive(gMu);
  return true;
}

const char* jobHostKind(uint32_t id) {
  if (!gBegun || id == 0) return nullptr;
  if (xSemaphoreTake(gMu, pdMS_TO_TICKS(50)) != pdTRUE) return nullptr;
  Slot* s = findLocked(id);
  if (!s) {
    xSemaphoreGive(gMu);
    return nullptr;
  }
  const char* k = nullptr;
  if (s->kind == KindFft) k = "fft";
  else if (s->kind == KindRms) k = "rms";
  xSemaphoreGive(gMu);
  return k;
}
