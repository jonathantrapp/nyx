# Session log format

Receive-only Wi-Fi + BLE logger and detector (firmware 0.3.1). It hops Wi-Fi channels 1-13
for probe requests, beacons and data frames, then spends 1 s passively listening to BLE adverts,
and writes everything to CSV sessions on the board's flash. Nothing is transmitted: BLE scanning
is passive and the advertising/connecting roles are compiled out.

What the `extra` column holds, by row type:

| Row | extra |
|---|---|
| `PROBE_REQ` | `ie=1,50,3,45,127,221:0050f208...` - the probe's information elements in order (SSID skipped, vendor elements as `221:` + first 8 bytes hex). Fingerprints the chip/driver even behind a random MAC; same format as flock-you. |
| `DATA` | `ds=to_ap bssid=.. rx=.. sub=.. n=..` - a device already connected to a network. One row per sender per 30 s (`data <ms>`, 0 = off); `n` = frames it sent since its last row. Payloads are encrypted and never read. |
| `BLE_ADV` | address type, company ID, service UUIDs, Find My state |
| `DETECT` | tier, category, which rule fired |

Limit to know about: the ESP32-S3 is a Wi-Fi 4 (802.11n) radio on 2.4 GHz only. It cannot
decode 5 GHz or Wi-Fi 6 (802.11ax) data frames, so devices on modern networks mostly show up
through their probe requests and the beacons of their access point, not their traffic.

Every row is checked against `watchlist/signatures.csv` (92 rules, all unverified):

| Tier | Effect |
|---|---|
| high / likely | inverted screen title, LED blink (red = high, yellow = likely), category beep |
| possible | screen + blue LED, no sound |
| possible_low | DETECT row in the log only |

Beeps: camera = 3 short high, body cam = 2 long, fleet modem = low-high, other = 1 long.
One alert per device per 5 minutes. Trackers are logged but stay off the screen and silent
(`SCREEN_TRACKER`, `BEEP_TRACKER` in config.h). After editing the CSV run
`python tools/gen_watchlist.py` and rebuild. Serial: `detections`, `mute on|off`, `ble on|off`.

## GPS camera alerts (firmware 0.4.0, not yet tested on hardware)

With a GPS module wired (see `WIRING.md`) and `cameras/cameras.bin` loaded onto the unit
(`python tools/push_cameras.py COM6`), the unit warns about mapped ALPR cameras ahead of you:
a screen, the camera beep and the LED when one is about 20 s ahead, and again when it is about
150 m away. This comes from the map only; it is not a radio detection. The map is OpenStreetMap
data (see `cameras/README.md`). GPS rows (`frame_type` = `GPS`, text in the `extra` column) log
your track every 5 s while moving and each camera warning; GPS time becomes the clock reference.
Serial: `gps`, `gps sim ...`, `cam`, `cam near`, `camput` (used by the tool).

## Build and flash

Install VS Code + the PlatformIO extension (or `pip install platformio`). Then in this folder:

```
pio run -e uart -t upload     # plug into the port labelled UART
pio device monitor
```

Use `-e native-usb` instead if you plug into the S3's own USB port. It shows up in Windows as
`USB Serial Device` with USB ID `303A:1001` (unit A is flashed this way). The serial output only
appears on the port that matches the environment.

**First flash of a new board:** the demo sketch it ships with ignores the automatic reset, so
upload fails with "No serial data received". Hold **BOOT**, tap **RST**, release **BOOT**, then
upload. After the first flash of this firmware, uploads reset the board on their own.

Type `info` in the monitor to reprint the board check (chip, real flash size, PSRAM, last reset).

Set the unit label per board in `platformio.ini` (`-DUNIT_ID=\"A\"`, then B, C).

## Getting sessions off the board

`src/csv_writer.cpp` (`csvWriteRow()`) is written by hand and done (`CSV_WRITER_DONE = true`).

```
python tools/pull_session.py s0027      # -> captures/s0027.csv, size + CRC32 checked, retries 3x
```

`dump` re-sends whatever the USB buffer didn't take and prints a `=== CRC32 name hex ok ===`
line after `=== END ===`. Capture pauses while a dump runs (the sniffer queue may drop rows;
`stats` shows it).

## Controls

| Input | Action |
|---|---|
| Short button press | MARK row (press at a confirmed camera, parked or as a passenger) |
| Long press (1 s) | start a new session file |

