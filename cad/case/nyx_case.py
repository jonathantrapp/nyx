"""Nyx dash unit: parametric case (CadQuery). Run:  python cad/case/nyx_case.py

Builds the printable parts (shell, base plate, mute button cap), simple stand-ins for the bought parts (to
check the fit), runs an overlap check, and writes STEP + STL files to cad/case/out/. Open the STEP files in NX
(File > Import > STEP) or any CAD program; the STL files are for slicers and for render_case.py.

EVERY number marked ASSUMED below is a guess from typical parts, because the screen, GPS, speaker and knob
have not been measured yet. Measure them when they arrive (cad/case/MEASURE.md), change the number here, run
the script again. Millimetres. Axes: X = width (left to right as the driver sees it), Y = depth (0 = the
driver-facing front, +Y = toward the windscreen), Z = up.
"""
import json
import math
import os
import sys

import cadquery as cq

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')

P = dict(
    # --- outside shape (design choices) ---
    W=130.0, D=84.0, HF=55.0, HR=28.0,      # lower front for the printed 42.5 mm board height
    CREST_Y=36.0, CREST_Z=50.0,             # keep the soft crown; lower it with the front
    TILT=8.0,                               # the front face leans back this many degrees
    WALL=2.4, BASE_T=3.0, BASE_Z0=0.6,      # base plate sits 0.6 mm up inside the shell: a crisp shadow line
    R_SIDE=4.5, R_CREST=45.0,

    # --- front layout (face coordinates: u = across from the left edge, v = up the face from its bottom edge) ---
    SCR_U=19.0, SCR_W=50.0, SCR_H=38.0,     # moves the 77 mm PCB clear of the left wall
    GLASS_W=61.0, GLASS_H=44.0,             # ASSUMED glass 60.3 x 42.7 + clearance
    TFT_PCB_W=77.0, TFT_PCB_H=42.5, TFT_PCB_T=1.6, # PROVISIONAL: real board's silkscreen, not calipers
    KNOB_U=104.5, KNOB_D=25.0, KNOB_H=15.0, KNOB_GAP=1.2,   # knob from the brief; gap = air under it
    KNOB_SHAFT_HOLE=7.4,                    # ASSUMED KY-040 M7 bushing
    MUTE_U=86.5, MUTE_V=17.0, MUTE_W=16.0, MUTE_H=6.5,
    LED_U=86.5, LED_V=31.0, LED_HOLE=2.4,
    WORD_U=86.5, WORD_V=40.0, WORD_SIZE=4.4, WORD_PITCH=6.2,

    # --- top and rear ---
    SPK_Y=68.0, SPK_X=100.0, SPK_D=30.0, SPK_HOLE=1.7, SPK_PITCH=3.3,   # grille on the rear slope; ASSUMED 28-30 mm speaker
    GPS_Y=62.0, GPS_X=28.0,
    GPS_L=19.0, GPS_W=6.0, GPS_H=6.0,       # 19x6 reference bundle; H=6 is a clearance envelope, NOT a measured thickness
    GPS_BAY_L=33.0, GPS_BAY_W=14.0,          # open-ended tray; foam shims/strap accommodate bundle variations
    GPS_CABLE_L=95.0,                       # PROVISIONAL reference bundle; slack/reach check, no taut cable
    GPS_BOARD_X=27.0, GPS_BOARD_Y=44.0,
    GPS_BOARD_L=26.0, GPS_BOARD_W=26.0, GPS_BOARD_H=10.0, # clearance reservation for an unmeasured breakout/battery
    BOARD_L=63.5, BOARD_W=25.5, BOARD_X=65.0, BOARD_RAIL=3.0,           # ESP32-S3 DevKitC-1, USB end at the rear
    USB_SLOT_W=33.0, USB_SLOT_H=9.8,                                     # opening for both USB-C ports; ASSUMED spacing
    BTN_HOLES=(-10.0, 10.0),                                             # BOOT / RST pinholes, x from the board centre
    SCREW_D=2.2, POST_D=7.0,
)
P['FACE_LEN'] = P['HF'] / math.cos(math.radians(P['TILT']))
P['SCR_V'] = P['FACE_LEN'] / 2.0 + 1.0                 # window and knob a little above centre: the leaning board hangs lower at the back
P['FLOOR'] = P['BASE_Z0'] + P['BASE_T']                # top of the base plate


