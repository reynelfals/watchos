#include "sd_host.h"
#include "pin_config.h"

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <string.h>

static constexpr const char* kMountPoint = "/sd";
static bool gMounted = false;
static bool gMscLocked = false;

void sdHostLockForMsc(bool locked) { gMscLocked = locked; }
bool sdHostMscLocked(void) { return gMscLocked; }

bool sdHostPathOk(const char* path) {
  if (!path || path[0] != '/') return false;
  // Exactly "/sd" or prefix "/sd/"
  if (strcmp(path, kMountPoint) == 0) return true;
  if (strncmp(path, "/sd/", 4) != 0) return false;
  const char* p = path;
  while ((p = strstr(p, "..")) != nullptr) {
    if ((p == path || p[-1] == '/') && (p[2] == '\0' || p[2] == '/')) {
      return false;
    }
    p += 2;
  }
  return true;
}

/**
 * Lua / public paths are under "/sd/...".
 * SD_MMC VFS open() prepends the mountpoint, so the FS-relative path must be
 * "/" for the card root and "/file" for files (NOT "/sd/file").
 * Returns false if path is not under /sd.
 */
static bool toFsPath(const char* publicPath, char* out, size_t outSz) {
  if (!out || outSz < 2 || !sdHostPathOk(publicPath)) return false;
  if (strcmp(publicPath, kMountPoint) == 0) {
    strncpy(out, "/", outSz - 1);
    out[outSz - 1] = '\0';
    return true;
  }
  // publicPath starts with "/sd/"
  const char* rest = publicPath + 3;  // keeps leading '/' of "/sd/..."
  if (rest[0] != '/') return false;
  strncpy(out, rest, outSz - 1);
  out[outSz - 1] = '\0';
  return true;
}

bool sdHostReady(void) { return gMounted; }

