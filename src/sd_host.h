#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#include <FS.h>
extern "C" {
#endif

/** Lazy microSD mount via SD_MMC 1-bit. Never call at boot. */
bool sdHostMount(void);
bool sdHostUnmount(void);
bool sdHostReady(void);

/** While true, file APIs fail — MSC owns the card (raw sector I/O only). */
void sdHostLockForMsc(bool locked);
bool sdHostMscLocked(void);

/** Card size helpers (bytes). Return 0 if not mounted. */
uint64_t sdHostTotalBytes(void);
uint64_t sdHostUsedBytes(void);

/** Path sandbox: must be under mount point "/sd", no ".." segments. */
bool sdHostPathOk(const char* path);

/**
 * List directory entries. Writes up to maxEntries into names/sizes/isDir.
 * Returns entry count, or -1 on error. Names are basenames only (truncated).
 */
int sdHostList(const char* path, char names[][64], uint32_t* sizes, bool* isDir,
               int maxEntries);

/**
 * Read up to maxBytes into buf (not necessarily NUL-terminated).
 * Returns bytes read, or -1 on error.
 */
int sdHostRead(const char* path, char* buf, size_t maxBytes);

/** Write data; creates parent dir one level deep if needed. */
bool sdHostWrite(const char* path, const char* data, size_t len);

/** True if mounted and path exists (file or dir) under /sd. */
bool sdHostExists(const char* path);

/** Create a directory under /sd (path must pass sdHostPathOk). Idempotent. */
bool sdHostMkdir(const char* path);

/**
 * Binary-safe write of raw bytes to SD (creates parent dir one level if needed).
 * Equivalent to sdHostWrite but takes void* for clarity at call sites.
 */
bool sdHostWriteBytes(const char* path, const void* data, size_t len);

/**
 * Recursively delete a file or directory under /sd/ (public path).
 * Refuses "/", "/sd", "/sd/", path escape, and MSC-locked card.
 * Returns true if gone (or already missing).
 */
bool sdHostRemoveTree(const char* path);

/** Rename/move within /sd (both paths must pass sdHostPathOk). */
bool sdHostRename(const char* fromPath, const char* toPath);

/** Delete a single file under /sd (not recursive). Prefer sdHostRemoveTree for dirs. */
bool sdHostRemove(const char* path);

#ifdef __cplusplus
}

/**
 * Open path under /sd for truncate-write. Creates parent dirs.
 * On success fills *out with an open File; caller must close it.
 * Returns false if not mounted / bad path / open failed (*out unchanged).
 */
bool sdHostOpenWrite(const char* path, fs::File* out);

/**
 * Open path under /sd for read. On success fills *out; caller must close.
 * Returns false if not mounted / bad path / open failed.
 */
bool sdHostOpenRead(const char* path, fs::File* out);
#endif
