/*
 * On-demand SoftAP / STA + HTTP sideload for Watch OS.
 * Radio stays OFF unless wifiOtaStartSoftAp() / wifiOtaStartSta() is called.
 *
 * SoftAP is the universal Install path (QR join + upload).
 * Home Wi‑Fi (STA) uses per-device NVS credentials provisioned from the SoftAP
 * web form — not compile-time secrets. Optional WATCHOS_WIFI_STA_* is a
 * developer override/fallback only.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <FS.h>
#include <LittleFS.h>
#include <ctype.h>
#include <string.h>

#include "app_secrets.h"
#include "wifi_ota.h"
#include "zip_install.h"
#include "sd_host.h"
#include "apps_host.h"

static WebServer* gServer = nullptr;
static Preferences gPrefs;
static WifiOtaState gState = WIFI_OTA_OFF;
static WifiOtaMode gMode = WIFI_OTA_MODE_NONE;
static char gPassword[12] = {0};       // session SoftAP pass + HTTP ?k=
static char gStaSsid[33] = {0};        // saved home SSID (NVS)
static char gStaPass[65] = {0};        // saved home password (NVS) — never UI-dump
static char gIp[16] = "0.0.0.0";
static char gStatus[96] = "off";
static char gUploadPath[160] = {0};
static File gUploadFile;
static bool gUploadOk = false;
static bool gUploadToSd = false;  // true while POST /upload_sd is streaming
static uint8_t* gZipBuf = nullptr;
static size_t gZipCap = 0;
static size_t gZipLen = 0;
static char gZipApp[33] = {0};
static char gZipDest[8] = "lfs";
static bool gZipOk = false;
static char gZipErr[64] = {0};
static bool gHttpUp = false;
static bool gStaLoaded = false;
static uint32_t gDeadlineMs = 0;
static uint32_t gStaConnectDeadlineMs = 0;
static uint32_t gLastByteMs = 0;
static uint32_t gBytesWritten = 0;

static void setStatus(const char* s) {
  if (!s) s = "";
  strncpy(gStatus, s, sizeof(gStatus) - 1);
  gStatus[sizeof(gStatus) - 1] = '\0';
}

static void genPassword() {
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  for (int i = 0; i < 8; ++i) {
    gPassword[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  gPassword[8] = '\0';
}

static void loadStaCreds(void) {
  gStaSsid[0] = '\0';
  gStaPass[0] = '\0';
  if (gPrefs.begin("watchos", true /* read-only */)) {
    String s = gPrefs.getString("sta_ssid", "");
    String p = gPrefs.getString("sta_pass", "");
    gPrefs.end();
    if (s.length() > 0 && s.length() < sizeof(gStaSsid)) {
      strncpy(gStaSsid, s.c_str(), sizeof(gStaSsid) - 1);
    }
    if (p.length() < sizeof(gStaPass)) {
      strncpy(gStaPass, p.c_str(), sizeof(gStaPass) - 1);
    }
  }
  // Developer override/fallback only — normal users provision via SoftAP form.
  if (!gStaSsid[0] && WATCHOS_WIFI_STA_SSID[0]) {
    strncpy(gStaSsid, WATCHOS_WIFI_STA_SSID, sizeof(gStaSsid) - 1);
    strncpy(gStaPass, WATCHOS_WIFI_STA_PASS, sizeof(gStaPass) - 1);
    Serial.println("wifi_ota: using compile-time STA fallback");
  }
  gStaLoaded = true;
}

static bool saveStaCreds(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(gStaSsid)) return false;
  if (pass && strlen(pass) >= sizeof(gStaPass)) return false;
  if (!gPrefs.begin("watchos", false)) {
    Serial.println("wifi_ota: NVS open failed");
    return false;
  }
  bool ok = gPrefs.putString("sta_ssid", ssid) > 0;
  ok = gPrefs.putString("sta_pass", pass ? pass : "") >= 0 && ok;
  gPrefs.end();
  if (!ok) {
    Serial.println("wifi_ota: NVS put failed");
    return false;
  }
  strncpy(gStaSsid, ssid, sizeof(gStaSsid) - 1);
  gStaSsid[sizeof(gStaSsid) - 1] = '\0';
  strncpy(gStaPass, pass ? pass : "", sizeof(gStaPass) - 1);
  gStaPass[sizeof(gStaPass) - 1] = '\0';
  gStaLoaded = true;
  Serial.printf("wifi_ota: saved home Wi-Fi SSID=%s (pass len=%u)\n",
                gStaSsid, (unsigned)strlen(gStaPass));
  return true;
}

static bool sanitizeAppName(const char* in, char* out, size_t outSz) {
  if (!in || !out || outSz < 2) return false;
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < outSz; ++i) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-') {
      out[j++] = c;
    }
  }
  out[j] = '\0';
  return j > 0;
}

