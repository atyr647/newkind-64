#!/usr/bin/env python3
"""
Add painted surface detail to an original Elite ship.

The ship's geometry and face colours are exactly those in shipdata.c and
shipface.c. Each flat face is given its own texture, painted here in the
face's original colour: armour plating with seams and rivets, a bevelled
trim along every edge, the odd vent, and a glow on engine faces.

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


def paint_face(base_rgb, pu, pv, poly, seed, is_engine, sym_u):
    """
    Colour for every sample of a face. pu, pv are positions on the face in
    game units, poly its outline. sym_u, if not None, is the u position of
    the ship's centre line: the pattern is then mirrored about it.
    """
    base = np.array(base_rgb, float)
    qu = np.abs(pu - sym_u) if sym_u is not None else pu
    edge = edge_distance(pu, pv, poly)
    shade = np.ones(pu.shape)

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
        ring = np.abs(r - 1.0) < 0.07                     # nozzle rim
        col[ring] = base * 0.3
        col[edge < TRIM * 0.6] = base * 0.4
        return col

    # --- plating: staggered rows of panels -----------------------------------
    row = np.floor(pv / PANEL_H).astype(int)
    row_shift = np.vectorize(lambda r: hash01(seed, "row", int(r)) * PANEL_W)(row)
    colf = (qu + row_shift) / PANEL_W
    col = np.floor(colf).astype(int)
    fu = (colf - col) * PANEL_W                 # position inside the panel
    fv = pv - row * PANEL_H

    # each panel is a slightly different shade
    tint = np.vectorize(lambda r, c: hash01(seed, "tint", int(r), int(c)))(row, col)
    shade *= 0.93 + 0.14 * tint

    # a few panels are vents: dark slats
    kind = np.vectorize(lambda r, c: hash01(seed, "kind", int(r), int(c)))(row, col)
    vent = (kind > 0.88) & (fu > 5) & (fu < PANEL_W - 5) & (fv > 4.5) & (fv < PANEL_H - 4.5)
    shade[vent & (np.floor(fv / 1.6) % 2 == 0)] *= 0.45
    shade[vent & (np.floor(fv / 1.6) % 2 == 1)] *= 0.8

    # seams: a dark groove with a catch-light along one side
    seam_d = np.minimum(np.minimum(fu, PANEL_W - fu), np.minimum(fv, PANEL_H - fv))
    light = (np.minimum(fu - SEAM, fv - SEAM) < SEAM * 1.3) & (seam_d >= SEAM)
    shade[light] *= 1.16
    shade[seam_d < SEAM] *= 0.5

    # rivets near the panel corners
    for ru, rv in ((4.0, 4.0), (PANEL_W - 4.0, 4.0), (4.0, PANEL_H - 4.0), (PANEL_W - 4.0, PANEL_H - 4.0)):
        shade[np.hypot(fu - ru, fv - rv) < 1.0] *= 0.6

    # --- edge trim: darker band, bright bevel at the very edge ------------------
    band = edge < TRIM
    shade[band] *= 0.74
    shade[edge < 1.0] *= 1.75
    shade[(edge >= TRIM) & (edge < TRIM + 0.9)] *= 0.62

    # a little grain so large faces are not perfectly flat
    grain = (np.sin(pu * 12.9898 + pv * 78.233 + seed) * 43758.5453) % 1.0
    shade *= 0.97 + 0.06 * grain

    return np.clip(base[None, None, :] * shade[..., None], 0, 255)


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
            img = paint_face(rgb, pu, pv, poly, seed + i, is_engine, sym_u)
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
