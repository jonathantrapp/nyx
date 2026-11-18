"""watchlist.py - compare capture-logger sessions against watchlist/signatures.csv.

    python tools/watchlist.py scan captures/A_s0020.csv [more files ...]
    python tools/watchlist.py check --manuf wireshark_manuf.txt [--write]

scan   Lists every device in the session files that matches a watchlist rule, one
       line per MAC address, strongest tier first. Accepts plain session .csv files
       or a raw serial dump containing "=== BEGIN ... ===" / "=== END ... ===" lines.
check  Looks up every OUI in Wireshark's copy of the IEEE registry
       (https://www.wireshark.org/download/automated/data/manuf) and prints who the
       prefix is really registered to. --write saves that into the registry_org column.

Matches oui / mac_exact on every device row (probes, beacons, DATA senders, BLE),
ssid_regex / ble_name_regex on names, and probe_ie_signature on the 'ie=' fingerprint
that firmware 0.3.0+ writes for probe requests. The other BLE rules are checked on the
board itself and arrive as DETECT rows. A match is a lead, never a detection - see
watchlist/README.md.
"""
import argparse
import csv
import io
import re
import sys
from collections import Counter
from pathlib import Path

SIGNATURES = Path(__file__).resolve().parent.parent / "watchlist" / "signatures.csv"
TIER_RANK = {"benign": -1, "reject": 0, "possible_low": 1, "possible": 2, "likely": 3, "high": 4}
WIFI_TYPES = {"oui", "mac_exact", "ssid_regex", "fleet_ssid", "probe_ie_signature"}


def load_signatures(path=SIGNATURES):
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        return reader.fieldnames, list(reader)


# ---- scan ------------------------------------------------------------------------

