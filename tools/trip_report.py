#!/usr/bin/env python3
"""Score Nyx field-test sessions and draw them on a map.

    python tools/trip_report.py captures/trip_2026-11-25 --out trip_report.html
    python tools/trip_report.py s0301.csv s0302.csv --map cameras/cameras_ne.bin --tz -5 --sightings 12:31,13:05

What it does
  * reads the unit's own session CSVs (GPS track, camera alerts, DETECT rows, MARKs, boot notes)
  * finds every mapped camera the track actually passed and checks whether an "ahead" alert fired in time
  * flags alerts for cameras you never approached (side roads, overpasses)
  * counts resets, logging gaps, police-class detections per hour, and MARKs/sightings that line up
  * applies the pass/fail rules from FIELD_TEST.md and writes one HTML page (map + tables)

The report holds no MAC addresses and no SSIDs; device rows are only counted by category.
Needs only the Python standard library. The map uses Leaflet and OpenStreetMap tiles when you are online
and falls back to a plain route drawing when you are not.
"""
import argparse
import csv
import glob
import html
import json
import math
import os
import re
import struct
import sys
from datetime import datetime, timedelta, timezone

PASS_M = 75.0          # a camera counts as "passed" when the track came this close
FAR_M = 150.0          # alerts for cameras never closer than this are suspicious
AHEAD_GOOD_M = 250.0   # FIELD_TEST: "ahead" at about 300 m; allow some margin
GAP_S = 120.0          # GPS gaps longer than this are not bridged when interpolating the track
CELL_M = 300.0
MAKER = {0: 'Unknown', 1: 'Flock', 2: 'Motorola', 3: 'Axon', 4: 'Other'}
BAD_RESET = ('panic', 'wdt', 'brownout', 'exception', 'crash')
POLICE_CATS = ('bodycam', 'fleet_modem', 'gear', 'tactical', 'acoustic')
ALERT_TIERS = ('likely', 'confirmed')
KV = re.compile(r'(\w+)=(\S+)')


# ---------------------------------------------------------------- geometry
def project(lat, lon, lat0, lon0):
    return ((lon - lon0) * math.cos(math.radians(lat0)) * 111320.0, (lat - lat0) * 110540.0)


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy)), t


