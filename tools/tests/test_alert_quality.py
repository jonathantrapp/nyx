"""Synthetic only. Run: python -m unittest discover -s tools/tests -v"""
import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("watchlist_scan", ROOT / "tools/watchlist.py")
wl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wl)
CASES = json.loads((Path(__file__).with_name("alert_cases.json")).read_text(encoding="utf-8"))


def capture(case):
    row = {k: case[k] for k in ("frame_type", "mac", "ssid", "extra")}
    row["locally_administered"] = str(int(bool(int(row["mac"][:2],16) & 2)))
    return row


class AlertQualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        _, cls.signatures = wl.load_signatures()
        cls.rules = wl.build_rules(cls.signatures)

    def test_shared_firmware_vectors(self):
        for case in CASES:
            with self.subTest(case=case["name"]):
                hits = wl.match_row(capture(case), *self.rules)
                top = wl.best_hit(hits)
                self.assertEqual(top["tier"] if top else "none", case["tier"])
                rule = top.get("rule", f"{top['match_type']}:{top['value']}") if top else ""
                self.assertTrue(rule.startswith(case["rule_prefix"]))
                self.assertEqual(any(s["tier"] == "benign" for s in hits), case["benign"])
                self.assertEqual(wl.alert_policy(top), (case["screen"],case["sound"]))

    def test_field_shape_would_have_matched_old_fallback(self):
        case = next(c for c in CASES if c["name"] == "flock_field_shape_named")
        sig = case["extra"][3:]
        rule = next(s["value"] for s in self.signatures if s["match_type"] == "probe_ie_signature")
        # It has the exact anchor tail that the old implementation accepted.
        self.assertEqual(sig[sig.index(wl.FLOCK_IE_ANCHOR):], rule[rule.index(wl.FLOCK_IE_ANCHOR):])
        self.assertFalse(wl.ie_sig_matches(sig,rule,case["ssid"]))
        self.assertFalse(wl.ie_sig_matches(sig,rule,""))

    def test_benign_observation_carries_to_unnamed_rows_and_ap_bssid(self):
        cache = {}
        bus = {"frame_type":"BEACON", "mac":"00:30:44:00:00:01", "ssid":"YTDriver",
               "extra":"", "locally_administered":"0"}
        wl.match_row(bus,*self.rules,benign_devices=cache)
        unnamed = dict(bus,frame_type="DATA",ssid="")
        top = wl.best_hit(wl.match_row(unnamed,*self.rules,benign_devices=cache))
        self.assertEqual(top.get("quiet"),"benign")
        self.assertEqual(wl.alert_policy(top),(False,False))
        client = dict(unnamed,mac="02:00:00:00:00:02",locally_administered="1",
                      extra="bssid=00:30:44:00:00:01")
        top = wl.best_hit(wl.match_row(client,*self.rules,benign_devices=cache))
        self.assertEqual(top["rule"],"bssid>oui:00:30:44")
        self.assertEqual(top.get("quiet"),"benign")

    def test_benign_does_not_cross_radio_or_other_ap(self):
        cache = {(False,"00:30:44:00:00:01"):True}
        ble = {"frame_type":"BLE_ADV", "mac":"00:30:44:00:00:01", "ssid":"",
               "extra":"", "locally_administered":"0"}
        self.assertEqual(wl.alert_policy(wl.best_hit(wl.match_row(ble,*self.rules,benign_devices=cache))),
                         (True,False))
        client = dict(ble,frame_type="DATA",extra="bssid=00:30:44:00:00:02")
        hits = wl.match_row(client,*self.rules,benign_devices=cache)
        via = next(s for s in hits if s.get("rule","").startswith("bssid>"))
        self.assertNotEqual(via.get("quiet"),"benign")

    def test_benign_never_suppresses_specific_evidence(self):
        cache = {(False,"00:30:44:00:00:01"):True}
        row = {"frame_type":"BEACON", "mac":"00:30:44:00:00:01", "ssid":"IBR600C-de7",
               "extra":"", "locally_administered":"0"}
        top = wl.best_hit(wl.match_row(row,*self.rules,benign_devices=cache))
        self.assertEqual(top["match_type"],"fleet_ssid")
        self.assertEqual(wl.alert_policy(top),(True,False))

    def test_vendor_prefix_never_sounds_even_if_misranked_high(self):
        hit = dict(match_type="oui",tier="high",category="fleet_modem")
        self.assertEqual(wl.alert_policy(hit),(True,False))

    def test_cache_bounded_and_oldest_benign_evicted(self):
        cache = {}
        for n in range(65):
            row = dict(frame_type="BEACON",mac=f"02:00:00:00:00:{n:02X}",ssid="eduroam",
                       locally_administered="1",extra="")
            wl.match_row(row,*self.rules,benign_devices=cache)
        self.assertEqual(len(cache),64)
        self.assertNotIn((False,"02:00:00:00:00:00"),cache)

    def test_scan_smoke_on_synthetic_file(self):
        fields = ["frame_type","mac","ssid","extra","locally_administered"]
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as tmp:
            path = Path(tmp)/"synthetic.csv"
            with path.open("w",newline="",encoding="utf-8") as f:
                writer=csv.DictWriter(f,fieldnames=fields)
                writer.writeheader()
                writer.writerows(capture(c) for c in CASES)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                wl.scan([path])
            self.assertIn("fleet_ssid:",out.getvalue())
            self.assertIn("quiet=benign",out.getvalue())

    def test_new_rules_are_sourced_unverified_and_screen_only(self):
        fleet = [s for s in self.signatures if s["match_type"]=="fleet_ssid"]
        self.assertEqual(len(fleet),5)
        self.assertTrue(all(s["tier"]=="possible" and s["verified"]=="no" and
                            s["source"].startswith("https://") for s in fleet))
        self.assertEqual(len([s for s in self.signatures if s["tier"]=="benign"]),8)


if __name__ == "__main__":
    unittest.main()