def rad(d):
    return d * math.pi / 180.0


# ---------------------------------------------------------------------------------------------
def side_profile(p):
    top_y = p['HF'] * math.tan(rad(p['TILT']))
    return [(0.0, 0.0), (p['D'], 0.0), (p['D'], p['HR']), (p['CREST_Y'], p['CREST_Z']), (top_y, p['HF'])]


def inset_polygon(pts, d):
    n = len(pts)
    lines = []
    for i in range(n):
        (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % n]
        dx, dy = x1 - x0, y1 - y0
        ln = math.hypot(dx, dy)
        nx, ny = -dy / ln, dx / ln
        lines.append(((x0 + nx * d, y0 + ny * d), (dx, dy)))
    out = []
    for i in range(n):
        (px, py), (dx, dy) = lines[i - 1]
        (qx, qy), (ex, ey) = lines[i]
        t = ((qx - px) * ey - (qy - py) * ex) / (dx * ey - dy * ex)
        out.append((px + dx * t, py + dy * t))
    return out


def prism(points_yz, x0, x1):
    return cq.Workplane('YZ', origin=(x0, 0, 0)).polyline(points_yz).close().extrude(x1 - x0)


def face_frame(p, shape):
    """Shapes are built with the front face on the plane y = 0 (u = x, v = z, into the case = +y); this tilts
    them onto the leaning face."""
    return shape.rotate((0, 0, 0), (1, 0, 0), -p['TILT'])


def box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane('XY').box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def cyl_y(u, v, d, y0, y1):
    return cq.Workplane('XZ', origin=(0, y1, 0)).center(u, v).circle(d / 2.0).extrude(y1 - y0)


def pill_y(u, v, w, h, y0, y1):
    return cq.Workplane('XZ', origin=(0, y1, 0)).center(u, v).slot2D(w, h, 0).extrude(y1 - y0)


def slope_angle(p):
    return math.degrees(math.atan2(p['CREST_Z'] - p['HR'], p['D'] - p['CREST_Y']))


def slope_z(p, y):
    return p['CREST_Z'] + (p['HR'] - p['CREST_Z']) * (y - p['CREST_Y']) / (p['D'] - p['CREST_Y'])


def post_positions(p):
    return [(5.7, 29.0), (p['W'] - 5.7, 29.0), (5.7, p['D'] - 12.0), (p['W'] - 5.7, p['D'] - 12.0)]


def gps_shelf_z(p):
    # Horizontal antenna, with clearance to the LOWEST roof point above its whole bay.
    return slope_z(p, p['GPS_Y'] + p['GPS_BAY_W'] / 2 + 2.0) - p['WALL'] - p['GPS_H'] - 1.5


def gps_cable_points(p):
    z=gps_shelf_z(p)+p['GPS_H']/2+0.1
    return [(p['GPS_X']+p['GPS_L']/2,p['GPS_Y'],z),
            (p['GPS_X']+p['GPS_BAY_L']/2+3,p['GPS_Y'],z),
            (p['GPS_X']+p['GPS_BAY_L']/2+3,p['GPS_Y']-12,z-5),
            (p['GPS_BOARD_X'],p['GPS_BOARD_Y'],p['FLOOR']+p['GPS_BOARD_H']+3)]


def union_all(items):
    out = None
    for it in items:
        out = it if out is None else out.union(it)
    return out


