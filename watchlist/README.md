# Watchlist

`signatures.csv` collects what the open-source detectors look for, one rule per row, so
Phase 2 can test each rule against your own captures. **Every row is `verified=no`**
until a photo-confirmed camera session shows it and the control sessions don't.

## Columns

| Column | Meaning |
|---|---|
| `category` | camera, acoustic, bodycam, fleet_modem, gear (vendor known, product not), tactical, tracker, rejected |
| `match_type` | `oui` (first 3 MAC bytes), `mac_exact`, `ssid_regex`, `fleet_ssid` (beacon-only default-format regex), `ble_*`, `probe_ie_signature` |
| `tier` | positive ranks: `high` > `likely` > `possible` > `possible_low`; `benign` suppresses only OUI screen/sound; `reject` is omitted from firmware |
| `source` | where the rule came from (repos below) |
| `registry_org` | who the IEEE registry says owns the prefix, filled by `watchlist.py check` |

Firmware checks all supported rules. `tools/watchlist.py` checks OUI/MAC, names, fleet
SSIDs and probe fingerprints; BLE company/service/data evidence is supplied by firmware
DETECT rows, not independently decoded by that script. DATA rows check the AP's BSSID,
one tier weaker (`bssid>...`), in both matchers as of 0.3.6.

## Alert quality in 0.3.6

Flock fingerprints match exactly, or use a restricted fallback: empty SSID, at most four
plain tags before the whole `221:506f9a16030103` element, and an exact remaining suffix.
Permitted preceding tags are 1, 2, 3, 12, 45, 50, 127 and 191. Any preceding vendor
element (including WPS/P2P), unexpected tag, longer anchor, or changed tail rejects
the fallback. Exact full matches remain accepted even with a named SSID. This loses
some variants to reduce false positives; it is still unverified equipment inference.

`fleet_ssid` rules require a BEACON: a directed probe for a router's name describes
the network sought, not the sender's equipment. The generator keeps these rules at
`fleet_modem` / `possible`. They identify a default-format name, not a police owner.
The `source` column of `signatures.csv` records where each router rule came from.

Benign SSID rules are also learned only from BEACON rows. They are anchored, case-sensitive
patterns corresponding to the supplied field observations; `^YankeeWifi-`, `^rpi_` and
`^My VW` mean those prefixes, while the other supplied names are exact. Benign OUI
00:54:AF applies only to non-random addresses. The firmware remembers up to 64 benign
radio/address identities per boot, replacing the least recently observed benign identity
when full. This survives new session files but not reboot. Eviction can allow OUI cards
to reappear. Python uses the same bounded learning for contiguous sessions in one input
file, clears it at boot META, and starts fresh for a separate input file.

A benign match suppresses only OUI-derived cards/sounds for that identity; raw rows and
positive OUI DETECT evidence remain logged, with `quiet=benign` on new DETECT rows when
the usual cooldown allows one. It never vetoes exact MAC, named equipment, probe-IE or
BLE payload evidence. AP-derived suppression follows the AP's BSSID, not an unrelated
DATA sender. Benign rules are annotations, not positive detection ranks or guarantees.

At equal rank, a specific signature beats an OUI. No OUI-only observation may beep,
including a DATA/BSSID match. A weaker current row cannot reuse an old stronger rule
for a sound alert. The `detections` list still retains each device's strongest evidence.

`python -m unittest discover -s tools/tests -v` checks synthetic cases. Run `alert-test`
on the device to check matching/policy with the same generated fixtures; it performs
no radio operations and does not change logging, live detections, mute or the display.

## Use

```
python tools/watchlist.py scan captures/A_s0020.csv
python tools/watchlist.py check --manuf wireshark_manuf.txt --write
```

`scan` lists every watchlist lead in one or more sessions (plain CSV or a raw serial dump).
OUI rules skip locally administered (randomized) MACs. `check` needs Wireshark's copy of the
IEEE registry: https://www.wireshark.org/download/automated/data/manuf

## How much a match means

- **Most "Flock" prefixes are not Flock's.** Of the 30 in the community list, only
  `b4:1e:52` is registered to Flock Safety; the rest are Liteon, USI and Silicon Labs radio
  modules that also ship in ordinary consumer gear. That's why they sit at `possible_low`.
- **Current Flock cameras mostly probe and don't beacon** (FlipDeFlock: beaconing stopped
  around December 2025), roughly every 125 ms. A module-vendor OUI *plus* that probe rhythm
  is the plan's Likely tier, and needs the timing analysis from Phase 2, not this table.
- **Vendor-exclusive OUIs** (Axon, Motorola Solutions, Cradlepoint, Sierra Wireless,
  Ubicquia, ...) name the vendor, not the product, and none has been captured on the air
  by any source. Motorola Solutions blocks also carry ordinary hand-held radios; one audit
  found 27 of 27 Motorola Wi-Fi hits were not police.
- **Probe requests alone never raise a tier.** Every phone sends them.

## Corrections found while building this

- `48:e7:29`, `c8:c9:a3` (flock-detection, unsourced) are **Espressif** ESP32 prefixes.
  They would flag any ESP32 gadget, this unit included. Rejected.
- `04:0d:84` is labelled Cradlepoint in flock-detection; the registry says **Silicon Labs**.
- `b8:35:32` is in every Flock list but **not in the IEEE MA-L registry**.
- `3c:71:bf`, `a4:cf:12` (Espressif) and `48:27:ea` (Samsung) are still in the flock-you
  lists; FlipDeFlock rejected them after real false positives.
- Vendor-name searches mislead: "axon" also finds Axon *Networks*, "motorola" finds
  Motorola *Mobility* (Lenovo phones). Those prefixes are in the `rejected` rows.

## Sources (shallow clones, 2026-11-05)

| Repo | Commit | Files used |
|---|---|---|
| ReconGrunt/FlipDeFlock | 343f87e | helpers/flock_db.c, docs/signatures.md, esp32_companion/flock_companion/ |
| simeononsecurity/flock-you-esp32 | ea5dc18 | fy_detect.h |
| colonelpanichacks/flock-you | 1e814bc | main.cpp |
| zmattmanz/flock-detection | e2fdb43 | FlockDetection/FlockDetection.ino |
| IEEE MA-L registry | via Wireshark manuf, 2026-11-05 | Cradlepoint, Sierra Wireless, Domo Tactical, Cobham |

DeFlock (deflock.me) is a map of camera *locations*, not RF signatures; it feeds the Phase 3
mapped-camera alerts. FlipDeFlock also covers drones via Remote ID, which isn't included here.
