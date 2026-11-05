#!/usr/bin/env python3
# Our64 crt-preview flat-color check: 16 flat patches (saturated, mid-tone,
# dark) through Off and each shader at 1080p and 720p, and each patch's
# middle averaged in linear light (the eye adds up light, not values). A
# shader should keep Off's brightness and hue: "lum" is its light against
# Off's, "drift" how far its channels part from that (a hue shift).
#
# Usage: flat_check.py <crt-preview binary> [shaders, default 1 2 3]
# Needs Pillow and NumPy.
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

PATCHES = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255),
           (128, 128, 128), (200, 150, 120), (40, 40, 120), (128, 128, 40), (60, 120, 60), (180, 60, 40),
           (90, 60, 140), (230, 200, 90), (32, 32, 32)]


def linear(v):
    v = v / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def encoded(l):
    return np.where(l <= 0.0031308, l * 12.92, 1.055 * l ** (1 / 2.4) - 0.055) * 255


def luma(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def main():
    exe = sys.argv[1]
    shaders = sys.argv[2:] or ["1", "2", "3"]
    tmp = tempfile.mkdtemp()
    frame = os.path.join(tmp, "flat.ppm")
    im = Image.new("RGB", (640, 480))
    for i, c in enumerate(PATCHES):
        x, y = (i % 4) * 160, (i // 4) * 120
        im.paste(c, (x, y, x + 160, y + 120))
    im.save(frame)
    for W, H in [(1920, 1080), (1280, 720)]:
        pw = (H * 4 // 3 + 1) & ~1
        x0 = (W - pw) // 2
        shots = {}
        for s in ["0"] + shaders:
            out = os.path.join(tmp, f"{s}.ppm")
            subprocess.run([exe, s, frame, out, str(W), str(H), "43"], check=True)
            shots[s] = np.asarray(Image.open(out).convert("RGB")).astype(float)
        print(f"{H}p: Off -> each shader, linear-light means re-encoded")
        worst = 0.0
        for i in range(len(PATCHES)):
            px, py = (i % 4) * 160, (i // 4) * 120
            X0, X1 = x0 + (px + 30) * pw // 640, x0 + (px + 130) * pw // 640
            Y0, Y1 = (py + 30) * H // 480, (py + 90) * H // 480
            ref = linear(shots["0"][Y0:Y1, X0:X1]).reshape(-1, 3).mean(0)
            row = []
            for s in shaders:
                m = linear(shots[s][Y0:Y1, X0:X1]).reshape(-1, 3).mean(0)
                lum = luma(m) / max(luma(ref), 1e-6)
                lit = ref > 0.01
                drift = float(np.abs(((m + 1e-4) / (ref + 1e-4))[lit] / lum - 1).max()) if lit.any() else 0.0
                worst = max(worst, drift)
                row.append(f"{s}: {np.round(encoded(m)).astype(int)} lum x{lum:.3f} drift {drift * 100:.1f}%")
            print(f"  {np.round(encoded(ref)).astype(int)}  " + " | ".join(row))
        print(f"  worst drift {worst * 100:.1f}%")


main()
