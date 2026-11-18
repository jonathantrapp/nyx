// sniffer.cpp - Wi-Fi promiscuous-mode capture of probe requests, beacons and data-frame
// senders, plus the schedule that shares the one 2.4 GHz radio with BLE.
//
// RECEIVE-ONLY. We put the radio in promiscuous (monitor) mode and listen. We never call
// esp_wifi_scan_start() (that SENDS probe requests), never start an AP, never connect, and
// never call any esp_wifi_80211_tx function. BLE scanning is passive (see ble_scan.cpp).
//
// Data flow:   radio -> promiscuousCb() [Wi-Fi task] / BLE callback [NimBLE task]
//                    -> FreeRTOS queue (in PSRAM)
//                    -> snifferPop() [called from loop() by the logger]
//
// Radio schedule (hop task): Wi-Fi channels 1..13 at `dwell` ms each, then one BLE slice
// of BLE_SLICE_MS with promiscuous mode paused. Espressif warns that sniffing and BLE
// scanning at the same time loses packets unpredictably, so they take turns explicitly.
#include "sniffer.h"
#include "breadcrumbs.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/idf_additions.h>
#include "ble_scan.h"

static QueueHandle_t gQueue = nullptr;
static SnifferStats gStats = {};
static volatile uint8_t gChannel = HOP_CHANNEL_MIN;
static volatile uint16_t gDwellMs = DEFAULT_DWELL_MS;
static volatile uint32_t gBeaconMinMs = DEFAULT_BEACON_MIN_MS;
static volatile uint32_t gDataMinMs = DEFAULT_DATA_MIN_MS;
static volatile bool gBleOn = ENABLE_BLE;

// Counters are written only by the radio tasks and read by loop(), so a plain
// read-modify-write is safe (a lone 32-bit store is atomic on the ESP32).
static inline void bump(volatile uint32_t& counter) { counter = counter + 1; }

// ---- Per-MAC rate limiter -----------------------------------------------------------
// Tiny direct-mapped table: hash(MAC) -> when we last logged it and how many frames it has
// sent since. A hash collision only means an occasional extra or missed row, which is fine.
// Beacons and data frames each get their own table. Only the Wi-Fi task touches them.
struct RateSlot { uint32_t key; uint32_t lastMs; uint16_t count; };
static const size_t kRateSlots = 512;  // power of two
static RateSlot gBeacons[kRateSlots];
static RateSlot gData[kRateSlots];

static uint32_t macHash(const uint8_t* mac) {  // FNV-1a
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
  return h ? h : 1;  // 0 is reserved for "empty slot"
}

// True when this MAC is due a row. *frames = frames heard from it since its last row,
// this one included.
static bool rateDue(RateSlot* table, const uint8_t* mac, uint32_t now, uint32_t limit, uint16_t* frames) {
  const uint32_t key = macHash(mac);
  RateSlot& s = table[key & (kRateSlots - 1)];
  if (s.key != key) { s.key = key; s.count = 0; s.lastMs = now - limit; }  // new MAC: log now
  if (s.count < 0xFFFF) s.count++;
  if (limit != 0 && (now - s.lastMs) < limit) return false;
  if (frames) *frames = s.count;
  s.count = 0;
  s.lastMs = now;
  return true;
}

static void macStr(char* out, const uint8_t* m) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ---- Probe request IE fingerprint -----------------------------------------------------
// Which information elements a probe request carries, in order, says a lot about the
// Wi-Fi chip and driver that sent it, even when the MAC is randomized. Same format as
// flock-you, so its drive-tested signatures match ours exactly:
//   * SSID (tag 0) is skipped (it's the per-probe part, logged in its own column)
//   * vendor-specific (tag 221) -> "221:" + first <= 8 payload bytes in hex (OUI + type)
//   * every other element -> its tag number in decimal
//   e.g. "1,50,3,45,127,191,221:0050f2080010"
// `complete` = every byte parsed as a clean element (no truncated or garbled tail).
static bool ieSigBuild(const uint8_t* ies, int len, char* out, size_t cap, bool* complete) {
  static const char hex[] = "0123456789abcdef";
  size_t pos = 0;
  int i = 0;
  out[0] = '\0';
  *complete = false;
  while (i + 2 <= len) {
    const uint8_t id = ies[i];
    const int elen = ies[i + 1];
    if (i + 2 + elen > len) return pos > 0;  // truncated element: stop, not complete
    const uint8_t* body = ies + i + 2;
    i += 2 + elen;
    if (id == 0) continue;
    char part[24];
    if (id == 221 && elen >= 4) {
      const int take = elen < 8 ? elen : 8;
      memcpy(part, "221:", 4);
      for (int k = 0; k < take; k++) { part[4 + 2 * k] = hex[body[k] >> 4]; part[5 + 2 * k] = hex[body[k] & 15]; }
      part[4 + 2 * take] = '\0';
    } else {
      snprintf(part, sizeof(part), "%u", id);
    }
    const size_t plen = strlen(part);
    if (pos + plen + 2 > cap) return pos > 0;  // out of room: keep what fits
    if (pos) out[pos++] = ',';
    memcpy(out + pos, part, plen + 1);
    pos += plen;
  }
  *complete = (i == len);
  return pos > 0;
}

