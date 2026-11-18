# Camera map (ALPR coordinates)

`cameras.bin` is what the device loads for GPS camera alerts. It is built from OpenStreetMap
nodes tagged `surveillance:type=ALPR` (data (c) OpenStreetMap contributors, ODbL).
Everything near this file is public map data; there are no captures in here.

Refresh and rebuild:

```
curl -A "esp32-camera-map-builder/0.1" --data-urlencode "data@cameras/src/query.txt" ^
     https://overpass-api.de/api/interpreter -o cameras/src/osm_alpr_us.json
python -I tools/build_camera_map.py cameras/src/osm_alpr_us.json --csv cameras/review.csv
python -I tools/test_camera_native.py
```

`cameras/src/query.txt` is the Overpass query (US-wide, nodes only). To shrink the file for a
region: `--near LAT LON KM` or `--bbox S W N E`. Current build: 145,851 cameras, 1.75 MB
(snapshot 2026-11-10; 117,751 tagged Flock Safety, the rest other makers or unknown).

Caveats worth knowing: OSM is volunteer-mapped, so some cameras are missing and a few are stale.
About 1 in 100 has no facing direction. The firmware treats a camera with no or several directions
as "facing you".
