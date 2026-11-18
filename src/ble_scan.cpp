// ble_scan.cpp - turns BLE advertisements into CaptureRows.
//
// PASSIVE SCAN ONLY: setActiveScan(false) means we never send a scan request, and the
// peripheral/broadcaster roles are compiled out (platformio.ini), so this code cannot
// advertise or connect. We only hear what devices broadcast anyway.
//
// Each address is logged at most once per BLE_LOG_MIN_MS so a busy building doesn't fill the
// flash; the detector sees the same rows, so its own cooldowns are unaffected.
#include "ble_scan.h"
#include "axon_shape.h"
#include "config.h"

#if ENABLE_BLE
#include <NimBLEDevice.h>
#include "sniffer.h"
#include "watchlist_data.h"

static NimBLEScan* gScan = nullptr;

struct SeenSlot { uint32_t key; uint32_t lastMs; };
static SeenSlot gSeen[512];

// Rate limit per address AND per advert content: the same unchanged advert is logged once
// per BLE_LOG_MIN_MS, but a changed one (new name, company, services, Find My state) goes
// through at once so the detector never misses it.
static bool logDue(const CaptureRow& row, uint32_t now) {
  uint32_t h = 2166136261u;  // FNV-1a
  auto mix = [&h](const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  };
  mix(row.mac, 6);
  mix(row.ssid, strnlen(row.ssid, sizeof(row.ssid)));
  mix(&row.bleCompany, sizeof(row.bleCompany));
  mix(row.bleSvc16, row.bleSvcCount * sizeof(row.bleSvc16[0]));
  const uint8_t flags[4] = {row.bleSvc128Mask, row.bleDataMask, row.findMyType,
                            static_cast<uint8_t>(row.findMySeparated | (row.bleHasCompany << 1) | (row.bleAxonShape << 2))};
  mix(flags, sizeof(flags));
  if (!h) h = 1;
  SeenSlot& s = gSeen[h & 511];
  if (s.key == h && now - s.lastMs < BLE_LOG_MIN_MS) return false;
  s.key = h;
  s.lastMs = now;
  return true;
}

// 16-bit SIG UUIDs arrive either as "0xfd5a" or in full 128-bit form
// "0000fd5a-0000-1000-8000-00805f9b34fb". Returns false for real 128-bit vendor UUIDs.
static bool asSvc16(const std::string& s, uint16_t& out) {
  if (s.size() == 6 && s.rfind("0x", 0) == 0) {
    out = static_cast<uint16_t>(strtoul(s.c_str() + 2, nullptr, 16));
    return true;
  }
  if (s.size() == 36 && s.compare(0, 4, "0000") == 0 &&
      s.compare(8, 28, "-0000-1000-8000-00805f9b34fb") == 0) {
    out = static_cast<uint16_t>(strtoul(s.substr(4, 4).c_str(), nullptr, 16));
    return true;
  }
  return false;
}

static void noteUuid(CaptureRow& row, const NimBLEUUID& uuid) {
  const std::string s = uuid.toString();
  uint16_t v;
  if (asSvc16(s, v)) {
    if (row.bleSvcCount < 6) row.bleSvc16[row.bleSvcCount++] = v;
    return;
  }
  for (size_t i = 0; i < kWlSvc128Count && i < 8; i++) {
    if (s == kWlSvc128[i].s) row.bleSvc128Mask |= 1u << i;
  }
}

static void noteData(CaptureRow& row, const std::string& data) {
  for (size_t i = 0; i < kWlDataCount && i < 8; i++) {
    if (data.find(kWlData[i].s) != std::string::npos) row.bleDataMask |= 1u << i;
  }
}

