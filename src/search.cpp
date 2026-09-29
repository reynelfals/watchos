#include "search.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cctype>

static CatalogSource gSource = CatalogSource::None;
static size_t gCount = 0;
static bool gLittleFsReady = false;
static bool gInited = false;

// PSRAM (or heap) cache of the full catalog for instant live prefix filter.
static SquishRecord* gCache = nullptr;
static size_t gCacheCount = 0;
static size_t gCacheCap = 0;

void squishCopyStr(char* dest, size_t destSize, const char* src) {
  if (destSize == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; i + 1 < destSize && src[i]; ++i) {
    dest[i] = src[i];
  }
  dest[i] = '\0';
}

static void trimInPlace(char* s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
  char* p = s;
  while (*p == ' ' || *p == '\t') ++p;
  if (p != s) memmove(s, p, strlen(p) + 1);
}

static bool iprefix(const char* s, const char* prefix) {
  if (!s || !prefix || !prefix[0]) return false;
  while (*prefix) {
    if (!*s) return false;
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    ++s;
    ++prefix;
  }
  return true;
}

static void clearRecord(SquishRecord* r) {
  r->name[0] = r->full_name[0] = r->animal[0] = r->squad[0] = '\0';
  r->size[0] = r->retail_price_usd[0] = r->bio[0] = '\0';
}

static void loadFallback(size_t index, SquishRecord* out) {
  clearRecord(out);
  if (index >= SQUISH_FALLBACK_COUNT) return;
  SquishEntry e;
  memcpy_P(&e, &SQUISH_FALLBACK[index], sizeof(SquishEntry));
  squishCopyStr(out->name, sizeof(out->name), e.name);
  squishCopyStr(out->full_name, sizeof(out->full_name), e.full_name);
  squishCopyStr(out->animal, sizeof(out->animal), e.animal);
  squishCopyStr(out->squad, sizeof(out->squad), e.squad);
  squishCopyStr(out->size, sizeof(out->size), e.size);
  squishCopyStr(out->retail_price_usd, sizeof(out->retail_price_usd), e.retail_price_usd);
  squishCopyStr(out->bio, sizeof(out->bio), e.bio);
}

// Parse one CSV line into fields (supports quoted fields with commas).
// name,full_name,animal,squad,size,retail_price_usd,bio
static bool parseCsvLine(const char* line, SquishRecord* out) {
  clearRecord(out);
  char buf[7][160];
  for (int i = 0; i < 7; ++i) buf[i][0] = '\0';

  int fi = 0;
  const char* p = line;
  while (*p && fi < 7) {
    if (*p == '"') {
      ++p;
      size_t o = 0;
      while (*p && o + 1 < sizeof(buf[0])) {
        if (*p == '"') {
          if (p[1] == '"') {
            buf[fi][o++] = '"';
            p += 2;
            continue;
          }
          ++p;
          break;
        }
        buf[fi][o++] = *p++;
      }
      buf[fi][o] = '\0';
      if (*p == ',') ++p;
    } else {
      size_t o = 0;
      while (*p && *p != ',' && o + 1 < sizeof(buf[0])) {
        buf[fi][o++] = *p++;
      }
      buf[fi][o] = '\0';
      if (*p == ',') ++p;
    }
    ++fi;
  }
  if (fi < 1 || !buf[0][0]) return false;

  squishCopyStr(out->name, sizeof(out->name), buf[0]);
  squishCopyStr(out->full_name, sizeof(out->full_name), buf[1]);
  squishCopyStr(out->animal, sizeof(out->animal), buf[2]);
  squishCopyStr(out->squad, sizeof(out->squad), buf[3]);
  squishCopyStr(out->size, sizeof(out->size), buf[4]);
  squishCopyStr(out->retail_price_usd, sizeof(out->retail_price_usd), buf[5]);
  squishCopyStr(out->bio, sizeof(out->bio), buf[6]);
  trimInPlace(out->name);
  trimInPlace(out->full_name);
  trimInPlace(out->animal);
  trimInPlace(out->squad);
  trimInPlace(out->size);
  trimInPlace(out->retail_price_usd);
  trimInPlace(out->bio);
  return out->name[0] != '\0';
}

static bool isHeaderLine(const char* line) {
  return strncmp(line, "name,", 5) == 0 || strncmp(line, "name\r", 5) == 0;
}

static bool readLine(File& f, char* buf, size_t bufSize) {
  if (bufSize == 0) return false;
  size_t i = 0;
  while (f.available()) {
    char c = (char)f.read();
    if (c == '\n') break;
    if (c == '\r') continue;
    if (i + 1 < bufSize) buf[i++] = c;
  }
  buf[i] = '\0';
  return i > 0 || f.available();
}