static bool sanitizeFileName(const char* in, char* out, size_t outSz) {
  if (!in || !out || outSz < 2) return false;
  const char* base = strrchr(in, '/');
  if (base) base++;
  else {
    base = strrchr(in, '\\');
    base = base ? base + 1 : in;
  }
  size_t j = 0;
  for (size_t i = 0; base[i] && j + 1 < outSz; ++i) {
    char c = base[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') {
      out[j++] = c;
    }
  }
  out[j] = '\0';
  if (j == 0 || out[0] == '.') return false;
  if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) return false;
  return true;
}


/** Build public /sd/... path from query path= (relative or /sd/...). */
static bool sanitizeSdDestPath(const char* in, char* out, size_t outSz) {
  if (!in || !out || outSz < 8) return false;
  // Skip leading slashes / optional "sd/" prefix from client.
  while (*in == '/') ++in;
  if (strncmp(in, "sd/", 3) == 0) in += 3;
  while (*in == '/') ++in;
  if (!*in) return false;

  // Reject empty, absolute-ish leftovers, and ".."
  if (strstr(in, "..") != nullptr) return false;

  char rel[148];
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < sizeof(rel); ++i) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/') {
      // no leading/trailing slash in rel; collapse //
      if (c == '/' && (j == 0 || rel[j - 1] == '/')) continue;
      rel[j++] = c;
    } else {
      return false;
    }
  }
  rel[j] = '\0';
  if (j == 0 || rel[j - 1] == '/' || rel[0] == '.') return false;
  // Must have a filename with an extension (e.g. .wrgb)
  const char* base = strrchr(rel, '/');
  base = base ? base + 1 : rel;
  if (!base[0] || base[0] == '.') return false;
  const char* dot = strrchr(base, '.');
  if (!dot || dot == base || !dot[1]) return false;

  int n = snprintf(out, outSz, "/sd/%s", rel);
  if (n <= 0 || (size_t)n >= outSz) return false;
  return sdHostPathOk(out);
}

static void ensureAppsDir() {
  if (!LittleFS.exists("/apps")) {
    LittleFS.mkdir("/apps");
  }
}

static void ensureAppJson(const char* folder, const char* appId) {
  if (!folder || !appId || !appId[0]) return;
  char path[96];
  snprintf(path, sizeof(path), "%s/app.json", folder);
  if (LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("wifi_ota: could not create %s\n", path);
    return;
  }
  f.printf("{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"0.1.0\",\"entry\":\"main.lua\"}\n",
           appId, appId);
  f.close();
  Serial.printf("wifi_ota: created stub %s\n", path);
}

static void bumpActivity() {
  gLastByteMs = millis();
  if (gState == WIFI_OTA_IDLE) gState = WIFI_OTA_ACTIVE;
  uint32_t now = millis();
  uint32_t extend = now + 60000u;
  if (extend > gDeadlineMs) gDeadlineMs = extend;
}

static bool uploadAuthOk() {
  if (!gServer || !gPassword[0]) return false;
  if (!gServer->hasArg("k")) return false;
  return gServer->arg("k").equals(gPassword);
}

