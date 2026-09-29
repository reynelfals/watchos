#include "apps_host.h"
#include "sd_host.h"

#include <FS.h>
#include <LittleFS.h>
#include <ctype.h>
#include <string.h>

static AppInfo gApps[APPS_MAX];
static size_t gCount = 0;
static bool gFsOk = false;

static void copyTrunc(char* dst, size_t dstSz, const char* src) {
  if (!dst || dstSz == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstSz - 1);
  dst[dstSz - 1] = '\0';
}

// Extremely small JSON string extractor: find "key":"value"
static bool jsonGetString(const char* json, const char* key, char* out, size_t outSz) {
  if (!json || !key || !out || outSz == 0) return false;
  out[0] = '\0';
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return false;
  p += strlen(pattern);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  if (*p != ':') return false;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  if (*p != '"') return false;
  ++p;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < outSz) {
    if (*p == '\\' && p[1]) {
      ++p;
      out[i++] = *p++;
    } else {
      out[i++] = *p++;
    }
  }
  out[i] = '\0';
  return i > 0;
}

/** Find "key": true/false (unquoted bool). Default false if missing. */
static bool jsonGetBool(const char* json, const char* key) {
  if (!json || !key) return false;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return false;
  p += strlen(pattern);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  if (*p != ':') return false;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  if (strncmp(p, "true", 4) == 0) return true;
  return false;
}


static bool pathExistsForStorage(AppStorage storage, const char* fullPath) {
  if (!fullPath || !fullPath[0]) return false;
  if (storage == APP_STORAGE_LFS) {
    return gFsOk && LittleFS.exists(fullPath);
  }
  if (sdHostMscLocked()) return false;
  if (!sdHostReady() && !sdHostMount()) return false;
  return sdHostExists(fullPath);
}

/** Resolve relative icon path; default icon.wrgb when present. Empty if missing. */
static void resolveAppIcon(AppInfo* info, const char* jsonIcon) {
  if (!info) return;
  info->icon[0] = '\0';
  char rel[APP_ICON_MAX];
  if (jsonIcon && jsonIcon[0]) {
    copyTrunc(rel, sizeof(rel), jsonIcon);
  } else {
    copyTrunc(rel, sizeof(rel), "icon.wrgb");
  }
  // Relative basename-ish only — no absolute, no .., no subdir escape beyond one level ok
  if (!rel[0] || rel[0] == '/' || strstr(rel, "..") != nullptr) return;
  char full[APP_PATH_MAX + APP_ICON_MAX];
  int n = snprintf(full, sizeof(full), "%s/%s", info->folder, rel);
  if (n <= 0 || (size_t)n >= sizeof(full)) return;
  if (!pathExistsForStorage(info->storage, full)) {
    // If explicit icon missing, do not fall through again; leave empty.
    return;
  }
  copyTrunc(info->icon, sizeof(info->icon), rel);
}