class Track:
    """GPS rows turned into usable segments with a grid index for 'how close did we get to X'."""

    def __init__(self, pts, lat0, lon0):
        self.pts = pts                      # (ms, lat, lon, spd_mps, crs, sats)
        self.lat0, self.lon0 = lat0, lon0
        self.xy = [project(p[1], p[2], lat0, lon0) for p in pts]
        self.segs = []
        self.grid = {}
        reach = 200.0
        for i in range(len(pts) - 1):
            dt = (pts[i + 1][0] - pts[i][0]) / 1000.0
            (ax, ay), (bx, by) = self.xy[i], self.xy[i + 1]
            d = math.hypot(bx - ax, by - ay)
            if dt <= 0 or dt > GAP_S or d > 5000 or d / dt > 90:
                continue
            self.segs.append(i)
            x0, x1 = int((min(ax, bx) - reach) // CELL_M), int((max(ax, bx) + reach) // CELL_M)
            y0, y1 = int((min(ay, by) - reach) // CELL_M), int((max(ay, by) + reach) // CELL_M)
            for cx in range(x0, x1 + 1):
                for cy in range(y0, y1 + 1):
                    self.grid.setdefault((cx, cy), []).append(i)

    def nearest(self, x, y):
        """(distance_m, segment_index, t) of the closest usable segment within ~200 m, else None."""
        best = None
        for i in self.grid.get((int(x // CELL_M), int(y // CELL_M)), ()):
            (ax, ay), (bx, by) = self.xy[i], self.xy[i + 1]
            d, t = seg_dist(x, y, ax, ay, bx, by)
            if best is None or d < best[0]:
                best = (d, i, t)
        return best

    def ms_at(self, i, t):
        a, b = self.pts[i][0], self.pts[i + 1][0]
        return a + t * (b - a)

    def position_at(self, ms, max_gap_ms=15000):
        """Interpolated (lat, lon) at a log time, or None when GPS had nothing near it."""
        lo, hi = 0, len(self.pts) - 1
        if hi < 0 or ms < self.pts[0][0] - max_gap_ms or ms > self.pts[-1][0] + max_gap_ms:
            return None
        while lo < hi:
            mid = (lo + hi) // 2
            if self.pts[mid][0] < ms:
                lo = mid + 1
            else:
                hi = mid
        j = lo
        if j == 0:
            p = self.pts[0]
            return (p[1], p[2]) if abs(p[0] - ms) <= max_gap_ms else None
        a, b = self.pts[j - 1], self.pts[j]
        if ms >= b[0] - 1 and abs(b[0] - ms) <= max_gap_ms:
            if b[0] - a[0] <= 2 * max_gap_ms and ms > a[0] and b[0] > a[0]:
                f = (ms - a[0]) / (b[0] - a[0])
                return (a[1] + f * (b[1] - a[1]), a[2] + f * (b[2] - a[2]))
            return (b[1], b[2])
        if abs(a[0] - ms) <= max_gap_ms:
            return (a[1], a[2])
        return None


# ---------------------------------------------------------------- reading sessions
def load_cams(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic, ver, recsz, count, date, _ = struct.unpack_from('<4sHHIII', data, 0)
    if magic != b'CAM1':
        raise ValueError('not a camera map: ' + path)
    cams, off = [], 20
    for _ in range(count):
        lat, lon, direc, maker, flags = struct.unpack_from('<iiHBB', data, off)
        cams.append((lat, lon, maker))
        off += recsz
    return cams, date


def parse_session(path):
    s = {'name': os.path.basename(path), 'notes': [], 'track': [], 'cam_events': [], 'detects': [],
         'marks': [], 'row_ms': [], 'rows': 0}
    buf, buf_ms = '', None
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rd = csv.reader(f)
        next(rd, None)
        for r in rd:
            if len(r) < 10:
                continue                      # a row cut off by power loss
            try:
                ms = int(r[0])
            except ValueError:
                continue
            s['rows'] += 1
            s['row_ms'].append(ms)
            ftype = r[2]
            if ftype == 'META':
                if not buf:
                    buf_ms = ms
                buf += r[6]
                if len(r[6]) < 32:            # pieces hold at most 32 chars; a short one ends the note
                    s['notes'].append((buf_ms, buf))
                    buf = ''
            elif ftype == 'GPS':
                extra = r[9]
                kv = dict(KV.findall(extra))
                if extra.startswith('event=camera'):
                    stage = extra.split()[1] if len(extra.split()) > 1 else ''
                    try:
                        s['cam_events'].append({
                            'ms': ms, 'stage': stage, 'maker': kv.get('maker', '').capitalize(),
                            'dist_m': int(kv.get('dist_m', '0')), 'facing': kv.get('facing'),
                            'dir_known': kv.get('dir_known'),
                            'lat': float(kv['cam_lat']), 'lon': float(kv['cam_lon'])})
                    except (KeyError, ValueError):
                        pass
                elif 'lat' in kv and 'lon' in kv:
                    try:
                        s['track'].append((ms, float(kv['lat']), float(kv['lon']), float(kv.get('spd', 0)),
                                           float(kv.get('crs', 0)), int(kv.get('sats', 0))))
                    except ValueError:
                        pass
            elif ftype == 'DETECT':
                kv = dict(KV.findall(r[9]))
                s['detects'].append({'ms': ms, 'cat': kv.get('cat', ''), 'tier': kv.get('tier', ''),
                                     'rule': kv.get('rule', ''),
                                     'bus': 'yankee' in (r[6] or '').lower()})
            elif ftype == 'MARK':
                s['marks'].append({'ms': ms, 'note': (r[6] or '')[:40]})
    if buf:
        s['notes'].append((buf_ms, buf))
    s['track'].sort(key=lambda p: p[0])
    s['row_ms'].sort()                         # rows are written in time order; sorting makes gap checks order-proof
    # boot info, log mode, clock
    s['fw'] = s['unit'] = s['reset'] = ''
    s['mode'] = 'research'
    s['utc0'] = None                           # epoch seconds at log time 0
    for ms, text in s['notes']:
        kv = dict(KV.findall(text))
        if 'reason=boot' in text and not s['unit']:
            s['unit'], s['fw'], s['reset'] = kv.get('unit', ''), kv.get('fw', ''), kv.get('reset', '')
        if 'log_mode' in kv:
            s['mode'] = kv['log_mode']
        hu = kv.get('host_utc')
        if hu and hu != 'unset' and s['utc0'] is None:
            try:
                t = datetime.strptime(hu, '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=timezone.utc)
                s['utc0'] = t.timestamp() - ms / 1000.0
            except ValueError:
                pass
    return s


def session_files(args_paths):
    files = []
    for p in args_paths:
        if os.path.isdir(p):
            files += glob.glob(os.path.join(p, '**', 's[0-9][0-9][0-9][0-9]*.csv'), recursive=True)
        elif any(ch in p for ch in '*?['):
            files += glob.glob(p)
        else:
            files.append(p)
    return sorted(dict.fromkeys(files), key=lambda x: (os.path.basename(x), x))


# ---------------------------------------------------------------- scoring
def score(sessions, cams, tz_h, sightings, pass_m, police_cats):
    result = {'sessions': [], 'cameras': [], 'false_alerts': [], 'near_alerts': [], 'untracked_alerts': [],
              'resets': [], 'gaps': [], 'detect_counts': {}, 'police_alerts': [], 'marks': [], 'sightings': []}
    all_pts = [p for s in sessions for p in s['track']]
    lat0 = sum(p[1] for p in all_pts) / len(all_pts) if all_pts else 0.0
    lon0 = sum(p[2] for p in all_pts) / len(all_pts) if all_pts else 0.0
    cam_xy = None
    if cams:
        cam_xy = [(c[0], c[1], c[2]) for c in cams]
    hours = 0.0
    meters = 0.0
    for s in sessions:
        track = Track(s['track'], lat0, lon0) if s['track'] else None
        s['_track'] = track
        dur = (s['row_ms'][-1] - s['row_ms'][0]) / 1000.0 if s['row_ms'] else 0.0
        hours += dur / 3600.0
        if track:
            for i in track.segs:
                meters += math.hypot(track.xy[i + 1][0] - track.xy[i][0], track.xy[i + 1][1] - track.xy[i][1])
        info = {'name': s['name'], 'rows': s['rows'], 'duration_s': round(dur), 'fw': s['fw'], 'unit': s['unit'],
                'reset': s['reset'], 'mode': s['mode'], 'gps_points': len(s['track']),
                'utc_start': (datetime.fromtimestamp(s['utc0'] + s['row_ms'][0] / 1000.0, timezone.utc)
                              .strftime('%Y-%m-%d %H:%M:%SZ') if s['utc0'] is not None and s['row_ms'] else '')}
        result['sessions'].append(info)
        if s['reset'] and any(b in s['reset'] for b in BAD_RESET):
            result['resets'].append({'session': s['name'], 'reason': s['reset']})
        # logging gaps (a freeze or a lost log); car mode logs sparsely, so allow the 60 s summary cadence
        limit = 90000 if s['mode'] == 'car' else 5000
        ms_list = s['row_ms']
        for a, b in zip(ms_list, ms_list[1:]):
            if b - a > limit:
                result['gaps'].append({'session': s['name'], 'at_ms': a, 'gap_s': round((b - a) / 1000.0, 1)})
        # cameras
        events = {}
        for e in s['cam_events']:
            events.setdefault((int(round(e['lat'] * 1e6)), int(round(e['lon'] * 1e6))), []).append(e)
        keys = {}
        if track and cam_xy:
            lats = [p[1] for p in s['track']]
            lons = [p[2] for p in s['track']]
            pad = 0.004
            for c in cam_xy:
                la, lo = c[0] / 1e6, c[1] / 1e6
                if min(lats) - pad <= la <= max(lats) + pad and min(lons) - pad <= lo <= max(lons) + pad:
                    keys[(c[0], c[1])] = c[2]
        for k in events:
            keys.setdefault(k, None)
        for (la6, lo6), maker in keys.items():
            evs = sorted(events.get((la6, lo6), []), key=lambda e: e['ms'])
            if not track:
                if evs:        # alerts with no GPS rows to check them against, e.g. a `gps sim` run
                    result['untracked_alerts'].append({'session': s['name'], 'maker': evs[0]['maker'],
                                                       'stage': evs[0]['stage'], 'dist_m': evs[0]['dist_m'],
                                                       'lat': la6 / 1e6, 'lon': lo6 / 1e6})
                continue
            x, y = project(la6 / 1e6, lo6 / 1e6, lat0, lon0)
            near = track.nearest(x, y)
            if near is None or near[0] > FAR_M:
                if evs:
                    result['false_alerts'].append({'session': s['name'], 'lat': la6 / 1e6, 'lon': lo6 / 1e6,
                                                   'maker': evs[0]['maker'], 'closest_m': (round(near[0]) if near else None),
                                                   'ms': evs[0]['ms'], 'stage': evs[0]['stage'],
                                                   'dist_m': evs[0]['dist_m']})
                continue
            d, i, t = near
            if d > pass_m:
                if evs:
                    result['near_alerts'].append({'session': s['name'], 'lat': la6 / 1e6, 'lon': lo6 / 1e6,
                                                  'maker': evs[0]['maker'], 'closest_m': round(d)})
                continue
            ms_close = track.ms_at(i, t)
            ahead = [e for e in evs if e['stage'] == 'ahead' and e['ms'] <= ms_close + 3000]
            close = [e for e in evs if e['stage'] == 'close']
            if ahead and ahead[0]['dist_m'] >= AHEAD_GOOD_M:
                status = 'good'
            elif ahead:
                status = 'late'
            elif close:
                status = 'close_only'
            else:
                status = 'missed'
            result['cameras'].append({
                'session': s['name'], 'lat': la6 / 1e6, 'lon': lo6 / 1e6,
                'maker': evs[0]['maker'] if evs else MAKER.get(maker, 'unknown'),
                'closest_m': round(d), 'status': status,
                'ahead_m': ahead[0]['dist_m'] if ahead else None,
                'close_m': close[0]['dist_m'] if close else None,
                'ms_close': round(ms_close), 'in_map': maker is not None,
                'facing': evs[0].get('facing') if evs else None,
                'speed_mph': round(_speed_at(s['track'], ms_close) * 2.23694)})
        # detections / marks
        for dtc in s['detects']:
            c = result['detect_counts'].setdefault(dtc['cat'] or '?', {})
            c[dtc['tier'] or '?'] = c.get(dtc['tier'] or '?', 0) + 1
            if dtc['cat'] in police_cats and dtc['tier'] in ALERT_TIERS:
                pos = track.position_at(dtc['ms']) if track else None
                result['police_alerts'].append({'session': s['name'], 'ms': dtc['ms'], 'cat': dtc['cat'],
                                                'tier': dtc['tier'], 'rule': dtc['rule'], 'bus': dtc['bus'],
                                                'lat': pos[0] if pos else None, 'lon': pos[1] if pos else None,
                                                'utc': _utc(s, dtc['ms'])})
        for m in s['marks']:
            pos = track.position_at(m['ms']) if track else None
            result['marks'].append({'session': s['name'], 'ms': m['ms'], 'note': m['note'],
                                    'lat': pos[0] if pos else None, 'lon': pos[1] if pos else None,
                                    'utc': _utc(s, m['ms'])})
    # match MARKs and sightings to police-class alerts
    pa = result['police_alerts']
    for m in result['marks']:
        m['matched'] = any(a['session'] == m['session'] and abs(a['ms'] - m['ms']) <= 60000 for a in pa)
    for hhmm in sightings:
        want = None
        for s in sessions:
            if s['utc0'] is None or not s['row_ms']:
                continue
            day = datetime.fromtimestamp(s['utc0'] + s['row_ms'][0] / 1000.0, timezone.utc) + timedelta(hours=tz_h)
            h, mi = map(int, hhmm.split(':'))
            local = day.replace(hour=h, minute=mi, second=0, microsecond=0)
            want = (local - timedelta(hours=tz_h)).timestamp()
            break
        hit = False
        if want is not None:
            hit = any(a['utc'] is not None and abs(a['utc'] - want) <= 180 for a in pa)
        result['sightings'].append({'time': hhmm, 'matched': hit, 'clock_known': want is not None})
    result['hours'] = round(hours, 2)
    result['km'] = round(meters / 1000.0, 1)
    return result


def _speed_at(track, ms):
    best = min(track, key=lambda p: abs(p[0] - ms)) if track else None
    return best[3] if best else 0.0


def _utc(s, ms):
    return s['utc0'] + ms / 1000.0 if s['utc0'] is not None else None


def verdicts(res, have_map):
    cams = res['cameras']
    n = len(cams)
    good = sum(1 for c in cams if c['status'] == 'good')
    missed = sum(1 for c in cams if c['status'] == 'missed')
    out = []
    if n < 5:
        state = 'CHECK'
        text = f'{good} of {n} cameras alerted "ahead" at 250 m or more; the rule needs 5 or more passes to judge.'
    else:
        state = 'PASS' if (good / n >= 0.8 and missed == 0) else 'FAIL'
        text = f'{good} of {n} passed cameras alerted "ahead" at 250 m or more; {missed} missed outright.'
    if not have_map:
        text += ' No camera map was given, so cameras that alerted nothing cannot be found.'
    if res['untracked_alerts']:
        text += (f" {len(res['untracked_alerts'])} camera alert(s) were logged with no GPS track rows "
                 '(a `gps sim` run?), so passing them cannot be checked.')
    out.append(('Cameras', state, text))
    stab_bad = len(res['resets']) + len(res['gaps'])
    out.append(('Stability', 'PASS' if stab_bad == 0 else 'FAIL',
                f"{len(res['resets'])} crash/watchdog reset(s), {len(res['gaps'])} logging gap(s)."))
    bus = sum(1 for a in res['police_alerts'] if a['bus'])
    per_h = len(res['police_alerts']) / res['hours'] if res['hours'] else 0.0
    sight_total = len(res['marks']) + len(res['sightings'])
    sight_hit = sum(1 for m in res['marks'] if m['matched']) + sum(1 for x in res['sightings'] if x['matched'])
    unverified = len(res['police_alerts']) - sight_hit
    state = 'CHECK' if sight_total == 0 else ('PASS' if (bus == 0 and max(unverified, 0) / max(res['hours'], 0.01) <= 1.0) else 'FAIL')
    out.append(('False alarms', state,
                f"{len(res['police_alerts'])} alert-level police-class detection(s), {per_h:.1f}/h; {bus} on buses; "
                f"{max(unverified, 0)} not backed by a MARK or sighting."
                + ('' if sight_total else ' Add --sightings or press MARK to judge this.')))
    out.append(('Police truth', 'CHECK' if sight_total == 0 else ('PASS' if sight_hit >= 1 else 'FAIL'),
                f'{sight_hit} of {sight_total} MARKs/sightings lined up with a detection.'))
    out.append(('Use', 'CHECK', 'Screen readable in daylight and beep audible over road noise: only you can say. '
                                'Write it down next to this report.'))
    return out


# ---------------------------------------------------------------- report
def mph_color(mph):
    f = max(0.0, min(1.0, mph / 70.0))
    return '#%02x%02x%02x' % (int(60 + 180 * f), int(140 + 60 * (1 - abs(f - 0.5) * 2)), int(230 - 190 * f))


def svg_route(sessions, res):
    pts = [p for s in sessions for p in s['track']]
    if len(pts) < 2:
        return '<p class="muted">No GPS track in these sessions.</p>'
    lat0 = sum(p[1] for p in pts) / len(pts)
    lons = [p[2] for p in pts]
    lats = [p[1] for p in pts]
    kx = math.cos(math.radians(lat0))
    w = max((max(lons) - min(lons)) * kx, 1e-6)
    h = max(max(lats) - min(lats), 1e-6)
    W, H, pad = 900.0, 420.0, 20.0
    sc = min((W - 2 * pad) / w, (H - 2 * pad) / h)

    def xy(la, lo):
        return (pad + (lo - min(lons)) * kx * sc, H - pad - (la - min(lats)) * sc)

    parts = [f'<svg viewBox="0 0 {W:.0f} {H:.0f}" class="route" role="img" aria-label="Route plot">']
    for s in sessions:
        t = s['_track']
        if not t:
            continue
        for i in t.segs:
            a, b = s['track'][i], s['track'][i + 1]
            (x1, y1), (x2, y2) = xy(a[1], a[2]), xy(b[1], b[2])
            parts.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{mph_color(a[3] * 2.23694)}" stroke-width="2.5"/>')
    colors = {'good': '#2e9e5b', 'late': '#e0a21b', 'close_only': '#e0a21b', 'missed': '#d9473f'}
    for c in res['cameras']:
        x, y = xy(c['lat'], c['lon'])
        parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="{colors[c["status"]]}"><title>{html.escape(c["maker"])} {c["status"]}</title></circle>')
    for c in res['false_alerts']:
        x, y = xy(c['lat'], c['lon'])
        parts.append(f'<rect x="{x - 4:.1f}" y="{y - 4:.1f}" width="8" height="8" fill="none" stroke="#d9473f" stroke-width="2"><title>alert for a camera never approached</title></rect>')
    for m in res['marks']:
        if m['lat'] is not None:
            x, y = xy(m['lat'], m['lon'])
            parts.append(f'<path d="M{x:.1f} {y - 7:.1f} l5 12 h-10 z" fill="#4c7cf0"><title>MARK</title></path>')
    parts.append('</svg>')
    return ''.join(parts)


CSS = """
:root{--bg:#f6f7f9;--card:#fff;--ink:#16181d;--muted:#667085;--line:#e3e6ec;--ok:#1f8a4d;--bad:#c93a32;--warn:#b07a0b;--chk:#4c5a77}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#0f1115;--card:#171a21;--ink:#e8eaee;--muted:#9aa3b2;--line:#262b35;--ok:#4cc27f;--bad:#ef6a62;--warn:#e3b04b;--chk:#9db0d6}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,Segoe UI,sans-serif}
main{max-width:1100px;margin:0 auto;padding:20px 16px 48px}h1{font-size:24px;margin:0 0 4px}h2{font-size:17px;margin:28px 0 10px}
.muted{color:var(--muted)}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin:16px 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}.card b{display:block;font-size:22px}
table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--line);font-size:14px;vertical-align:top}th{color:var(--muted);font-weight:600}
tr:last-child td{border-bottom:0}.tag{display:inline-block;padding:1px 8px;border-radius:999px;font-weight:600;font-size:12px;border:1px solid currentColor}
.PASS,.good{color:var(--ok)}.FAIL,.missed{color:var(--bad)}.CHECK{color:var(--chk)}.late,.close_only{color:var(--warn)}
#map{height:460px;border-radius:10px;border:1px solid var(--line);margin:8px 0}.route{width:100%;height:auto;background:var(--card);border:1px solid var(--line);border-radius:10px}
.scroll{overflow-x:auto}
"""

JS = """
(function(){var D=window.NYX;var el=document.getElementById('map');
if(typeof L==='undefined'||!D.track.length){el.style.display='none';return;}
document.getElementById('fallback').style.display='none';
var m=L.map('map');L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OpenStreetMap contributors'}).addTo(m);
var all=[];D.track.forEach(function(s){var ll=[[s[0],s[1]],[s[2],s[3]]];all.push(ll[0],ll[1]);L.polyline(ll,{color:s[4],weight:4,opacity:.9}).addTo(m);});
var col={good:'#2e9e5b',late:'#e0a21b',close_only:'#e0a21b',missed:'#d9473f'};
D.cameras.forEach(function(c){L.circleMarker([c.lat,c.lon],{radius:7,color:col[c.status],fillOpacity:.85}).addTo(m).bindPopup(c.maker+' - '+c.status+(c.ahead_m?(' - ahead at '+c.ahead_m+' m'):'')+' - closest '+c.closest_m+' m');});
D.nearby.forEach(function(c){L.circleMarker([c[0],c[1]],{radius:3,color:'#8791a3',weight:1,fillOpacity:.5}).addTo(m);});
D.false_alerts.forEach(function(c){L.circleMarker([c.lat,c.lon],{radius:8,color:'#d9473f',dashArray:'3 3',fill:false}).addTo(m).bindPopup('alert for a camera you never approached (closest '+(c.closest_m===null?'>200':c.closest_m)+' m)');});
D.marks.forEach(function(k){if(k.lat!==null)L.marker([k.lat,k.lon]).addTo(m).bindPopup('MARK '+(k.matched?'(matched a detection)':'(no detection nearby)'));});
D.police.forEach(function(k){if(k.lat!==null)L.circleMarker([k.lat,k.lon],{radius:6,color:'#4c7cf0'}).addTo(m).bindPopup(k.cat+' / '+k.tier+' / '+k.rule);});
m.fitBounds(all,{padding:[20,20]});})();
"""


def build_html(sessions, res, ver, cams_info, args):
    def esc(x):
        return html.escape(str(x))

    track_js = []
    for s in sessions:
        for i in (s['_track'].segs if s['_track'] else []):
            a, b = s['track'][i], s['track'][i + 1]
            track_js.append([round(a[1], 6), round(a[2], 6), round(b[1], 6), round(b[2], 6), mph_color(a[3] * 2.23694)])
    nearby = []
    seen = set()
    for c in res['cameras']:
        seen.add((round(c['lat'], 5), round(c['lon'], 5)))
    data = {'track': track_js[:20000], 'cameras': res['cameras'], 'false_alerts': res['false_alerts'],
            'marks': res['marks'], 'police': res['police_alerts'], 'nearby': nearby}
    cams = res['cameras']
    good = sum(1 for c in cams if c['status'] == 'good')
    first = res['sessions'][0] if res['sessions'] else {}
    rows_v = ''.join(f'<tr><td>{esc(n)}</td><td><span class="tag {st}">{st}</span></td><td>{esc(t)}</td></tr>' for n, st, t in ver)
    cam_rows = ''.join(
        f'<tr><td>{esc(c["maker"])}</td><td class="{c["status"]}">{esc(c["status"].replace("_", " "))}</td>'
        f'<td>{c["ahead_m"] if c["ahead_m"] is not None else "-"}</td><td>{c["close_m"] if c["close_m"] is not None else "-"}</td>'
        f'<td>{c["closest_m"]}</td><td>{c["speed_mph"]}</td><td>{"yes" if c["in_map"] else "no"}</td><td>{esc(c["session"])}</td></tr>'
        for c in sorted(cams, key=lambda c: (c['session'], c['ms_close']))) or '<tr><td colspan="8" class="muted">No mapped camera was passed.</td></tr>'
    fa = ''.join(f'<tr><td>{esc(c["maker"])}</td><td>{esc(c["stage"])} at {c["dist_m"]} m</td><td>{c["closest_m"] if c["closest_m"] is not None else ">200"} m</td><td>{esc(c["session"])}</td></tr>'
                 for c in res['false_alerts']) or '<tr><td colspan="4" class="muted">None.</td></tr>'
    det_rows = ''.join(f'<tr><td>{esc(cat)}</td><td>{esc(", ".join(f"{k}: {v}" for k, v in sorted(tiers.items())))}</td></tr>'
                       for cat, tiers in sorted(res['detect_counts'].items())) or '<tr><td colspan="2" class="muted">No detections.</td></tr>'
    sess_rows = ''.join(f'<tr><td>{esc(x["name"])}</td><td>{esc(x["fw"])}</td><td>{esc(x["mode"])}</td><td>{esc(x["reset"])}</td>'
                        f'<td>{x["duration_s"]} s</td><td>{x["gps_points"]}</td><td>{esc(x["utc_start"])}</td></tr>' for x in res['sessions'])
    body = f"""<main>
<h1>Nyx field report</h1>
<div class="muted">{len(res['sessions'])} session(s), {res['hours']} h logged, {res['km']} km of GPS track{(' - firmware ' + esc(first.get('fw'))) if first.get('fw') else ''}. Made {datetime.now().strftime('%Y-%m-%d %H:%M')} by tools/trip_report.py.</div>
<div class="cards">
<div class="card"><span class="muted">Cameras ahead in time</span><b>{good} / {len(cams)}</b></div>
<div class="card"><span class="muted">Missed outright</span><b>{sum(1 for c in cams if c['status'] == 'missed')}</b></div>
<div class="card"><span class="muted">Alerts for cameras never passed</span><b>{len(res['false_alerts'])}</b></div>
<div class="card"><span class="muted">Resets / gaps</span><b>{len(res['resets'])} / {len(res['gaps'])}</b></div>
<div class="card"><span class="muted">Police-class alerts</span><b>{len(res['police_alerts'])}</b></div>
</div>
<h2>Field-test rules</h2><div class="scroll"><table><tr><th>Rule</th><th>Result</th><th>Detail</th></tr>{rows_v}</table></div>
<h2>Map</h2><div id="map"></div><div id="fallback">{svg_route(sessions, res)}</div>
<p class="muted">Green = alerted ahead in time, amber = late or close only, red = missed, dashed red = alert for a camera never approached, blue = MARK / police-class detection.</p>
<h2>Cameras passed</h2><div class="scroll"><table><tr><th>Maker</th><th>Result</th><th>Ahead (m)</th><th>Close (m)</th><th>Closest (m)</th><th>mph</th><th>In map</th><th>Session</th></tr>{cam_rows}</table></div>
<h2>Alerts for cameras the track never reached</h2><div class="scroll"><table><tr><th>Maker</th><th>Alert</th><th>Closest approach</th><th>Session</th></tr>{fa}</table></div>
<h2>Detections by category</h2><div class="scroll"><table><tr><th>Category</th><th>Tiers</th></tr>{det_rows}</table></div>
<h2>Sessions</h2><div class="scroll"><table><tr><th>File</th><th>fw</th><th>Mode</th><th>Reset</th><th>Length</th><th>GPS pts</th><th>Start (UTC)</th></tr>{sess_rows}</table></div>
<p class="muted">No MAC addresses or SSIDs are stored in this page. A passed camera is one the GPS track came within {args.pass_m:.0f} m of; an "ahead" alert counts as on time at {AHEAD_GOOD_M:.0f} m or more.</p>
</main>"""
    data_json = json.dumps(data).replace('</', '<\\/')
    return (f'<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<title>Nyx field report</title>'
            f'<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.css"><style>{CSS}</style></head><body>{body}'
            f'<script src="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.js"></script>'
            f'<script>window.NYX={data_json};</script><script>{JS}</script></body></html>')


# ---------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description='Score Nyx field-test sessions and draw them on a map.')
    ap.add_argument('inputs', nargs='+', help='session CSVs, globs, or folders that contain sNNNN.csv files')
    ap.add_argument('--map', default=None, help='camera map (.bin); default cameras/cameras_ne.bin next to tools/')
    ap.add_argument('--out', default='trip_report.html')
    ap.add_argument('--json', default=None, help='also write the numbers as JSON')
    ap.add_argument('--tz', type=float, default=-5.0, help='local UTC offset in hours for --sightings (default -5)')
    ap.add_argument('--sightings', default='', help='comma list of local HH:MM times you saw police')
    ap.add_argument('--pass-m', dest='pass_m', type=float, default=PASS_M)
    ap.add_argument('--police-cats', default=','.join(POLICE_CATS))
    args = ap.parse_args(argv)

    files = session_files(args.inputs)
    if not files:
        sys.exit('no session CSVs found')
    sessions = [parse_session(f) for f in files]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    map_path = args.map or next((p for p in (os.path.join(root, 'cameras', 'cameras_ne.bin'),
                                              os.path.join(root, 'cameras', 'cameras.bin')) if os.path.exists(p)), None)
    cams, cam_date = ([], None)
    if map_path and os.path.exists(map_path):
        cams, cam_date = load_cams(map_path)
    sightings = [x.strip() for x in args.sightings.split(',') if x.strip()]
    res = score(sessions, cams, args.tz, sightings, args.pass_m, tuple(c.strip() for c in args.police_cats.split(',')))
    ver = verdicts(res, bool(cams))
    res['verdicts'] = [{'rule': n, 'result': st, 'detail': t} for n, st, t in ver]
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(build_html(sessions, res, ver, cam_date, args))
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(res, f, indent=1)
    for n, st, t in ver:
        print(f'{st:5} {n}: {t}')
    print(f'wrote {args.out}')
    return res


if __name__ == '__main__':
    main()
