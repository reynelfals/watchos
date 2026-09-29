/**
 * Meshtastic BLE PhoneAPI central (NimBLE).
 * Watch = central; SenseCAP T1000-E / any Meshtastic radio = peripheral.
 * GATT only (no UART). Callbacks never touch Lua/LVGL — queue + tick.
 */
#include "ble_mesh_host.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>

#include <NimBLEDevice.h>

// ---- UUIDs (Meshtastic MeshBluetoothService) ----
static const char* kSvcUuid = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
static const char* kFromRadioUuid = "2c55e69e-4993-11ed-b878-0242ac120002";
static const char* kToRadioUuid = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static const char* kFromNumUuid = "ed9da18c-a800-4f66-a670-aa7547e34453";

static constexpr size_t kMaxPkt = 512;
static constexpr int kMaxScan = 12;
static constexpr int kMsgQ = 8;
static constexpr uint32_t kConfigNonce = 69420u;  // Stage-1 / config-only

enum class St : uint8_t { Idle, Scanning, Connecting, Connected, Error };

struct ScanHit {
  char addr[24];
  char name[28];
  int rssi;
  uint8_t addrType;
};

// Static state — avoid large stack locals (Sound Deck lesson).
static bool gReady = false;
static St gSt = St::Idle;
static char gErr[48] = {0};
static ScanHit gHits[kMaxScan];
static int gHitN = 0;
static uint32_t gScanUntilMs = 0;

static NimBLEClient* gClient = nullptr;
static NimBLERemoteCharacteristic* gFromRadio = nullptr;
static NimBLERemoteCharacteristic* gToRadio = nullptr;
static NimBLERemoteCharacteristic* gFromNum = nullptr;

static volatile bool gNeedDrain = false;
static bool gHandshakeDone = false;
static uint32_t gPasskey = 0;
static bool gUsePasskey = false;
static char gPeerAddr[24] = {0};
static uint32_t gPktId = 1;
static uint8_t gChannel = 1;  // default FamilyCh secondary (index 1)
static bool gAlertBeep = true;     // mesh_alert_beep (NVS al_beep)
static bool gAlertVibrate = true;  // mesh_alert_vibrate (NVS al_vib)


// Background alert (inbound TEXT) — main loop only reads these.
static volatile bool gBgAlertPending = false;
static char gBgAlertText[96] = {0};

static BleMeshMsg gMsgQ[kMsgQ];
static int gMsgHead = 0, gMsgTail = 0, gMsgN = 0;

static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

static void setErr(const char* m) {
  strncpy(gErr, m ? m : "error", sizeof(gErr) - 1);
  gErr[sizeof(gErr) - 1] = '\0';
  gSt = St::Error;
}

static void pushMsg(uint32_t from, const char* text, uint32_t t) {
  if (!text || !text[0]) return;
  portENTER_CRITICAL(&gMux);
  if (gMsgN >= kMsgQ) {
    // Drop oldest.
    gMsgHead = (gMsgHead + 1) % kMsgQ;
    gMsgN--;
  }
  BleMeshMsg* m = &gMsgQ[gMsgTail];
  m->from = from;
  m->rx_time = t;
  strncpy(m->text, text, sizeof(m->text) - 1);
  m->text[sizeof(m->text) - 1] = '\0';
  gMsgTail = (gMsgTail + 1) % kMsgQ;
  gMsgN++;
  // Inbound TEXT (not local echo from=0): arm background vibrate/banner.
  if (from != 0) {
    strncpy(gBgAlertText, text, sizeof(gBgAlertText) - 1);
    gBgAlertText[sizeof(gBgAlertText) - 1] = '\0';
    gBgAlertPending = true;
  }
  portEXIT_CRITICAL(&gMux);
}

// ---- Minimal protobuf helpers (encode only what we need) ----
static size_t pbWriteVarint(uint8_t* out, size_t cap, uint32_t v) {
  size_t n = 0;
  while (v >= 0x80) {
    if (n >= cap) return 0;
    out[n++] = (uint8_t)((v & 0x7f) | 0x80);
    v >>= 7;
  }
  if (n >= cap) return 0;
  out[n++] = (uint8_t)v;
  return n;
}

static size_t pbTag(uint8_t* out, size_t cap, uint32_t field, uint8_t wt) {
  return pbWriteVarint(out, cap, (field << 3) | wt);
}

