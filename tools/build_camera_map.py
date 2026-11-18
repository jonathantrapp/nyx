"""Build cameras/cameras.bin, the on-device ALPR camera map, from OpenStreetMap data.

    python -I tools/build_camera_map.py cameras/src/osm_alpr_us.json
    python -I tools/build_camera_map.py cameras/src/osm_alpr_us.json --near 42.73 -73.68 300
    python -I tools/build_camera_map.py data.json --bbox 40 -80 45 -71 --csv cameras/review.csv

Input is an Overpass JSON dump (`[out:json]` with `node["surveillance:type"="ALPR"]; out;`).
To fetch one, see cameras/README.md. OSM data is (c) OpenStreetMap contributors, ODbL.

File format (little endian), read by src/camera_map.h:
    header  20 bytes: "CAM1", u16 version=1, u16 record_size=12, u32 count, u32 data_date (YYYYMMDD), u32 0
    record  12 bytes: i32 lat_e6, i32 lon_e6, u16 direction (0-359, 0xFFFF unknown), u8 maker, u8 flags
    Records are sorted by latitude so the firmware can binary-search a latitude band.

maker: 0 unknown, 1 Flock Safety, 2 Motorola/Vigilant, 3 Axon, 4 other known maker
flags: bit0 = direction known, bit1 = several directions listed (camera can face more than one way)
"""
import argparse
import csv
import json
import math
from pathlib import Path
import struct
import sys

MAGIC = b'CAM1'
VERSION = 1
REC_SIZE = 12
HEADER_FMT = '<4sHHIII'
REC_FMT = '<iiHBB'
DIR_UNKNOWN = 0xFFFF
FLAG_DIR = 1
FLAG_MULTI = 2

MAKER_UNKNOWN, MAKER_FLOCK, MAKER_MOTOROLA, MAKER_AXON, MAKER_OTHER = range(5)

CARDINALS = {
    'N': 0, 'NNE': 22.5, 'NE': 45, 'ENE': 67.5, 'E': 90, 'ESE': 112.5, 'SE': 135, 'SSE': 157.5,
    'S': 180, 'SSW': 202.5, 'SW': 225, 'WSW': 247.5, 'W': 270, 'WNW': 292.5, 'NW': 315, 'NNW': 337.5,
}


def parse_one_direction(text):
    """One OSM direction value -> degrees 0-359, or None. Handles 270, 'NE', '140-160'."""
    text = text.strip().upper()
    if not text:
        return None
    if text in CARDINALS:
        return int(round(CARDINALS[text])) % 360
    try:
        return int(round(float(text))) % 360
    except ValueError:
        pass
    if '-' in text[1:]:
        lo, _, hi = text.partition('-') if text[0] != '-' else ('', '', text)
        try:
            a, b = float(lo), float(hi)
        except ValueError:
            return None
        diff = ((b - a + 180) % 360) - 180
        return int(round(a + diff / 2)) % 360
    return None


def parse_direction(tags):
    """-> (direction_degrees or DIR_UNKNOWN, flags)"""
    raw = tags.get('direction') or tags.get('camera:direction') or ''
    values = [parse_one_direction(v) for v in raw.replace(',', ';').split(';')]
    values = [v for v in values if v is not None]
    if not values:
        return DIR_UNKNOWN, 0
    return values[0], FLAG_DIR | (FLAG_MULTI if len(values) > 1 else 0)


def classify_maker(tags):
    text = ' '.join(tags.get(k, '') for k in ('manufacturer', 'brand', 'operator', 'name', 'description')).lower()
    if 'flock' in text:
        return MAKER_FLOCK
    if 'motorola' in text or 'vigilant' in text:
        return MAKER_MOTOROLA
    if 'axon' in text:
        return MAKER_AXON
    if tags.get('manufacturer') or tags.get('brand'):
        return MAKER_OTHER
    return MAKER_UNKNOWN


def haversine_km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2
    return 6371.0088 * 2 * math.asin(math.sqrt(a))


def build(elements, near=None, bbox=None):
    records = []
    for el in elements:
        if el.get('type') != 'node' or 'lat' not in el:
            continue
        lat, lon = el['lat'], el['lon']
        if bbox and not (bbox[0] <= lat <= bbox[2] and bbox[1] <= lon <= bbox[3]):
            continue
        if near and haversine_km(near[0], near[1], lat, lon) > near[2]:
            continue
        tags = el.get('tags', {})
        direction, flags = parse_direction(tags)
        records.append((int(round(lat * 1e6)), int(round(lon * 1e6)), direction, classify_maker(tags), flags,
                        el.get('id', 0), tags.get('operator', '')))
    records.sort(key=lambda r: (r[0], r[1]))
    return records


def data_date(doc):
    stamp = doc.get('osm3s', {}).get('timestamp_osm_base', '')
    digits = stamp[:10].replace('-', '')
    return int(digits) if digits.isdigit() and len(digits) == 8 else 0


def pack(records, date):
    out = bytearray(struct.pack(HEADER_FMT, MAGIC, VERSION, REC_SIZE, len(records), date, 0))
    for lat, lon, direction, maker, flags, *_ in records:
        out += struct.pack(REC_FMT, lat, lon, direction, maker, flags)
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('osm_json', type=Path)
    ap.add_argument('-o', '--out', type=Path, default=Path('cameras/cameras.bin'))
    ap.add_argument('--near', nargs=3, type=float, metavar=('LAT', 'LON', 'KM'), help='keep cameras within KM of a point')
    ap.add_argument('--bbox', nargs=4, type=float, metavar=('S', 'W', 'N', 'E'), help='keep cameras inside a box')
    ap.add_argument('--csv', type=Path, help='also write a human-readable review CSV')
    args = ap.parse_args(argv)

    doc = json.loads(args.osm_json.read_text(encoding='utf-8'))
    records = build(doc.get('elements', []), args.near, args.bbox)
    if not records:
        sys.exit('no cameras matched')
    blob = pack(records, data_date(doc))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    if args.csv:
        with args.csv.open('w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['osm_id', 'lat', 'lon', 'direction', 'maker', 'flags', 'operator'])
            for lat, lon, direction, maker, flags, osm_id, operator in records:
                w.writerow([osm_id, lat / 1e6, lon / 1e6, '' if direction == DIR_UNKNOWN else direction, maker, flags, operator])
    makers = {}
    for r in records:
        makers[r[3]] = makers.get(r[3], 0) + 1
    print(f'{len(records)} cameras, {len(blob)} bytes -> {args.out}  (flock={makers.get(MAKER_FLOCK, 0)}, '
          f'other={len(records) - makers.get(MAKER_FLOCK, 0)})')


if __name__ == '__main__':
    main()
