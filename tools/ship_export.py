#!/usr/bin/env python3
"""
Export an original Elite ship (shipdata.c / shipface.c) for art work.

  ship_export.py <ship> render out.png [points] [view]  - a shaded picture
        (view: persp, top, bottom, front, back, left, right)
  ship_export.py <ship> obj out.obj      - the exact original mesh

<ship> is the name used in the C source, e.g. cobra3 (faces: cobra3_face,
points: cobra3a_point - pass the point table name as an optional 4th arg).
"""
import math
import re
import sys

ROOT = __file__.rsplit("/", 2)[0]

PALETTE = {  # the GFX_COL_ names used by the ship faces -> RGB
}


def load_palette():
    """Read colour indices from gfx.h and RGB values from scanner.bmp."""
    idx = {}
    for line in open(ROOT + "/gfx.h"):
        m = re.match(r"#define\s+(GFX_\w+)\s+(\d+)\s*$", line)
        if m:
            idx[m.group(1)] = int(m.group(2))
    bmp = open(ROOT + "/scanner.bmp", "rb").read()
    pal = [(bmp[54 + i * 4 + 2], bmp[54 + i * 4 + 1], bmp[54 + i * 4]) for i in range(256)]
    return idx, pal


def cnum(tok):
    tok = tok.strip()
    return int(tok, 0)


def load_ship(face_name, point_name):
    pts_src = open(ROOT + "/shipdata.c").read()
    face_src = open(ROOT + "/shipface.c").read()
    m = re.search(r"struct ship_point %s\[\d*\]\s*=\s*\{(.*?)\n\};" % point_name, pts_src, re.S)
    points = []
    for row in re.findall(r"\{([^{}]*)\}", m.group(1)):
        v = [cnum(t) for t in row.split(",")[:3]]
        points.append(tuple(v))
    m = re.search(r"struct ship_face %s\[\d*\]\s*=\s*\{(.*?)\n\};" % face_name, face_src, re.S)
    faces = []
    for row in re.findall(r"\{([^{}]*)\}", m.group(1)):
        t = [x.strip() for x in row.split(",")]
        n = int(t[4])
        faces.append((t[0], (cnum(t[1]), cnum(t[2]), cnum(t[3])), [int(x) for x in t[5:5 + n]]))
    return points, faces


def main():
    ship, mode, out = sys.argv[1], sys.argv[2], sys.argv[3]
    point_name = sys.argv[4] if len(sys.argv) > 4 else ship + "_point"
    points, faces = load_ship(ship + "_face", point_name)
    idx, pal = load_palette()

    def colour(name):
        return pal[idx.get(name, 255)] if not name.isdigit() else pal[int(name)]

    if mode == "obj":
        with open(out, "w") as f:
            for p in points:
                f.write("v %d %d %d\n" % p)
            for _, _, vs in faces:
                if len(vs) >= 3:
                    f.write("f " + " ".join(str(v + 1) for v in vs) + "\n")
        return

    from PIL import Image, ImageDraw
    size = 1024
    # Optional 5th argument: a named view (the default is a 3/4 view).
    views = {"persp": (215, 28), "top": (180, 90), "bottom": (180, -90),
             "front": (180, 0), "back": (0, 0), "left": (90, 0), "right": (270, 0)}
    yaw, pitch = [math.radians(a) for a in views.get(sys.argv[5] if len(sys.argv) > 5 else "persp", (0, 0))]

    # "noseup" views look straight at the top/bottom/sides with the nose
    # pointing up the picture (for flat ships, whose front view shows little).
    basis = {"top_noseup": ((0, -1, 0), (0, 0, 1)), "bottom_noseup": ((0, 1, 0), (0, 0, 1)),
             "left_noseup": ((1, 0, 0), (0, 0, 1)), "right_noseup": ((-1, 0, 0), (0, 0, 1))}
    vname = sys.argv[5] if len(sys.argv) > 5 else "persp"

    def cross(a, b):
        return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    def view(p):
        if vname in basis:
            f, u = basis[vname]
            return dot(p, cross(f, u)), dot(p, u), dot(p, f)
        x, y, z = p
        x, z = x * math.cos(yaw) + z * math.sin(yaw), -x * math.sin(yaw) + z * math.cos(yaw)
        y, z = y * math.cos(pitch) - z * math.sin(pitch), y * math.sin(pitch) + z * math.cos(pitch)
        return x, y, z

    vp = [view(p) for p in points]
    ext = max(max(abs(v[0]), abs(v[1])) for v in vp)
    scale = size * 0.42 / ext
    light = (0.35, 0.75, -0.55)
    ln = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ln for c in light)

    img = Image.new("RGB", (size, size), (255, 255, 255))
    d = ImageDraw.Draw(img)
    order = sorted([f for f in faces if len(f[2]) >= 3],
                   key=lambda f: -sum(vp[v][2] for v in f[2]) / len(f[2]))
    for name, normal, vs in order:
        n = view(normal)
        nl = math.sqrt(sum(c * c for c in n)) or 1
        n = tuple(c / nl for c in n)
        if n[2] > 0.05:      # facing away from the viewer
            continue
        shade = 0.45 + 0.55 * max(0.0, sum(a * b for a, b in zip(n, light)))
        r, g, b = colour(name)
        poly = [(size / 2 + vp[v][0] * scale, size / 2 - vp[v][1] * scale) for v in vs]
        d.polygon(poly, fill=(int(r * shade), int(g * shade), int(b * shade)), outline=(20, 20, 20))
    img.save(out)


if __name__ == "__main__":
    main()
