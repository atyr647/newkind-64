#!/usr/bin/env python3
"""
Add painted surface detail to an original Elite ship.

The ship's geometry and face colours are exactly those in shipdata.c and
shipface.c. Each flat face is given its own texture, painted here in the
face's original colour. The paintwork is laid out over the whole ship, not
face by face, so it reads as one design: irregular hull panels, a cockpit
canopy at the nose, wing stripes and a roundel, hazard markings and scorching
by the engines, a belly hatch, weathering, and an exhaust glow on engine faces.

  detail_ship.py <ship> [points]

e.g. detail_ship.py cobra3 cobra3a_point. Writes ships/<ship>.mesh.json and
ships/<ship>_<n>.png; run tools/gen_ship_textures.py afterwards.

Textures are 16-colour tiles of at most 64x64 (what fits in the N64's
texture memory at once). A face larger than one tile is covered by up to
2x2 tiles so the detail stays sharp. Left/right mirror-image faces share
their textures.

Needs: numpy, pillow.
"""
import json
import math
import os
import sys
import zlib

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ship_export import load_ship, load_palette  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VERT_CACHE = 62          # vertices per batch (the RSP cache holds 70)
TEXEL_DENSITY = 1.0      # texels per game unit, when the face is small enough
TILE = 64
TILE_STEP = TILE - 2     # tiles overlap by a texel each side for filtering
MAX_TILES = 2            # per face, in each direction
SUPERSAMPLE = 3          # paint at 3x and average down, for clean thin lines

PANEL_W, PANEL_H = 30.0, 20.0   # plating size, in game units
SEAM = 0.9                      # half-width of a seam, in game units
TRIM = 3.5                      # width of the edge trim, in game units


def hash01(*keys):
    """A repeatable pseudo-random number in 0..1 from some integers."""
    return (zlib.crc32(repr(keys).encode()) & 0xFFFFFF) / float(0xFFFFFF)


def face_frame(normal, pts):
    """Axes to lay the face out along: u is horizontal where possible."""
    n = normal / np.linalg.norm(normal)
    if abs(n[1]) > 0.75:                 # mostly facing up or down: u = ship's x
        u = np.array([1.0, 0, 0])
    else:                                # a side: u runs along the horizon
        u = np.cross(n, [0, 1.0, 0])
    u = u - n * (u @ n)
    u = u / np.linalg.norm(u)
    v = np.cross(n, u)
    return n, u, v


def edge_distance(px, py, poly):
    """Distance from each point to the nearest edge of the polygon."""
    best = np.full(px.shape, 1e9)
    for i in range(len(poly)):
        ax, ay = poly[i]
        bx, by = poly[(i + 1) % len(poly)]
        dx, dy = bx - ax, by - ay
        ln = dx * dx + dy * dy
        t = np.clip(((px - ax) * dx + (py - ay) * dy) / ln, 0, 1) if ln > 1e-12 else 0
        best = np.minimum(best, np.hypot(px - (ax + t * dx), py - (ay + t * dy)))
    return best


def inside_poly(px, py, poly):
    inside = np.zeros(px.shape, bool)
    for i in range(len(poly)):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % len(poly)]
        cond = (y1 > py) != (y2 > py)
        with np.errstate(divide="ignore", invalid="ignore"):
            xi = (x2 - x1) * (py - y1) / (y2 - y1) + x1
        inside ^= cond & (px < xi)
    return inside