def read_sessions(path):
    """Yield (session_name, rows) for each session in a file."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    framed = re.findall(r"=== BEGIN (\S+) \d+ ===\n(.*?)=== END \1 ===", text, re.S)
    if not framed:  # a plain session file
        framed = [(Path(path).name, text)]
    for name, body in framed:
        yield name, list(csv.DictReader(io.StringIO(body)))


def build_rules(signatures):
    ouis, exact, ssids, ble_names, iesigs = {}, {}, [], [], {}
    for sig in signatures:
        kind, value = sig["match_type"], sig["value"].strip()
        if kind == "oui":
            ouis[value.upper()] = sig
        elif kind == "mac_exact":
            exact[value.upper()] = sig
        elif kind in ("ssid_regex", "fleet_ssid"):
            ssids.append((re.compile(value), sig))
        elif kind == "ble_name_regex":
            ble_names.append((re.compile(value), sig))
        elif kind == "probe_ie_signature":
            iesigs[value] = sig
    return ouis, exact, ssids, ble_names, iesigs


FLOCK_IE_ANCHOR = "221:506f9a16030103"


FLOCK_PLAIN_TAGS = {"1", "2", "3", "12", "45", "50", "127", "191"}


def ie_sig_matches(sig, rule, ssid=""):
    """Identical to detector.cpp: exact, or wildcard + 0..4 allowed plain tags + exact tail."""
    if sig == rule:
        return True
    if ssid or not rule.startswith("2,12,127,"):
        return False
    rule_parts = rule.split(",")
    if FLOCK_IE_ANCHOR not in rule_parts:
        return False
    tail = ",".join(rule_parts[rule_parts.index(FLOCK_IE_ANCHOR):])
    parts = sig.split(",")
    for i, part in enumerate(parts):
        if part == FLOCK_IE_ANCHOR:
            return ",".join(parts[i:]) == tail
        if i >= 4 or part not in FLOCK_PLAIN_TAGS:
            return False
    return False


def match_row(row, ouis, exact, ssids, ble_names, iesigs, benign_devices=None):
    """Return the signature rows this one capture row matches.

    BLE rows are checked on address and name only; the device itself also matches
    company IDs and service UUIDs and records those as DETECT rows.
    """
    mac = (row.get("mac") or "").upper()
    frame = row.get("frame_type")
    if not mac or frame not in ("PROBE_REQ", "BEACON", "DATA", "BLE_ADV"):
        return []
    ble = frame == "BLE_ADV"
    if ble:
        ssids = ble_names  # in BLE rows the ssid column holds the device name
    hits = []
    if mac in exact:
        hits.append(exact[mac])
    # A locally administered (randomized) MAC was invented by the device, so its
    # first three bytes are not anyone's registered prefix.
    if row.get("locally_administered") == "0" and mac[:8] in ouis:
        hits.append(ouis[mac[:8]])
    ssid = row.get("ssid") or ""
    hits += [sig for rx, sig in ssids if ssid and rx.search(ssid)
             and (sig["match_type"] != "fleet_ssid" or frame == "BEACON")
             and (sig["tier"] != "benign" or frame == "BEACON")]
    extra = row.get("extra") or ""
    if row.get("frame_type") == "PROBE_REQ" and extra.startswith("ie="):
        hits += [sig for value, sig in iesigs.items() if ie_sig_matches(extra[3:], value, ssid)]
    if benign_devices is None:
        benign_devices = {}
    identity = (ble, mac)
    if any(sig["tier"] == "benign" for sig in hits):
        # Same bounded, least-recent-benign-observation policy as firmware.
        benign_devices.pop(identity, None)
        benign_devices[identity] = True
        if len(benign_devices) > 64:
            del benign_devices[next(iter(benign_devices))]
    result = []
    for sig in hits:
        hit = dict(sig)
        if sig["match_type"] == "oui" and sig["tier"] not in ("benign", "reject"):
            hit["oui_device"] = identity
            hit["quiet"] = "benign" if identity in benign_devices else ""
        result.append(hit)
    # Mirror firmware's weaker AP evidence, including suppression attached to the
    # AP address, not to an unrelated DATA transmitter. Never recurse into DATA.
    if frame == "DATA":
        found = re.search(r"bssid=([0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5})", extra)
        if found and found[1].upper() != mac:
            ap = found[1].upper()
            ap_row = {"frame_type": "BEACON", "mac": ap, "ssid": "", "extra": "",
                      "locally_administered": str(int(bool(int(ap[:2],16) & 2)))}
            via = best_hit(match_row(ap_row, ouis, exact, ssids, ble_names, iesigs, benign_devices))
            if via is not None:
                hit = dict(via)
                rank = max(1,TIER_RANK[via["tier"]]-1)
                hit["tier"] = next(k for k,v in TIER_RANK.items() if v == rank)
                hit["rule"] = "bssid>" + hit.get("rule", f"{hit['match_type']}:{hit['value']}")
                result.append(hit)
    return result


def best_hit(hits):
    positive = [s for s in hits if s["tier"] not in ("reject", "benign")]
    return max(positive, key=lambda s: (TIER_RANK[s["tier"]], s["match_type"] != "oui",
                                       not bool(s.get("quiet"))), default=None)


def alert_policy(hit):
    """Screen/sound eligibility before mute; trackers retain today's quiet policy."""
    if hit is None:
        return False, False
    rank = TIER_RANK[hit["tier"]]
    screen = rank >= 2 and hit.get("quiet") != "benign" and hit["category"] != "tracker"
    sound = rank >= 3 and hit["match_type"] != "oui" and hit["category"] != "tracker"
    return screen, sound