# ---------------------------------------------------------------------------------------------
def build_outer(p):
    body = prism(side_profile(p), 0.0, p['W'])

    def fillet_at(solid, y, z, r):
        sel = cq.selectors.BoxSelector((-1, y - 0.5, z - 0.5), (p['W'] + 1, y + 0.5, z + 0.5))
        return solid.edges(sel).fillet(r)
    top_y = p['HF'] * math.tan(rad(p['TILT']))
    body = fillet_at(body, p['CREST_Y'], p['CREST_Z'], p['R_CREST'])
    body = fillet_at(body, top_y, p['HF'], 5.0)          # the lip above the screen
    body = fillet_at(body, p['D'], p['HR'], 3.5)         # rear top edge
    body = fillet_at(body, p['D'], 0.0, 1.0)
    body = fillet_at(body, 0.0, 0.0, 1.0)
    return body.faces('<X or >X').edges().fillet(p['R_SIDE'])


def build_cavity(p):
    inner = inset_polygon(side_profile(p), p['WALL'])
    inner = [(y, (-2.0 if abs(z - p['WALL']) < 1e-6 else z)) for (y, z) in inner]     # open at the bottom
    return prism(inner, p['WALL'], p['W'] - p['WALL'])


def wordmark(p):
    """Engraved NYX (letters cut 0.4 mm into the face). Returns None if the CAD kernel has no font."""
    letters = None
    try:
        for i, ch in enumerate('NYX'):
            x = p['WORD_U'] + (i - 1) * p['WORD_PITCH']
            t = (cq.Workplane('XZ', origin=(x, 0.0, p['WORD_V'])).text(ch, p['WORD_SIZE'], -0.4, combine=False,
                                                                        halign='center', valign='center', kind='regular'))
            letters = t if letters is None else letters.union(t)
    except Exception as e:                                # no font installed: skip the engraving, keep the rest
        print('  (wordmark skipped: %s)' % e)
        return None
    return letters


