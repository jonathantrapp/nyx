"""Render the case STL files to PNG pictures (software z-buffer, numpy only).

    python cad/case/render_case.py            # -> cad/case/renders/*.png and sheet.png

Reads cad/case/out/*.stl written by nyx_case.py. Printed parts are drawn black (matte, slightly
glossy), the knob gunmetal, bought-part stand-ins in grey, so you can judge the look against the video.
"""
import math
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
RENDERS = os.path.join(HERE, 'renders')


def load_stl(path):
    raw = open(path, 'rb').read()
    if raw[:5] == b'solid' and b'facet' in raw[:300]:          # ASCII
        vals = []
        for line in raw.decode('ascii', 'ignore').splitlines():
            line = line.strip()
            if line.startswith('vertex'):
                vals.append([float(v) for v in line.split()[1:4]])
        return np.array(vals, dtype=np.float64).reshape(-1, 3, 3)
    n = struct.unpack_from('<I', raw, 80)[0]
    rec = np.frombuffer(raw, dtype=np.dtype([('n', '<f4', 3), ('v', '<f4', (3, 3)), ('a', '<u2')]), count=n, offset=84)
    return rec['v'].astype(np.float64)


def png_write(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def png_read(path):
    raw = open(path, 'rb').read()
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(raw):
        n = struct.unpack_from('>I', raw, pos)[0]
        tag = raw[pos + 4:pos + 8]
        data = raw[pos + 8:pos + 8 + n]
        if tag == b'IHDR':
            w, h = struct.unpack('>II', data[:8])
        elif tag == b'IDAT':
            idat += data
        pos += 12 + n
    d = zlib.decompress(idat)
    stride = w * 3 + 1
    out = np.zeros((h, w, 3), np.uint8)
    prev = np.zeros(w * 3, np.int32)
    for y in range(h):
        ft = d[y * stride]
        line = np.frombuffer(d[y * stride + 1:(y + 1) * stride], np.uint8).astype(np.int32)
        if ft == 0:
            cur = line
        elif ft == 2:
            cur = (line + prev) & 255
        elif ft == 1:
            cur = line.copy()
            for i in range(3, len(cur)):
                cur[i] = (cur[i] + cur[i - 3]) & 255
        else:
            raise SystemExit('unsupported PNG filter %d in %s' % (ft, path))
        out[y] = cur.reshape(w, 3).astype(np.uint8)
        prev = cur
    return out


def look_at(eye, target, up=(0, 0, 1)):
    f = np.array(target, float) - np.array(eye, float)
    f /= np.linalg.norm(f)
    r = np.cross(f, np.array(up, float))
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    return r, u, f


def render(meshes, azim, elev, size=(1100, 760), ss=2, dist=None, fov=24.0, target=None, ortho=False, screen=None):
    """meshes: list of (triangles Nx3x3, base colour rgb 0-255, shininess). Returns an RGB uint8 image."""
    allv = np.concatenate([m[0].reshape(-1, 3) for m in meshes])
    lo, hi = allv.min(0), allv.max(0)
    centre = (lo + hi) / 2 if target is None else np.array(target, float)
    radius = np.linalg.norm(hi - lo) / 2
    dist = dist or radius / math.tan(math.radians(fov / 2)) * 1.05
    a, e = math.radians(azim), math.radians(elev)
    eye = centre + dist * np.array([math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e)])
    r, u, f = look_at(eye, centre)
    W, H = size[0] * ss, size[1] * ss
    focal = (H / 2) / math.tan(math.radians(fov / 2))
    zbuf = np.full((H, W), np.inf)
    img = np.zeros((H, W, 3))
    # background: soft vertical gradient
    yy = np.linspace(0, 1, H)[:, None, None]
    xx = np.linspace(-1, 1, W)[None, :, None]
    img[:] = (np.array([74, 76, 86]) * (1 - yy) + np.array([20, 21, 25]) * yy) * (1.0 - 0.28 * xx ** 2)

    light_dirs = [(np.array([-0.5, -0.7, 0.9]), 1.0, np.array([1.0, 0.97, 0.92])),     # key
                  (np.array([0.8, -0.2, 0.4]), 0.35, np.array([0.8, 0.88, 1.0])),      # cool fill
                  (np.array([0.0, 0.9, 0.5]), 0.5, np.array([1.0, 1.0, 1.0]))]          # rim from behind
    light_dirs = [(d / np.linalg.norm(d), k, c) for d, k, c in light_dirs]

    for tris, base, shin in meshes:
        n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        ln = np.linalg.norm(n, axis=1)
        ok = ln > 1e-12
        tris, n, ln = tris[ok], n[ok], ln[ok]
        n = n / ln[:, None]
        # camera space
        rel = tris - eye
        cx, cy, cz = rel @ r, rel @ u, rel @ f
        zc = cz
        if ortho:
            sx = W / 2 + cx * (H / (2 * radius * 1.05))
            sy = H / 2 - cy * (H / (2 * radius * 1.05))
        else:
            sx = W / 2 + focal * cx / zc
            sy = H / 2 - focal * cy / zc
        vdir = -(rel.mean(1))
        vdir /= np.linalg.norm(vdir, axis=1)[:, None]
        facing = (n * vdir).sum(1)
        # shading per triangle
        col = np.zeros((len(tris), 3))
        amb = 0.16
        col += amb
        for d, k, c in light_dirs:
            diff = np.clip(n @ d, 0, 1)
            col += (diff * k)[:, None] * c
            hvec = d[None, :] + vdir
            hvec /= np.linalg.norm(hvec, axis=1)[:, None]
            spec = np.clip((n * hvec).sum(1), 0, 1) ** shin
            col += (spec * 0.9 * k)[:, None] * c * (0.6 if shin > 20 else 0.25)
        # a subtle sky/ground bounce makes the black parts read as glossy plastic
        col += (0.12 * np.clip(n[:, 2], -1, 1) + 0.06)[:, None]
        shade = np.clip(col, 0, 1.5) * (np.array(base) / 255.0)
        for i in range(len(tris)):
            if facing[i] <= 0:
                continue
            x, y, z = sx[i], sy[i], zc[i]
            x0, x1 = int(max(0, np.floor(x.min()))), int(min(W - 1, np.ceil(x.max())))
            y0, y1 = int(max(0, np.floor(y.min()))), int(min(H - 1, np.ceil(y.max())))
            if x1 < x0 or y1 < y0:
                continue
            den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
            if abs(den) < 1e-12:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den
            w1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            depth = w0 * z[0] + w1 * z[1] + w2 * z[2]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            m = inside & (depth < sub)
            if m.any():
                sub[m] = depth[m]
                img[y0:y1 + 1, x0:x1 + 1][m] = shade[i] * 255.0
    if screen is not None:
        quad, tex = screen          # quad: 4 corners (TL, TR, BR, BL) in case coordinates; tex: HxWx3 uint8
        q = np.array(quad, float)
        rel = q - eye
        cxs, cys, czs = rel @ r, rel @ u, rel @ f
        qx = W / 2 + focal * cxs / czs
        qy = H / 2 - focal * cys / czs
        th, tw = tex.shape[:2]
        uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
        for tri in ((0, 1, 2), (0, 2, 3)):
            x, y, z = qx[list(tri)], qy[list(tri)], czs[list(tri)]
            x0, x1 = int(max(0, np.floor(x.min()))), int(min(W - 1, np.ceil(x.max())))
            y0, y1 = int(max(0, np.floor(y.min()))), int(min(H - 1, np.ceil(y.max())))
            if x1 < x0 or y1 < y0:
                continue
            den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den
            w1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            iz = w0 / z[0] + w1 / z[1] + w2 / z[2]                    # perspective-correct interpolation
            depth = 1.0 / iz
            uu = (w0 * uvs[tri[0]][0] / z[0] + w1 * uvs[tri[1]][0] / z[1] + w2 * uvs[tri[2]][0] / z[2]) / iz
            vv = (w0 * uvs[tri[0]][1] / z[0] + w1 * uvs[tri[1]][1] / z[1] + w2 * uvs[tri[2]][1] / z[2]) / iz
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            m = inside & (depth < sub + 0.6)
            if m.any():
                ti = np.clip((vv * th).astype(int), 0, th - 1)
                tj = np.clip((uu * tw).astype(int), 0, tw - 1)
                px = tex[ti, tj].astype(float)
                sheen = (0.04 + 0.10 * np.clip(1.0 - (gy - y0) / max(1, (y1 - y0)), 0, 1))[..., None] * 255.0   # faint glass reflection
                img[y0:y1 + 1, x0:x1 + 1][m] = np.clip(px[m] + sheen[m] * 0.6, 0, 255)
    img = np.clip(img, 0, 255)
    img = img.reshape(size[1], ss, size[0], ss, 3).mean(axis=(1, 3))
    return img.astype(np.uint8)