static size_t pbPutVarintField(uint8_t* out, size_t cap, uint32_t field,
                               uint32_t v) {
  size_t n = pbTag(out, cap, field, 0);
  if (!n) return 0;
  size_t m = pbWriteVarint(out + n, cap - n, v);
  return m ? n + m : 0;
}

static size_t pbPutFixed32(uint8_t* out, size_t cap, uint32_t field,
                           uint32_t v) {
  size_t n = pbTag(out, cap, field, 5);
  if (!n || n + 4 > cap) return 0;
  out[n] = (uint8_t)(v);
  out[n + 1] = (uint8_t)(v >> 8);
  out[n + 2] = (uint8_t)(v >> 16);
  out[n + 3] = (uint8_t)(v >> 24);
  return n + 4;
}

static size_t pbPutBytes(uint8_t* out, size_t cap, uint32_t field,
                         const uint8_t* data, size_t len) {
  size_t n = pbTag(out, cap, field, 2);
  if (!n) return 0;
  size_t m = pbWriteVarint(out + n, cap - n, (uint32_t)len);
  if (!m || n + m + len > cap) return 0;
  memcpy(out + n + m, data, len);
  return n + m + len;
}

static size_t pbPutMsg(uint8_t* out, size_t cap, uint32_t field,
                       const uint8_t* msg, size_t msgLen) {
  return pbPutBytes(out, cap, field, msg, msgLen);
}

/** ToRadio { want_config_id = nonce } */
static size_t encodeWantConfig(uint8_t* out, size_t cap, uint32_t nonce) {
  return pbPutVarintField(out, cap, 3, nonce);
}

/** ToRadio { packet = MeshPacket{ to=bcast, channel, decoded=Data{port=1,payload}, id, hop=3 } } */
static size_t encodeTextToRadio(uint8_t* out, size_t cap, const char* text,
                                uint32_t pktId) {
  static uint8_t dataBuf[280];
  static uint8_t pktBuf[360];
  size_t tlen = text ? strlen(text) : 0;
  if (tlen == 0 || tlen > 220) return 0;

  size_t dn = 0;
  size_t m = pbPutVarintField(dataBuf + dn, sizeof(dataBuf) - dn, 1, 1);  // portnum
  if (!m) return 0;
  dn += m;
  m = pbPutBytes(dataBuf + dn, sizeof(dataBuf) - dn, 2, (const uint8_t*)text,
                 tlen);
  if (!m) return 0;
  dn += m;

  size_t pn = 0;
  m = pbPutFixed32(pktBuf + pn, sizeof(pktBuf) - pn, 2, 0xFFFFFFFFu);  // to
  if (!m) return 0;
  pn += m;
  // MeshPacket.channel (uint32 field 3); omit only when 0 (proto3 default).
  if (gChannel != 0) {
    m = pbPutVarintField(pktBuf + pn, sizeof(pktBuf) - pn, 3, gChannel);
    if (!m) return 0;
    pn += m;
  }
  m = pbPutMsg(pktBuf + pn, sizeof(pktBuf) - pn, 4, dataBuf, dn);  // decoded
  if (!m) return 0;
  pn += m;
  m = pbPutFixed32(pktBuf + pn, sizeof(pktBuf) - pn, 6, pktId);  // id
  if (!m) return 0;
  pn += m;
  m = pbPutVarintField(pktBuf + pn, sizeof(pktBuf) - pn, 9, 3);  // hop_limit
  if (!m) return 0;
  pn += m;

  return pbPutMsg(out, cap, 1, pktBuf, pn);  // ToRadio.packet
}

// ---- Decode FromRadio for text packets only ----
static bool pbReadVarint(const uint8_t* p, size_t len, size_t* off, uint32_t* out) {
  uint32_t v = 0;
  int shift = 0;
  while (*off < len) {
    uint8_t b = p[(*off)++];
    v |= (uint32_t)(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      *out = v;
      return true;
    }
    shift += 7;
    if (shift > 28) return false;
  }
  return false;
}