static void handleRoot() {
  if (!gServer) return;
  if (!gStaLoaded) loadStaCreds();

  char kArg[12] = {0};
  if (gServer->hasArg("k")) {
    strncpy(kArg, gServer->arg("k").c_str(), sizeof(kArg) - 1);
  } else if (gPassword[0]) {
    strncpy(kArg, gPassword, sizeof(kArg) - 1);
  }
  const char* modeLabel =
      (gMode == WIFI_OTA_MODE_STA) ? "Home Wi-Fi (STA)" : "SoftAP";
  const char* savedHint = gStaSsid[0] ? gStaSsid : "(none yet)";

  // Heap String — page is too large for a stack snprintf buffer.
  String page;
  page.reserve(4600);
  page += F("<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "<title>WatchOS OTA</title>"
            "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:16px}"
            "input,button{font-size:16px;margin:6px 0;padding:8px;max-width:100%}"
            "code{background:#222;padding:2px 6px;border-radius:4px}"
            "section{margin:20px 0;padding:12px;background:#1a1a1a;border-radius:8px}"
            ".approw{display:flex;justify-content:space-between;gap:8px;"
            "padding:8px 0;border-bottom:1px solid #333}"
            ".approw button{background:#a33;color:#fff;border:0;border-radius:6px;"
            "padding:6px 10px}</style></head><body>");
  page += F("<h2>WatchOS Install / OTA</h2><p>Mode <code>");
  page += modeLabel;
  page += F("</code> SSID <code>");
  page += wifiOtaSsid();
  page += F("</code> IP <code>");
  page += gIp;
  page += F("</code></p>");

  page += F("<section><h3>Upload app files</h3>"
            "<p>One file at a time into <code>/apps/&lt;folder&gt;/</code> "
            "(e.g. <code>app.json</code> then <code>main.lua</code>). "
            "Zip packages are <b>not</b> unpacked in v1.</p>"
            "<form id=f method=POST enctype=\"multipart/form-data\">"
            "App folder<br><input id=app name=app required pattern=\"[A-Za-z0-9_\\-]{1,32}\" "
            "placeholder=\"my_app\"><br>"
            "File<br><input type=file name=file required><br>"
            "<button type=submit>Upload</button></form><script>"
            "document.getElementById('f').onsubmit=function(){"
            "var k='");
  page += kArg;
  page += F("';this.action='/upload?app='+encodeURIComponent("
            "document.getElementById('app').value)+(k?('&k='+encodeURIComponent(k)):'');};"
            "</script><p>curl: <code>curl -F file=@main.lua \"http://");
  page += gIp;
  page += F("/upload?app=my_app&amp;k=");
  page += kArg;
  page += F("\"</code></p></section>");

  page += F("<section><h3>Manage apps</h3>"
            "<p>Uninstall deletes LittleFS <code>/apps/&lt;id&gt;/</code> or SD <code>/sd/apps/&lt;id&gt;/</code> (see fs in /apps JSON). "
            "Protected (<code>\"protected\":true</code>) is refused. Phone SoftAP upload still targets LittleFS. Zip: <code>POST /upload_zip?app=ID&amp;dest=lfs|sd&amp;k=</code> (cap ~512KB unzipped); use upload_sd for SD apps.</p>"
            "<div id=applist>Loading...</div>"
            "<p>curl: <code>curl -X POST \"http://");
  page += gIp;
  page += F("/uninstall?app=My_app&amp;k=");
  page += kArg;
  page += F("\"</code></p><script>(async function(){var k='");
  page += kArg;
  page += F("';var el=document.getElementById('applist');try{"
            "var r=await fetch('/apps?k='+encodeURIComponent(k||''));"
            "if(!r.ok){el.textContent='Auth/list failed ('+r.status+')';return;}"
            "var data=await r.json();"
            "if(!data.apps||!data.apps.length){el.textContent='No apps installed.';return;}"
            "el.innerHTML='';"
            "data.apps.forEach(function(a){"
            "var row=document.createElement('div');row.className='approw';"
            "var left=document.createElement('div');"
            "left.innerHTML='<b>'+a.name+'</b><br><code>'+a.id+'</code>'+"
            "(a.version?(' v'+a.version):'')+(a.protected?' (protected)':'');"
            "row.appendChild(left);"
            "var btn=document.createElement('button');"
            "btn.textContent=a.protected?'Protected':'Uninstall';"
            "btn.disabled=!!a.protected;"
            "btn.onclick=async function(){"
            "if(!confirm('Uninstall '+a.name+' ('+a.id+')?'))return;"
            "btn.disabled=true;btn.textContent='...';"
            "var ur=await fetch('/uninstall?app='+encodeURIComponent(a.id)+"
            "'&k='+encodeURIComponent(k||''),{method:'POST'});"
            "var t=await ur.text();"
            "if(ur.ok){row.remove();}else{alert(t||('Failed '+ur.status));"
            "btn.disabled=false;btn.textContent='Uninstall';}};"
            "row.appendChild(btn);el.appendChild(row);});"
            "}catch(e){el.textContent='List error: '+e;}})();</script></section>");

  page += F("<section><h3>Upload to microSD</h3>"
            "<p>Binary files under <code>/sd/&lt;path&gt;</code> (e.g. Squish thumbs). "
            "Keep Install open; use the host script for bulk.</p>"
            "<p>curl: <code>curl -F file=@slug.wrgb \"http://");
  page += gIp;
  page += F("/upload_sd?path=squish/img/slug.wrgb&amp;k=");
  page += kArg;
  page += F("\"</code></p>"
            "<p>Bulk: <code>python3 tools/push_squish_sd.py --stage tools/squish_sd_stage/img "
            "--host ");
  page += gIp;
  page += F(" --k ");
  page += kArg;
  page += F("</code></p></section>");

  page += F("<section><h3>Save home Wi-Fi</h3>"
            "<p>For LAN install from an Ethernet PC: save your home AP here "
            "(stored on this watch only). Later tap <b>Home Wi-Fi</b> on the watch. "
            "Saved SSID: <code>");
  page += savedHint;
  page += F("</code></p><form method=POST action=\"/wifi\">"
            "<input type=hidden name=k value=\"");
  page += kArg;
  page += F("\">Home SSID<br><input name=ssid required maxlength=32 placeholder=\"MyHomeWiFi\" value=\"");
  page += gStaSsid;
  page += F("\"><br>Password<br><input name=pass type=password maxlength=64 "
            "placeholder=\"(blank = keep previous)\"><br>"
            "<button type=submit>Save home Wi-Fi</button></form></section>"
            "<p>Status: ");
  page += gStatus;
  page += F("</p></body></html>");

  gServer->send(200, "text/html", page);
}