def build_shell(p):
    t = p['WALL']
    outer = build_outer(p)
    shell = outer.cut(build_cavity(p))

    # screw posts for the base
    posts = union_all([cq.Workplane('XY').center(x, y).circle(p['POST_D'] / 2.0).extrude(p['HF'] - 4.0).translate((0, 0, p['FLOOR']))
                       for (x, y) in post_positions(p)]).intersect(outer)
    shell = shell.union(posts)
    for (x, y) in post_positions(p):
        shell = shell.cut(cq.Workplane('XY').center(x, y).circle(p['SCREW_D'] / 2.0).extrude(16.0).translate((0, 0, p['FLOOR'] - 0.5)))

    v0 = p['SCR_V']
    cx = p['SCR_U'] + p['SCR_W'] / 2.0
    cuts = [
        box(p['SCR_U'], p['SCR_U'] + p['SCR_W'], -3.0, t + 1.5, v0 - p['SCR_H'] / 2, v0 + p['SCR_H'] / 2),          # window
        box(cx - p['GLASS_W'] / 2, cx + p['GLASS_W'] / 2, t - 1.4, t + 8.0, v0 - p['GLASS_H'] / 2, v0 + p['GLASS_H'] / 2),  # glass pocket, 1.0 mm lip
        cyl_y(p['KNOB_U'], v0, p['KNOB_SHAFT_HOLE'], -3.0, t + 2.0),                                                   # knob shaft
        cyl_y(p['KNOB_U'], v0, 14.0, -3.0, 1.6),                                                                       # nut pocket
        pill_y(p['MUTE_U'], p['MUTE_V'], p['MUTE_W'] + 0.5, p['MUTE_H'] + 0.5, -3.0, t + 1.0),
        cyl_y(p['LED_U'], p['LED_V'], p['LED_HOLE'], -3.0, t + 1.0),
    ]
    shell = shell.cut(face_frame(p, union_all(cuts)))
    ring = cyl_y(p['KNOB_U'], v0, 32.0, -3.0, 0.4).cut(cyl_y(p['KNOB_U'], v0, 30.8, -3.0, 0.4))    # fine ring groove round the knob
    shell = shell.cut(face_frame(p, ring))
    wm = wordmark(p)
    if wm is not None:
        shell = shell.cut(face_frame(p, wm))

    # inside the face: a rim that locates the glass, a collar for the 5 mm LED, a shelf for the mute switch
    gw, gh = p['GLASS_W'] + 3.2, p['GLASS_H'] + 3.2
    rim = box(cx - gw / 2, cx + gw / 2, t - 1.4, t + 2.7, v0 - gh / 2, v0 + gh / 2).cut(      # as tall as the glass; the board behind it is held by foam tape until its real holes are known
        box(cx - p['GLASS_W'] / 2, cx + p['GLASS_W'] / 2, t - 1.5, t + 2.8, v0 - p['GLASS_H'] / 2, v0 + p['GLASS_H'] / 2))
    collar = cyl_y(p['LED_U'], p['LED_V'], 7.0, t - 0.5, t + 5.0).cut(cyl_y(p['LED_U'], p['LED_V'], 5.2, t - 0.6, t + 5.1))
    shelf = box(p['MUTE_U'] - 9.0, p['MUTE_U'] + 9.0, t - 0.5, t + 5.0, p['MUTE_V'] - 9.0, p['MUTE_V'] - 6.0)   # under the cap flange
    inner = union_all([rim, collar, shelf])
    shell = shell.union(face_frame(p, inner).intersect(outer))
    # The real 77 mm board extends behind the center controls. Notch their INTERNAL support
    # only where the PCB needs room; keep 0.3 mm per side, without changing the glass bezel.
    pcb_clear=box(cx-p['TFT_PCB_W']/2-0.3,cx+p['TFT_PCB_W']/2+0.3,
                  t+2.5,t+3.1+p['TFT_PCB_T'],v0-p['TFT_PCB_H']/2-0.3,v0+p['TFT_PCB_H']/2+0.3)
    shell = shell.cut(face_frame(p,pcb_clear))
    shell = shell.cut(face_frame(p, cyl_y(p['LED_U'], p['LED_V'], p['LED_HOLE'], -3.0, t + 1.0)))
    shell = shell.cut(face_frame(p, pill_y(p['MUTE_U'], p['MUTE_V'], p['MUTE_W'] + 0.5, p['MUTE_H'] + 0.5, -3.0, t + 1.0)))

    shell = cut_grille(p, shell)
    z_usb = p['FLOOR'] + p['BOARD_RAIL'] + 1.6
    usb = box(p['BOARD_X'] - p['USB_SLOT_W'] / 2, p['BOARD_X'] + p['USB_SLOT_W'] / 2, p['D'] - t - 2.0, p['D'] + 2.0, z_usb, z_usb + p['USB_SLOT_H'])
    shell = shell.cut(usb.edges('|Y').fillet(2.0))
    for x in (-1.0, p['W'] - t - 1.0):                       # low side vents near the back (a parked car gets hot)
        for k in range(5):
            shell = shell.cut(box(x, x + t + 2.0, 42.0 + k * 6.0, 42.0 + k * 6.0 + 1.6, 7.0, 17.0))
    return shell