class Layout:
    """
    The ship-wide panel plan: irregular cuts across the hull (by distance
    from the centre line) and along it, shared by every face so panel lines
    run on from one face to the next and match left and right.
    """

    def __init__(self, pts, seed):
        self.lo, self.hi = pts.min(axis=0), pts.max(axis=0)
        self.half_width = max(abs(self.lo[0]), abs(self.hi[0]), 1.0)
        self.length = max(self.hi[2] - self.lo[2], 1.0)
        self.seed = seed
        self.cuts_x = self._cuts(0.0, self.half_width, seed, "x")
        self.cuts_z = self._cuts(self.lo[2], self.hi[2], seed, "z")

    @staticmethod
    def _cuts(lo, hi, seed, tag):
        cuts, pos, k = [lo - 1e-3], lo, 0
        while pos < hi:
            pos += 16.0 + 30.0 * hash01(seed, tag, k)      # panels 16-46 units
            cuts.append(pos)
            k += 1
        return np.array(cuts)

    def table(self, tag, shape):
        """A repeatable table of random numbers, one per panel."""
        return np.array([[hash01(self.seed, tag, i, j) for j in range(shape[1])] for i in range(shape[0])])


def paint_face(base_rgb, pu, pv, poly, seed, is_engine, sym_u, world, normal, layout):
    """
    Colour for every sample of a face. pu, pv are positions on the face in
    game units and poly its outline; world gives each sample's position on
    the ship, normal the way the face points.
    """
    base = np.array(base_rgb, float)
    edge = edge_distance(pu, pv, poly)

    if is_engine:
        # Exhaust: white-hot core, through yellow and orange to the face colour.
        cx = sym_u if sym_u is not None else np.mean([p[0] for p in poly])
        cy = np.mean([p[1] for p in poly])
        reach = max(edge_distance(np.array([cx]), np.array([cy]), poly)[0], 1.0)
        r = np.clip(np.hypot((pu - cx) * 0.8, pv - cy) / reach, 0, 2)
        col = np.zeros(pu.shape + (3,))
        stops = [(0.0, (255, 250, 225)), (0.3, (255, 215, 90)), (0.6, (245, 120, 25)),
                 (0.9, tuple(np.clip(base * 1.1, 0, 255))), (2.0, tuple(base * 0.45))]
        for (r0, c0), (r1, c1) in zip(stops, stops[1:]):
            m = (r >= r0) & (r <= r1)
            t = ((r - r0) / (r1 - r0))[..., None]
            col[m] = (np.array(c0) * (1 - t) + np.array(c1) * t)[m]
        col[np.abs(r - 1.0) < 0.07] = base * 0.3              # nozzle rim
        col[edge < TRIM * 0.6] = base * 0.4
        return col

    wx, wy, wz = world[..., 0], world[..., 1], world[..., 2]
    xa = np.abs(wx)                                   # distance from the centre line
    xn = xa / layout.half_width                       # 0 centre .. 1 wing tip
    zn = (wz - layout.lo[2]) / layout.length          # 0 stern .. 1 nose
    flat = abs(normal[1]) > 0.45                      # a top or bottom face
    top = normal[1] > 0.45
    bottom = normal[1] < -0.45

    col = np.broadcast_to(base, pu.shape + (3,)).copy()
    shade = np.ones(pu.shape)

    # --- hull panels ----------------------------------------------------------
    if flat:
        a, b, ca, cb = xa, wz, layout.cuts_x, layout.cuts_z
    else:                                             # sides: plan them along the face
        ca = Layout._cuts(0.0, float(pu.max()) + 1, seed, "u")
        cb = Layout._cuts(0.0, float(pv.max()) + 1, seed, "v")
        a, b = pu, pv
    ia = np.clip(np.searchsorted(ca, a) - 1, 0, len(ca) - 2)
    ib = np.clip(np.searchsorted(cb, b) - 1, 0, len(cb) - 2)
    shape = (len(ca) + 1, len(cb) + 1)
    tag = "flat" if flat else ("side", seed)
    tint = layout.table(("tint", tag), shape)
    seam_a = layout.table(("seam_a", tag), shape) > 0.30     # is this boundary a seam?
    seam_b = layout.table(("seam_b", tag), shape) > 0.30
    shade *= 0.92 + 0.16 * tint[ia, ib]

    d_lo_a, d_hi_a = a - ca[ia], ca[ia + 1] - a
    d_lo_b, d_hi_b = b - cb[ib], cb[ib + 1] - b
    groove = np.zeros(pu.shape, bool)
    light = np.zeros(pu.shape, bool)
    for dist, drawn, far in ((d_lo_a, seam_a[ia, ib], False), (d_hi_a, seam_a[ia + 1, ib], True),
                             (d_lo_b, seam_b[ia, ib], False), (d_hi_b, seam_b[ia, ib + 1], True)):
        groove |= drawn & (dist < SEAM)
        if not far:
            light |= drawn & (dist >= SEAM) & (dist < SEAM * 2.4)

    # a recessed frame running parallel to the face's own edges
    frame = np.abs(edge - 7.0) < SEAM
    groove |= frame
    light |= (edge > 7.0 + SEAM) & (edge < 7.0 + SEAM * 2.4)

    shade[light] *= 1.14
    shade[groove] *= 0.48

    # an occasional access hatch: a darker inset rectangle with a handle
    kind = layout.table(("kind", tag), shape)[ia, ib]
    in_a = (d_lo_a > 5) & (d_hi_a > 5)
    in_b = (d_lo_b > 4) & (d_hi_b > 4)
    hatch = (kind > 0.86) & in_a & in_b & (edge > 9)
    hatch_edge = hatch & ((d_lo_a < 6.2) | (d_hi_a < 6.2) | (d_lo_b < 5.2) | (d_hi_b < 5.2))
    shade[hatch] *= 0.86
    shade[hatch_edge] *= 0.55
    vent = (kind < 0.10) & in_a & in_b & (edge > 9) & (d_lo_a > 7) & (d_hi_a > 7)
    shade[vent & (np.floor(b / 1.7) % 2 == 0)] *= 0.42

    # --- markings ---------------------------------------------------------------
    inner = edge > 3.0
    if top:
        # cockpit canopy: dark glass across the nose, with glazing bars and a glint
        canopy = (zn > 0.60) & (zn < 0.86) & (xn < 0.20) & inner
        bars = canopy & ((np.abs((xa / 9.0) % 1.0 - 0.5) > 0.42) | (np.abs(zn - 0.73) < 0.012))
        glass = canopy & ~bars
        glint = glass & (np.abs((xa * 0.9 + wz) % 26.0 - 6.0) < 2.2)
        col[glass] = (22, 34, 58)
        col[glint] = (120, 160, 205)
        col[bars] = np.clip(base * 0.55, 0, 255)
        shade[canopy] = 1.0
        rim = (zn > 0.585) & (zn < 0.875) & (xn < 0.215) & inner & ~canopy
        shade[rim] *= 0.55

        # wing stripes and a roundel
        stripe = inner & (((xn > 0.50) & (xn < 0.535)) | ((xn > 0.56) & (xn < 0.575))) & (zn < 0.62) & (zn > 0.10)
        col[stripe] = (225, 225, 215)
        r = np.hypot(xa - 0.34 * layout.half_width, (wz - (layout.lo[2] + 0.30 * layout.length)))
        col[inner & (r < 8.5)] = (225, 225, 215)
        col[inner & (r < 6.0)] = (190, 30, 30)
        col[inner & (r < 2.6)] = (225, 225, 215)

    if bottom:
        # cargo bay doors on the belly
        doors = (xn < 0.24) & (zn > 0.30) & (zn < 0.62)
        door_line = doors & ((xn > 0.225) | (xa < 0.9) | (np.abs(zn - 0.30) < 0.012) | (np.abs(zn - 0.62) < 0.012))
        shade[doors & inner] *= 0.9
        shade[door_line & inner] *= 0.45
        hinge = (np.abs(xn - 0.25) < 0.012) & (np.abs((wz % 14.0) - 7.0) < 2.0) & (zn > 0.30) & (zn < 0.62)
        col[hinge & inner] = (200, 200, 190)

    if flat:
        # hazard stripes along the stern, and scorching from the engines
        hazard = (zn < 0.075) & inner & (xn < 0.62)
        yellow = hazard & (np.floor((xa + wz) / 5.0) % 2 == 0)
        col[yellow] = (215, 175, 25)
        col[hazard & ~yellow] = (28, 28, 28)
        shade[hazard] = 1.0
        scorch = np.clip(1.0 - (zn - 0.075) / 0.22, 0, 1) * np.clip(1.0 - np.abs(xn - 0.16) / 0.20, 0, 1)
        streak = 0.75 + 0.25 * np.sin(xa * 1.9 + np.sin(xa * 0.37) * 3.0)
        shade *= 1.0 - 0.42 * scorch * streak * (zn >= 0.075)

    # --- lighting and wear baked into the paint -----------------------------------
    shade *= 0.90 + 0.18 * np.clip(zn, 0, 1)                   # lighter towards the nose
    grain = (np.sin(wx * 12.9898 + wz * 78.233 + wy * 37.719) * 43758.5453) % 1.0
    streaks = 0.5 + 0.5 * np.sin(xa * 2.3 + np.sin(xa * 0.61 + seed) * 2.0)
    shade *= 0.955 + 0.05 * grain + 0.04 * streaks             # fore-aft dirt streaks

    band = edge < TRIM                                         # bevelled edge
    shade[band] *= 0.76
    worn = (edge < 1.1)
    shade[worn] *= 1.55 + 0.5 * (grain[worn] > 0.55)           # chipped, bright edges
    shade[(edge >= TRIM) & (edge < TRIM + 0.9)] *= 0.62

    return np.clip(col * shade[..., None], 0, 255)


