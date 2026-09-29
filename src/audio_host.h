#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mic / speaker host for Waveshare ESP32-S3-Touch-AMOLED-2.06.
 * Capture: ES7210 (I2C 0x40) + I2S DIN=GPIO42.
 * Playback: ES8311 (I2C 0x18) + I2S DOUT=GPIO40, PA=GPIO46.
 * Shared I2S clocks (MCLK/BCLK/WS); mic and speaker are mutually exclusive.
 */

/** Lazy-probe ES7210 on shared I2C. True if ACK'd at least once. */
bool audioHostReady(void);

/** Lazy-probe ES8311. True if ACK'd at least once. */
bool audioHostSpeakerReady(void);

/**
 * Start I2S + ES7210 capture at sample rate (8000..48000; default 16000).
 * Returns true on success. On failure leaves mic stopped and sets *errOut
 * (optional, may be NULL) to a short static reason string.
 */
bool audioHostMicStart(uint32_t rate, const char** errOut);

/** Stop capture; safe if already stopped. */
bool audioHostMicStop(void);

/** True while mic capture is running. */
bool audioHostMicRunning(void);

/**
 * Read recent PCM, run native real FFT, fold into outBins magnitudes in [0,1].
 * bins clamped to 8..128. Returns false if mic not running or read failed.
 * Shares the same I2S capture stream as audioHostMicRead (competing consumers).
 */
bool audioHostMicSpectrum(float* outBins, int bins);

/** Hard cap for a single micRead call (mono samples). */
enum { AUDIO_MIC_READ_MAX = 1024 };

/** Hard cap for mic_record_file duration (seconds). */
enum { AUDIO_MIC_RECORD_MAX_SEC = 20 };

/**
 * Read up to maxSamples new mono int16 PCM samples from the capture stream.
 * Caps at AUDIO_MIC_READ_MAX. On success writes *gotOut samples (may be 0 if
 * the I2S buffer was empty). Returns false if mic not running or bad args.
 * Format: signed 16-bit mono (louder of L/R stereo I2S slots), little-endian host.
 */
bool audioHostMicRead(int16_t* out, int maxSamples, int* gotOut);

/** Current capture rate while running; 0 if stopped. */
uint32_t audioHostMicSampleRate(void);

/** Bits per sample exposed to apps (always 16). */
int audioHostMicBits(void);

/** Channel count exposed to apps (always 1 = mono). */
int audioHostMicChannels(void);

/**
 * Record mono int16 WAV to an SD path under /sd (e.g. /sd/recordings/a.wav).
 * Blocks for up to `seconds` (1..AUDIO_MIC_RECORD_MAX_SEC). Creates parents.
 * Rate defaults to 16000. Stops any active speaker first.
 * On failure removes the incomplete file. Errors include sd_not_ready,
 * sd_busy_usb, mic_no_pcm, mic_near_silence, es7210_*, sd_*.
 */
bool audioHostMicRecordFile(const char* path, uint32_t seconds, uint32_t rate,
                            const char** errOut);

/**
 * Play a PCM WAV (or raw mono int16 .pcm/.raw) from SD through ES8311 + PA.
 * Supports mono or stereo int16 LE (mono is upmixed L=R). Blocks until done.
 * Applies current session speaker volume (default 0 dB / reg32=0xBF) and
 * PA_CTRL HIGH for the whole clip. Rejects empty / near-silent PCM. Stops mic
 * first. Path under /sd.
 */
bool audioHostSpeakerPlayFile(const char* path, const char** errOut);

/**
 * Streaming speaker: start ES8311 + I2S TX at rate (8/16/48k).
 * PCM via audioHostSpeakerWrite must be stereo int16 LE frames, or use
 * audioHostSpeakerWriteMono which duplicates mono to L/R.
 */
bool audioHostSpeakerStart(uint32_t rate, const char** errOut);
bool audioHostSpeakerWrite(const void* pcm, size_t bytes);
bool audioHostSpeakerWriteMono(const int16_t* mono, int samples);
bool audioHostSpeakerStop(void);
bool audioHostSpeakerRunning(void);

/**
 * Speaker volume — Lua scale 0..100% mapped linearly onto ES8311 DAC reg 0x32
 * (0x00=-95.5dB … 0xBF=0dB). Max clamped at 0xBF (no boost). Session runtime
 * only; not persisted. Applied on every speaker start / play.
 */
int audioHostSpeakerVolumeGet(void);
bool audioHostSpeakerVolumeSet(int percent);
/** Raw reg 0x32 value (0..0xBF). For labs; same session state as percent API. */
uint8_t audioHostSpeakerVolumeRawGet(void);
bool audioHostSpeakerVolumeRawSet(uint8_t raw);

/**
 * Short blocking alert tone via ES8311 (sine). duration_ms clamped 20..400;
 * freq_hz clamped 200..4000 (default 880 if 0). No-op / false if speaker
 * unavailable. Safe to call from main loop only (not from BLE callbacks).
 */
bool audioHostBeep(uint32_t duration_ms, uint32_t freq_hz);

/**
 * ICQ-style "uh-oh" alert: short ascending then descending two-tone motif
 * at full session volume via ES8311. Blocking; main-loop only (not BLE cb).
 */
bool audioHostAlertUhOh(void);

/** Last error string from start/spectrum/read/play/record (may be empty). */
const char* audioHostLastError(void);

#ifdef __cplusplus
}
#endif
