// detector.cpp - watchlist matching, per-device tracking and alerting.
//
// For each row: collect every rule it matches and keep the strongest tier. Then:
//   * possible_low  -> DETECT row in the log only
//   * possible      -> log + OLED/LED, no sound
//   * likely / high -> log + OLED/LED + category beep (unless muted or category silenced)
// One alert per device per DETECT_COOLDOWN_MS, except when its tier goes up.
//
// Every rule is an unverified community claim (see watchlist/README.md). A DETECT row
// records the evidence - which rule fired - so Phase 2 can measure how often each rule
// is right, instead of trusting it.
#include "detector.h"
#include "breadcrumbs.h"
#include "config.h"
#include "logger.h"
#include "ui.h"
#include "watchlist.h"
#include "watchlist_data.h"
#include "alert_test_data.h"
#include "axon_shape.h"

struct Tracked {
  bool used;
  uint8_t mac[6];
  uint8_t tier, cat;
  bool ble;
  bool vendorOnly;
  const char* vendor;
  char rule[40];
  float rssiAvg;
  uint16_t count;
  uint32_t firstMs, lastMs, lastAlertMs;
  bool show;            // its best match is one the screen shows (not benign, not a hidden category)
  float rssiRef;        // rssiAvg at the start of the current trend window
  uint32_t refMs;
  int8_t trend;         // dB change of rssiAvg over the last window: + getting stronger (closer)
};

static const uint32_t TREND_WINDOW_MS = 4000;

static Tracked gTracked[DETECT_MAX_TRACKED];
static DetectView gView = {};
static bool gMuted = false;

static const bool kCategoryBeeps[WL_CAT_COUNT] = {
    BEEP_CAMERA, BEEP_ACOUSTIC, BEEP_BODYCAM, BEEP_FLEET_MODEM, BEEP_GEAR, BEEP_TACTICAL, BEEP_TRACKER};

// ---- Matching ------------------------------------------------------------------------
struct Best {
  uint8_t tier = WL_NONE, cat = 0;
  const char* vendor = nullptr;
  char rule[40] = "";
  bool vendorOnly = false, benign = false;
  void consider(uint8_t t, uint8_t c, const char* v, const char* r,
                bool oui = false, bool quiet = false) {
    // At equal rank, a specific signature outranks an OUI; a live AP lead
    // must not be hidden by an unrelated benign sender's equally ranked OUI.
    if (t < tier || (t == tier && !(vendorOnly && !oui) &&
                    !(vendorOnly && oui && benign && !quiet))) return;
    tier = t; cat = c; vendor = v;
    vendorOnly = oui; benign = oui && quiet;
    strlcpy(rule, r, sizeof(rule));
  }
};

static bool ieSigMatches(const char* sig, const char* rule, const char* ssid);