def cut_grille(p, shell):
    ang = slope_angle(p)
    z0 = slope_z(p, p['SPK_Y'])
    pitch, r = p['SPK_PITCH'], p['SPK_D'] / 2.0
    rows = int(r / (pitch * 0.866)) + 1
    holes = []
    for j in range(-rows, rows + 1):
        for i in range(-rows - 1, rows + 2):
            x = i * pitch + (pitch / 2.0 if j % 2 else 0.0)
            yy = j * pitch * 0.866
            if math.hypot(x, yy) <= r - p['SPK_HOLE']:
                holes.append(cq.Workplane('XY').center(x, yy).circle(p['SPK_HOLE'] / 2.0).extrude(10.0).translate((0, 0, -6.0)))
    grille = union_all(holes).rotate((0, 0, 0), (1, 0, 0), -ang).translate((p['SPK_X'], p['SPK_Y'], z0))
    shell = shell.cut(grille)
    # speaker seat: a ring on the inside of the slope that keeps the speaker from sliding
    ring = (cq.Workplane('XY').circle(p['SPK_D'] / 2.0 + 2.0).circle(p['SPK_D'] / 2.0 + 0.3).extrude(7.0).translate((0, 0, -8.0))
            .rotate((0, 0, 0), (1, 0, 0), -ang).translate((p['SPK_X'], p['SPK_Y'], z0)))
    shell = shell.union(ring.intersect(build_outer(p)))
    shell = shell.cut(grille)
    for dx in p['BTN_HOLES']:                                # pinholes for BOOT and RST
        shell = shell.cut(cq.Workplane('XY').center(p['BOARD_X'] + dx, p['D'] - 7.0).circle(1.3).extrude(40.0).translate((0, 0, 5.0)))
    return shell


def build_base(p):
    t = p['WALL']
    z0, z1 = p['BASE_Z0'], p['FLOOR']
    front_y = t / math.cos(rad(p['TILT'])) + z1 * math.tan(rad(p['TILT'])) + 0.25
    base = box(t + 0.25, p['W'] - t - 0.25, front_y, p['D'] - t - 0.25, z0, z1).edges('|Z').fillet(3.0)
    for (x, y) in post_positions(p):                          # countersunk holes under the posts
        base = base.cut(cq.Workplane('XY').center(x, y).circle(1.4).extrude(p['BASE_T'] + 1.0).translate((0, 0, z0 - 0.5)))
        base = base.cut(cq.Workplane('XY').workplane(offset=z1 - 1.6).center(x, y).circle(1.4).workplane(offset=1.6).circle(2.7).loft())
    base = base.cut(box(p['W'] / 2 - 35.0, p['W'] / 2 + 35.0, p['D'] / 2 - 25.0, p['D'] / 2 + 25.0, z0, z0 + 0.8))       # 3M dash-pad recess
    for (x, y) in [(18.0, 24.0), (p['W'] - 18.0, 24.0), (18.0, p['D'] - 22.0), (p['W'] - 18.0, p['D'] - 22.0)]:           # rubber feet
        base = base.cut(cq.Workplane('XY').center(x, y).circle(4.5).extrude(1.0).translate((0, 0, z0)))
    # board tray: two rails that locate the ESP32 board with its USB end toward the rear
    by = p['D'] - t - 0.4 - p['BOARD_L'] / 2.0
    for sgn in (-1, 1):
        xin = p['BOARD_X'] + sgn * (p['BOARD_W'] / 2 + 0.3)
        base = base.union(box(min(xin, xin + sgn * 1.6), max(xin, xin + sgn * 1.6), by - p['BOARD_L'] / 2 + 4.0, by + p['BOARD_L'] / 2, z1, z1 + p['BOARD_RAIL'] + 2.0))   # stops 4 mm short of the board's front end: clear of the screen's pins
    # A shallow, flat GNSS bar cradle on two base-mounted pillars. Open ends allow a longer bar;
    # the large bay is a design allowance rather than a claimed measurement of the owner's part.
    gx, gy, gz = p['GPS_X'], p['GPS_Y'], gps_shelf_z(p)
    hl, hw = p['GPS_BAY_L'] / 2, p['GPS_BAY_W'] / 2
    tray = box(gx-hl-1.2,gx+hl+1.2,gy-hw-1.2,gy+hw+1.2,gz-1.2,gz)
    for side in (-1,1):
        yc=gy+side*(hw+0.6)
        tray=tray.union(box(gx-hl-1.2,gx+hl+1.2,yc-0.6,yc+0.6,gz,gz+1.8))
        # A soft tape/strap passes through each slot; no clamp force on the ceramic.
        tray=tray.cut(box(gx-4,gx+4,yc-1.0,yc+1.0,gz-1.4,gz-0.2))
    for side in (-1,1):
        xc=gx+side*(hl-0.5)
        base=base.union(box(xc-1.6,xc+1.6,gy-2,gy+2,z1,gz))
    base=base.union(tray)
    # Generic breakout pads: strap/foam mounting until real hole positions are known.
    for dx in (-1,1):
        for dy in (-1,1):
            x=p['GPS_BOARD_X']+dx*(p['GPS_BOARD_W']/2-2)
            y=p['GPS_BOARD_Y']+dy*(p['GPS_BOARD_L']/2-2)
            base=base.union(box(x-2,x+2,y-2,y+2,z1,z1+2))
    return base