static bool endsWithCi(const char* s, const char* suffix) {
  if (!s || !suffix) return false;
  size_t n = strlen(s), m = strlen(suffix);
  if (m > n) return false;
  for (size_t i = 0; i < m; ++i) {
    char a = s[n - m + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

static bool fillAppFromJson(const char* folderPath, AppStorage storage, const char* json,
                            AppInfo* info) {
  if (!folderPath || !json || !info) return false;
  memset(info, 0, sizeof(*info));
  info->storage = storage;
  copyTrunc(info->folder, sizeof(info->folder), folderPath);
  if (!jsonGetString(json, "id", info->id, sizeof(info->id))) {
    const char* slash = strrchr(folderPath, '/');
    copyTrunc(info->id, sizeof(info->id), slash ? slash + 1 : folderPath);
  }
  if (!jsonGetString(json, "name", info->name, sizeof(info->name))) {
    copyTrunc(info->name, sizeof(info->name), info->id);
  }
  jsonGetString(json, "version", info->version, sizeof(info->version));

  char kindField[16];
  kindField[0] = '\0';
  jsonGetString(json, "kind", kindField, sizeof(kindField));

  bool hasEntry = jsonGetString(json, "entry", info->entry, sizeof(info->entry));
  // kind: "wasm" or entry ending in .wasm => WASM; else LUA (native is firmware-only)
  bool wantWasm = (strcmp(kindField, "wasm") == 0) ||
                  (hasEntry && endsWithCi(info->entry, ".wasm"));
  if (!hasEntry) {
    copyTrunc(info->entry, sizeof(info->entry), wantWasm ? "main.wasm" : "main.lua");
  }
  info->kind = wantWasm ? APP_KIND_WASM : APP_KIND_LUA;
  info->native_index = -1;

  info->is_protected = jsonGetBool(json, "protected");
  char iconField[APP_ICON_MAX];
  iconField[0] = '\0';
  jsonGetString(json, "icon", iconField, sizeof(iconField));
  int n = snprintf(info->script, sizeof(info->script), "%s/%s", info->folder, info->entry);
  if (n <= 0 || (size_t)n >= sizeof(info->script)) {
    info->script[0] = '\0';
    return false;
  }
  if (info->id[0] == '\0') return false;
  resolveAppIcon(info, iconField);
  return true;
}

static bool loadAppJsonLfs(const char* folderPath, AppInfo* info) {
  char path[96];
  snprintf(path, sizeof(path), "%s/app.json", folderPath);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  char buf[512];
  size_t n = f.readBytes(buf, sizeof(buf) - 1);
  f.close();
  if (n == 0) return false;
  buf[n] = '\0';
  return fillAppFromJson(folderPath, APP_STORAGE_LFS, buf, info);
}

static bool loadAppJsonSd(const char* folderPath, AppInfo* info) {
  char path[112];
  snprintf(path, sizeof(path), "%s/app.json", folderPath);
  char buf[512];
  int n = sdHostRead(path, buf, sizeof(buf) - 1);
  if (n <= 0) return false;
  buf[n] = '\0';
  return fillAppFromJson(folderPath, APP_STORAGE_SD, buf, info);
}

static bool idAlreadyPresent(const char* id) {
  if (!id || !id[0]) return true;
  for (size_t i = 0; i < gCount; ++i) {
    if (strcmp(gApps[i].id, id) == 0) return true;
  }
  return false;
}

static void scanLittleFsApps() {
  if (!gFsOk) return;
  if (!LittleFS.exists("/apps")) {
    Serial.println("apps: /apps missing");
    return;
  }
  File root = LittleFS.open("/apps");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }
  File ent = root.openNextFile();
  while (ent && gCount < APPS_MAX) {
    if (ent.isDirectory()) {
      String name = ent.name();
      char folder[APP_PATH_MAX];
      if (name.startsWith("/")) {
        copyTrunc(folder, sizeof(folder), name.c_str());
      } else {
        snprintf(folder, sizeof(folder), "/apps/%s", name.c_str());
      }
      AppInfo info;
      if (loadAppJsonLfs(folder, &info)) {
        gApps[gCount++] = info;
        Serial.printf("apps: [lfs] %s (%s v%s)%s\n", info.name, info.id, info.version,
                      info.is_protected ? " [protected]" : "");
      }
    }
    ent = root.openNextFile();
  }
  root.close();
}

static void scanSdApps() {
  if (sdHostMscLocked()) {
    Serial.println("apps: skip SD scan (MSC locked)");
    return;
  }
  // Lazy mount so launcher sees SD apps without visiting SD Lab first.
  if (!sdHostReady()) {
    if (!sdHostMount()) return;
  }
  if (!sdHostExists("/sd/apps")) {
    Serial.println("apps: /sd/apps missing (ok)");
    return;
  }

  char names[APPS_MAX][64];
  uint32_t sizes[APPS_MAX];
  bool isDir[APPS_MAX];
  int n = sdHostList("/sd/apps", names, sizes, isDir, (int)APPS_MAX);
  if (n < 0) {
    Serial.println("apps: SD list /sd/apps failed");
    return;
  }
  for (int i = 0; i < n && gCount < APPS_MAX; ++i) {
    if (!isDir[i] || !names[i][0]) continue;
    if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0) continue;
    char folder[APP_PATH_MAX];
    int wn = snprintf(folder, sizeof(folder), "/sd/apps/%s", names[i]);
    if (wn <= 0 || (size_t)wn >= sizeof(folder)) continue;
    AppInfo info;
    if (!loadAppJsonSd(folder, &info)) continue;
    if (idAlreadyPresent(info.id)) {
      Serial.printf("apps: skip SD %s — id clash, LittleFS wins\n", info.id);
      continue;
    }
    gApps[gCount++] = info;
    Serial.printf("apps: [sd] %s (%s v%s)%s\n", info.name, info.id, info.version,
                  info.is_protected ? " [protected]" : "");
  }
}

void appsHostRescan() {
  gCount = 0;
  scanLittleFsApps();
  scanSdApps();
  Serial.printf("apps: found %u\n", (unsigned)gCount);
}

bool appsHostBegin() {
  gFsOk = LittleFS.begin(false);
  if (!gFsOk) {
    Serial.println("LittleFS.begin(false) failed — run: pio run -t uploadfs");
    gCount = 0;
    // Still try SD-only catalog if card mounts.
    scanSdApps();
    Serial.printf("apps: found %u (LittleFS missing)\n", (unsigned)gCount);
    return false;
  }
  Serial.println("LittleFS ok");
  appsHostRescan();
  return true;
}

size_t appsHostCount() { return gCount; }

const AppInfo* appsHostGet(size_t index) {
  if (index >= gCount) return nullptr;
  return &gApps[index];
}

bool appsHostFsOk() { return gFsOk; }

bool appsHostSanitizeId(const char* folderOrId, char* outId, size_t outSz) {
  if (!folderOrId || !outId || outSz < 2) return false;
  if (strstr(folderOrId, "..") != nullptr) return false;

  const char* p = folderOrId;
  const char* slash = strrchr(folderOrId, '/');
  if (slash) p = slash + 1;
  if (!p[0] || strcmp(p, ".") == 0 || strcmp(p, "..") == 0) return false;

  size_t j = 0;
  for (size_t i = 0; p[i] && j + 1 < outSz; ++i) {
    char c = p[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-') {
      outId[j++] = c;
    } else {
      return false;
    }
  }
  outId[j] = '\0';
  if (j == 0 || j >= APP_ID_MAX) return false;
  return true;
}

/** Resolve child entry name under parent into out (handles absolute or relative). */
static bool resolveChildPath(const char* parent, const char* entName, char* out,
                             size_t outSz, const char* rootPrefix) {
  if (!parent || !entName || !out || outSz < 2 || !rootPrefix) return false;
  if (entName[0] == '/') {
    copyTrunc(out, outSz, entName);
  } else {
    int n = snprintf(out, outSz, "%s/%s", parent, entName);
    if (n <= 0 || (size_t)n >= outSz) return false;
  }
  if (strstr(out, "..") != nullptr) return false;
  size_t rlen = strlen(rootPrefix);
  if (strncmp(out, rootPrefix, rlen) != 0) return false;
  return true;
}

/**
 * Recursively delete a file or directory under /apps/ (LittleFS).
 */
static bool deleteTreeUnderLfs(const char* path) {
  if (!path || strncmp(path, "/apps/", 6) != 0) return false;
  if (strstr(path, "..") != nullptr) return false;
  if (strcmp(path, "/apps") == 0 || strcmp(path, "/apps/") == 0) return false;

  if (!LittleFS.exists(path)) return true;

  File probe = LittleFS.open(path);
  if (!probe) return false;
  bool isDir = probe.isDirectory();
  probe.close();

  if (!isDir) {
    return LittleFS.remove(path);
  }

  for (;;) {
    File dir = LittleFS.open(path);
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

    char child[96];
    if (!resolveChildPath(path, name.c_str(), child, sizeof(child), "/apps/")) {
      Serial.printf("apps: refuse unsafe child under %s\n", path);
      return false;
    }
    size_t plen = strlen(path);
    if (strncmp(child, path, plen) != 0 ||
        (child[plen] != '/' && child[plen] != '\0')) {
      Serial.printf("apps: child escape %s\n", child);
      return false;
    }

    if (childIsDir) {
      if (!deleteTreeUnderLfs(child)) return false;
    } else {
      if (!LittleFS.remove(child)) {
        Serial.printf("apps: remove file failed %s\n", child);
        return false;
      }
    }
  }

  if (!LittleFS.rmdir(path)) {
    Serial.printf("apps: rmdir failed %s\n", path);
    return false;
  }
  return true;
}

static const AppInfo* findById(const char* id) {
  if (!id) return nullptr;
  for (size_t i = 0; i < gCount; ++i) {
    if (strcmp(gApps[i].id, id) == 0) return &gApps[i];
  }
  return nullptr;
}

const AppInfo* appsHostFindById(const char* id) { return findById(id); }

bool appsHostRemove(const char* folderOrId) {
  char id[APP_ID_MAX];
  if (!appsHostSanitizeId(folderOrId, id, sizeof(id))) {
    Serial.println("apps: remove refused — bad id");
    return false;
  }

  // Prefer catalog entry (knows LFS vs SD). Rescan first so SoftAP/UI stay fresh.
  appsHostRescan();
  const AppInfo* known = findById(id);

  AppStorage storage = APP_STORAGE_LFS;
  char folder[APP_PATH_MAX];
  bool protectedFlag = false;

  if (known) {
    storage = known->storage;
    copyTrunc(folder, sizeof(folder), known->folder);
    protectedFlag = known->is_protected;
  } else {
    // Orphan / not in catalog: try LittleFS then SD.
    int n = snprintf(folder, sizeof(folder), "/apps/%s", id);
    if (n <= 0 || (size_t)n >= sizeof(folder)) return false;
    if (gFsOk && LittleFS.exists(folder)) {
      storage = APP_STORAGE_LFS;
      AppInfo tmp;
      if (loadAppJsonLfs(folder, &tmp)) protectedFlag = tmp.is_protected;
    } else {
      n = snprintf(folder, sizeof(folder), "/sd/apps/%s", id);
      if (n <= 0 || (size_t)n >= sizeof(folder)) return false;
      if (!sdHostReady() && !sdHostMount()) {
        Serial.printf("apps: remove — not found %s\n", id);
        return false;
      }
      if (!sdHostExists(folder)) {
        Serial.printf("apps: remove — not found %s\n", id);
        appsHostRescan();
        return false;
      }
      storage = APP_STORAGE_SD;
      AppInfo tmp;
      if (loadAppJsonSd(folder, &tmp)) protectedFlag = tmp.is_protected;
    }
  }

  if (protectedFlag) {
    Serial.printf("apps: remove refused — protected %s\n", id);
    return false;
  }

  if (storage == APP_STORAGE_LFS) {
    if (!gFsOk) {
      Serial.println("apps: remove refused — LittleFS not mounted");
      return false;
    }
    if (strncmp(folder, "/apps/", 6) != 0 || strstr(folder, "..") != nullptr) {
      return false;
    }
    if (!LittleFS.exists(folder)) {
      Serial.printf("apps: remove — not found %s\n", folder);
      appsHostRescan();
      return false;
    }
    Serial.printf("apps: uninstalling [lfs] %s\n", folder);
    bool ok = deleteTreeUnderLfs(folder);
    appsHostRescan();
    if (ok) {
      Serial.printf("apps: uninstalled %s\n", id);
    } else {
      Serial.printf("apps: uninstall failed %s\n", id);
    }
    return ok;
  }

  // SD package
  if (strncmp(folder, "/sd/apps/", 9) != 0 || strstr(folder, "..") != nullptr) {
    return false;
  }
  if (sdHostMscLocked()) {
    Serial.println("apps: remove refused — MSC locked");
    return false;
  }
  if (!sdHostReady() && !sdHostMount()) {
    Serial.println("apps: remove refused — SD not mounted");
    return false;
  }
  if (!sdHostExists(folder)) {
    Serial.printf("apps: remove — not found %s\n", folder);
    appsHostRescan();
    return false;
  }
  Serial.printf("apps: uninstalling [sd] %s\n", folder);
  bool ok = sdHostRemoveTree(folder);
  appsHostRescan();
  if (ok) {
    Serial.printf("apps: uninstalled %s\n", id);
  } else {
    Serial.printf("apps: uninstall failed %s\n", id);
  }
  return ok;
}
