# Logging and settings in firmware 0.3.7

`mode research|car` is saved in NVS. A mode change opens a new session, so a car
session contains only DETECT, MARK, META and SUMMARY rows. Detection always runs
before either mode's raw-row filter. Switching modes does not clear detector state.

Research keeps the existing capture rows except ordinary Apple BLE adverts:
company 0x004C logs once per address per session, then only counts repeats.
Parsed separated Find My adverts are exempt and keep logging normally. The
existing BLE rate/content limiter still runs first. A new session clears the Apple
set. This uses an exact collision-resolving address set in PSRAM, not a fingerprint
or an eviction cache. If allocation fails or it fills, unknown addresses are logged
rather than falsely classified as duplicates, and the SUMMARY reports degradation.

## SUMMARY schema

One SUMMARY row is written per 60 seconds in both modes. A session/mode boundary
also closes a shorter interval with `partial=1`. A blocking USB dump or explicit
watchdog test can delay the next interval; `dur_ms` gives the real interval length.
The normal ten-column CSV header is unchanged. Device fields are empty, SSID is
`minute`, and `extra` contains space-separated numeric `key=value` pairs:

- `v`, `start_ms`, `dur_ms`, `partial`: schema version and boot-relative interval.
- `wifi_heard`, `data_heard`, `ble_heard`, `dropped`: deltas of the existing raw
  sniffer counters. These precede the existing row rate limits. Counter reads are
  sequential, not a simultaneous snapshot of every radio callback.
- `probe_rows`, `beacon_rows`, `data_rows`, `ble_rows`: rows actually drained from
  the queue, before mode filtering or Apple collapse. Corresponding `_unique`
  fields count distinct addresses within that row type in this interval.
- `apple_rows`, `apple_unique`: company-0x004C BLE observations/addresses in those
  drained rows, including Find My. `apple_collapsed` counts research rows omitted
  by the once-per-session filter; car mode omits all raw rows independently.
- `tracker_rows`, `tracker_unique`: parsed Find My or known tracker-family service
  observations/addresses. Google/Eddystone UUID overlap remains an unverified
  lead; these counts never mean a tracker is following the car.
- `det_camera`, `det_acoustic`, `det_bodycam`, `det_fleet_modem`, `det_gear`,
  `det_tactical`, `det_tracker`: detector evidence rows generated in the interval,
  subject to the existing detection cooldown. These are not vehicle counts.
- `free_bytes`: last sampled free LittleFS space, normally at the last flush.
- `unique_capped`: the bounded per-minute unique set exhausted memory/capacity.
  If 1, unique values are lower bounds. Even when 0, they describe queued rows,
  not every device that might have transmitted during radio time sharing.
- `apple_cache_capped`: the session Apple set exhausted memory/capacity; first-only
  logging is degraded for unknown addresses. Detection continues.

The summary writer is separate because its extra column can exceed 136 bytes.
the original `src/csv_writer.cpp` is unchanged; raw queued CaptureRow sizes are unchanged.
`status` row totals now include control and SUMMARY rows, making totals useful in car
mode. Flush maxima and Apple-collapse/rotation/error counters in stats are boot-local.

## Retention

Rotation starts around 1 MiB free, also allowing room for the pending RAM buffer,
and aims for 1.5 MiB free. Only filenames `sNNNN.csv` (four or more decimal digits)
are eligible. Camera databases and other files are excluded. The current file is
always excluded. An explicitly requested export is also pinned during the pre-dump
flush (a dump no longer rotates; rotation runs outside the flush). As of 0.3.8, closed files without MARK/non-tracker DETECT are deleted oldest first; only after these
are exhausted are protected files deleted, again oldest first.

Protection is determined from CSV frame-type and DETECT category fields, not text inside
SSIDs. Tracker-only DETECT files are plain. Missing/ambiguous categories, torn DETECT rows,
or unreadable files are conservatively protected.

**Since 0.3.12** a DETECT row with a single `tier=possible_low` token (the log-only tier) does not protect a session; `possible` and above, MARK rows and ambiguous rows still do. NVS key `prv` records the rule version; when it changes, old verdicts are dropped and stored sessions are re-scanned once in the background.

**As of 0.3.10 protection is recorded when it happens, not discovered by reading files.**
When a MARK or non-tracker DETECT row is first buffered for the current session, the next
flush stores that session number as protected in a small NVS table (`ptab`, up to 96
records) *before* the rows are written to flash, so a reset can never leave a protected
file looking plain. A session at or above the saved watermark (`pwm`) with no record is
plain. Sessions below it with no record are UNKNOWN (everything written by 0.3.9 or
older, or after an NVS table problem). Each UNKNOWN session is scanned once by content, in
~12 ms slices (1 KiB reads, one slice per ~100 ms), and the result is stored in the table.
The oldest UNKNOWN is scanned first. Rotation will not delete a plain file while an older
UNKNOWN one exists, and never deletes a protected file while any UNKNOWN or plain one
remains. If a table write fails or the table is full, the watermark is raised so the
affected sessions become UNKNOWN again and get re-scanned (the safe direction).
Each successful removal records `rotated=sNNNN.csv` in the current/new session META.
`rotation` prints the cached index and the next victim without reading or deleting any file.

