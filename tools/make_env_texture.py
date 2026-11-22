#!/usr/bin/env python3
"""
Make env/env_metal.png, the reflection texture for the ships' metal pass.

It is looked up by the direction a surface faces as seen from the camera
(centre = facing the camera). Colour is what is reflected; alpha is how
strongly it shows over the paint: a key-light highlight up and to the left,
a softer fill light, a bright horizon band, a faint sky, and slight
darkening towards the rim (surfaces turned away). Saved as 32x32 RGBA and
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


key = blob(-0.32, -0.38, 0.17, 0.12)          # main highlight
fill = blob(0.48, -0.05, 0.12, 0.26) * 0.45   # second, softer light
horizon = np.exp(-((v - 0.22) / 0.045) ** 2) * 0.35
sky = np.clip(-v, 0, 1) * 0.10
rim = np.clip((r - 0.75) / 0.25, 0, 1)

light = np.clip(key + fill + horizon + sky, 0, 1)
alpha = np.clip(light * 0.85 + rim * 0.22, 0, 0.9)
# Bright where it reflects light, dark where it is only the rim darkening.
grey = np.where(light > rim * 0.25, 0.80 + 0.20 * np.clip(key * 2, 0, 1), 0.0)

img = np.zeros((N, N, 4), np.uint8)
img[..., 0] = img[..., 1] = img[..., 2] = np.clip(grey * 255, 0, 255)
img[..., 3] = np.clip(alpha * 255, 0, 255)
os.makedirs(os.path.join(ROOT, "env"), exist_ok=True)
Image.fromarray(img, "RGBA").save(os.path.join(ROOT, "env", "env_metal.png"))
print("wrote env/env_metal.png")