static bool cacheGrow(size_t minCap) {
  if (gCacheCap >= minCap) return true;
  size_t cap = gCacheCap ? gCacheCap : 512;
  while (cap < minCap) cap *= 2;
  SquishRecord* next = (SquishRecord*)heap_caps_realloc(
      gCache, cap * sizeof(SquishRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!next) {
    next = (SquishRecord*)heap_caps_realloc(gCache, cap * sizeof(SquishRecord),
                                            MALLOC_CAP_8BIT);
  }
  if (!next) return false;
  gCache = next;
  gCacheCap = cap;
  return true;
}

static bool loadFileIntoCache(File& f) {
  char line[384];
  bool first = true;
  gCacheCount = 0;
  while (f.available()) {
    if (!readLine(f, line, sizeof(line))) break;
    if (!line[0]) continue;
    if (first) {
      first = false;
      if (isHeaderLine(line)) continue;
    }
    if (!cacheGrow(gCacheCount + 1)) break;
    if (!parseCsvLine(line, &gCache[gCacheCount])) continue;
    ++gCacheCount;
  }
  return gCacheCount > 0;
}

static bool loadLittleFsPathIntoCache(const char* path) {
  if (!path || !LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  bool ok = loadFileIntoCache(f);
  f.close();
  return ok;
}

static bool loadFallbackIntoCache() {
  if (!cacheGrow(SQUISH_FALLBACK_COUNT)) return false;
  gCacheCount = 0;
  for (size_t i = 0; i < SQUISH_FALLBACK_COUNT; ++i) {
    loadFallback(i, &gCache[gCacheCount]);
    ++gCacheCount;
  }
  return gCacheCount > 0;
}

CatalogSource catalogSource() { return gSource; }

// Forward decl — catalogInit defined below.
bool catalogInit();

size_t catalogCount() {
  if (!gInited) catalogInit();
  return gCount;
}

static void printMemStats(const char* tag) {
  Serial.printf(
      "[mem:%s] heap free=%u min=%u largest=%u  psram free=%u size=%u\n",
      tag,
      (unsigned)ESP.getFreeHeap(),
      (unsigned)ESP.getMinFreeHeap(),
      (unsigned)ESP.getMaxAllocHeap(),
      (unsigned)ESP.getFreePsram(),
      (unsigned)ESP.getPsramSize());
}

bool catalogInit() {
  gSource = CatalogSource::None;
  gCount = 0;
  gLittleFsReady = false;
  gCacheCount = 0;
  gInited = true;

  printMemStats("catalog-start");

  // Prefer already-mounted LittleFS (apps_host). Do not format on failure.
  if (LittleFS.begin(false)) {
    gLittleFsReady = true;
  } else {
    Serial.println("catalog: LittleFS.begin(false) failed (no format)");
  }

  if (gLittleFsReady) {
    // 1) App package catalog
    if (loadLittleFsPathIntoCache(SQUISH_APP_CSV_PATH)) {
      gSource = CatalogSource::LittleFs;
      gCount = gCacheCount;
      printMemStats("catalog-app");
      Serial.printf("catalog LittleFS %s cache=%u\n", SQUISH_APP_CSV_PATH,
                    (unsigned)gCacheCount);
      return true;
    }
    // 2) Root fallback
    if (loadLittleFsPathIntoCache(SQUISH_LITTLEFS_CSV_PATH)) {
      gSource = CatalogSource::LittleFs;
      gCount = gCacheCount;
      printMemStats("catalog-littlefs-root");
      Serial.printf("catalog LittleFS %s cache=%u\n", SQUISH_LITTLEFS_CSV_PATH,
                    (unsigned)gCacheCount);
      return true;
    }
    Serial.println("catalog: no CSV at /apps/squish_id/catalog.csv or /squishmallows.csv");
  }

  // 3) Tiny PROGMEM fallback
  gSource = CatalogSource::Fallback;
  if (!loadFallbackIntoCache()) {
    Serial.println("catalog: PROGMEM cache alloc failed — streaming fallback only");
    gCacheCount = 0;
  }
  gCount = gCacheCount ? gCacheCount : SQUISH_FALLBACK_COUNT;
  printMemStats("catalog-fallback");
  return false;
}

static void ensureCatalog() {
  if (!gInited) catalogInit();
}

static bool recordMatchesPrefix(const SquishRecord& r, const char* qs) {
  if (iprefix(r.name, qs)) return true;
  if (r.full_name[0] && iprefix(r.full_name, qs)) return true;
  return false;
}

size_t searchPrefix(const char* query, SquishRecord* out, size_t maxOut) {
  ensureCatalog();
  if (!query || !out || maxOut == 0) return 0;
  char qs[64];
  squishCopyStr(qs, sizeof(qs), query);
  trimInPlace(qs);
  if (!qs[0]) return 0;

  size_t n = 0;
  if (gCache && gCacheCount) {
    for (size_t i = 0; i < gCacheCount && n < maxOut; ++i) {
      if (recordMatchesPrefix(gCache[i], qs)) {
        out[n++] = gCache[i];
      }
    }
    return n;
  }

  // Last-resort stream if cache never allocated.
  if (gSource == CatalogSource::LittleFs && gLittleFsReady) {
    const char* paths[] = {SQUISH_APP_CSV_PATH, SQUISH_LITTLEFS_CSV_PATH};
    for (const char* path : paths) {
      if (!LittleFS.exists(path)) continue;
      File f = LittleFS.open(path, FILE_READ);
      if (!f) continue;
      char line[384];
      bool first = true;
      n = 0;
      while (f.available() && n < maxOut) {
        if (!readLine(f, line, sizeof(line))) break;
        if (!line[0]) continue;
        if (first) {
          first = false;
          if (isHeaderLine(line)) continue;
        }
        SquishRecord r;
        if (!parseCsvLine(line, &r)) continue;
        if (recordMatchesPrefix(r, qs)) out[n++] = r;
      }
      f.close();
      if (n > 0) return n;
    }
  }

  for (size_t i = 0; i < SQUISH_FALLBACK_COUNT && n < maxOut; ++i) {
    SquishRecord r;
    loadFallback(i, &r);
    if (recordMatchesPrefix(r, qs)) out[n++] = r;
  }
  return n;
}