Sessions close at approximately 1 MiB (the last flush can overshoot that slightly).
At 4 MiB/hour this is roughly 15 minutes per file. This lets rotation make progress
even when the device runs all day without rebooting. The approximately 12.9 MB
filesystem still holds only a few hours at the measured busy research rate; rotation
provides continued recording by discarding old sessions, not unlimited history.
Car retention depends heavily on detection activity and cannot be promised as a fixed
number of days.

Rotation no longer runs inside `loggerFlush()` and no longer pauses the loop watchdog.
`loggerService()` runs it as its own stage (breadcrumbs `rotation`, `rotate_scan`,
`rotate_delete`, `storage_space`) and performs at most ONE bounded operation per loop
pass: a classification slice, one file deletion (~0.4 s), or one free-space query. After a
delete or query it waits 250 ms so draining, the display and flushes run in between.
Radio dwell/BLE scheduling and passive-only operation are unchanged.

Free space is a cached estimate (`SpaceCache`). A real `esp_littlefs_info` query (one
traversal; Arduino's `totalBytes()`+`usedBytes()` did two) runs at most every 60 s, every
15 s within 512 KiB of the rotation target, or after 128 KiB flushed. Flushes only subtract
the bytes they wrote; a deletion adds back its whole 4 KiB blocks and marks the estimate
stale so one real query confirms it before rotation stops. Rotation never acts before the
first real query succeeds. `status`/OLED free space is therefore an estimate between queries.

Stats: `rotation_max_ms` / `phase` (scan, remove, print, space, index, nvs) is the slowest single
background operation since boot; `rot_win_max_ms` / `win_phase` is the same per 10-minute
window and is written to the META rows (`rot_win_max_ms`, `rot_win_phase`, `rot_max_ms`,
`rot_max_phase`, `rot_unclassified`). `space_ms` is the last real query (outside the flush).
`rotation-bench` is a read-only timing check: one space query, a directory listing, a replay
of the classification scan over every closed file with the real slice budget and normal
`loggerService()` passes in between (compared with the stored classification, MISMATCH =
index says plain but content has protected rows), and a simulated victim pick. It prints
PASS when the worst single stage is under 1000 ms. It takes ~4 s per 2.8 MB scanned plus
one pass per slice and deletes nothing.

The old permanent STORAGE FULL discard path is removed. A healthy filesystem with
old eligible sessions keeps recording through rotation. Physical flash failures,
failed deletions, a filesystem consumed by non-session files, or an unrecoverable
mount/write failure still need attention; the display and error counters report this.
Open/zero-byte write failures retain the pending buffer for retries; a returning
partial write is counted as an error and its already-written prefix is not duplicated.
As before, a partial write can leave an incomplete CSV row. Recovery by USB reflash
is unchanged, and mounting a previously initialized filesystem never auto-formats it.

## Saved controls

`settings` prints mode, detection mute, software tone volume, dwell, BLE enable,
OLED contrast brightness and the LED level. Changes save only when values differ.
One validated, versioned NVS blob lives in `rf_settings`, separate from session numbers
and the filesystem initialization flag in `cap`. `settings reset` restores:

```text
mode=research mute=off volume=100 brightness=100 led=dim dwell_ms=250 ble=on
```

Resetting app settings does not erase captures or reuse session numbers. `format yes`
erases LittleFS but leaves these settings/session numbering in NVS. Beacon/DATA row
rate-limit commands remain boot-local, as before; they were not added to the saved list.

`volume 0..100` changes PWM tone duty on the existing audio output; it is a coarse,
nonlinear sound-level control, not calibrated amplifier gain. 100 preserves the
core's previous tone duty. Active-buzzer builds support zero/nonzero only. `brightness`
changes current SSD1306 contrast; it does not guess or initialize a future TFT driver.
`led off` keeps the earlier behaviour: hide the heartbeat while allowing dim alerts/errors.

## OLED

The home screen shows a thin mode/BLE header, large LISTENING or storage-error state,
free space with a storage-use bar, and volume/mute plus RECEIVE ONLY in the footer.
An alert replaces the middle with category, vendor, RSSI in dBm and possible/unverified
status. Every alert names its source as RF equipment inference. No confidence percentage
or all-clear claim is displayed. The footer shows CHECK LOG during storage trouble,
including while an RF alert is visible. Existing boot/error diagnostic rendering remains.

## Tests

`storage-test` on the device checks filename eligibility, priority ordering, exact-set
behavior and the separated Find My exception without modifying files or settings.
The native test executes the actual portable firmware policy, settings decoder and
SUMMARY writer and verifies all CSV chunk boundaries and 10-column serialization:

```text
python tools/test_storage_native.py
python -m unittest discover -s tools/tests -v
```

The native runner needs clang++/g++ (`CXX`) or the local Zig installation under .pio.
It uses synthetic data only; it does not mount the device filesystem or read captures.