def build_mute_cap(p):
    t = p['WALL']
    body = pill_y(p['MUTE_U'], p['MUTE_V'], p['MUTE_W'], p['MUTE_H'], -1.0, t + 1.0)
    body = body.edges('<Y').fillet(1.0) if False else body
    flange = pill_y(p['MUTE_U'], p['MUTE_V'], p['MUTE_W'] + 4.0, p['MUTE_H'] + 4.0, t + 1.0, t + 2.2)
    stem = cyl_y(p['MUTE_U'], p['MUTE_V'], 3.0, t + 2.2, t + 5.5)
    return face_frame(p, body.union(flange).union(stem))


# ---------------------------------------------------------------------------------------------
def knurled_knob(p):
    """Gunmetal knob: knurled sides, rounded top with a shallow dish. Built facing out of the face."""
    r, h, n = p['KNOB_D'] / 2.0, p['KNOB_H'], 72
    pts = []
    for i in range(n * 2):
        a = i * math.pi / n
        rr = r if i % 2 == 0 else r - 0.55
        pts.append((rr * math.cos(a), rr * math.sin(a)))
    star = cq.Workplane('XZ').polyline(pts).close().extrude(h)                         # toward -Y
    rounder = cq.Workplane('XZ').circle(r + 0.1).extrude(h).edges('<Y').fillet(2.2)    # rounds the top edge
    knob = star.intersect(rounder)
    dish = cq.Workplane('XZ', origin=(0, -h + 0.5, 0)).circle(8.0).extrude(1.0)
    knob = knob.cut(dish)
    dot_cut = cq.Workplane('XZ', origin=(0, -h + 0.6, 0)).center(0, 5.3).circle(1.4).extrude(1.0)
    return knob.cut(dot_cut), dot_cut.translate((0, 0.0, 0))


