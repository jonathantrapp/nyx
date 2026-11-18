"""gen_watchlist.py - turn watchlist/signatures.csv into src/watchlist_data.h.

    python tools/gen_watchlist.py

Run it after every edit to signatures.csv, then rebuild the firmware. Rows with tier
'reject' are left out (they exist to stop prefixes being re-added, not to match), and
match types the firmware can't check yet are listed and skipped.
"""
import csv
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CSV_PATH = ROOT / "watchlist" / "signatures.csv"
OUT_PATH = ROOT / "src" / "watchlist_data.h"

TIERS = {"possible_low": "WL_POSSIBLE_LOW", "possible": "WL_POSSIBLE", "likely": "WL_LIKELY", "high": "WL_HIGH",
         "benign": "WL_BENIGN"}
CATS = {"camera": "WL_CAT_CAMERA", "acoustic": "WL_CAT_ACOUSTIC", "bodycam": "WL_CAT_BODYCAM",
        "fleet_modem": "WL_CAT_FLEET_MODEM", "gear": "WL_CAT_GEAR", "tactical": "WL_CAT_TACTICAL",
        "tracker": "WL_CAT_TRACKER"}

# The regex subset the firmware matcher understands (see watchlist.h).
NAME_RX = re.compile(r"(\(\?i\))?(\^)?([A-Za-z0-9 _\-]*?)"
                     r"(?:(\\d|\[0-9a-f\])(\+|\{(\d+)(?:,(\d+))?\})([A-Za-z0-9 _\-]*))?(\$)?")


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def mac_bytes(value, n):
    parts = value.split(":")
    if len(parts) != n:
        raise ValueError(f"expected {n} bytes: {value}")
    return "{" + ", ".join(f"0x{int(p, 16):02x}" for p in parts) + "}"


def name_rule(kind, pattern):
    m = NAME_RX.fullmatch(pattern)
    if not m:
        raise ValueError(f"pattern outside the firmware's regex subset: {pattern}")
    ci, start, lit, cls, quant, qmin, qmax, suffix, end = m.groups()
    flags = " | ".join(f for f, on in (("WL_F_CI", ci), ("WL_F_START", start), ("WL_F_END", end)) if on) or "0"
    tail, tmin, tmax = "WL_TAIL_NONE", 0, 0
    if cls:
        tail = "WL_TAIL_DIGIT" if cls == "\\d" else "WL_TAIL_HEX"
        if quant == "+":
            tmin, tmax = 1, 255
        else:
            tmin = int(qmin)
            tmax = int(qmax) if qmax else tmin
    return f"{kind}, {flags}, {cstr(lit)}, {tail}, {tmin}, {tmax}, {cstr(suffix or '')}"