def scan(files):
    _, signatures = load_signatures()
    rules = build_rules(signatures)
    devices = {}   # mac -> summary of everything seen from it
    benign_devices = {}  # bounded, boot-scoped for contiguous sessions in one input
    total_rows, all_macs = 0, set()

    for path in files:
        benign_devices.clear()  # unrelated input files must not label one another
        for session, rows in read_sessions(path):
            meta = "".join(r.get("ssid", "") for r in rows if r.get("frame_type") == "META")
            if "reason=boot" in meta:
                benign_devices.clear()
            for row in rows:
                total_rows += 1
                if row.get("mac"):
                    all_macs.add(row["mac"])
                hits = match_row(row, *rules, benign_devices=benign_devices)
                if not hits:
                    continue
                d = devices.setdefault(row["mac"], {
                    "sigs": {}, "frames": 0, "types": Counter(), "ssids": set(),
                    "rssi": [], "sessions": set()})
                for sig in hits:
                    d["sigs"][(sig["match_type"], sig["value"], sig.get("rule", ""))] = sig
                d["frames"] += 1
                d["types"][row["frame_type"]] += 1
                if row.get("ssid"):
                    d["ssids"].add(row["ssid"])
                if row.get("rssi"):
                    d["rssi"].append(int(row["rssi"]))
                d["sessions"].add(session)

    def best(d):
        return best_hit(d["sigs"].values())

    flagged = {m: d for m, d in devices.items() if best(d) is not None}
    rejected = len(devices) - len(flagged)
    print(f"{total_rows} rows, {len(all_macs)} distinct MACs, "
          f"{len(flagged)} watchlist leads ({rejected} more matched only benign/rejected rules)")
    if not flagged:
        return
    print(f"\n{'tier':<13}{'category':<12}{'vendor':<32}{'mac':<19}{'frames':>6}  {'rssi':<9}  rules / ssids")
    for mac, d in sorted(flagged.items(), key=lambda kv: (-TIER_RANK[best(kv[1])["tier"]], -kv[1]["frames"])):
        top = best(d)
        rssi = f"{min(d['rssi'])}..{max(d['rssi'])}" if d["rssi"] else "-"
        rules = ", ".join(s.get("rule", f"{s['match_type']}:{s['value']}") +
                          (" [quiet=benign]" if s.get("quiet") else "") for s in d["sigs"].values())
        ssid_list = "; ".join(sorted(d["ssids"])[:3])
        print(f"{top['tier']:<13}{top['category']:<12}{top['vendor'][:31]:<32}{mac:<19}"
              f"{d['frames']:>6}  {rssi:<9}  {rules}  {ssid_list}")


# ---- check -----------------------------------------------------------------------

def load_manuf(path):
    """Map 'AA:BB:CC' -> organisation, plus the set of prefixes split into smaller blocks."""
    orgs, split = {}, set()
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.strip() or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.rstrip("\n").split("\t")]
        prefix = parts[0].upper()
        if "/" in prefix:          # an MA-M / MA-S sub-block inside a shared prefix
            split.add(prefix[:8])
        elif len(prefix) == 8:
            orgs[prefix] = parts[2] if len(parts) > 2 else parts[1]
    return orgs, split


def check(manuf_path, write):
    fields, signatures = load_signatures()
    orgs, split = load_manuf(manuf_path)
    for sig in signatures:
        if sig["match_type"] not in ("oui", "mac_exact"):
            continue
        prefix = sig["value"].upper()[:8]
        org = orgs.get(prefix, "NOT REGISTERED (MA-L)")
        if prefix in split:
            org += " [shared block: a 3-byte match names nobody]"
        sig["registry_org"] = org
        print(f"{sig['tier']:<13}{sig['value']:<19}{sig['vendor'][:34]:<36}{org}")
    if write:
        with open(SIGNATURES, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
            writer.writeheader()
            writer.writerows(signatures)
        print(f"\nregistry_org written to {SIGNATURES}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_scan = sub.add_parser("scan", help="find watchlist matches in session files")
    p_scan.add_argument("files", nargs="+")
    p_check = sub.add_parser("check", help="verify OUIs against the IEEE registry")
    p_check.add_argument("--manuf", required=True, help="path to Wireshark's manuf file")
    p_check.add_argument("--write", action="store_true", help="save registry_org into signatures.csv")
    args = parser.parse_args()
    if args.cmd == "scan":
        scan(args.files)
    else:
        check(args.manuf, args.write)


if __name__ == "__main__":
    sys.exit(main())