static void handleWifiSave() {
  if (!gServer) return;
  if (!uploadAuthOk()) {
    gServer->send(401, "text/plain",
                  "Missing or bad session token (?k=). Re-open the SoftAP page QR.\n");
    return;
  }
  if (!gServer->hasArg("ssid")) {
    gServer->send(400, "text/plain", "ssid required\n");
    return;
  }
  String ssid = gServer->arg("ssid");
  ssid.trim();
  String pass = gServer->hasArg("pass") ? gServer->arg("pass") : "";
  if (ssid.length() == 0 || ssid.length() >= 33) {
    gServer->send(400, "text/plain", "bad ssid\n");
    return;
  }
  if (pass.length() >= 65) {
    gServer->send(400, "text/plain", "password too long\n");
    return;
  }
  // Blank password field keeps the previously saved password (if any).
  if (pass.length() == 0 && gStaPass[0]) {
    pass = gStaPass;
  }
  if (!saveStaCreds(ssid.c_str(), pass.c_str())) {
    gServer->send(500, "text/plain", "NVS save failed\n");
    return;
  }
  setStatus("home Wi-Fi saved");
  char html[480];
  snprintf(html, sizeof(html),
           "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<meta http-equiv=refresh content=\"2;url=/?k=%s\">"
           "<title>Saved</title>"
           "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:16px}</style></head><body>"
           "<h2>Home Wi-Fi saved</h2>"
           "<p>SSID <code>%s</code> stored on this watch.</p>"
           "<p>On the watch, tap <b>Home Wi-Fi</b> to join the LAN.</p>"
           "<p><a href=\"/?k=%s\">Back</a></p></body></html>",
           gPassword, gStaSsid, gPassword);
  gServer->send(200, "text/html", html);
}

static void handleStatus() {
  if (!gServer) return;
  if (!gStaLoaded) loadStaCreds();
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"state\":%d,\"mode\":%d,\"ssid\":\"%s\",\"ip\":\"%s\","
           "\"remaining\":%u,\"status\":\"%s\",\"bytes\":%u,"
           "\"home_ssid\":\"%s\",\"home_saved\":%s}",
           (int)gState, (int)gMode, wifiOtaSsid(), gIp,
           (unsigned)wifiOtaRemainingSec(), gStatus, (unsigned)gBytesWritten,
           gStaSsid, gStaSsid[0] ? "true" : "false");
  gServer->send(200, "application/json", buf);
}

static void closeUploadFile() {
  if (gUploadFile) {
    gUploadFile.close();
  }
  gUploadPath[0] = '\0';
  gUploadToSd = false;
}

static void handleUploadPost() {
  if (!gServer) return;
  if (!uploadAuthOk()) {
    closeUploadFile();
    gUploadOk = false;
    setStatus("auth required");
    gServer->send(401, "text/plain",
                  "Missing or bad session token. Open http://IP/?k=<session password> "
                  "or add &k=<password> to the upload URL.\n");
    return;
  }
  if (gUploadOk) {
    char msg[160];
    snprintf(msg, sizeof(msg), "OK wrote %s (%u bytes)\n", gUploadPath, (unsigned)gBytesWritten);
    setStatus("uploaded");
    gServer->send(200, "text/plain", msg);
  } else {
    setStatus("upload failed");
    gServer->send(400, "text/plain", "Upload failed — check app name and file\n");
  }
}