def texel_layout(extent):
    """Texels along one side of a face, and the number of tiles."""
    want = extent * TEXEL_DENSITY
    if want <= TILE:
        size = 8
        while size < want:
            size *= 2
        return size, 1
    tiles = min(int(math.ceil(want / TILE_STEP)), MAX_TILES)
    return tiles * TILE_STEP, tiles


def clip_polygon(poly, axis, bound, keep_greater):
    out = []
    for i in range(len(poly)):
        cur, nxt = poly[i], poly[(i + 1) % len(poly)]
        dc, dn = cur[0][axis] - bound, nxt[0][axis] - bound
        ic = dc >= -1e-9 if keep_greater else dc <= 1e-9
        inn = dn >= -1e-9 if keep_greater else dn <= 1e-9
        if ic:
            out.append(cur)
        if ic != inn and abs(dc - dn) > 1e-12:
            t = dc / (dc - dn)
            out.append((cur[0] + (nxt[0] - cur[0]) * t, cur[1] + (nxt[1] - cur[1]) * t))
    return out


def main():
    ship = sys.argv[1]
    point_name = sys.argv[2] if len(sys.argv) > 2 else ship + "_point"
    points, faces = load_ship(ship + "_face", point_name)
    idx, pal = load_palette()
    pts = np.array(points, float)
    seed = zlib.crc32(ship.encode()) & 0xFFFF
    layout = Layout(pts, seed)

    for old in os.listdir(os.path.join(ROOT, "ships")):
        if old.startswith(ship + "_") and old.endswith(".png"):
            os.remove(os.path.join(ROOT, "ships", old))

    solid = [(i, f) for i, f in enumerate(faces) if len(f[2]) >= 3]

    def colour(name):
        return pal[int(name)] if name.isdigit() else pal[idx.get(name, 255)]

    def centroid(vs):
        return pts[vs].mean(axis=0)

    # Which face is the left/right mirror image of which.
    def mirror_of(i):
        want = sorted((round(-pts[v][0], 3), round(pts[v][1], 3), round(pts[v][2], 3)) for v in faces[i][2])
        for j, f in solid:
            if j != i and sorted(tuple(round(c, 3) for c in pts[v]) for v in f[2]) == want:
                return j
        return None

    # Some faces lie exactly on top of a larger one (engines on the rear
    # face, the slot on a station). With a depth buffer those would flicker,
    # so the smaller face is lifted slightly off the larger.
    def plane(i):
        n = np.array(faces[i][1], float)
        vs_ = faces[i][2]
        if np.linalg.norm(n) < 1e-6:
            n = np.cross(pts[vs_[1]] - pts[vs_[0]], pts[vs_[2]] - pts[vs_[0]])
        n = n / np.linalg.norm(n)
        p = pts[vs_]
        area = 0.5 * np.linalg.norm(sum(np.cross(p[k] - p[0], p[k + 1] - p[0]) for k in range(1, len(p) - 1)))
        return n, float(np.mean(p @ n)), area

    planes = {i: plane(i) for i, _ in solid}
    lift = {}
    for i, _ in solid:
        ni, di, ai = planes[i]
        for j, _ in solid:
            nj, dj, aj = planes[j]
            if j != i and ni @ nj > 0.999 and abs(di - dj) < 1.0 and aj > ai * 1.05:
                lift[i] = ni * 2.0
                break

    textures = []           # {"file", "w", "h"}
    painted = {}            # face -> (frame, origin, scale, tile table)
    tris = []               # (texture, [3 x (x, y, z, nx, ny, nz, u, v)])

    for i, (name, normal, vs) in solid:
        n = np.array(normal, float)
        if np.linalg.norm(n) < 1e-6:
            n = np.cross(pts[vs[1]] - pts[vs[0]], pts[vs[2]] - pts[vs[0]])
        src = i
        mirrored = False
        if centroid(vs)[0] < -1e-3:
            j = mirror_of(i)
            if j is not None and j in painted:
                src, mirrored = j, True

        if not mirrored:
            nn, u, v = face_frame(n, pts[vs])
            fu = np.array([pts[k] @ u for k in vs])
            fv = np.array([pts[k] @ v for k in vs])
            u0, v0 = fu.min(), fv.min()
            w, h = max(fu.max() - u0, 1e-3), max(fv.max() - v0, 1e-3)
            rgb = colour(name)
            is_engine = (n[2] / np.linalg.norm(n) < -0.7) and rgb[0] > 150 and rgb[1] < 90 and rgb[2] < 90
            # Engine faces are small but eye-catching: give them more texels.
            boost = 3.0 if is_engine else 1.0
            tw, tiles_u = texel_layout(w * boost)
            th, tiles_v = texel_layout(h * boost)
            su, sv = (tw - 1.0) / w, (th - 1.0) / h
            poly = list(zip(fu - u0, fv - v0))

            # Does the ship's centre line cross this face? Then mirror the pattern.
            xs = pts[vs][:, 0]
            sym_u = None
            if xs.min() < -1e-3 and xs.max() > 1e-3 and abs(u[0]) > 0.98:
                sym_u = float(np.mean(fu - u0 - xs * u[0]))

            ss = SUPERSAMPLE
            gy, gx = np.mgrid[0:th * ss, 0:tw * ss]
            pu = ((gx + 0.5) / ss - 0.5) / su
            pv = ((gy + 0.5) / ss - 0.5) / sv
            nn_ = n / np.linalg.norm(n)
            world = (pu[..., None] + u0) * u + (pv[..., None] + v0) * v + nn_ * float(pts[vs[0]] @ nn_)
            img = paint_face(rgb, pu, pv, poly, seed + i, is_engine, sym_u, world, nn_, layout)
            img = img.reshape(th, ss, tw, ss, 3).mean(axis=(1, 3))
            img = np.clip(img, 0, 255).astype(np.uint8)

            tiles = {}
            for tv in range(tiles_v):
                for tu in range(tiles_u):
                    ox = tu * TILE_STEP - 1 if tiles_u > 1 else 0
                    oy = tv * TILE_STEP - 1 if tiles_v > 1 else 0
                    pw = TILE if tiles_u > 1 else tw
                    ph = TILE if tiles_v > 1 else th
                    ys = np.clip(np.arange(oy, oy + ph), 0, th - 1)
                    xs_ = np.clip(np.arange(ox, ox + pw), 0, tw - 1)
                    fname = "%s_%d" % (ship, len(textures))
                    Image.fromarray(img[np.ix_(ys, xs_)], "RGB").quantize(
                        16, method=Image.MEDIANCUT, dither=Image.NONE).save(
                        os.path.join(ROOT, "ships", fname + ".png"))
                    tiles[(tu, tv)] = (len(textures), ox, oy)
                    textures.append({"file": fname, "w": pw, "h": ph})
            painted[i] = (u, v, u0, v0, su, sv, tiles, tiles_u, tiles_v)

        u, v, u0, v0, su, sv, tiles, tiles_u, tiles_v = painted[src]
        nn = n / np.linalg.norm(n)

        # Texture coordinates of this face's corners (a mirrored face looks up
        # its mirror image's layout with x negated).
        poly = []
        for k in vs:
            p = pts[k]
            q = np.array([-p[0], p[1], p[2]]) if mirrored else p
            poly.append((np.array([0.5 + (q @ u - u0) * su, 0.5 + (q @ v - v0) * sv]),
                         p + lift.get(i, 0.0)))

        for tu in range(tiles_u):
            colp = poly
            if tiles_u > 1:
                if tu > 0:
                    colp = clip_polygon(colp, 0, tu * TILE_STEP, True)
                if tu < tiles_u - 1:
                    colp = clip_polygon(colp, 0, (tu + 1) * TILE_STEP, False)
            for tv in range(tiles_v):
                cell = colp
                if tiles_v > 1:
                    if tv > 0:
                        cell = clip_polygon(cell, 1, tv * TILE_STEP, True)
                    if tv < tiles_v - 1:
                        cell = clip_polygon(cell, 1, (tv + 1) * TILE_STEP, False)
                if len(cell) < 3:
                    continue
                tex, ox, oy = tiles[(tu, tv)]
                for k in range(1, len(cell) - 1):
                    corners = [(p[0], p[1], p[2], nn[0], nn[1], nn[2], uvc[0] - ox, uvc[1] - oy)
                               for uvc, p in (cell[0], cell[k], cell[k + 1])]
                    tris.append((tex, corners))

    # --- cut into batches that fit the vertex cache ---------------------------
    tris.sort(key=lambda t: t[0])

    def pack(c):
        return (int(round(c[0])), int(round(c[1])), int(round(c[2])),
                int(round(c[3] * 127)), int(round(c[4] * 127)), int(round(c[5] * 127)),
                int(round(c[6] * 32)), int(round(c[7] * 32)))

    batches = []
    cur = None
    for tex, corners in tris:
        packed = [pack(c) for c in corners]
        new = [p for p in set(packed) if cur is None or p not in cur["index"]]
        if cur is None or cur["chart"] != tex or len(cur["verts"]) + len(new) > VERT_CACHE:
            cur = {"chart": tex, "verts": [], "tris": [], "index": {}}
            batches.append(cur)
        ids = []
        for p in packed:
            if p not in cur["index"]:
                cur["index"][p] = len(cur["verts"])
                cur["verts"].append(p)
            ids.append(cur["index"][p])
        if len(set(ids)) == 3:
            cur["tris"].append(ids)
    for b in batches:
        del b["index"]

    json.dump({"ship": ship, "charts": textures, "batches": batches},
              open(os.path.join(ROOT, "ships", ship + ".mesh.json"), "w"))
    print("%s: %d faces -> %d triangles, %d textures, %d batches"
          % (ship, len(solid), sum(len(b["tris"]) for b in batches), len(textures), len(batches)))


if __name__ == "__main__":
    main()