def build_standins(p):
    t = p['WALL']
    v = p['SCR_V']
    parts = {}
    knob, dot = knurled_knob(p)
    place = lambda s: face_frame(p, s.translate((p['KNOB_U'], -p['KNOB_GAP'], v)))
    parts['knob'] = place(knob)
    parts['knob_dot'] = place(dot.translate((0, 0, 0)))
    enc = box(p['KNOB_U'] - 13.0, p['KNOB_U'] + 13.0, t + 6.5, t + 8.1, v - 9.75, v + 9.75).union(
        box(p['KNOB_U'] - 6.0, p['KNOB_U'] + 6.0, t + 8.1, t + 20.0, v - 6.0, v + 6.0))
    parts['encoder'] = face_frame(p, enc)
    cx = p['SCR_U'] + p['SCR_W'] / 2.0
    tft = union_all([box(cx - 30.15, cx + 30.15, t - 1.4, t + 2.8, v - 21.35, v + 21.35),                     # glass + backlight
                     box(cx - p['TFT_PCB_W'] / 2, cx + p['TFT_PCB_W'] / 2, t + 2.8, t + 2.8+p['TFT_PCB_T'], v - p['TFT_PCB_H'] / 2, v + p['TFT_PCB_H'] / 2),
                     box(cx-p['TFT_PCB_W']/2,cx-p['TFT_PCB_W']/2+3.5,t+4.4,t+12.5,v-p['TFT_PCB_H']/2+3,v+p['TFT_PCB_H']/2-3)]) # left short-edge header; depth still provisional
    parts['tft_module'] = face_frame(p, tft)
    by = p['D'] - t - 0.4 - p['BOARD_L'] / 2.0
    zb = p['FLOOR'] + p['BOARD_RAIL']
    parts['esp32_board'] = box(p['BOARD_X'] - p['BOARD_W'] / 2, p['BOARD_X'] + p['BOARD_W'] / 2, by - p['BOARD_L'] / 2, by + p['BOARD_L'] / 2, zb, zb + 12.0)
    ang = slope_angle(p)
    spk = cq.Workplane('XY').circle(p['SPK_D'] / 2.0 - 0.5).extrude(7.0).translate((0, 0, -9.9)).rotate((0, 0, 0), (1, 0, 0), -ang)
    parts['speaker'] = spk.translate((p['SPK_X'], p['SPK_Y'], slope_z(p, p['SPK_Y'])))
    gx,gy,gz=p['GPS_X'],p['GPS_Y'],gps_shelf_z(p)+0.1
    parts['gps_bar'] = box(gx-p['GPS_L']/2,gx+p['GPS_L']/2,gy-p['GPS_W']/2,gy+p['GPS_W']/2,gz,gz+p['GPS_H'])
    bx,by=p['GPS_BOARD_X'],p['GPS_BOARD_Y']
    parts['gps_breakout'] = box(bx-p['GPS_BOARD_W']/2,bx+p['GPS_BOARD_W']/2,by-p['GPS_BOARD_L']/2,by+p['GPS_BOARD_L']/2,p['FLOOR']+2.1,p['FLOOR']+2.1+p['GPS_BOARD_H'])
    # A route reservation, not a model of the actual coax. Both bends leave slack at the plugs.
    route=gps_cable_points(p)
    cable=None
    for a,b in zip(route,route[1:]):
        vec=cq.Vector(*b)-cq.Vector(*a)
        segment=cq.Workplane(obj=cq.Solid.makeCylinder(0.6,vec.Length,cq.Vector(*a),vec.normalized()))
        cable=segment if cable is None else cable.union(segment)
    parts['gps_cable_route']=cable
    return parts


def screen_quad(p):
    """The 3D corners of the lit picture (the active area, on the glass), for the renderer's screen texture."""
    t = p['WALL']
    v0 = p['SCR_V']
    cx = p['SCR_U'] + p['SCR_W'] / 2.0
    hw, hh = 48.96 / 2, 36.72 / 2
    pts = [(cx - hw, t - 1.43, v0 + hh), (cx + hw, t - 1.43, v0 + hh), (cx + hw, t - 1.43, v0 - hh), (cx - hw, t - 1.43, v0 - hh)]
    th = rad(p['TILT'])
    out = []
    for (x, y, z) in pts:                                    # same rotation as face_frame
        out.append([x, y * math.cos(-th) - z * math.sin(-th), y * math.sin(-th) + z * math.cos(-th)])
    return out


def export(name, wp):
    os.makedirs(OUT, exist_ok=True)
    cq.exporters.export(wp, os.path.join(OUT, name + '.step'))
    cq.exporters.export(wp, os.path.join(OUT, name + '.stl'), tolerance=0.02, angularTolerance=0.05)


def vol(wp):
    return wp.val().Volume()


def export_assembly(parts):
    assembly=cq.Assembly(name='nyx_provisional')
    for name,part in parts.items():
        rgb=(0.10,0.105,0.12) if name.startswith('nyx_') else (0.36,0.39,0.43)
        if name=='ref_gps_bar': rgb=(0.72,0.64,0.42)
        elif name=='ref_gps_breakout': rgb=(0.14,0.39,0.25)
        assembly.add(part,name=name,color=cq.Color(*rgb))
    assembly.export(os.path.join(OUT,'nyx_assembly.step'))


