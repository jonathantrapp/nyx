// capture.h - the one struct that travels from the radios to the CSV file.
#pragma once
#include <Arduino.h>

enum class FrameType : uint8_t {
  PROBE_REQ,  // a device asking "is network X here?"  (management subtype 4)
  BEACON,     // an access point announcing itself     (management subtype 8)
  BLE_ADV,    // a Bluetooth LE advertisement (heard passively)
  MARK,       // not a frame: you pressed the button / typed "mark"
  META,       // not a frame: session info (firmware, dwell, clock reference...)
  DETECT,     // not a frame: the detector matched a watchlist rule
  DATA,       // a data frame from a device already on a network (rate-limited per sender)
  SUMMARY,    // logger-generated minute aggregate; serialized by summary_writer.cpp
  GPS,        // not a frame: where the unit was (text in `extra`), or a camera-map event
};

inline const char* frameTypeName(FrameType t) {
  switch (t) {
    case FrameType::PROBE_REQ: return "PROBE_REQ";
    case FrameType::BEACON:    return "BEACON";
    case FrameType::BLE_ADV:   return "BLE_ADV";
    case FrameType::MARK:      return "MARK";
    case FrameType::META:      return "META";
    case FrameType::DETECT:    return "DETECT";
    case FrameType::DATA:      return "DATA";
    case FrameType::SUMMARY:   return "SUMMARY";
    case FrameType::GPS:       return "GPS";
  }
  return "?";
}

struct CaptureRow {
  uint32_t ms;          // millis() since boot - NOT a wall-clock time
  uint8_t channel;      // Wi-Fi channel the frame was heard on (0 for BLE/MARK/META)
  FrameType type;
  uint8_t mac[6];       // transmitter address (Wi-Fi MAC or BLE address), zero for MARK/META
  bool locallyAdministered;  // Wi-Fi: randomized MAC. BLE: random (not public) address
  int8_t rssi;          // dBm as reported by the radio (0 for MARK/META)
  uint16_t seq;         // 802.11 sequence number, 0..4095 (0 for BLE/MARK/META)
  char ssid[33];        // SSID (Wi-Fi), device name (BLE), note (MARK/META), vendor (DETECT).
                        // NUL-terminated, empty when absent. May hold odd bytes!

  // ---- BLE only (zero for everything else); filled by ble_scan.cpp ----
  bool bleHasCompany;
  uint16_t bleCompany;     // manufacturer-data company ID
  bool bleAxonShape;       // company 0x034D AND payload 0x02 + 9 upper-case alphanumerics (src/axon_shape.h)
  uint8_t bleSvcCount;
  uint16_t bleSvc16[6];    // 16-bit service UUIDs (advertised or service-data)
  uint8_t bleSvc128Mask;   // bit i set: kWlSvc128[i] was advertised
  uint8_t bleDataMask;     // bit i set: kWlData[i] appeared in service/manufacturer data
  uint8_t findMyType;      // 1 = AirTag, 2 = third-party Find My accessory, 0 = neither
  bool findMySeparated;    // Find My "separated from owner" payload

  // Free-form details, written as the CSV 'extra' column. Probe requests: "ie=<fingerprint>"
  // (see sniffer.cpp). Data frames: "ds=.. bssid=.. n=..". BLE: company/services/Find My.
  char extra[136];
};
