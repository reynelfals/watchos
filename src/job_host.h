#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Named background jobs on the non-UI FreeRTOS core.
 * ESP32-S3: Arduino/LVGL loop runs on core 1; worker is pinned to core 0.
 * Core IDs are NOT exposed to Lua — only job names + opaque ids.
 *
 * v1 names:
 *   "fft" / "mic_fft" — async mic spectrum (float magnitudes)
 *   "rms"             — RMS of a short mic PCM window
 */

#ifndef JOB_HOST_MAX_JOBS
#define JOB_HOST_MAX_JOBS 4
#endif
#ifndef JOB_HOST_QUEUE_DEPTH
#define JOB_HOST_QUEUE_DEPTH 2
#endif

void jobHostBegin(void);
void jobHostReset(void);

/** Enqueue job; returns id >= 1, or 0 on failure (errBuf set). */
uint32_t jobHostStart(const char* name, int optsBins, char* errBuf, size_t errLen);

/** "queued"|"running"|"done"|"error"|"cancelled"|"unknown" */
const char* jobHostStatus(uint32_t id);

bool jobHostTakeSpectrum(uint32_t id, float* out, int maxBins, int* nOut,
                         char* errBuf, size_t errLen);
bool jobHostTakeRms(uint32_t id, float* outRms, char* errBuf, size_t errLen);
bool jobHostCancel(uint32_t id);

/** "fft"|"rms"|nullptr if unknown. */
const char* jobHostKind(uint32_t id);

/** For Serial/docs only. */
int jobHostWorkerCore(void);

#ifdef __cplusplus
}
#endif