static bool pbSkip(const uint8_t* p, size_t len, size_t* off, uint8_t wt) {
  if (wt == 0) {
    uint32_t v;
    return pbReadVarint(p, len, off, &v);
  }
  if (wt == 1) {
    if (*off + 8 > len) return false;
    *off += 8;
    return true;
  }
  if (wt == 2) {
    uint32_t l;
    if (!pbReadVarint(p, len, off, &l)) return false;
    if (*off + l > len) return false;
    *off += l;
    return true;
  }
  if (wt == 5) {
    if (*off + 4 > len) return false;
    *off += 4;
    return true;
  }
  return false;
}

static uint32_t readFixed32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void decodeDataText(const uint8_t* p, size_t len, uint32_t from,
                           uint32_t rxTime) {
  size_t off = 0;
  uint32_t port = 0;
  const uint8_t* payload = nullptr;
  size_t plen = 0;
  while (off < len) {
    uint32_t tag;
    if (!pbReadVarint(p, len, &off, &tag)) break;
    uint32_t field = tag >> 3;
    uint8_t wt = (uint8_t)(tag & 7);
    if (field == 1 && wt == 0) {
      if (!pbReadVarint(p, len, &off, &port)) break;
    } else if (field == 2 && wt == 2) {
      uint32_t l;
      if (!pbReadVarint(p, len, &off, &l)) break;
      if (off + l > len) break;
      payload = p + off;
      plen = l;
      off += l;
    } else {
      if (!pbSkip(p, len, &off, wt)) break;
    }
  }
  if (port == 1 && payload && plen > 0) {
    char tmp[200];
    size_t n = plen < sizeof(tmp) - 1 ? plen : sizeof(tmp) - 1;
    memcpy(tmp, payload, n);
    tmp[n] = '\0';
    pushMsg(from, tmp, rxTime);
  }
}

static void decodeMeshPacket(const uint8_t* p, size_t len) {
  size_t off = 0;
  uint32_t from = 0;
  uint32_t rxTime = 0;
  while (off < len) {
    uint32_t tag;
    if (!pbReadVarint(p, len, &off, &tag)) break;
    uint32_t field = tag >> 3;
    uint8_t wt = (uint8_t)(tag & 7);
    if (field == 1 && wt == 5) {
      if (off + 4 > len) break;
      from = readFixed32(p + off);
      off += 4;
    } else if (field == 7 && wt == 5) {
      if (off + 4 > len) break;
      rxTime = readFixed32(p + off);
      off += 4;
    } else if (field == 4 && wt == 2) {
      uint32_t l;
      if (!pbReadVarint(p, len, &off, &l)) break;
      if (off + l > len) break;
      decodeDataText(p + off, l, from, rxTime);
      off += l;
    } else {
      if (!pbSkip(p, len, &off, wt)) break;
    }
  }
}

static void decodeFromRadio(const uint8_t* p, size_t len) {
  if (!p || len == 0) return;
  size_t off = 0;
  while (off < len) {
    uint32_t tag;
    if (!pbReadVarint(p, len, &off, &tag)) break;
    uint32_t field = tag >> 3;
    uint8_t wt = (uint8_t)(tag & 7);
    if (field == 2 && wt == 2) {
      uint32_t l;
      if (!pbReadVarint(p, len, &off, &l)) break;
      if (off + l > len) break;
      decodeMeshPacket(p + off, l);
      off += l;
    } else if (field == 7 && wt == 0) {
      uint32_t id;
      if (!pbReadVarint(p, len, &off, &id)) break;
      if (id == kConfigNonce) gHandshakeDone = true;
    } else {
      if (!pbSkip(p, len, &off, wt)) break;
    }
  }
}

// ---- NimBLE callbacks ----
class MeshClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    (void)c;
    Serial.println("ble_mesh: GATT connected");
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    (void)c;
    Serial.printf("ble_mesh: disconnect reason=%d\n", reason);
    gFromRadio = gToRadio = gFromNum = nullptr;
    if (gSt == St::Connected || gSt == St::Connecting) {
      gSt = St::Idle;
      gHandshakeDone = false;
    }
  }
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    uint32_t pin = gUsePasskey ? gPasskey : 0;
    Serial.printf("ble_mesh: passkey entry inject=%u\n", (unsigned)pin);
    NimBLEDevice::injectPassKey(connInfo, pin);
  }
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
    Serial.printf("ble_mesh: confirm passkey %u\n", (unsigned)pass_key);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    Serial.printf("ble_mesh: auth complete bonded=%d\n",
                  (int)connInfo.isBonded());
  }
};

class MeshScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    if (!adv) return;
    if (!adv->isAdvertisingService(NimBLEUUID(kSvcUuid))) return;
    if (gHitN >= kMaxScan) return;
    std::string a = adv->getAddress().toString();
    // Dedupe by address
    for (int i = 0; i < gHitN; ++i) {
      if (strcasecmp(gHits[i].addr, a.c_str()) == 0) {
        gHits[i].rssi = adv->getRSSI();
        return;
      }
    }
    ScanHit* h = &gHits[gHitN++];
    strncpy(h->addr, a.c_str(), sizeof(h->addr) - 1);
    h->addr[sizeof(h->addr) - 1] = '\0';
    h->name[0] = '\0';
    if (adv->haveName()) {
      std::string n = adv->getName();
      strncpy(h->name, n.c_str(), sizeof(h->name) - 1);
      h->name[sizeof(h->name) - 1] = '\0';
    }
    h->rssi = adv->getRSSI();
    h->addrType = adv->getAddress().getType();
  }
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    (void)reason;
    if (gSt == St::Scanning) gSt = St::Idle;
    Serial.printf("ble_mesh: scan end hits=%d\n", gHitN);
  }
};

static MeshClientCallbacks gClientCb;
static MeshScanCallbacks gScanCb;

static void onFromNumNotify(NimBLERemoteCharacteristic* /*c*/, uint8_t* /*d*/,
                            size_t /*l*/, bool /*isNotify*/) {
  gNeedDrain = true;
}

static bool writeToRadio(const uint8_t* data, size_t len) {
  if (!gToRadio || !data || len == 0) return false;
  return gToRadio->writeValue(data, len, true /* response */);
}

static void drainFromRadio(int maxReads) {
  if (!gFromRadio) return;
  static uint8_t buf[kMaxPkt];
  for (int i = 0; i < maxReads; ++i) {
    NimBLEAttValue v = gFromRadio->readValue();
    size_t n = v.size();
    if (n == 0) break;
    if (n > kMaxPkt) n = kMaxPkt;
    memcpy(buf, v.data(), n);
    decodeFromRadio(buf, n);
  }
}

static bool discoverAndHandshake() {
  if (!gClient || !gClient->isConnected()) return false;
  NimBLERemoteService* svc = gClient->getService(kSvcUuid);
  if (!svc) {
    setErr("no_mesh_svc");
    return false;
  }
  gFromRadio = svc->getCharacteristic(kFromRadioUuid);
  gToRadio = svc->getCharacteristic(kToRadioUuid);
  gFromNum = svc->getCharacteristic(kFromNumUuid);
  if (!gFromRadio || !gToRadio) {
    setErr("no_chars");
    return false;
  }

  // Prefer larger MTU for 512-byte PhoneAPI packets (connect already requests).
  gClient->exchangeMTU();

  static uint8_t want[16];
  size_t wn = encodeWantConfig(want, sizeof(want), kConfigNonce);
  if (!wn || !writeToRadio(want, wn)) {
    setErr("want_config_fail");
    return false;
  }

  // Drain config stream (bounded).
  gHandshakeDone = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 8000) {
    drainFromRadio(8);
    if (gHandshakeDone) break;
    delay(20);
  }
  // Even without sentinel, empty drain means we can proceed (config-only ok).
  drainFromRadio(16);

  if (gFromNum && gFromNum->canNotify()) {
    if (!gFromNum->subscribe(true, onFromNumNotify)) {
      Serial.println("ble_mesh: FromNum subscribe failed (poll-only)");
    }
  }
  gNeedDrain = true;
  gSt = St::Connected;
  Serial.println("ble_mesh: connected + handshake drained");
  return true;
}

static void clearClientChars() {
  gFromRadio = gToRadio = gFromNum = nullptr;
  gHandshakeDone = false;
}

