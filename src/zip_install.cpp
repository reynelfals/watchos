#include "zip_install.h"
#include "sd_host.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "miniz.h"

static void setErr(char* errBuf, size_t errLen, const char* msg) {
  if (!errBuf || errLen == 0) return;
  strncpy(errBuf, msg ? msg : "error", errLen - 1);
  errBuf[errLen - 1] = '\0';
}

static bool sanitizeAppId(const char* id, char* out, size_t outSz) {
  if (!id || !out || outSz < 2) return false;
  size_t j = 0;
  for (size_t i = 0; id[i] && j + 1 < outSz; ++i) {
    char c = id[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-') {
      out[j++] = c;
    } else {
      return false;
    }
  }
  out[j] = '\0';
  return j > 0 && j < 32;
}

static bool pathHasDotDot(const char* path) {
  if (!path) return true;
  const char* p = path;
  while ((p = strstr(p, "..")) != nullptr) {
    if ((p == path || p[-1] == '/') && (p[2] == '\0' || p[2] == '/')) return true;
    p += 2;
  }
  return false;
}

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool ensureLfsParent(const char* path) {
  if (!path) return false;
  char parent[128];
  strncpy(parent, path, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char* slash = strrchr(parent, '/');
  if (!slash || slash == parent) return true;
  *slash = '\0';
  if (parent[0] && !LittleFS.exists(parent)) {
    // create nested one level at a time
    char acc[128];
    acc[0] = '\0';
    const char* s = parent;
    if (*s == '/') {
      strcat(acc, "/");
      s++;
    }
    while (*s) {
      const char* slash2 = strchr(s, '/');
      char part[64];
      size_t n = slash2 ? (size_t)(slash2 - s) : strlen(s);
      if (n >= sizeof(part)) return false;
      memcpy(part, s, n);
      part[n] = '\0';
      if (strlen(acc) + 1 + n >= sizeof(acc)) return false;
      if (acc[0] && acc[strlen(acc) - 1] != '/') strcat(acc, "/");
      strcat(acc, part);
      if (!LittleFS.exists(acc)) {
        if (!LittleFS.mkdir(acc)) return false;
      }
      if (!slash2) break;
      s = slash2 + 1;
    }
  }
  return true;
}

static bool writeDestFile(const char* dest, const char* appId, const char* rel,
                          const uint8_t* data, size_t len) {
  if (!rel || !rel[0] || pathHasDotDot(rel) || rel[0] == '/') return false;
  // skip absolute / windows drive junk
  if (strchr(rel, ':')) return false;

  char outPath[160];
  if (strcmp(dest, "sd") == 0) {
    if (!sdHostReady() && !sdHostMount()) return false;
    snprintf(outPath, sizeof(outPath), "/sd/apps/%s/%s", appId, rel);
    if (!sdHostPathOk(outPath)) return false;
    // ensure parent dirs under /sd/apps/<id>/...
    char parent[160];
    strncpy(parent, outPath, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char* slash = strrchr(parent, '/');
    if (slash && slash != parent) {
      *slash = '\0';
      if (!sdHostMkdir(parent)) {
        // try create /sd/apps and /sd/apps/id
        sdHostMkdir("/sd/apps");
        char appDir[96];
        snprintf(appDir, sizeof(appDir), "/sd/apps/%s", appId);
        sdHostMkdir(appDir);
        sdHostMkdir(parent);
      }
    }
    return sdHostWriteBytes(outPath, data, len);
  }

  // lfs
  snprintf(outPath, sizeof(outPath), "/apps/%s/%s", appId, rel);
  if (strncmp(outPath, "/apps/", 6) != 0 || pathHasDotDot(outPath)) return false;
  if (!LittleFS.exists("/apps")) LittleFS.mkdir("/apps");
  char appDir[64];
  snprintf(appDir, sizeof(appDir), "/apps/%s", appId);
  if (!LittleFS.exists(appDir)) LittleFS.mkdir(appDir);
  if (!ensureLfsParent(outPath)) return false;
  File f = LittleFS.open(outPath, "w");
  if (!f) return false;
  size_t n = f.write(data, len);
  f.close();
  return n == len;
}

static bool inflateRaw(const uint8_t* src, size_t srcLen, uint8_t* dst,
                       size_t dstLen, size_t* outLen) {
  if (!src || !dst || !outLen) return false;
  size_t n = tinfl_decompress_mem_to_mem(
      dst, dstLen, src, srcLen, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
  *outLen = n;
  return true;
}

static bool unpackZipBuf(const uint8_t* data, size_t len, const char* appId,
                         const char* dest, char* errBuf, size_t errLen) {
  if (!data || len < 22) {
    setErr(errBuf, errLen, "zip too small");
    return false;
  }
  // Find EOCD (end of central directory) — scan last 64KB.
  size_t scan = len < 65557 ? len : 65557;
  const uint8_t* eocd = nullptr;
  for (size_t i = len - 22; i + 22 <= len && (len - i) <= scan; --i) {
    if (data[i] == 0x50 && data[i + 1] == 0x4b && data[i + 2] == 0x05 &&
        data[i + 3] == 0x06) {
      eocd = data + i;
      break;
    }
    if (i == 0) break;
  }
  if (!eocd) {
    setErr(errBuf, errLen, "no EOCD");
    return false;
  }
  uint16_t entries = rd16(eocd + 10);
  uint32_t cdOff = rd32(eocd + 16);
  if ((size_t)cdOff + 46 > len) {
    setErr(errBuf, errLen, "bad CD offset");
    return false;
  }

  size_t totalUnc = 0;
  const uint8_t* p = data + cdOff;
  for (uint16_t e = 0; e < entries; ++e) {
    if ((size_t)(p - data) + 46 > len) {
      setErr(errBuf, errLen, "CD truncated");
      return false;
    }
    if (rd32(p) != 0x02014b50u) {
      setErr(errBuf, errLen, "bad CD sig");
      return false;
    }
    uint16_t method = rd16(p + 10);
    uint32_t compSize = rd32(p + 20);
    uint32_t uncSize = rd32(p + 24);
    uint16_t nameLen = rd16(p + 28);
    uint16_t extraLen = rd16(p + 30);
    uint16_t commentLen = rd16(p + 32);
    uint32_t localOff = rd32(p + 42);
    if ((size_t)(p - data) + 46 + nameLen > len) {
      setErr(errBuf, errLen, "name truncated");
      return false;
    }
    char name[96];
    size_t copy = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
    memcpy(name, p + 46, copy);
    name[copy] = '\0';
    // Skip directories
    bool isDir = (nameLen > 0 && name[nameLen - 1] == '/') || (uncSize == 0 && compSize == 0);
    p += 46 + nameLen + extraLen + commentLen;

    if (isDir) continue;
    if (method != 0 && method != 8) {
      setErr(errBuf, errLen, "unsupported method");
      return false;
    }
    if (uncSize > WATCHOS_ZIP_MAX_FILE) {
      setErr(errBuf, errLen, "file too large");
      return false;
    }
    if (totalUnc + uncSize > WATCHOS_ZIP_MAX_UNCOMPRESSED) {
      setErr(errBuf, errLen, "zip too large");
      return false;
    }
    if ((size_t)localOff + 30 > len) {
      setErr(errBuf, errLen, "bad local off");
      return false;
    }
    const uint8_t* loc = data + localOff;
    if (rd32(loc) != 0x04034b50u) {
      setErr(errBuf, errLen, "bad local sig");
      return false;
    }
    uint16_t locName = rd16(loc + 26);
    uint16_t locExtra = rd16(loc + 28);
    const uint8_t* payload = loc + 30 + locName + locExtra;
    if ((size_t)(payload - data) + compSize > len) {
      setErr(errBuf, errLen, "payload truncated");
      return false;
    }

    uint8_t* outBuf = (uint8_t*)malloc(uncSize ? uncSize : 1);
    if (!outBuf) {
      setErr(errBuf, errLen, "oom");
      return false;
    }
    size_t got = 0;
    bool ok = false;
    if (method == 0) {
      if (compSize != uncSize) {
        free(outBuf);
        setErr(errBuf, errLen, "store size mismatch");
        return false;
      }
      memcpy(outBuf, payload, uncSize);
      got = uncSize;
      ok = true;
    } else {
      ok = inflateRaw(payload, compSize, outBuf, uncSize, &got);
      if (ok && got != uncSize) ok = false;
    }
    if (!ok) {
      free(outBuf);
      setErr(errBuf, errLen, "inflate failed");
      return false;
    }
    if (!writeDestFile(dest, appId, name, outBuf, got)) {
      free(outBuf);
      setErr(errBuf, errLen, "write failed");
      return false;
    }
    free(outBuf);
    totalUnc += got;
  }
  setErr(errBuf, errLen, "ok");
  return true;
}

bool zipInstallFromMemory(const uint8_t* data, size_t len, const char* appId,
                          const char* dest, char* errBuf, size_t errLen) {
  char id[32];
  if (!sanitizeAppId(appId, id, sizeof(id))) {
    setErr(errBuf, errLen, "bad app id");
    return false;
  }
  if (!dest) dest = "lfs";
  if (strcmp(dest, "lfs") != 0 && strcmp(dest, "sd") != 0) {
    setErr(errBuf, errLen, "dest must be lfs|sd");
    return false;
  }
  return unpackZipBuf(data, len, id, dest, errBuf, errLen);
}

bool zipInstallFromPath(const char* zipPath, const char* appId, const char* dest,
                        char* errBuf, size_t errLen) {
  if (!zipPath || !zipPath[0]) {
    setErr(errBuf, errLen, "no path");
    return false;
  }
  // Cap read size to uncompressed cap + overhead
  const size_t maxZip = WATCHOS_ZIP_MAX_UNCOMPRESSED + 64 * 1024;
  uint8_t* buf = nullptr;
  size_t n = 0;
  if (strncmp(zipPath, "/sd/", 4) == 0) {
    if (!sdHostPathOk(zipPath)) {
      setErr(errBuf, errLen, "bad sd path");
      return false;
    }
    if (!sdHostReady() && !sdHostMount()) {
      setErr(errBuf, errLen, "sd not mounted");
      return false;
    }
    buf = (uint8_t*)malloc(maxZip);
    if (!buf) {
      setErr(errBuf, errLen, "oom");
      return false;
    }
    int r = sdHostRead(zipPath, (char*)buf, maxZip);
    if (r < 0) {
      free(buf);
      setErr(errBuf, errLen, "sd read failed");
      return false;
    }
    n = (size_t)r;
  } else {
    if (strncmp(zipPath, "/apps/", 6) != 0 || pathHasDotDot(zipPath)) {
      setErr(errBuf, errLen, "path not under /apps");
      return false;
    }
    if (!LittleFS.exists(zipPath)) {
      setErr(errBuf, errLen, "missing zip");
      return false;
    }
    File f = LittleFS.open(zipPath, "r");
    if (!f) {
      setErr(errBuf, errLen, "open failed");
      return false;
    }
    size_t sz = f.size();
    if (sz == 0 || sz > maxZip) {
      f.close();
      setErr(errBuf, errLen, "zip size");
      return false;
    }
    buf = (uint8_t*)malloc(sz);
    if (!buf) {
      f.close();
      setErr(errBuf, errLen, "oom");
      return false;
    }
    n = f.read(buf, sz);
    f.close();
    if (n != sz) {
      free(buf);
      setErr(errBuf, errLen, "short read");
      return false;
    }
  }
  bool ok = zipInstallFromMemory(buf, n, appId, dest, errBuf, errLen);
  free(buf);
  return ok;
}
