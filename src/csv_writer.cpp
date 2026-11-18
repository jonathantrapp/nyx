// csv_writer.cpp - YOUR FUNCTION. Deliberately left as a stub (see the build plan).
#include "csv_writer.h"

const char CSV_HEADER[] = "millis,channel,frame_type,mac,locally_administered,oui,ssid,rssi,seq,extra";

const bool CSV_WRITER_DONE = true;

// What `out` is: a Print. Anything you can do to Serial you can do to `out`:
//     out.print("text");   out.print(123);   out.printf("%02X:%02X", a, b);   out.print('\n');
// The logger passes a RAM buffer here and later copies it to the LittleFS file, so this
// function never touches a File itself. (The same print()/printf() calls work on a real
// File opened with LittleFS.open(), so what you learn here carries over.)
//
// NOTE: write one line per row, columns in CSV_HEADER order:
//   millis                  row.ms
//   channel                 row.channel
//   frame_type              frameTypeName(row.type)
//   mac                     "AA:BB:CC:DD:EE:FF" from row.mac[0..5]   (printf %02X)
//   locally_administered    1 or 0
//   oui                     first three bytes only, "AA:BB:CC"
//   ssid                    row.ssid - SEE QUOTING NOTE BELOW
//   rssi                    row.rssi
//   seq                     row.seq
// End with '\n'.
//
// QUOTING NOTE: SSIDs are attacker/stranger-controlled text. They can contain commas,
// double quotes, even newlines or non-printable bytes. If you print them raw, one odd SSID
// breaks every column after it. Standard CSV rule: wrap the field in "..." and write each
// embedded " as "". Consider replacing control characters (< 0x20) with '?' as well.
//
// MARK and META rows: only millis, frame_type and ssid (the note) mean anything. Leave
// channel, mac, locally_administered, oui, rssi and seq EMPTY (just the commas) so
// Python/pandas reads them as missing instead of as a bogus 00:00:00:00:00:00 device.
//
// Check it by typing "mark test" in the serial monitor, then "list" and "dump <name>".
void csvWriteRow(Print& out, const CaptureRow& row) {
  const String frameType = frameTypeName(row.type);

  // Keep notes in MARK/META rows, but leave device fields genuinely empty.
  const bool isNote = frameType.equalsIgnoreCase("MARK") ||
                      frameType.equalsIgnoreCase("META");

  auto printSsid = [&out](const String& ssid) {
    out.print('"');
    for (size_t i = 0; i < ssid.length(); ++i) {
      const uint8_t c = static_cast<uint8_t>(ssid.charAt(i));
      if (c < 0x20) {
        out.print('?');
      } else if (c == '"') {
        out.print("\"\"");
      } else {
        out.print(static_cast<char>(c));
      }
    }
    out.print('"');
  };

  out.print(row.ms);
  out.print(',');

  if (row.type == FrameType::GPS) {           // position / camera-map event: only the extra column
    out.print(",GPS,,,,,,,");                 // empty channel, type, then mac..seq empty, then extra
    printSsid(row.extra);
    out.print('\n');
    return;
  }

  if (isNote) {
    out.print(',');                         // channel
    out.print(frameType);
    out.print(",,,,");                      // end frame_type, mac, local, oui -> ssid next
    printSsid(row.ssid);
    out.print(",,,");                       // rssi, seq, extra (empty)
    out.print('\n');
    return;
  }

  out.print(row.channel);
  out.print(',');
  out.print(frameType);
  out.print(',');
  out.printf("%02X:%02X:%02X:%02X:%02X:%02X",
             row.mac[0], row.mac[1], row.mac[2],
             row.mac[3], row.mac[4], row.mac[5]);
  out.print(',');
  out.print(row.locallyAdministered ? 1 : 0);  // BLE: random address, so not the MAC bit
  out.print(',');
  out.printf("%02X:%02X:%02X", row.mac[0], row.mac[1], row.mac[2]);
  out.print(',');
  printSsid(row.ssid);
  out.print(',');
  out.print(row.rssi);
  out.print(',');
  out.print(row.seq);
  out.print(',');
  if (row.extra[0]) printSsid(row.extra);  // BLE details / detection evidence
  out.print('\n');
}