// ---- Public API ----
bool bleMeshInit(void) {
  if (gReady) return true;
  // Keep classic BT memory free; NimBLE only.
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEDevice::setMTU(517);

  gClient = NimBLEDevice::createClient();
  if (!gClient) {
    setErr("no_client");
    return false;
  }
  gClient->setClientCallbacks(&gClientCb, false);
  gClient->setConnectTimeout(15 * 1000);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&gScanCb, false);
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(50);

  // Restore last PIN + channel (default channel index 1 = FamilyCh).
  Preferences prefs;
  if (prefs.begin("ble_mesh", true)) {
    gPasskey = prefs.getUInt("pin", 0);
    gChannel = (uint8_t)prefs.getUChar("ch", 1);
    if (gChannel > 7) gChannel = 1;
    gAlertBeep = prefs.getBool("al_beep", true);
    gAlertVibrate = prefs.getBool("al_vib", true);
    prefs.end();
  } else {
    gChannel = 1;
    gAlertBeep = true;
    gAlertVibrate = true;
  }

  gPktId = (uint32_t)(esp_random() | 1u);
  gReady = true;
  gSt = St::Idle;
  Serial.println("ble_mesh: NimBLE central ready");
  return true;
}

bool bleMeshReady(void) { return gReady; }

void bleMeshTick(void) {
  if (!gReady) return;
  if (gSt == St::Scanning && gScanUntilMs && (int32_t)(millis() - gScanUntilMs) >= 0) {
    NimBLEDevice::getScan()->stop();
    gSt = St::Idle;
    gScanUntilMs = 0;
  }
  if (gSt == St::Connected && gNeedDrain) {
    gNeedDrain = false;
    drainFromRadio(12);
  }
}

const char* bleMeshStatus(void) {
  switch (gSt) {
    case St::Idle:
      return "idle";
    case St::Scanning:
      return "scanning";
    case St::Connecting:
      return "connecting";
    case St::Connected:
      return "connected";
    case St::Error:
      return gErr[0] ? gErr : "error";
    default:
      return "idle";
  }
}

bool bleMeshScanStart(uint32_t timeout_ms) {
  if (!gReady) return false;
  if (gSt == St::Connected || gSt == St::Connecting) return false;
  if (timeout_ms == 0) timeout_ms = 4000;
  if (timeout_ms > 15000) timeout_ms = 15000;
  gHitN = 0;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->stop();
  scan->clearResults();
  gSt = St::Scanning;
  gScanUntilMs = millis() + timeout_ms;
  // duration in ms for NimBLE 2.x start(duration, isContinue, restart)
  bool ok = scan->start(timeout_ms, false, true);
  if (!ok) {
    setErr("scan_fail");
    return false;
  }
  return true;
}

int bleMeshScanCount(void) { return gHitN; }

bool bleMeshScanAt(int i, char* addrOut, size_t addrLen, char* nameOut,
                   size_t nameLen, int* rssiOut) {
  if (i < 0 || i >= gHitN) return false;
  if (addrOut && addrLen) {
    strncpy(addrOut, gHits[i].addr, addrLen - 1);
    addrOut[addrLen - 1] = '\0';
  }
  if (nameOut && nameLen) {
    strncpy(nameOut, gHits[i].name, nameLen - 1);
    nameOut[nameLen - 1] = '\0';
  }
  if (rssiOut) *rssiOut = gHits[i].rssi;
  return true;
}