static void ieSigProbe(const uint8_t* body, int bodyLen, char* out, size_t cap) {
  // The radio hands us the frame WITH its 4-byte FCS (checksum) on the end. Parse without
  // it first; fall back to the full length in case a driver already stripped it.
  char best[sizeof(CaptureRow::extra)] = "";
  bool bestComplete = false;
  const int lens[2] = {bodyLen - 4, bodyLen};
  for (int a = 0; a < 2; a++) {
    for (int skip = 0; skip <= 2; skip += 2) {  // also try past a leading empty SSID pair
      const int n = lens[a] - skip;
      if (n < 2 || (skip && !(body[0] == 0 && body[1] == 0))) continue;
      char sig[sizeof(best)];
      bool complete;
      if (!ieSigBuild(body + skip, n, sig, sizeof(sig) - 4, &complete)) continue;  // room for "ie="
      if ((complete && !bestComplete) || (complete == bestComplete && strlen(sig) > strlen(best))) {
        strlcpy(best, sig, sizeof(best));
        bestComplete = complete;
      }
    }
    if (bestComplete) break;
  }
  snprintf(out, cap, "ie=%s", best);  // raw, never rewritten: the log keeps what was sent
}

// ---- Frame parsing -------------------------------------------------------------------
// 802.11 header (management and data frames share it):
//   0-1  frame control   (byte 0: bits 2-3 = type, bits 4-7 = subtype;
//                         byte 1: bit 0 = to-DS, bit 1 = from-DS)
//   2-3  duration
//   4-9  addr1 (receiver)   10-15 addr2 (TRANSMITTER = the device we care about)
//   16-21 addr3             22-23 sequence control (upper 12 bits = sequence number)
//   24-  body. Probe request: tagged elements start here.
//         Beacon: 12 fixed bytes (timestamp, interval, capabilities), then elements.
// Tagged element = [tag][length][data...]. Tag 0 is the SSID.
static void fillCommon(CaptureRow& row, const wifi_promiscuous_pkt_t* pkt, const uint8_t* p) {
  row.channel = pkt->rx_ctrl.channel;
  memcpy(row.mac, p + 10, 6);
  row.locallyAdministered = (row.mac[0] & 0x02) != 0;
  row.rssi = pkt->rx_ctrl.rssi;
  row.seq = (static_cast<uint16_t>(p[22]) | (static_cast<uint16_t>(p[23]) << 8)) >> 4;
}

// A data frame: someone already connected to a network. We log who sent it (addr2),
// which network it belongs to (the BSSID) and which direction it went. The payload is
// encrypted and never looked at.
static void handleData(const wifi_promiscuous_pkt_t* pkt, const uint8_t* p, uint16_t len, uint32_t now) {
  bump(gStats.dataSeen);
  const uint32_t limit = gDataMinMs;
  if (limit == 0 || len < 24) return;
  if (p[10] & 0x01) return;  // group address as transmitter: malformed
  uint16_t frames = 0;
  if (!rateDue(gData, p + 10, now, limit, &frames)) return;

  CaptureRow row = {};
  row.type = FrameType::DATA;
  row.ms = now;
  fillCommon(row, pkt, p);
  const bool toDs = p[1] & 0x01, fromDs = p[1] & 0x02;
  const char* ds;
  const uint8_t* bssid;
  if (toDs && !fromDs)      { ds = "to_ap";   bssid = p + 4; }   // client -> its access point
  else if (!toDs && fromDs) { ds = "from_ap"; bssid = p + 10; }  // access point -> client
  else if (!toDs)           { ds = "adhoc";   bssid = p + 16; }  // direct / ad-hoc
  else                      { ds = "wds";     bssid = nullptr; } // mesh/bridge: 4 addresses
  char b[18] = "-", rx[18];
  if (bssid) macStr(b, bssid);
  macStr(rx, p + 4);
  snprintf(row.extra, sizeof(row.extra), "ds=%s bssid=%s rx=%s sub=%u n=%u", ds, b, rx,
           static_cast<unsigned>(p[0] >> 4), static_cast<unsigned>(frames));
  snifferPush(row);
}

