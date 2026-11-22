#!/usr/bin/env python3
"""
Make env/env_metal.png, the reflection texture for the ships' metal pass.

It is looked up by the direction a surface faces as seen from the camera
(centre = facing the camera). Colour is what is reflected; alpha is how
strongly it shows over the paint. The finish is satin rather than gloss: a
broad, soft key-light highlight up and to the left at no more than about a
third strength, a fainter fill light, a faint sky, and slight darkening
towards the rim (surfaces turned away). No sharp highlights or horizon band,
which read as polished chrome. Saved as 32x32 RGBA and
converted to IA16 (intensity + alpha) by the Makefile.
"""
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N = 32

y, x = np.mgrid[0:N, 0:N]
u = (x + 0.5) / N * 2 - 1
v = (y + 0.5) / N * 2 - 1
r = np.hypot(u, v)


def blob(cu, cv, su, sv):
    return np.exp(-(((u - cu) / su) ** 2 + ((v - cv) / sv) ** 2))


key = blob(-0.30, -0.35, 0.34, 0.28)          # main highlight, broad
fill = blob(0.45, -0.05, 0.25, 0.40) * 0.35   # second, softer light
sky = np.clip(-v, 0, 1) * 0.08
rim = np.clip((r - 0.70) / 0.30, 0, 1)

light = np.clip(key + fill + sky, 0, 1)
alpha = np.clip(light * 0.32 + rim * 0.14, 0, 0.35)
# Bright where it reflects light, dark where it is only the rim darkening.
grey = np.where(light > rim * 0.25, 0.80 + 0.20 * np.clip(key * 2, 0, 1), 0.0)

img = np.zeros((N, N, 4), np.uint8)
img[..., 0] = img[..., 1] = img[..., 2] = np.clip(grey * 255, 0, 255)
img[..., 3] = np.clip(alpha * 255, 0, 255)
os.makedirs(os.path.join(ROOT, "env"), exist_ok=True)
Image.fromarray(img, "RGBA").save(os.path.join(ROOT, "env", "env_metal.png"))
print("wrote env/env_metal.png")