def main():
    rows = list(csv.DictReader(open(CSV_PATH, newline="", encoding="utf-8")))
    tables = {k: [] for k in ("oui", "mac", "name", "company", "svc16", "range", "svc128", "data", "iesig",
                              "benign_oui", "benign_name")}
    findmy_tier, skipped = "WL_NONE", []

    for r in rows:
        if r["tier"] == "reject":
            continue
        kind, value = r["match_type"], r["value"].strip()
        tier, cat = TIERS[r["tier"]], CATS[r["category"]]
        vendor = cstr(r["vendor"])
        rule = cstr(f"{kind.replace('ble_', '')}:{value}"[:39])
        tail = f"{tier}, {cat}, {vendor}, {rule}"
        if r["tier"] == "benign":
            if kind == "oui":
                tables["benign_oui"].append(f"{{{mac_bytes(value, 3)}, {tier}, {cat}, {vendor}}}")
            elif kind == "ssid_regex":
                tables["benign_name"].append(f"{{{name_rule('WL_KIND_SSID', value)}, {tail}}}")
            else:
                raise ValueError(f"benign supports only oui/ssid_regex: {kind}")
            continue
        if kind == "oui":
            tables["oui"].append(f"{{{mac_bytes(value, 3)}, {tier}, {cat}, {vendor}}}")
        elif kind == "mac_exact":
            tables["mac"].append(f"{{{mac_bytes(value, 6)}, {tail}}}")
        elif kind == "ssid_regex":
            tables["name"].append(f"{{{name_rule('WL_KIND_SSID', value)}, {tail}}}")
        elif kind == "fleet_ssid":
            if r["tier"] != "possible" or r["category"] != "fleet_modem":
                raise ValueError("fleet_ssid must stay fleet_modem/possible until field validation")
            tables["name"].append(f"{{{name_rule('WL_KIND_FLEET_SSID', value)}, {tail}}}")
        elif kind == "ble_name_regex":
            tables["name"].append(f"{{{name_rule('WL_KIND_BLE_NAME', value)}, {tail}}}")
        elif kind == "ble_company_id":
            tables["company"].append(f"{{0x{int(value, 16):04x}, {tail}}}")
        elif kind == "ble_service_uuid" and value.lower().startswith("0x"):
            tables["svc16"].append(f"{{0x{int(value, 16):04x}, {tail}}}")
        elif kind == "ble_service_uuid":
            tables["svc128"].append(f"{{{cstr(value.lower())}, {tail}}}")
        elif kind == "ble_service_range":
            lo, hi = (int(v, 16) for v in value.split("-"))
            tables["range"].append(f"{{0x{lo:04x}, 0x{hi:04x}, {tail}}}")
        elif kind == "ble_data_substring":
            tables["data"].append(f"{{{cstr(value)}, {tail}}}")
        elif kind == "probe_ie_signature":
            tables["iesig"].append(f"{{{cstr(value)}, {tail}}}")
        elif kind == "ble_findmy":
            findmy_tier = tier
        else:
            skipped.append(f"{kind}:{value}")

    if len(tables["svc128"]) > 8 or len(tables["data"]) > 8:
        sys.exit("BLE row masks are 8 bits wide: at most 8 svc128 and 8 data rules")

    spec = [("oui", "WlOui", "kWlOui"), ("mac", "WlMac", "kWlMac"), ("name", "WlName", "kWlName"),
            ("company", "WlU16", "kWlCompany"), ("svc16", "WlU16", "kWlSvc16"),
            ("range", "WlRange", "kWlSvcRange"), ("svc128", "WlStr", "kWlSvc128"), ("data", "WlStr", "kWlData"),
            ("iesig", "WlStr", "kWlIeSig"), ("benign_oui", "WlOui", "kWlBenignOui"),
            ("benign_name", "WlName", "kWlBenignName")]
    total = sum(len(v) for v in tables.values()) + (findmy_tier != "WL_NONE")
    out = ["// GENERATED by tools/gen_watchlist.py from watchlist/signatures.csv - do not edit.",
           "// Every rule is unverified until your own captures confirm it.",
           "#pragma once", '#include "watchlist.h"', "",
           f"static const size_t kWlRuleCount = {total};",
           f"static const uint8_t kWlFindMyTier = {findmy_tier};  // AirTag / Find My accessory, separated", ""]
    for key, typ, name in spec:
        items = tables[key]
        body = ",\n".join("    " + i for i in items) if items else "    {}"
        out += [f"static const {typ} {name}[] = {{", body, "};",
                f"static const size_t {name}Count = {len(items)};", ""]
    OUT_PATH.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"{total} rules -> {OUT_PATH}")
    for s in skipped:
        print(f"  skipped (firmware can't check it yet): {s}")
    write_test_vectors()


def write_test_vectors():
    cases = json.loads((ROOT / "tools/tests/alert_cases.json").read_text(encoding="utf-8"))
    out = ["// GENERATED from tools/tests/alert_cases.json by tools/gen_watchlist.py.",
           "#pragma once", '#include "capture.h"', '#include "watchlist.h"',
           "struct AlertTestCase { const char *name; FrameType type; uint8_t mac[6];",
           "  const char *ssid, *extra; uint8_t tier; const char *rule; bool benign, screen, sound; };",
           "static const AlertTestCase kAlertTests[] = {"]
    for c in cases:
        if len(c["ssid"].encode()) > 32 or len(c["extra"].encode()) > 135:
            raise ValueError(f"test exceeds CaptureRow field size: {c['name']}")
        tier = "WL_NONE" if c["tier"] == "none" else TIERS[c["tier"]]
        flags = ", ".join(str(c[k]).lower() for k in ("benign", "screen", "sound"))
        out.append(f"  {{{cstr(c['name'])}, FrameType::{c['frame_type']}, {mac_bytes(c['mac'],6)}, "
                   f"{cstr(c['ssid'])}, {cstr(c['extra'])}, {tier}, {cstr(c['rule_prefix'])}, {flags}}},")
    out.append("};\n")
    (ROOT / "src/alert_test_data.h").write_text("\n".join(out), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