static void handleUploadFile() {
  if (!gServer) return;
  HTTPUpload& upload = gServer->upload();

  if (upload.status == UPLOAD_FILE_START) {
    gUploadOk = false;
    gBytesWritten = 0;
    closeUploadFile();

    if (!uploadAuthOk()) {
      setStatus("auth required");
      Serial.println("wifi_ota: upload missing/bad ?k=");
      return;
    }

    char app[33] = {0};
    char fname[49] = {0};
    String appArg;
    if (gServer->hasArg("app")) appArg = gServer->arg("app");
    if (!sanitizeAppName(appArg.c_str(), app, sizeof(app))) {
      setStatus("bad app name");
      Serial.println("wifi_ota: bad app name");
      return;
    }
    if (!sanitizeFileName(upload.filename.c_str(), fname, sizeof(fname))) {
      setStatus("bad filename");
      Serial.println("wifi_ota: bad filename");
      return;
    }

    ensureAppsDir();
    char folder[64];
    snprintf(folder, sizeof(folder), "/apps/%s", app);
    if (!LittleFS.exists(folder)) {
      if (!LittleFS.mkdir(folder)) {
        setStatus("mkdir failed");
        Serial.printf("wifi_ota: mkdir %s failed\n", folder);
        return;
      }
    }

    snprintf(gUploadPath, sizeof(gUploadPath), "%s/%s", folder, fname);
    gUploadFile = LittleFS.open(gUploadPath, "w");
    if (!gUploadFile) {
      setStatus("open failed");
      Serial.printf("wifi_ota: open %s failed\n", gUploadPath);
      gUploadPath[0] = '\0';
      return;
    }
    bumpActivity();
    setStatus("receiving");
    Serial.printf("wifi_ota: receiving %s\n", gUploadPath);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (gUploadFile) {
      size_t n = gUploadFile.write(upload.buf, upload.currentSize);
      gBytesWritten += (uint32_t)n;
      bumpActivity();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (gUploadFile) {
      gUploadFile.close();
      gUploadOk = (gUploadPath[0] != '\0');
      setStatus(gUploadOk ? "uploaded" : "upload failed");
      Serial.printf("wifi_ota: done %s (%u bytes)\n", gUploadPath,
                    (unsigned)gBytesWritten);
      if (gUploadOk) {
        const char* slash = strrchr(gUploadPath, '/');
        const char* base = slash ? slash + 1 : gUploadPath;
        if (strcmp(base, "main.lua") == 0 && slash) {
          char folder[64];
          size_t flen = (size_t)(slash - gUploadPath);
          if (flen + 1 < sizeof(folder)) {
            memcpy(folder, gUploadPath, flen);
            folder[flen] = '\0';
            const char* appId = strrchr(folder, '/');
            appId = appId ? appId + 1 : folder;
            ensureAppJson(folder, appId);
          }
        }
      }
      bumpActivity();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    closeUploadFile();
    gUploadOk = false;
    setStatus("aborted");
  }
}


static void handleUploadSdPost() {
  if (!gServer) return;
  if (!uploadAuthOk()) {
    closeUploadFile();
    gUploadOk = false;
    setStatus("auth required");
    gServer->send(401, "text/plain",
                  "Missing or bad session token. Open http://IP/?k=<session password> "
                  "or add &k=<password> to the upload_sd URL.\n");
    return;
  }
  if (gUploadOk) {
    char msg[192];
    snprintf(msg, sizeof(msg), "OK wrote %s (%u bytes)\n", gUploadPath,
             (unsigned)gBytesWritten);
    setStatus("sd uploaded");
    gServer->send(200, "text/plain", msg);
  } else {
    setStatus("sd upload failed");
    gServer->send(400, "text/plain",
                  "SD upload failed — check path=, card mounted, and free space\n");
  }
}

static void handleUploadSdFile() {
  if (!gServer) return;
  HTTPUpload& upload = gServer->upload();

  if (upload.status == UPLOAD_FILE_START) {
    gUploadOk = false;
    gBytesWritten = 0;
    closeUploadFile();
    gUploadToSd = true;

    if (!uploadAuthOk()) {
      setStatus("auth required");
      Serial.println("wifi_ota: upload_sd missing/bad ?k=");
      gUploadToSd = false;
      return;
    }

    if (!gServer->hasArg("path")) {
      setStatus("path required");
      Serial.println("wifi_ota: upload_sd missing path=");
      gUploadToSd = false;
      return;
    }

    char dest[160] = {0};
    if (!sanitizeSdDestPath(gServer->arg("path").c_str(), dest, sizeof(dest))) {
      setStatus("bad sd path");
      Serial.printf("wifi_ota: bad sd path '%s'\n",
                    gServer->arg("path").c_str());
      gUploadToSd = false;
      return;
    }

    if (!sdHostMount()) {
      setStatus("sd mount failed");
      Serial.println("wifi_ota: sd mount failed");
      gUploadToSd = false;
      return;
    }

    strncpy(gUploadPath, dest, sizeof(gUploadPath) - 1);
    gUploadPath[sizeof(gUploadPath) - 1] = '\0';

    if (!sdHostOpenWrite(gUploadPath, &gUploadFile)) {
      setStatus("sd open failed");
      Serial.printf("wifi_ota: sd open %s failed\n", gUploadPath);
      gUploadPath[0] = '\0';
      gUploadToSd = false;
      return;
    }

    bumpActivity();
    setStatus("sd receiving");
    Serial.printf("wifi_ota: sd receiving %s\n", gUploadPath);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (gUploadFile && gUploadToSd) {
      size_t n = gUploadFile.write(upload.buf, upload.currentSize);
      gBytesWritten += (uint32_t)n;
      bumpActivity();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (gUploadFile && gUploadToSd) {
      gUploadFile.close();
      gUploadOk = (gUploadPath[0] != '\0');
      setStatus(gUploadOk ? "sd uploaded" : "sd upload failed");
      Serial.printf("wifi_ota: sd done %s (%u bytes)\n", gUploadPath,
                    (unsigned)gBytesWritten);
      bumpActivity();
    }
    gUploadToSd = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    closeUploadFile();
    gUploadOk = false;
    setStatus("sd aborted");
  }
}



static void freeZipBuf() {
  if (gZipBuf) {
    free(gZipBuf);
    gZipBuf = nullptr;
  }
  gZipCap = 0;
  gZipLen = 0;
}

static void handleUploadZipPost() {
  if (!gServer) return;
  if (!uploadAuthOk()) {
    freeZipBuf();
    gZipOk = false;
    setStatus("auth required");
    gServer->send(401, "text/plain", "auth required\n");
    return;
  }
  if (gZipOk) {
    char msg[192];
    snprintf(msg, sizeof(msg), "OK unzipped app=%s dest=%s (%u zip bytes) %s\n",
             gZipApp, gZipDest, (unsigned)gZipLen, gZipErr);
    setStatus("zip installed");
    appsHostRescan();
    gServer->send(200, "text/plain", msg);
  } else {
    setStatus("zip failed");
    char msg[160];
    snprintf(msg, sizeof(msg), "Zip install failed: %s\n", gZipErr[0] ? gZipErr : "unknown");
    gServer->send(400, "text/plain", msg);
  }
  freeZipBuf();
}

static void handleUploadZipFile() {
  if (!gServer) return;
  HTTPUpload& upload = gServer->upload();
  if (upload.status == UPLOAD_FILE_START) {
    gZipOk = false;
    gZipErr[0] = '\0';
    freeZipBuf();
    gZipLen = 0;
    if (!uploadAuthOk()) {
      setStatus("auth required");
      return;
    }
    gZipApp[0] = '\0';
    strncpy(gZipDest, "lfs", sizeof(gZipDest) - 1);
    if (gServer->hasArg("app")) {
      if (!sanitizeAppName(gServer->arg("app").c_str(), gZipApp, sizeof(gZipApp))) {
        setStatus("bad app name");
        strncpy(gZipErr, "bad app name", sizeof(gZipErr) - 1);
        return;
      }
    } else {
      strncpy(gZipErr, "missing app=", sizeof(gZipErr) - 1);
      setStatus("missing app");
      return;
    }
    if (gServer->hasArg("dest")) {
      String d = gServer->arg("dest");
      if (d == "sd") strncpy(gZipDest, "sd", sizeof(gZipDest) - 1);
      else strncpy(gZipDest, "lfs", sizeof(gZipDest) - 1);
    }
    gZipCap = WATCHOS_ZIP_MAX_UNCOMPRESSED + 64 * 1024;
    gZipBuf = (uint8_t*)malloc(gZipCap);
    if (!gZipBuf) {
      strncpy(gZipErr, "oom", sizeof(gZipErr) - 1);
      setStatus("oom");
      return;
    }
    setStatus("receiving zip");
    bumpActivity();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!gZipBuf) return;
    if (gZipLen + upload.currentSize > gZipCap) {
      strncpy(gZipErr, "zip too large", sizeof(gZipErr) - 1);
      freeZipBuf();
      setStatus("zip too large");
      return;
    }
    memcpy(gZipBuf + gZipLen, upload.buf, upload.currentSize);
    gZipLen += upload.currentSize;
    bumpActivity();
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!gZipBuf || gZipLen == 0) {
      strncpy(gZipErr, "empty zip", sizeof(gZipErr) - 1);
      gZipOk = false;
      setStatus("zip failed");
      freeZipBuf();
      return;
    }
    gZipOk = zipInstallFromMemory(gZipBuf, gZipLen, gZipApp, gZipDest, gZipErr, sizeof(gZipErr));
    setStatus(gZipOk ? "zip installed" : "zip failed");
    Serial.printf("wifi_ota: zip app=%s dest=%s ok=%d err=%s\n", gZipApp, gZipDest,
                  (int)gZipOk, gZipErr);
    // keep buf until POST handler sends response, then free there
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    freeZipBuf();
    gZipOk = false;
    setStatus("zip aborted");
  }
}


static void handleAppsList() {
  if (!gServer) return;
  if (!uploadAuthOk()) {
    gServer->send(401, "text/plain",
                  "Missing or bad session token (?k=). Re-open the SoftAP page QR.\n");
    return;
  }
  appsHostRescan();
  // Build compact JSON: {"apps":[{"id":"...","name":"...","version":"...","protected":false},...]}
  String json = "{\"apps\":[";
  size_t n = appsHostCount();
  for (size_t i = 0; i < n; ++i) {
    const AppInfo* a = appsHostGet(i);
    if (!a) continue;
    if (i) json += ",";
    json += "{\"id\":\"";
    json += a->id;
    json += "\",\"name\":\"";
    json += a->name;
    json += "\",\"version\":\"";
    json += a->version;
    json += "\",\"protected\":";
    json += a->is_protected ? "true" : "false";
    json += ",\"fs\":\"";
    json += (a->storage == APP_STORAGE_SD) ? "sd" : "lfs";
    json += "\"}";
  }
  json += "]}";
  gServer->send(200, "application/json", json);
}

static void handleUninstall() {
  if (!gServer) return;
  if (!uploadAuthOk()) {
    gServer->send(401, "text/plain",
                  "Missing or bad session token (?k=). Re-open the SoftAP page QR.\n");
    return;
  }
  if (!gServer->hasArg("app")) {
    gServer->send(400, "text/plain", "app= required\n");
    return;
  }
  String appArg = gServer->arg("app");
  char id[APP_ID_MAX];
  if (!appsHostSanitizeId(appArg.c_str(), id, sizeof(id))) {
    gServer->send(400, "text/plain", "bad app id\n");
    return;
  }

  bumpActivity();
  setStatus("uninstalling");
  bool ok = appsHostRemove(id);
  if (ok) {
    char msg[96];
    snprintf(msg, sizeof(msg), "OK uninstalled %s\n", id);
    setStatus("uninstalled");
    bumpActivity();
    gServer->send(200, "text/plain", msg);
  } else {
    // Distinguish protected vs missing vs other via a second probe.
    char folder[APP_PATH_MAX];
    snprintf(folder, sizeof(folder), "/apps/%s", id);
    char sdFolder[APP_PATH_MAX];
    snprintf(sdFolder, sizeof(sdFolder), "/sd/apps/%s", id);
    bool stillThere = LittleFS.exists(folder);
    if (!stillThere && (sdHostReady() || sdHostMount())) {
      stillThere = sdHostExists(sdFolder);
    }
    if (stillThere) {
      setStatus("uninstall refused");
      gServer->send(403, "text/plain",
                    "Uninstall refused (protected or delete failed)\n");
    } else {
      setStatus("not found");
      gServer->send(404, "text/plain", "App not found\n");
    }
  }
}

static void handleCaptiveRedirect() {
  if (!gServer) return;
  char loc[48];
  snprintf(loc, sizeof(loc), "http://%s/", gIp);
  gServer->sendHeader("Location", loc, true);
  gServer->send(302, "text/plain", "Redirecting to WatchOS Install\n");
}

static void handleNotFound() {
  if (!gServer) return;
  if (gMode == WIFI_OTA_MODE_SOFTAP && gServer->method() == HTTP_GET) {
    handleCaptiveRedirect();
    return;
  }
  gServer->send(404, "text/plain", "Not found. Try GET / or /apps, POST /upload /upload_sd /uninstall\n");
}

static void startHttpServer() {
  if (!gServer) {
    gServer = new WebServer(80);
  }
  gServer->on("/", HTTP_GET, handleRoot);
  gServer->on("/status", HTTP_GET, handleStatus);
  gServer->on("/apps", HTTP_GET, handleAppsList);
  gServer->on("/upload", HTTP_POST, handleUploadPost, handleUploadFile);
  gServer->on("/upload_sd", HTTP_POST, handleUploadSdPost, handleUploadSdFile);
  gServer->on("/upload_zip", HTTP_POST, handleUploadZipPost, handleUploadZipFile);
  gServer->on("/uninstall", HTTP_POST, handleUninstall);
  gServer->on("/wifi", HTTP_POST, handleWifiSave);
  gServer->on("/generate_204", HTTP_GET, handleCaptiveRedirect);
  gServer->on("/gen_204", HTTP_GET, handleCaptiveRedirect);
  gServer->on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect);
  gServer->on("/library/test/success.html", HTTP_GET, handleCaptiveRedirect);
  gServer->on("/ncsi.txt", HTTP_GET, handleCaptiveRedirect);
  gServer->on("/connecttest.txt", HTTP_GET, handleCaptiveRedirect);
  gServer->on("/canonical.html", HTTP_GET, handleCaptiveRedirect);
  gServer->onNotFound(handleNotFound);
  gServer->begin();
  gHttpUp = true;

  if (MDNS.begin("watchos")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("wifi_ota: mDNS watchos.local");
  }
}