def main():
    p = P
    shell, base, cap = build_shell(p), build_base(p), build_mute_cap(p)
    stand = build_standins(p)
    for nm, part in (('shell', shell), ('base', base), ('mute cap', cap)):
        if len(part.solids().vals()) != 1 or not part.val().isValid():
            print('  PROBLEM  %s is not one valid solid (%d pieces)' % (nm, len(part.solids().vals())))
            return 1
    print('printed parts: shell %.1f cm3, base %.1f cm3, mute cap %.2f cm3' % (vol(shell) / 1000, vol(base) / 1000, vol(cap) / 1000))
    print('PROVISIONAL: TFT PCB from silkscreen 77 x 42.5 x 1.6; GPS reference bar 19 x 6, 95 mm cable.')
    print('GPS thickness/breakout, glass, SD socket, holes, headers and wiring still need measurements; NOT print-approved.')
    problems = 0
    checks = dict(stand)
    for name, part in checks.items():
        if name == 'knob_dot':
            continue
        for host_name, host in (('shell', shell), ('base', base)):
            try:
                v = vol(part.intersect(host))
            except Exception as e:
                print('  CHECK FAILED  %s / %s: %s' % (name,host_name,e)); problems += 1; continue
            if v > 1.0:
                print('  OVERLAP  %-12s with %-5s %8.1f mm3' % (name, host_name, v))
                problems += 1
    # the parts must not collide with each other either
    names = [n for n in stand if n != 'knob_dot']
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            if 'gps_cable_route' in (a,b) and ('gps_bar' in (a,b) or 'gps_breakout' in (a,b)):
                continue # intentional connector endpoints only; other cable collisions still count
            try:
                v = vol(stand[a].intersect(stand[b]))
            except Exception as e:
                print('  CHECK FAILED  %s / %s: %s' % (a,b,e)); problems += 1; continue
            if v > 1.0:
                print('  OVERLAP  %-12s with %-12s %8.1f mm3' % (a, b, v))
                problems += 1
    print('fit check:', 'OK, nothing collides' if not problems else '%d collision(s), see above' % problems)
    route=gps_cable_points(p)
    route_min=math.dist(route[0],route[-1])
    route_plan=sum(math.dist(a,b) for a,b in zip(route,route[1:]))
    print('GPS cable: straight %.1f mm, proposed route %.1f mm + 20 mm slack; reference cable %.1f mm (confirm actual)' % (route_min,route_plan,p['GPS_CABLE_L']))
    if route_plan+20>p['GPS_CABLE_L']: print('  PROBLEM: GPS cable reach'); problems+=1
    export('nyx_shell', shell)
    export('nyx_base', base)
    export('nyx_mute_cap', cap)
    for name, part in stand.items():
        export('ref_' + name, part)
    for ext in ('.step','.stl'):
        old=os.path.join(OUT,'ref_gps_patch'+ext)
        if os.path.exists(old): os.unlink(old) # superseded generated square placeholder only
    export_assembly(dict(nyx_shell=shell,nyx_base=base,nyx_mute_cap=cap,**{'ref_'+n:s for n,s in stand.items()}))
    with open(os.path.join(OUT, 'screen_quad.json'), 'w') as f:
        json.dump(screen_quad(p), f)
    with open(os.path.join(OUT, 'parameters.json'), 'w') as f:
        json.dump(p,f,indent=2)
    bb = shell.val().BoundingBox()
    print('overall size %.1f x %.1f x %.1f mm' % (bb.xlen, bb.ylen, bb.zlen))
    print('wrote', OUT)
    return 0 if not problems else 1


if __name__ == '__main__':
    sys.exit(main())