Serial console (`help` lists all): `status`, `stats`, `list`, `dump <name>`, `delete <name>`,
`delete all yes`, `space`, `mark [note]`, `new`, `time <UTC>`, `dwell <ms>`, `beacon <ms>`,
`data <ms>`, `format yes`, `reboot`.

Dumps are framed with `=== BEGIN sXXXX.csv <bytes> ===` and `=== END sXXXX.csv ===`.

## Files

| File | Job |
|---|---|
| `include/pins.h` | all GPIO assignments |
| `include/config.h` | dwell, buffer sizes, flush interval, thresholds |
| `src/sniffer.*` | promiscuous-mode capture, frame parsing, channel hopping, beacon rate limit |
| `src/logger.*` | LittleFS sessions, RAM buffer, flush, file operations |
| `src/csv_writer.*` | **your** row formatter (stub) |
| `src/ui.*` | OLED, buzzer, LED, button |
| `src/commands.*` | serial console |
| `src/main.cpp` | boot check, main loop |
| `partitions.csv` | 3 MB app, ~12.9 MB LittleFS, no OTA |
| `watchlist/signatures.csv` | cited, unverified rules from the open-source detectors (see watchlist/README.md) |
| `tools/watchlist.py` | `scan` sessions for watchlist leads; `check` OUIs against the IEEE registry |

## Behaviour you should know about

- **Time.** Rows carry `millis()` since boot, not wall-clock time. Type `time 2026-11-05T14:30:00Z`
  in the monitor to write a META row tying the millis clock to real time. If you do not, the
  data simply has no absolute time. Nothing is invented.
- **Sessions.** Each boot starts a new `sNNNN.csv`; numbers are never reused, even after delete.
  The first rows are META rows (unit, firmware, scan settings, reset reason such as BROWNOUT).
- **Power loss.** Rows are buffered in RAM and flushed every 5 s (open, append, close), so a cut
  loses at most the last ~5 s. LittleFS does not corrupt older files. The brownout detector stays on.
- **Storage full.** Below 128 KB free, logging stops, the LED goes red and the screen says
  `STORAGE FULL-STOP`. Old sessions are kept. Export with `dump`, then `delete`.
- **Beacons are rate-limited** to one row per MAC per 5 s (`beacon <ms>` to change, 0 = all).
  Probe requests are never limited, because their timing is what you are studying.
- **Data volume.** Probe-heavy places can generate several MB per hour. Check `space` after long sessions.
- **Mismatched baud.** If the monitor is garbled after the first boot banner, lower `SERIAL_BAUD`
  (`include/config.h`) and `monitor_speed` (`platformio.ini`) to 115200 together.

## Colour screen, knob and menu (0.5.0)

An optional 2.4" ILI9341 screen shows five screens: listening, camera warning (heading-up map with a trail and
nearby mapped cameras), radio alert, follower alert (screen only, no detection logic yet) and a settings menu,
plus a boot splash and brief volume/mute overlays. The OLED keeps working. See `WIRING.md` for pins.

| File | Job |
|---|---|
| `src/gfx.*`, `src/gfx_fonts.h` | RGB565 drawing library (anti-aliased shapes, U8g2 fonts); portable |
| `src/tft_screens.*`, `src/tft_model.h` | the screens, drawn from a plain data model; portable |
| `src/tft_demo.h` | sample data for every screen (the `tft demo` command and the preview) |
| `src/minimap.h`, `src/menu_logic.h`, `src/knob_decode.h` | map maths, settings menu, encoder decoding; pure logic |
| `src/tft_driver.*` | ILI9341 SPI driver: renders 16-row strips into internal RAM, no frame buffer |
| `src/tft_ui.*` | picks the screen, fills the model from live state, runs the render task |
| `src/controls.*` | knob, mute button, menu actions, saving |
| `tools/test_tft_native.py` | PC tests + renders every screen to `tools/tft_preview/*.png` (no hardware) |
| `tools/tft_shot.py` | screenshot of what the unit is drawing |

Drawing runs in its own low-priority task on core 0 and uses only leftover CPU, so it cannot slow capture
(measured on unit A: camera screen about 4-5 frames per second, 0 dropped frames). **Not yet tested against a real
panel**: the SPI/ILI9341 bring-up code compiles and runs, the picture on the glass is unverified.
