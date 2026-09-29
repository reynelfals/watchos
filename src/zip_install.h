#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max total uncompressed bytes extracted from one zip (soft cap). */
#ifndef WATCHOS_ZIP_MAX_UNCOMPRESSED
#define WATCHOS_ZIP_MAX_UNCOMPRESSED (512u * 1024u)
#endif

/** Max single file uncompressed. */
#ifndef WATCHOS_ZIP_MAX_FILE
#define WATCHOS_ZIP_MAX_FILE (256u * 1024u)
#endif

/**
 * Unpack a zip already on-device into an app folder.
 * dest: "lfs" → /apps/<id>/ ; "sd" → /sd/apps/<id>/
 * Zip methods: 0 (store) and 8 (deflate via ROM tinfl). Paths are sanitized.
 * Returns true on success. errBuf gets a short reason on failure.
 */
bool zipInstallFromPath(const char* zipPath, const char* appId, const char* dest,
                        char* errBuf, size_t errLen);

/**
 * Unpack zip bytes from memory (SoftAP upload buffer) into app folder.
 * Same dest / caps as zipInstallFromPath.
 */
bool zipInstallFromMemory(const uint8_t* data, size_t len, const char* appId,
                          const char* dest, char* errBuf, size_t errLen);

#ifdef __cplusplus
}
#endif