COLORS = {
    'nyx_shell': ((26, 27, 31), 60),
    'nyx_base': ((20, 21, 24), 40),
    'ref_knob': ((88, 91, 100), 70),
    'ref_knob_dot': ((236, 170, 70), 4),
    'nyx_mute_cap': ((30, 31, 36), 60),
    'ref_encoder': ((40, 90, 40), 10),
    'ref_tft_module': ((8, 9, 13), 90),
    'ref_esp32_board': ((30, 40, 60), 10),
    'ref_speaker': ((70, 70, 70), 10),
    'ref_gps_bar': ((184, 162, 109), 10),
    'ref_gps_breakout': ((35, 100, 65), 10),
    'ref_gps_cable_route': ((36, 36, 39), 10),
    'ref_mute_cap': ((26, 27, 31), 60),
}


def load_meshes(names):
    out = []
    for n in names:
        path = os.path.join(OUT, n + '.stl')
        if not os.path.exists(path):
            continue
        col, shin = COLORS.get(n, ((120, 120, 120), 10))
        out.append((load_stl(path), col, shin))
    return out


def screen_texture(name):
    import json
    quad = json.load(open(os.path.join(OUT, 'screen_quad.json')))
    tex_path = os.path.join(HERE, '..', '..', 'tools', 'tft_preview', name + '.png')
    return quad, png_read(tex_path)