static void promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* p = pkt->payload;
  const uint16_t len = pkt->rx_ctrl.sig_len;
  if (len < 24) return;
  const uint32_t now = millis();
  const uint8_t ftype = (p[0] >> 2) & 0x3;

  if (type == WIFI_PKT_DATA && ftype == 2) { handleData(pkt, p, len, now); return; }
  if (type != WIFI_PKT_MGMT || ftype != 0) return;
  const uint8_t subtype = p[0] >> 4;

  CaptureRow row = {};
  size_t pos;
  if (subtype == 4)      { row.type = FrameType::PROBE_REQ; pos = 24; }
  else if (subtype == 8) { row.type = FrameType::BEACON;    pos = 36; }
  else return;

  const uint8_t ch = pkt->rx_ctrl.channel;  // primary channel the frame was heard on
  bump(gStats.seen);
  if (ch < 15) bump(gStats.perChannel[ch]);

  row.ms = now;
  if (row.type == FrameType::BEACON && !rateDue(gBeacons, p + 10, now, gBeaconMinMs, nullptr)) return;
  fillCommon(row, pkt, p);

  // Walk the tagged elements looking for the SSID (tag 0). Bounds-checked: a malformed
  // frame must never make us read past the end of the buffer.
  while (pos + 2 <= len) {
    const uint8_t tag = p[pos];
    const uint8_t elen = p[pos + 1];
    if (pos + 2 + elen > len) break;
    if (tag == 0) {
      const uint8_t n = elen > 32 ? 32 : elen;
      memcpy(row.ssid, p + pos + 2, n);
      row.ssid[n] = '\0';
      break;
    }
    pos += 2 + elen;
  }
  if (row.type == FrameType::PROBE_REQ) ieSigProbe(p + 24, len - 24, row.extra, sizeof(row.extra));

  snifferPush(row);
}

// ---- Radio schedule ------------------------------------------------------------------
static void hopTask(void*) {
  for (;;) {
    for (uint8_t ch = HOP_CHANNEL_MIN; ch <= HOP_CHANNEL_MAX; ch++) {
      { BreadcrumbScope stage(Crumb::WifiChannel, CrumbTask::Radio);
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }
      gChannel = ch;
      { BreadcrumbScope stage(Crumb::WifiDwell, CrumbTask::Radio);
        vTaskDelay(pdMS_TO_TICKS(gDwellMs)); }
    }
#if ENABLE_BLE
    if (gBleOn) {
      { BreadcrumbScope stage(Crumb::BlePause, CrumbTask::Radio);
        esp_wifi_set_promiscuous(false); }
      gChannel = 0;
      { BreadcrumbScope stage(Crumb::BleSlice, CrumbTask::Radio);
        bleScanSlice(BLE_SLICE_MS); }
      { BreadcrumbScope stage(Crumb::BleResume, CrumbTask::Radio);
        esp_wifi_set_promiscuous(true); }
    }
#endif
  }
}

void snifferBegin() {
  // Rows are ~160 bytes, so the queue lives in PSRAM rather than scarce internal RAM.
  gQueue = xQueueCreateWithCaps(SNIFFER_QUEUE_LEN, sizeof(CaptureRow), MALLOC_CAP_SPIRAM);
  if (!gQueue) gQueue = xQueueCreate(SNIFFER_QUEUE_LEN / 4, sizeof(CaptureRow));

  // STA mode, never connected, nothing stored: the radio is up but silent.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  esp_wifi_set_ps(WIFI_PS_NONE);  // keep the receiver awake

  // Management frames (probes, beacons) and data frames. Control frames carry no
  // transmitter we can use, so they stay filtered out in the radio.
  wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&promiscuousCb);
  esp_wifi_set_channel(HOP_CHANNEL_MIN, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(true);

#if ENABLE_BLE
  bleBegin();
#endif
  xTaskCreate(hopTask, "hop", 4096, nullptr, 2, nullptr);
}

bool snifferPush(const CaptureRow& row) {
  if (gQueue && xQueueSend(gQueue, &row, 0) == pdTRUE) { bump(gStats.queued); return true; }
  bump(gStats.dropped);
  return false;
}

bool snifferPop(CaptureRow& out) { return xQueueReceive(gQueue, &out, 0) == pdTRUE; }
const SnifferStats& snifferStats() { return gStats; }
void snifferCountBle() { bump(gStats.bleSeen); }
uint8_t snifferChannel() { return gChannel; }
uint16_t snifferDwellMs() { return gDwellMs; }
void snifferSetDwellMs(uint16_t ms) { gDwellMs = ms < 20 ? 20 : ms; }
uint32_t snifferBeaconMinMs() { return gBeaconMinMs; }
void snifferSetBeaconMinMs(uint32_t ms) { gBeaconMinMs = ms; }
uint32_t snifferDataMinMs() { return gDataMinMs; }
void snifferSetDataMinMs(uint32_t ms) { gDataMinMs = ms; }
bool snifferBleEnabled() { return ENABLE_BLE && gBleOn; }
void snifferSetBleEnabled(bool on) { gBleOn = on; }
