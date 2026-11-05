#!/usr/bin/env python3
# Our64 crt-preview comparison sheets: each frame through Off, Scanlines, CRT
# and CRT Curved (crt-preview, built from crt_preview.cpp) at the TV sizes,
# saved whole and as side-by-side 1:1 crops (scanline detail is lost when a
# 1080p picture is shrunk to look at), and the crops' middles 4x. CRT
# Curved is also drawn on a 16:9 picture (<name>-<H>p-curved-169.png).
#
# Usage: compare.py <crt-preview binary> <out dir> <name>=<frame.ppm>[@lines][@x,y] ...
#   lines: the game's line count (default 240); x,y: the crop's top-left
#   corner in the 640x480 frame (default its center).
# Needs Pillow.
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

SHADERS = ["off", "scanlines", "crt", "curved"]
SIZES = [(1920, 1080), (1280, 720)]
CROP = 200


def main():
    exe, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp()
    for arg in sys.argv[3:]:
        name, rest = arg.split("=", 1)
        parts = rest.split("@")
        src, lines, at = parts[0], "240", None
        for p in parts[1:]:
            if "," in p:
                at = tuple(int(v) for v in p.split(","))
            else:
                lines = p
        for W, H in SIZES:
            shots = []
            for i, s in enumerate(SHADERS):
                ppm = os.path.join(tmp, f"{name}-{s}-{H}.ppm")
                subprocess.run([exe, str(i), src, ppm, str(W), str(H), "43", lines], check=True)
                im = Image.open(ppm).convert("RGB")
                im.save(os.path.join(out, f"{name}-{H}p-{s}.png"))
                shots.append(im)
            ppm = os.path.join(tmp, f"{name}-curved-169-{H}.ppm")
            subprocess.run([exe, "3", src, ppm, str(W), str(H), "169", lines], check=True)
            Image.open(ppm).convert("RGB").save(os.path.join(out, f"{name}-{H}p-curved-169.png"))
            # The crop's spot: a frame point mapped into the 4:3 picture.
            pw = (H * 4 // 3 + 1) & ~1
            x0 = (W - pw) // 2
            fx, fy = at if at else (320 - CROP * 640 // pw // 2, 240 - CROP * 480 // H // 2)
            cx, cy = x0 + fx * pw // 640, fy * H // 480
            crops = Image.new("RGB", (len(SHADERS) * (CROP + 4), CROP + 14), (40, 40, 40))
            d = ImageDraw.Draw(crops)
            for i, im in enumerate(shots):
                crops.paste(im.crop((cx, cy, cx + CROP, cy + CROP)), (i * (CROP + 4), 14))
                d.text((i * (CROP + 4) + 2, 1), SHADERS[i], fill=(255, 255, 255))
            crops.save(os.path.join(out, f"{name}-{H}p-crops.png"))
            # The crop's middle, 4x with no smoothing: the TV's own pixels.
            Z, k = 64, 4
            zoom = Image.new("RGB", (len(SHADERS) * (Z * k + 4), Z * k), (40, 40, 40))
            for i, im in enumerate(shots):
                zoom.paste(im.crop((cx + (CROP - Z) // 2, cy + (CROP - Z) // 2, cx + (CROP + Z) // 2, cy + (CROP + Z) // 2)).resize((Z * k, Z * k), Image.NEAREST), (i * (Z * k + 4), 0))
            zoom.save(os.path.join(out, f"{name}-{H}p-zoom.png"))
            # The four whole pictures, halved, in a 2x2 sheet.
            sw, sh = W // 2, H // 2
            sheet = Image.new("RGB", (2 * sw, 2 * sh))
            for i, im in enumerate(shots):
                sheet.paste(im.resize((sw, sh), Image.LANCZOS), ((i % 2) * sw, (i // 2) * sh))
            sheet.save(os.path.join(out, f"{name}-{H}p-sheet.png"))


main()