static char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static bool inClass(char c, uint8_t tail) {
  if (tail == WL_TAIL_DIGIT) return c >= '0' && c <= '9';
  c = lowerAscii(c);
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// The regex subset from watchlist.h, including a literal suffix after the class.
static bool nameMatches(const WlName& r, const char* s) {
  const size_t litLen = strlen(r.lit), suffixLen = strlen(r.suffix), n = strlen(s);
  if (n < litLen + suffixLen) return false;
  const bool ci = r.flags & WL_F_CI;
  const size_t lastStart = (r.flags & WL_F_START) ? 0 : n - litLen;
  for (size_t p = 0; p <= lastStart; p++) {
    size_t i = 0;
    while (i < litLen && (ci ? lowerAscii(s[p + i]) == lowerAscii(r.lit[i]) : s[p + i] == r.lit[i])) i++;
    if (i < litLen) continue;
    size_t q = p + litLen, k = 0;
    if (r.tail != WL_TAIL_NONE) while (k < r.tmax && s[q + k] && inClass(s[q + k], r.tail)) k++;
    if (k < r.tmin) continue;
    q += k;
    if (q + suffixLen > n) continue;
    i = 0;
    while (i < suffixLen && (ci ? lowerAscii(s[q + i]) == lowerAscii(r.suffix[i])
                               : s[q + i] == r.suffix[i])) i++;
    if (i != suffixLen) continue;
    if ((r.flags & WL_F_END) && s[q + suffixLen] != '\0') continue;
    return true;
  }
  return false;
}

// SSIDs identify an AP only in BEACON rows. A directed probe names a network
// the sender seeks; it must not label that sender a benign AP or a fleet router.
static bool benignNow(const CaptureRow& row) {
  if (!row.locallyAdministered) {
    for (size_t i = 0; i < kWlBenignOuiCount; i++)
      if (memcmp(row.mac, kWlBenignOui[i].b, 3) == 0) return true;
  }
  if (row.type == FrameType::BEACON && row.ssid[0]) {
    for (size_t i = 0; i < kWlBenignNameCount; i++)
      if (nameMatches(kWlBenignName[i], row.ssid)) return true;
  }
  return false;
}

struct BenignDevice { bool used, ble; uint8_t mac[6]; uint32_t seenMs; };
static BenignDevice gBenign[DETECT_MAX_TRACKED];
static bool knownBenign(const uint8_t* mac, bool ble) {
  for (const auto& b : gBenign)
    if (b.used && b.ble == ble && memcmp(b.mac, mac, 6) == 0) return true;
  return false;
}
static void rememberBenign(const CaptureRow& row, uint32_t now) {
  BenignDevice* oldest = &gBenign[0];
  for (auto& b : gBenign) {
    if (b.used && b.ble == (row.type == FrameType::BLE_ADV) && memcmp(b.mac,row.mac,6) == 0) {
      b.seenMs = now; return;
    }
    if (!b.used) { oldest = &b; break; }
    if (now - b.seenMs > now - oldest->seenMs) oldest = &b;
  }
  *oldest = {};
  oldest->used = true; oldest->ble = row.type == FrameType::BLE_ADV;
  memcpy(oldest->mac,row.mac,6); oldest->seenMs = now;
}

static void matchRow(const CaptureRow& row, Best& best, bool useCache = true) {
  const bool ble = row.type == FrameType::BLE_ADV;
  char rule[40];

  for (size_t i = 0; i < kWlMacCount; i++) {
    const WlMac& m = kWlMac[i];
    if (memcmp(row.mac, m.b, 6) == 0) best.consider(m.tier, m.cat, m.vendor, m.rule);
  }
  // Randomized Wi-Fi MACs and random BLE addresses have no registered prefix.
  if (!row.locallyAdministered) {
    for (size_t i = 0; i < kWlOuiCount; i++) {
      const WlOui& o = kWlOui[i];
      if (memcmp(row.mac, o.b, 3) == 0) {
        snprintf(rule, sizeof(rule), "oui:%02x:%02x:%02x", o.b[0], o.b[1], o.b[2]);
        best.consider(o.tier, o.cat, o.vendor, rule, true,
                      benignNow(row) || (useCache && knownBenign(row.mac,ble)));
      }
    }
  }
  if (row.ssid[0]) {
    const uint8_t kind = ble ? WL_KIND_BLE_NAME : WL_KIND_SSID;
    for (size_t i = 0; i < kWlNameCount; i++) {
      const WlName& r = kWlName[i];
      if ((r.kind == kind || (r.kind == WL_KIND_FLEET_SSID && row.type == FrameType::BEACON)) &&
          nameMatches(r, row.ssid)) best.consider(r.tier, r.cat, r.vendor, r.rule);
    }
  }
  // Probe IE fingerprint: exact match on the whole element list (see sniffer.cpp).
  if (row.type == FrameType::PROBE_REQ && strncmp(row.extra, "ie=", 3) == 0) {
    for (size_t i = 0; i < kWlIeSigCount; i++) {
      const WlStr& s = kWlIeSig[i];
      if (ieSigMatches(row.extra + 3, s.s, row.ssid)) best.consider(s.tier, s.cat, s.vendor, s.rule);
    }
  }
  if (!ble) return;

  if (row.bleHasCompany) {
    for (size_t i = 0; i < kWlCompanyCount; i++) {
      const WlU16& c = kWlCompany[i];
      if (c.id == row.bleCompany)
        best.consider(axonCompanyTier(c.id, c.tier, row.bleAxonShape, WL_POSSIBLE_LOW), c.cat, c.vendor, c.rule);
    }
  }
  for (uint8_t s = 0; s < row.bleSvcCount; s++) {
    const uint16_t svc = row.bleSvc16[s];
    for (size_t i = 0; i < kWlSvc16Count; i++) {
      const WlU16& u = kWlSvc16[i];
      if (u.id == svc) best.consider(u.tier, u.cat, u.vendor, u.rule);
    }
    for (size_t i = 0; i < kWlSvcRangeCount; i++) {
      const WlRange& r = kWlSvcRange[i];
      if (svc >= r.lo && svc <= r.hi) best.consider(r.tier, r.cat, r.vendor, r.rule);
    }
  }
  for (size_t i = 0; i < kWlSvc128Count && i < 8; i++) {
    const WlStr& u = kWlSvc128[i];
    if (row.bleSvc128Mask & (1u << i)) best.consider(u.tier, u.cat, u.vendor, u.rule);
  }
  for (size_t i = 0; i < kWlDataCount && i < 8; i++) {
    const WlStr& d = kWlData[i];
    if (row.bleDataMask & (1u << i)) best.consider(d.tier, d.cat, d.vendor, d.rule);
  }
  // A tag next to its owner is just someone's AirTag. "Separated" is the one that matters.
  if (row.findMyType) {
    best.consider(row.findMySeparated ? kWlFindMyTier : static_cast<uint8_t>(WL_POSSIBLE_LOW), WL_CAT_TRACKER,
                  row.findMyType == 1 ? "Apple AirTag" : "Find My accessory",
                  row.findMySeparated ? "findmy:separated" : "findmy:near owner");
  }
}

// Keep this policy identical to tools/watchlist.py ie_sig_matches(): full exact
// match, otherwise wildcard probes only, with 0..4 permitted plain prefix tags.
// No vendor element may precede the whole anchor element. The suffix stays exact.
static const char kFlockIeAnchor[] = "221:506f9a16030103";

static bool ieSigMatches(const char* sig, const char* rule, const char* ssid) {
  if (strcmp(sig, rule) == 0) return true;
  if (ssid[0]) return false;
  const char* ruleAnchor = strstr(rule, kFlockIeAnchor);
  constexpr size_t anchorLen = sizeof(kFlockIeAnchor) - 1;
  if (!ruleAnchor || strncmp(rule, "2,12,127,", 9) != 0 || ruleAnchor[-1] != ',' ||
      (ruleAnchor[anchorLen] != ',' && ruleAnchor[anchorLen] != '\0')) return false;
  static const char* const plain[] = {"1","2","3","12","45","50","127","191"};
  unsigned count = 0;
  for (const char* p = sig; *p;) {
    const char* comma = strchr(p, ',');
    const size_t n = comma ? static_cast<size_t>(comma - p) : strlen(p);
    if (n == anchorLen && memcmp(p,kFlockIeAnchor,n) == 0) return strcmp(p,ruleAnchor) == 0;
    if (++count > 4) return false;
    bool allowed = false;
    for (const char* tag : plain) if (strlen(tag) == n && memcmp(p,tag,n) == 0) allowed = true;
    if (!allowed || !comma) return false;
    p = comma + 1;
  }
  return false;
}

// DATA rows: a device talking on a network whose access point is a watched vendor (e.g. a
// laptop on a patrol car's Cradlepoint). The match is on the AP, not the sender, so it is
// one tier weaker and the rule says "bssid>" to keep that role visible in the evidence.
static void matchDataBssid(const CaptureRow& row, Best& best, bool useCache = true) {
  const char* p = strstr(row.extra, "bssid=");
  unsigned b[6];
  if (!p || sscanf(p + 6, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return;
  CaptureRow ap = {};
  ap.type = FrameType::BEACON;  // match it like an AP's address
  for (int i = 0; i < 6; i++) ap.mac[i] = static_cast<uint8_t>(b[i]);
  if (memcmp(ap.mac, row.mac, 6) == 0) return;  // the sender IS the AP: already matched
  ap.locallyAdministered = ap.mac[0] & 0x02;
  Best viaAp;
  matchRow(ap, viaAp, useCache);
  if (viaAp.tier == WL_NONE) return;
  char rule[48];  // Best::consider() trims it to 39 chars
  snprintf(rule, sizeof(rule), "bssid>%s", viaAp.rule);
  best.consider(viaAp.tier > WL_POSSIBLE_LOW ? viaAp.tier - 1 : WL_POSSIBLE_LOW,
                viaAp.cat, viaAp.vendor, rule, viaAp.vendorOnly, viaAp.benign);
}

static bool showBest(const Best& b) {
  return b.tier >= WL_POSSIBLE && !b.benign && (b.cat != WL_CAT_TRACKER || SCREEN_TRACKER);
}
static bool soundBest(const Best& b) {
  return b.tier >= WL_LIKELY && !b.vendorOnly && kCategoryBeeps[b.cat];
}

// ---- Tracking ------------------------------------------------------------------------
// Identity = radio + address, so a Wi-Fi MAC and a BLE address that happen to be equal
// stay separate devices.
static Tracked* track(const uint8_t* mac, bool ble, uint32_t now) {
  Tracked* oldest = &gTracked[0];
  for (Tracked& t : gTracked) {
    if (t.used && t.ble == ble && memcmp(t.mac, mac, 6) == 0) return &t;
    if (!t.used) { oldest = &t; break; }
    if (t.lastMs < oldest->lastMs) oldest = &t;
  }
  *oldest = {};
  oldest->used = true;
  oldest->ble = ble;
  memcpy(oldest->mac, mac, 6);
  oldest->firstMs = now;
  return oldest;
}

static uint32_t gAlertCount = 0;
uint32_t detectorAlertCount() { return gAlertCount; }

void detectorProcess(const CaptureRow& row) {
  BreadcrumbScope stage(Crumb::Detector);
  if (row.type != FrameType::PROBE_REQ && row.type != FrameType::BEACON && row.type != FrameType::BLE_ADV &&
      row.type != FrameType::DATA) return;
  Best best;
  const uint32_t now = millis();
  if (benignNow(row)) rememberBenign(row,now);
  // Stop refreshing an OUI-only card as soon as this same radio/address is
  // identified as benign. Stronger evidence is never cleared by the benign list.
  if (gView.valid && gView.vendorOnly && gView.ble == (row.type == FrameType::BLE_ADV) &&
      memcmp(gView.mac,row.mac,6) == 0 && knownBenign(row.mac,gView.ble)) gView.valid = false;
  matchRow(row, best);  // the sender's own address (DATA: the transmitter, addr2)
  if (row.type == FrameType::DATA) matchDataBssid(row, best);
  if (best.benign && gView.valid && gView.vendorOnly && gView.ble == (row.type == FrameType::BLE_ADV) &&
      memcmp(gView.mac,row.mac,6) == 0) gView.valid = false;
  if (best.tier == WL_NONE) return;

  Tracked* t = track(row.mac, row.type == FrameType::BLE_ADV, now);
  t->rssiAvg = t->count ? 0.7f * t->rssiAvg + 0.3f * row.rssi : row.rssi;
  t->count++;
  t->lastMs = now;
  if (!t->refMs) { t->rssiRef = t->rssiAvg; t->refMs = now ? now : 1; }
  else if (now - t->refMs >= TREND_WINDOW_MS) {
    const float d = t->rssiAvg - t->rssiRef;
    t->trend = static_cast<int8_t>(d > 60 ? 60 : (d < -60 ? -60 : d));
    t->rssiRef = t->rssiAvg; t->refMs = now ? now : 1;
  }
  const bool upgraded = best.tier > t->tier || (best.tier == t->tier && t->vendorOnly && !best.vendorOnly);
  if (upgraded) {
    t->tier = best.tier;
    t->cat = best.cat;
    t->vendor = best.vendor;
    t->vendorOnly = best.vendorOnly;
    strlcpy(t->rule, best.rule, sizeof(t->rule));
    t->show = showBest(best);
  }

  // Keep the on-screen view live while the same device keeps being heard.
  if (gView.valid && gView.ble == t->ble && memcmp(gView.mac, row.mac, 6) == 0 &&
      showBest(best) && best.tier >= gView.tier && (!best.vendorOnly || gView.vendorOnly)) {
    gView.atMs = now;
    gView.rssi = static_cast<int8_t>(t->rssiAvg);
    gView.count = t->count;
    gView.trend = t->trend;
  }

  const bool due = t->lastAlertMs == 0 || now - t->lastAlertMs >= DETECT_COOLDOWN_MS || upgraded;
  if (!due) return;
  t->lastAlertMs = now ? now : 1;

  // Evidence row in the session file.
  CaptureRow d = {};
  d.ms = now;
  d.type = FrameType::DETECT;
  loggerCountDetection(best.cat);
  if (!best.benign) gAlertCount++;
  d.channel = row.channel;
  memcpy(d.mac, row.mac, 6);
  d.locallyAdministered = row.locallyAdministered;
  d.rssi = row.rssi;
  strlcpy(d.ssid, best.vendor, sizeof(d.ssid));
  snprintf(d.extra, sizeof(d.extra), "tier=%s cat=%s rule=%s n=%u%s", wlTierName(best.tier),
           wlCategoryName(best.cat), best.rule, t->count, best.benign ? " quiet=benign" : "");
  loggerAppendRow(d);

  Serial.printf("[detect] %-12s %-11s %-28s %02X:%02X:%02X:%02X:%02X:%02X %s rssi %d  %s%s\n",
                wlTierName(best.tier), wlCategoryName(best.cat), best.vendor, row.mac[0], row.mac[1], row.mac[2],
                row.mac[3], row.mac[4], row.mac[5], t->ble ? "BLE " : "WiFi", row.rssi, best.rule,
                best.benign ? " quiet=benign" : "");

  if (showBest(best)) {
    gView.valid = true;
    gView.atMs = now;
    memcpy(gView.mac, row.mac, 6);
    gView.tier = best.tier;
    gView.cat = best.cat;
    gView.ble = t->ble;
    gView.vendorOnly = best.vendorOnly;
    gView.vendor = best.vendor;
    strlcpy(gView.rule, best.rule, sizeof(gView.rule));
    gView.rssi = static_cast<int8_t>(t->rssiAvg);
    gView.count = t->count;
    gView.trend = t->trend;
  }
  if (soundBest(best) && !gMuted) uiBeepCategory(best.cat);
}

void detectorSelfTest(Print& out) {
  unsigned passed = 0;
  for (const auto& c : kAlertTests) {
    CaptureRow row = {};
    row.type = c.type; memcpy(row.mac,c.mac,6); row.locallyAdministered = row.mac[0] & 2;
    strlcpy(row.ssid,c.ssid,sizeof(row.ssid)); strlcpy(row.extra,c.extra,sizeof(row.extra));
    Best b; matchRow(row,b,false);
    if (row.type == FrameType::DATA) matchDataBssid(row,b,false);
    const bool ok = b.tier == c.tier && strncmp(b.rule,c.rule,strlen(c.rule)) == 0 &&
                    benignNow(row) == c.benign && showBest(b) == c.screen && soundBest(b) == c.sound;
    if (ok) passed++;
    else out.printf("[alert-test] FAIL %s tier=%s rule=%s benign=%u screen=%u sound=%u\n",
                    c.name,wlTierName(b.tier),b.rule,benignNow(row),showBest(b),soundBest(b));
  }
  out.printf("[alert-test] %u/%u passed (synthetic; no RF, logging or live-state changes)\n",
             passed,static_cast<unsigned>(sizeof(kAlertTests)/sizeof(kAlertTests[0])));
}

const DetectView& detectorView() { return gView; }

DetectSummary detectorSummary(uint32_t windowMs) {
  DetectSummary s = {};
  const uint32_t now = millis();
  const Tracked* top[4] = {};
  // "Stronger" = higher confidence first, then the louder signal (what a radar detector would put first).
  auto stronger = [](const Tracked* a, const Tracked* b) {
    return a->tier > b->tier || (a->tier == b->tier && a->rssiAvg > b->rssiAvg);
  };
  for (const Tracked& t : gTracked) {
    if (!t.used || !t.show || now - t.lastMs > windowMs) continue;
    s.count++;
    if (t.tier >= WL_LIKELY && !t.vendorOnly) s.likely++;
    const Tracked* cur = &t;
    for (int k = 0; k < 4 && cur; k++) {
      if (!top[k] || stronger(cur, top[k])) { const Tracked* tmp = top[k]; top[k] = cur; cur = tmp; }
    }
  }
  for (int k = 0; k < 4 && top[k]; k++) {
    const Tracked& t = *top[k];
    DetectEntry& e = s.top[s.n++];
    e.cat = t.cat; e.tier = t.tier; e.ble = t.ble; e.vendorOnly = t.vendorOnly; e.vendor = t.vendor;
    e.rssi = static_cast<int8_t>(t.rssiAvg); e.trend = t.trend; e.agoS = (now - t.lastMs) / 1000UL;
  }
  return s;
}
void detectorSetMuted(bool muted) { gMuted = muted; }
bool detectorMuted() { return gMuted; }

void detectorList(Print& out) {
  const uint32_t now = millis();
  uint16_t n = 0;
  for (const Tracked& t : gTracked) {
    if (!t.used) continue;
    out.printf("%-12s %-11s %-28s %02X:%02X:%02X:%02X:%02X:%02X %s x%-4u avg %4d  %3lus ago  %s\n",
               wlTierName(t.tier), wlCategoryName(t.cat), t.vendor, t.mac[0], t.mac[1], t.mac[2], t.mac[3],
               t.mac[4], t.mac[5], t.ble ? "BLE " : "WiFi", t.count, static_cast<int>(t.rssiAvg),
               static_cast<unsigned long>((now - t.lastMs) / 1000), t.rule);
    n++;
  }
  out.printf("%u device(s) matched this boot (%u rules loaded)%s\n", n, static_cast<unsigned>(kWlRuleCount),
             gMuted ? "  [muted]" : "");
}