// Apple Find My: company 0x004C, payload type 0x12, length 0x02 (near owner) or 0x19
// (separated). Status byte bits 4-5 = device type: 1 AirTag, 2 third-party accessory.
// iPhones, Macs and AirPods use other values and are deliberately not flagged.
static void noteFindMy(CaptureRow& row, const std::string& md) {
  if (md.size() < 5 || static_cast<uint8_t>(md[2]) != 0x12) return;
  const uint8_t plen = static_cast<uint8_t>(md[3]);
  if ((plen != 0x02 && plen != 0x19) || md.size() < 4u + plen) return;
  const uint8_t type = (static_cast<uint8_t>(md[4]) >> 4) & 0x03;
  if (type == 1 || type == 2) {
    row.findMyType = type;
    row.findMySeparated = plen == 0x19;
  }
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    snifferCountBle();
    CaptureRow row = {};
    row.ms = millis();
    row.type = FrameType::BLE_ADV;

    const NimBLEAddress& addr = d->getAddress();
    unsigned b[6];
    if (sscanf(addr.toString().c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return;
    for (int i = 0; i < 6; i++) row.mac[i] = static_cast<uint8_t>(b[i]);

    const bool isPublic = addr.getType() == BLE_ADDR_PUBLIC;
    row.locallyAdministered = !isPublic;  // random addresses carry no vendor prefix
    row.rssi = static_cast<int8_t>(d->getRSSI());
    if (d->haveName()) strlcpy(row.ssid, d->getName().c_str(), sizeof(row.ssid));

    if (d->haveManufacturerData()) {
      const std::string md = d->getManufacturerData();
      if (md.size() >= 2) {
        row.bleHasCompany = true;
        row.bleCompany = static_cast<uint8_t>(md[0]) | (static_cast<uint8_t>(md[1]) << 8);
        if (row.bleCompany == 0x004C) noteFindMy(row, md);
        if (row.bleCompany == kAxonCompanyId)
          row.bleAxonShape = axonMfrShape(reinterpret_cast<const uint8_t*>(md.data()), md.size());
      }
      noteData(row, md);
    }
    for (int i = 0; i < d->getServiceUUIDCount(); i++) noteUuid(row, d->getServiceUUID(i));
    for (int i = 0; i < d->getServiceDataCount(); i++) {
      noteUuid(row, d->getServiceDataUUID(i));
      noteData(row, d->getServiceData(i));
    }
    if (!logDue(row, row.ms)) return;  // after parsing, so a changed advert isn't suppressed

    // Human-readable summary for the CSV 'extra' column.
    char* p = row.extra;
    size_t left = sizeof(row.extra);
    auto add = [&](const char* fmt, auto... args) {
      const int n = snprintf(p, left, fmt, args...);
      if (n > 0 && static_cast<size_t>(n) < left) { p += n; left -= n; }
    };
    add("%s", isPublic ? "pub" : "rnd");
    if (row.bleHasCompany) add(" mfr=%04x", row.bleCompany);
    if (row.bleHasCompany && row.bleCompany == kAxonCompanyId) add(" axon_shape=%u", row.bleAxonShape ? 1u : 0u);
    for (uint8_t i = 0; i < row.bleSvcCount; i++) add(i ? ",%04x" : " svc=%04x", row.bleSvc16[i]);
    if (row.findMyType) add(" findmy=%u%s", row.findMyType, row.findMySeparated ? "sep" : "");
    if (row.bleSvc128Mask) add(" s128=%x", row.bleSvc128Mask);
    if (row.bleDataMask) add(" data=%x", row.bleDataMask);

    snifferPush(row);
  }
};

static ScanCallbacks gCallbacks;

void bleBegin() {
  NimBLEDevice::init("");
  gScan = NimBLEDevice::getScan();
  gScan->setScanCallbacks(&gCallbacks, false);
  gScan->setActiveScan(false);  // passive: never transmit a scan request
  gScan->setInterval(100);
  gScan->setWindow(100);
  gScan->setMaxResults(0);      // callbacks only, keep nothing in RAM
}

void bleScanSlice(uint32_t ms) {
  if (!gScan) return;
  gScan->start(ms, false, true);
  vTaskDelay(pdMS_TO_TICKS(ms + 10));
  if (gScan->isScanning()) gScan->stop();
}

#else
void bleBegin() {}
void bleScanSlice(uint32_t) {}
#endif