bool sdHostMount(void) {
  if (gMounted) return true;

  // Waveshare 07_LVGL_SD_Test: CLK=2 CMD=1 D0=3, 1-bit mode.
  if (!SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA)) {
    Serial.println("sd: setPins failed");
    return false;
  }
  // mode1bit=true; do not format on failure (soft fail).
  if (!SD_MMC.begin(kMountPoint, true, false)) {
    Serial.println("sd: mount failed (no card or bad FS)");
    gMounted = false;
    return false;
  }
  gMounted = true;
  Serial.printf("sd: mounted %s  type=%d  size=%llu MB\n", kMountPoint,
                (int)SD_MMC.cardType(),
                (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

bool sdHostUnmount(void) {
  if (gMscLocked) {
    Serial.println("sd: unmount blocked (MSC active)");
    return false;
  }
  if (!gMounted) return true;
  SD_MMC.end();
  gMounted = false;
  Serial.println("sd: unmounted");
  return true;
}

uint64_t sdHostTotalBytes(void) {
  if (!gMounted) return 0;
  return SD_MMC.totalBytes();
}

uint64_t sdHostUsedBytes(void) {
  if (!gMounted) return 0;
  return SD_MMC.usedBytes();
}

static void ensureParentDirFs(const char* fsPath) {
  if (!fsPath) return;
  char parent[160];
  strncpy(parent, fsPath, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char* slash = strrchr(parent, '/');
  if (!slash || slash == parent) return;
  *slash = '\0';
  if (parent[0] == '\0') return;
  if (strcmp(parent, "/") == 0) return;
  if (!SD_MMC.exists(parent)) {
    SD_MMC.mkdir(parent);
  }
}

int sdHostList(const char* path, char names[][64], uint32_t* sizes, bool* isDir,
               int maxEntries) {
  if (gMscLocked || !gMounted || !sdHostPathOk(path) || maxEntries <= 0) return -1;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return -1;
  File root = SD_MMC.open(fsPath);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return -1;
  }
  int n = 0;
  File ent = root.openNextFile();
  while (ent && n < maxEntries) {
    String nm = ent.name();
    const char* base = nm.c_str();
    const char* slash = strrchr(base, '/');
    if (slash && slash[1]) base = slash + 1;
    strncpy(names[n], base, 63);
    names[n][63] = '\0';
    sizes[n] = (uint32_t)ent.size();
    isDir[n] = ent.isDirectory();
    n++;
    ent = root.openNextFile();
  }
  root.close();
  return n;
}

int sdHostRead(const char* path, char* buf, size_t maxBytes) {
  if (gMscLocked || !gMounted || !buf || maxBytes == 0 || !sdHostPathOk(path)) return -1;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return -1;
  if (strcmp(fsPath, "/") == 0) return -1;
  File f = SD_MMC.open(fsPath, "r");
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return -1;
  }
  size_t n = f.readBytes(buf, maxBytes);
  f.close();
  return (int)n;
}

bool sdHostWrite(const char* path, const char* data, size_t len) {
  if (gMscLocked || !gMounted || !data || !sdHostPathOk(path)) return false;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return false;
  if (strcmp(fsPath, "/") == 0) return false;
  ensureParentDirFs(fsPath);
  File f = SD_MMC.open(fsPath, "w");
  if (!f) return false;
  size_t wrote = f.write((const uint8_t*)data, len);
  f.close();
  return wrote == len;
}

bool sdHostExists(const char* path) {
  if (gMscLocked || !gMounted || !sdHostPathOk(path)) return false;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return false;
  return SD_MMC.exists(fsPath);
}

bool sdHostMkdir(const char* path) {
  if (gMscLocked || !gMounted || !sdHostPathOk(path)) return false;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return false;
  if (strcmp(fsPath, "/") == 0) return true;
  if (SD_MMC.exists(fsPath)) {
    File f = SD_MMC.open(fsPath);
    bool ok = f && f.isDirectory();
    if (f) f.close();
    return ok;
  }
  // Create intermediate segments: /a, /a/b, ...
  char tmp[160];
  strncpy(tmp, fsPath, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  size_t len = strlen(tmp);
  for (size_t i = 1; i < len; ++i) {
    if (tmp[i] != '/') continue;
    tmp[i] = '\0';
    if (tmp[0] && !SD_MMC.exists(tmp)) {
      if (!SD_MMC.mkdir(tmp)) {
        tmp[i] = '/';
        return false;
      }
    }
    tmp[i] = '/';
  }
  if (!SD_MMC.exists(fsPath)) {
    if (!SD_MMC.mkdir(fsPath)) return false;
  }
  return true;
}

bool sdHostWriteBytes(const char* path, const void* data, size_t len) {
  return sdHostWrite(path, (const char*)data, len);
}

bool sdHostOpenWrite(const char* path, fs::File* out) {
  if (gMscLocked || !gMounted || !out || !sdHostPathOk(path)) return false;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return false;
  if (strcmp(fsPath, "/") == 0) return false;

  // Ensure all parent segments exist (nested: /squish/img/...).
  char parentPub[160];
  strncpy(parentPub, path, sizeof(parentPub) - 1);
  parentPub[sizeof(parentPub) - 1] = '\0';
  char* slash = strrchr(parentPub, '/');
  if (slash && slash != parentPub) {
    *slash = '\0';
    if (strcmp(parentPub, "/sd") != 0 && !sdHostMkdir(parentPub)) {
      Serial.printf("sd: mkdir parents for %s failed\n", path);
      return false;
    }
  }

  // Open directly into *out so a temporary File dtor cannot close the FD.
  *out = SD_MMC.open(fsPath, "w");
  if (!*out) {
    Serial.printf("sd: open write %s (fs %s) failed\n", path, fsPath);
    return false;
  }
  return true;
}

bool sdHostOpenRead(const char* path, fs::File* out) {
  if (gMscLocked || !gMounted || !out || !sdHostPathOk(path)) return false;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return false;
  if (strcmp(fsPath, "/") == 0) return false;
  *out = SD_MMC.open(fsPath, "r");
  if (!*out || out->isDirectory()) {
    if (*out) out->close();
    return false;
  }
  return true;
}

static bool resolveSdChild(const char* parentPub, const char* entName, char* out,
                           size_t outSz) {
  if (!parentPub || !entName || !out || outSz < 2) return false;
  const char* base = entName;
  const char* slash = strrchr(entName, '/');
  if (slash && slash[1]) base = slash + 1;
  if (!base[0] || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) return false;
  int n = snprintf(out, outSz, "%s/%s", parentPub, base);
  if (n <= 0 || (size_t)n >= outSz) return false;
  if (!sdHostPathOk(out)) return false;
  // Child must stay under parent.
  size_t plen = strlen(parentPub);
  if (strncmp(out, parentPub, plen) != 0 ||
      (out[plen] != '/' && out[plen] != '\0')) {
    return false;
  }
  return true;
}

bool sdHostRemoveTree(const char* path) {
  if (gMscLocked || !gMounted || !path) return false;
  if (!sdHostPathOk(path)) return false;
  // Refuse wiping the whole card or mount root.
  if (strcmp(path, "/sd") == 0 || strcmp(path, "/sd/") == 0 ||
      strcmp(path, "/") == 0) {
    return false;
  }
  if (strstr(path, "..") != nullptr) return false;

  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return false;
  if (strcmp(fsPath, "/") == 0) return false;

  if (!SD_MMC.exists(fsPath)) return true;

  File probe = SD_MMC.open(fsPath);
  if (!probe) return false;
  bool isDir = probe.isDirectory();
  probe.close();

  if (!isDir) {
    return SD_MMC.remove(fsPath);
  }

  for (;;) {
    File dir = SD_MMC.open(fsPath);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return false;
    }
    File ent = dir.openNextFile();
    if (!ent) {
      dir.close();
      break;
    }
    String name = ent.name();
    bool childIsDir = ent.isDirectory();
    ent.close();
    dir.close();

    char childPub[160];
    if (!resolveSdChild(path, name.c_str(), childPub, sizeof(childPub))) {
      Serial.printf("sd: refuse unsafe child under %s\n", path);
      return false;
    }
    if (childIsDir) {
      if (!sdHostRemoveTree(childPub)) return false;
    } else {
      char childFs[160];
      if (!toFsPath(childPub, childFs, sizeof(childFs))) return false;
      if (!SD_MMC.remove(childFs)) {
        Serial.printf("sd: remove file failed %s\n", childPub);
        return false;
      }
    }
  }

  if (!SD_MMC.rmdir(fsPath)) {
    Serial.printf("sd: rmdir failed %s\n", path);
    return false;
  }
  return true;
}


bool sdHostRemove(const char* path) {
  if (gMscLocked || !gMounted || !path) return false;
  if (!sdHostPathOk(path)) return false;
  if (strcmp(path, "/sd") == 0 || strcmp(path, "/sd/") == 0) return false;
  if (strstr(path, "..") != nullptr) return false;
  char fsPath[160];
  if (!toFsPath(path, fsPath, sizeof(fsPath))) return false;
  if (!SD_MMC.exists(fsPath)) return true;
  File probe = SD_MMC.open(fsPath);
  if (!probe) return false;
  bool isDir = probe.isDirectory();
  probe.close();
  if (isDir) return false;
  return SD_MMC.remove(fsPath);
}

bool sdHostRename(const char* fromPath, const char* toPath) {
  if (gMscLocked || !gMounted || !fromPath || !toPath) return false;
  if (!sdHostPathOk(fromPath) || !sdHostPathOk(toPath)) return false;
  if (strstr(fromPath, "..") || strstr(toPath, "..")) return false;
  char fromFs[160], toFs[160];
  if (!toFsPath(fromPath, fromFs, sizeof(fromFs))) return false;
  if (!toFsPath(toPath, toFs, sizeof(toFs))) return false;
  if (!SD_MMC.exists(fromFs)) return false;
  // Ensure parent of destination exists (one level).
  char parent[160];
  strncpy(parent, toPath, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char* slash = strrchr(parent, '/');
  if (slash && slash != parent) {
    *slash = '\0';
    if (!sdHostExists(parent)) {
      if (!sdHostMkdir(parent)) return false;
    }
  }
  return SD_MMC.rename(fromFs, toFs);
}
