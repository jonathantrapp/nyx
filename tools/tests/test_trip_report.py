"""Tests for tools/trip_report.py using a made-up drive (no real captures needed).

Run:  python tools/tests/test_trip_report.py
"""
import math
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import trip_report as tr  # noqa: E402

HEADER = 'millis,channel,frame_type,mac,locally_administered,oui,ssid,rssi,seq,extra\n'
LAT0, LON0 = 42.70, -73.70
M_PER_DEG_LON = 111320.0 * math.cos(math.radians(LAT0))


def lon_at(x_m):
    return LON0 + x_m / M_PER_DEG_LON


def lat_at(y_m):
    return LAT0 + y_m / 110540.0


def meta_rows(ms, text):
    if len(text) % 32 == 0:
        text += ' '
    pieces = [text[i:i + 32] for i in range(0, len(text), 32)]
    return ''.join(f'{ms},,META,,,,"{p}",,,\n' for p in pieces)


def gps_row(ms, extra):
    return f'{ms},,GPS,,,,,,,"{extra}"\n'


def drive_session(path, reset='poweron', gap_at=None):
    """16 km east at 25 m/s, one GPS row every 5 s, five cameras 10 m off the road."""
    cams = [3000, 6000, 9000, 12000, 15000]
    out = [HEADER]
    out.append(meta_rows(1000, f'unit=A fw=0.5.19 reason=boot reset={reset} host_utc=2026-11-25T17:00:00Z log_mode=research'))
    t = 0
    x = 0.0
    while x <= 16000:
        if not (gap_at and gap_at[0] <= t <= gap_at[1]):
            out.append(gps_row(2000 + t * 1000, f'lat={lat_at(0):.6f} lon={lon_at(x):.6f} spd=25.0 crs=90 sats=9 hdop=1.1'))
        t += 5
        x += 125.0
    # camera events: ms = 2000 + (x_cam - dist) / 25 * 1000
    def ev(cam_x, stage, dist, y=10.0):
        ms = 2000 + int((cam_x - dist) / 25.0 * 1000)
        return gps_row(ms, f'event=camera {stage} maker=FLOCK dist_m={dist} facing=0 dir_known=1 cam_lat={lat_at(y):.6f} cam_lon={lon_at(cam_x):.6f}')
    out.append(ev(3000, 'ahead', 300))
    out.append(ev(3000, 'close', 140))
    out.append(ev(6000, 'ahead', 310))
    out.append(ev(9000, 'close', 130))
    out.append(ev(15000, 'ahead', 120))
    out.append(ev(4500, 'ahead', 300, y=100.0))     # 100 m off the road: "near", not a pass
    out.append(ev(8000, 'ahead', 300, y=400.0))     # 400 m off the road: a false alert
    # detections: a police-class one, one on a bus, and a MARK close to the first
    out.append(f'200000,1,DETECT,AA:BB:CC:00:00:01,0,AA:BB:CC,"",-60,0,"tier=likely cat=bodycam rule=axon n=1"\n')
    out.append(f'400000,1,DETECT,AA:BB:CC:00:00:02,0,AA:BB:CC,"YankeeWifi",-60,0,"tier=likely cat=fleet_modem rule=x n=1"\n')
    out.append(f'230000,,MARK,,,,"mark",,,\n')
    out.append(f'210000,1,DETECT,AA:BB:CC:00:00:03,0,AA:BB:CC,"",-70,0,"tier=likely cat=tracker rule=findmy n=4"\n')
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(''.join(out))


def make_map(path):
    recs = [(3000, 10), (6000, 10), (9000, 10), (12000, 10), (15000, 10), (4500, 100), (8000, 400)]
    blob = struct.pack('<4sHHIII', b'CAM1', 1, 12, len(recs), 20261110, 0)
    for x, y in recs:
        blob += struct.pack('<iiHBB', int(round(lat_at(y) * 1e6)), int(round(lon_at(x) * 1e6)), 0xFFFF, 1, 0)
    with open(path, 'wb') as f:
        f.write(blob)


class TripReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.csv = os.path.join(self.dir, 's0301.csv')
        self.map = os.path.join(self.dir, 'cams.bin')
        drive_session(self.csv)
        make_map(self.map)

    def tearDown(self):
        self.tmp.cleanup()

    def run_score(self, files=None, sightings=()):
        sessions = [tr.parse_session(f) for f in (files or [self.csv])]
        cams, _ = tr.load_cams(self.map)
        return sessions, tr.score(sessions, cams, -5.0, list(sightings), tr.PASS_M, tr.POLICE_CATS)

    def test_meta_pieces_are_reassembled(self):
        s = tr.parse_session(self.csv)
        self.assertEqual(s['unit'], 'A')
        self.assertEqual(s['fw'], '0.5.19')
        self.assertEqual(s['reset'], 'poweron')
        self.assertAlmostEqual(s['utc0'], 1795626000.0 - 1.0, delta=2)   # 2026-11-25T17:00:00Z minus 1 s

    def test_camera_statuses(self):
        _, res = self.run_score()
        by_x = {}
        for c in res['cameras']:
            by_x[round((c['lon'] - LON0) * M_PER_DEG_LON / 1000)] = c
        self.assertEqual(len(res['cameras']), 5)
        self.assertEqual(by_x[3]['status'], 'good')
        self.assertEqual(by_x[3]['ahead_m'], 300)
        self.assertEqual(by_x[3]['close_m'], 140)
        self.assertEqual(by_x[6]['status'], 'good')
        self.assertEqual(by_x[9]['status'], 'close_only')
        self.assertEqual(by_x[12]['status'], 'missed')
        self.assertEqual(by_x[15]['status'], 'late')
        self.assertTrue(all(c['in_map'] for c in res['cameras']))

    def test_false_and_near_alerts(self):
        _, res = self.run_score()
        self.assertEqual(len(res['false_alerts']), 1)
        self.assertIsNone(res['false_alerts'][0]['closest_m'])      # farther than the 200 m the index reaches
        self.assertEqual(len(res['near_alerts']), 1)

    def test_verdicts_on_the_drive(self):
        _, res = self.run_score()
        v = {n: st for n, st, _ in tr.verdicts(res, True)}
        self.assertEqual(v['Cameras'], 'FAIL')          # 2 good of 5, one missed outright
        self.assertEqual(v['Stability'], 'PASS')
        self.assertEqual(v['Use'], 'CHECK')

    def test_police_alerts_marks_and_buses(self):
        _, res = self.run_score()
        cats = {a['cat'] for a in res['police_alerts']}
        self.assertEqual(cats, {'bodycam', 'fleet_modem'})          # the tracker is never police-class
        self.assertEqual(sum(1 for a in res['police_alerts'] if a['bus']), 1)
        self.assertEqual(len(res['marks']), 1)
        self.assertTrue(res['marks'][0]['matched'])                 # MARK at 230 s, bodycam at 200 s
        v = {n: st for n, st, _ in tr.verdicts(res, True)}
        self.assertEqual(v['False alarms'], 'FAIL')                 # one alert came from a bus

    def test_sighting_matches_by_clock(self):
        # bodycam at log 200000 ms = 17:03:19 UTC = 12:03 local at -5
        _, res = self.run_score(sightings=['12:03', '15:30'])
        self.assertTrue(res['sightings'][0]['matched'])
        self.assertFalse(res['sightings'][1]['matched'])

    def test_reset_and_gap_detection(self):
        bad = os.path.join(self.dir, 's0302.csv')
        drive_session(bad, reset='panic', gap_at=(200, 240))
        _, res = self.run_score([bad])
        self.assertEqual(len(res['resets']), 1)
        self.assertGreaterEqual(len(res['gaps']), 1)
        self.assertGreater(res['gaps'][0]['gap_s'], 5)

    def test_no_map_cannot_find_misses_but_still_scores_alerts(self):
        sessions = [tr.parse_session(self.csv)]
        res = tr.score(sessions, [], -5.0, [], tr.PASS_M, tr.POLICE_CATS)
        self.assertEqual(sum(1 for c in res['cameras'] if c['status'] == 'missed'), 0)
        self.assertEqual(len(res['cameras']), 4)                    # only cameras that alerted can be found

    def test_alerts_without_a_track_are_reported_not_dropped(self):
        sim = os.path.join(self.dir, 's0303.csv')
        with open(sim, 'w', encoding='utf-8', newline='') as f:
            f.write(HEADER + meta_rows(1000, 'unit=A fw=0.5.19 reason=boot reset=poweron')
                    + gps_row(5000, f'event=camera ahead maker=FLOCK dist_m=298 facing=0 dir_known=1 cam_lat={lat_at(0):.6f} cam_lon={lon_at(0):.6f}'))
        _, res = self.run_score([sim])
        self.assertEqual(len(res['untracked_alerts']), 1)
        v = {n: t for n, st, t in tr.verdicts(res, True)}
        self.assertIn('no GPS track', v['Cameras'])

    def test_report_page_has_no_device_addresses(self):
        out = os.path.join(self.dir, 'r.html')
        js = os.path.join(self.dir, 'r.json')
        tr.main([self.csv, '--map', self.map, '--out', out, '--json', js])
        with open(out, encoding='utf-8') as fh:
            text = fh.read()
        self.assertIn('Nyx field report', text)
        self.assertNotIn('AA:BB:CC', text)
        self.assertNotIn('YankeeWifi', text)
        self.assertTrue(os.path.getsize(js) > 100)


if __name__ == '__main__':
    unittest.main(verbosity=2)