def main():
    os.makedirs(RENDERS, exist_ok=True)
    exterior = ['nyx_shell', 'nyx_base', 'ref_knob', 'ref_knob_dot', 'nyx_mute_cap', 'ref_tft_module']
    views = {
        'iso_front_left': (-34, 20, 'camera_ahead'), 'iso_front_right': (32, 18, 'listening'), 'front': (0, 3, 'camera_ahead'),
        'side': (90, 0, None), 'top': (0, 88, None), 'rear': (180, 16, None), 'under': (-30, -50, None),
    }
    for name, (az, el, shot) in views.items():
        print('rendering', name, flush=True)
        scr = screen_texture(shot) if shot else None
        img = render(load_meshes(exterior), az, el, size=(1000, 680), screen=scr)
        png_write(os.path.join(RENDERS, name + '.png'), img)
    # cutaway: the left half of the shell removed so the fit of the bought parts can be judged
    print('rendering cutaway', flush=True)
    meshes = load_meshes(['nyx_shell', 'nyx_base', 'ref_knob', 'ref_knob_dot', 'ref_encoder', 'ref_tft_module', 'ref_esp32_board', 'ref_speaker', 'ref_gps_bar', 'ref_gps_breakout', 'ref_gps_cable_route', 'nyx_mute_cap'])
    cut = []
    for tris, col, shin in meshes:
        if col == COLORS['nyx_shell'][0]:
            keep = tris[:, :, 0].mean(1) < 64.0          # keep the left half only (x < half width)
            tris = tris[keep]
        cut.append((tris, col, shin))
    png_write(os.path.join(RENDERS, 'cutaway.png'), render(cut, 58, 28, size=(1000, 680)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