bool bleMeshConnect(const char* addr, const char* pin) {
  if (!gReady || !addr || !addr[0]) return false;
  if (gSt == St::Connected || gSt == St::Connecting) {
    bleMeshDisconnect();
  }
  NimBLEDevice::getScan()->stop();

  gUsePasskey = false;
  gPasskey = 0;
  if (pin && pin[0]) {
    // Accept 4–6 digit PIN.
    char* end = nullptr;
    unsigned long v = strtoul(pin, &end, 10);
    if (end == pin || (end && *end) || v > 999999ul) {
      setErr("bad_pin");
      return false;
    }
    gPasskey = (uint32_t)v;
    gUsePasskey = true;
    Preferences prefs;
    if (prefs.begin("ble_mesh", false)) {
      prefs.putUInt("pin", gPasskey);
      prefs.end();
    }
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  } else {
    // NO_PIN / open: no MITM requirement.
    NimBLEDevice::setSecurityAuth(false, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  }

  strncpy(gPeerAddr, addr, sizeof(gPeerAddr) - 1);
  gPeerAddr[sizeof(gPeerAddr) - 1] = '\0';
  gSt = St::Connecting;
  clearClientChars();

  // Resolve address type from last scan if possible.
  uint8_t addrType = BLE_ADDR_PUBLIC;
  for (int i = 0; i < gHitN; ++i) {
    if (strcasecmp(gHits[i].addr, addr) == 0) {
      addrType = gHits[i].addrType;
      break;
    }
  }
  NimBLEAddress bleAddr(std::string(addr), addrType);
  if (!gClient) {
    setErr("no_client");
    return false;
  }
  if (gClient->isConnected()) gClient->disconnect();

  bool ok = gClient->connect(bleAddr, true /* deleteAttributes */, false,
                             true /* exchangeMTU */);
  if (!ok && addrType == BLE_ADDR_PUBLIC) {
    // Retry as random (common for Meshtastic / privacy addresses).
    NimBLEAddress bleAddrRnd(std::string(addr), BLE_ADDR_RANDOM);
    ok = gClient->connect(bleAddrRnd, true, false, true);
  }
  if (!ok) {
    setErr("connect_fail");
    return false;
  }
  if (!discoverAndHandshake()) {
    if (gClient->isConnected()) gClient->disconnect();
    return false;
  }
  return true;
}

bool bleMeshDisconnect(void) {
  if (!gReady) return true;
  NimBLEDevice::getScan()->stop();
  clearClientChars();
  if (gClient && gClient->isConnected()) {
    gClient->disconnect();
  }
  gSt = St::Idle;
  gNeedDrain = false;
  return true;
}

bool bleMeshConnected(void) {
  return gReady && gSt == St::Connected && gClient && gClient->isConnected();
}

bool bleMeshSendText(const char* utf8, char* err, size_t errLen) {
  auto fail = [&](const char* m) {
    if (err && errLen) {
      strncpy(err, m, errLen - 1);
      err[errLen - 1] = '\0';
    }
    return false;
  };
  if (!bleMeshConnected()) return fail("not_connected");
  if (!utf8 || !utf8[0]) return fail("empty");
  static uint8_t pkt[kMaxPkt];
  if (gPktId == 0) gPktId = 1;
  size_t n = encodeTextToRadio(pkt, sizeof(pkt), utf8, gPktId++);
  if (!n) return fail("encode");
  if (!writeToRadio(pkt, n)) return fail("write");
  // Echo locally so UI shows outbound immediately.
  pushMsg(0, utf8, (uint32_t)(millis() / 1000u));
  gNeedDrain = true;
  if (err && errLen) err[0] = '\0';
  return true;
}

bool bleMeshPoll(BleMeshMsg* out) {
  if (!out) return false;
  portENTER_CRITICAL(&gMux);
  if (gMsgN <= 0) {
    portEXIT_CRITICAL(&gMux);
    return false;
  }
  *out = gMsgQ[gMsgHead];
  gMsgHead = (gMsgHead + 1) % kMsgQ;
  gMsgN--;
  portEXIT_CRITICAL(&gMux);
  return true;
}

void bleMeshSetChannel(uint8_t ch) {
  if (ch > 7) ch = 7;
  gChannel = ch;
  Preferences prefs;
  if (prefs.begin("ble_mesh", false)) {
    prefs.putUChar("ch", gChannel);
    prefs.end();
  }
}

uint8_t bleMeshGetChannel(void) { return gChannel; }

bool bleMeshTakeBgAlert(char* out, size_t outLen) {
  if (!gBgAlertPending) return false;
  portENTER_CRITICAL(&gMux);
  if (!gBgAlertPending) {
    portEXIT_CRITICAL(&gMux);
    return false;
  }
  gBgAlertPending = false;
  if (out && outLen) {
    strncpy(out, gBgAlertText, outLen - 1);
    out[outLen - 1] = '\0';
  }
  portEXIT_CRITICAL(&gMux);
  return true;
}

bool bleMeshAlertBeepEnabled(void) { return gAlertBeep; }

void bleMeshAlertBeepSet(bool on) {
  gAlertBeep = on;
  Preferences prefs;
  if (prefs.begin("ble_mesh", false)) {
    prefs.putBool("al_beep", gAlertBeep);
    prefs.end();
  }
}

bool bleMeshAlertVibrateEnabled(void) { return gAlertVibrate; }

void bleMeshAlertVibrateSet(bool on) {
  gAlertVibrate = on;
  Preferences prefs;
  if (prefs.begin("ble_mesh", false)) {
    prefs.putBool("al_vib", gAlertVibrate);
    prefs.end();
  }
}