static void stopHttpServer() {
  if (gServer) {
    gServer->stop();
  }
  gHttpUp = false;
  MDNS.end();
}

static void fillIpFrom(const IPAddress& ip) {
  snprintf(gIp, sizeof(gIp), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void radioOffHard() {
  stopHttpServer();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

bool wifiOtaStaConfigured(void) {
  if (!gStaLoaded) loadStaCreds();
  return gStaSsid[0] != '\0';
}

const char* wifiOtaStaSavedSsid(void) {
  if (!gStaLoaded) loadStaCreds();
  return gStaSsid;
}

bool wifiOtaStartSoftAp(void) {
  if (gState != WIFI_OTA_OFF) {
    gDeadlineMs = millis() + (uint32_t)WATCHOS_WIFI_OTA_TIMEOUT_SEC * 1000u;
    setStatus("already on");
    return true;
  }

  if (!gStaLoaded) loadStaCreds();
  genPassword();
  setStatus("starting");
  gBytesWritten = 0;
  gUploadOk = false;
  gHttpUp = false;
  closeUploadFile();
  gMode = WIFI_OTA_MODE_SOFTAP;

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(WATCHOS_WIFI_OTA_SSID, gPassword);
  if (!ok) {
    Serial.println("wifi_ota: softAP failed");
    radioOffHard();
    setStatus("softAP failed");
    gState = WIFI_OTA_OFF;
    gMode = WIFI_OTA_MODE_NONE;
    return false;
  }

  fillIpFrom(WiFi.softAPIP());
  startHttpServer();

  gState = WIFI_OTA_IDLE;
  gLastByteMs = 0;
  gStaConnectDeadlineMs = 0;
  gDeadlineMs = millis() + (uint32_t)WATCHOS_WIFI_OTA_TIMEOUT_SEC * 1000u;
  setStatus("waiting");
  Serial.printf("wifi_ota: SoftAP %s  pass=%s  ip=%s  timeout=%us\n",
                WATCHOS_WIFI_OTA_SSID, gPassword, gIp,
                (unsigned)WATCHOS_WIFI_OTA_TIMEOUT_SEC);
  return true;
}

bool wifiOtaStartSta(void) {
  if (gState != WIFI_OTA_OFF) {
    gDeadlineMs = millis() + (uint32_t)WATCHOS_WIFI_OTA_TIMEOUT_SEC * 1000u;
    setStatus("already on");
    return true;
  }

  if (!wifiOtaStaConfigured()) {
    setStatus("save home Wi-Fi via SoftAP");
    Serial.println("wifi_ota: no saved home Wi-Fi — provision from SoftAP page");
    return false;
  }

  genPassword();
  setStatus("connecting");
  gBytesWritten = 0;
  gUploadOk = false;
  gHttpUp = false;
  closeUploadFile();
  gMode = WIFI_OTA_MODE_STA;
  snprintf(gIp, sizeof(gIp), "0.0.0.0");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("watchos");
  WiFi.begin(gStaSsid, gStaPass);

  gState = WIFI_OTA_CONNECTING;
  gLastByteMs = 0;
  gDeadlineMs = millis() + (uint32_t)WATCHOS_WIFI_OTA_TIMEOUT_SEC * 1000u;
  gStaConnectDeadlineMs =
      millis() + (uint32_t)WATCHOS_WIFI_STA_CONNECT_SEC * 1000u;
  Serial.printf("wifi_ota: STA joining %s  token=%s  connect_timeout=%us\n",
                gStaSsid, gPassword,
                (unsigned)WATCHOS_WIFI_STA_CONNECT_SEC);
  return true;
}

bool wifiOtaStart(void) { return wifiOtaStartSoftAp(); }

void wifiOtaStop(void) {
  closeUploadFile();
  radioOffHard();
  gState = WIFI_OTA_OFF;
  gMode = WIFI_OTA_MODE_NONE;
  gDeadlineMs = 0;
  gStaConnectDeadlineMs = 0;
  gLastByteMs = 0;
  snprintf(gIp, sizeof(gIp), "0.0.0.0");
  gPassword[0] = '\0';
  setStatus("off");
  Serial.println("wifi_ota: radio off");
}

static void finishStaConnected() {
  fillIpFrom(WiFi.localIP());
  startHttpServer();
  gState = WIFI_OTA_IDLE;
  setStatus("waiting");
  Serial.printf("wifi_ota: STA ok  ssid=%s  ip=%s  token=%s\n",
                gStaSsid, gIp, gPassword);
}

void wifiOtaPoll(void) {
  if (gState == WIFI_OTA_OFF) return;

  if (gState == WIFI_OTA_CONNECTING && gMode == WIFI_OTA_MODE_STA) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      finishStaConnected();
    } else if (gStaConnectDeadlineMs != 0 &&
               (int32_t)(millis() - gStaConnectDeadlineMs) >= 0) {
      Serial.printf("wifi_ota: STA connect timeout (status=%d)\n", (int)st);
      wifiOtaStop();
      setStatus("STA connect failed");
      return;
    } else {
      setStatus("connecting");
    }
  }

  if (gHttpUp && gServer) gServer->handleClient();

  if (gState == WIFI_OTA_ACTIVE && gLastByteMs != 0 &&
      (millis() - gLastByteMs) > 2500u) {
    gState = WIFI_OTA_IDLE;
    if (strcmp(gStatus, "receiving") == 0) setStatus("waiting");
  }

  if (gDeadlineMs != 0 && (int32_t)(millis() - gDeadlineMs) >= 0) {
    Serial.println("wifi_ota: timeout → stop");
    setStatus("timeout");
    wifiOtaStop();
  }
}

WifiOtaState wifiOtaState(void) { return gState; }
WifiOtaMode wifiOtaMode(void) { return gMode; }
bool wifiOtaIsRunning(void) { return gState != WIFI_OTA_OFF; }

const char* wifiOtaSsid(void) {
  if (gMode == WIFI_OTA_MODE_STA) {
    if (!gStaLoaded) loadStaCreds();
    return gStaSsid[0] ? gStaSsid : WATCHOS_WIFI_OTA_SSID;
  }
  return WATCHOS_WIFI_OTA_SSID;
}

const char* wifiOtaPassword(void) { return gPassword; }
const char* wifiOtaIp(void) { return gIp; }
const char* wifiOtaStatusLine(void) { return gStatus; }

uint32_t wifiOtaRemainingSec(void) {
  if (gState == WIFI_OTA_OFF || gDeadlineMs == 0) return 0;
  uint32_t now = millis();
  if ((int32_t)(gDeadlineMs - now) <= 0) return 0;
  return (gDeadlineMs - now) / 1000u;
}
