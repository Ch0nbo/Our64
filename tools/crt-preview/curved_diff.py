#!/usr/bin/env python3
# Our64 crt-preview: CRT Curved against an earlier version of it. For each
# <name>-<H>p-curved.png (4:3) and <name>-<H>p-curved-169.png in both
# directories (compare.py's output): how far the pixels differ, over the
# whole TV picture and away from the picture's edge, and 1:1 crops of the
# top-left corner and the middle of the left edge (old | new | the
# difference times 8), as <name>-<H>p-curved[-169]-diff.png in the out dir,
# and the corner alone (-corner-old.png, -corner-new.png).
#
# Usage: curved_diff.py <old dir> <new dir> <out dir>
# Needs Pillow and numpy.
import glob
import os
import sys

import numpy as np
from PIL import Image

CROP = 160


def main():
    old_dir, new_dir, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    print("image                       max  mean  >2 levels  >8 levels  inner max  inner >8")
    for new_path in sorted(glob.glob(os.path.join(new_dir, "*-curved*.png"))):
        base = os.path.basename(new_path)
        if base.endswith("-diff.png"):
            continue
        old_path = os.path.join(old_dir, base)
        if not os.path.exists(old_path):
            continue
        a = np.asarray(Image.open(old_path).convert("RGB")).astype(int)
        b = np.asarray(Image.open(new_path).convert("RGB")).astype(int)
        H, W = a.shape[:2]
        wide = base.endswith("-169.png")
        pw = W if wide else min(W, (H * 4 // 3 + 1) & ~1)
        x0 = (W - pw) // 2
        d = np.abs(a - b).max(axis=2)
        pic = d[:, x0:x0 + pw]
        # away from the edge: the middle 80% each way
        inner = pic[H // 10:H - H // 10, pw // 10:pw - pw // 10]
        n = pic.size
        print("%-26s %4d %5.2f %8.3f%% %9.3f%% %10d %8.3f%%" % (
            base[:-4], pic.max(), pic.mean(), 100.0 * (pic > 2).sum() / n, 100.0 * (pic > 8).sum() / n,
            inner.max(), 100.0 * (inner > 8).sum() / inner.size))
        spots = [(x0, 0), (x0, H // 2 - CROP // 2)]
        sheet = Image.new("RGB", (3 * (CROP + 4), len(spots) * (CROP + 4)), (40, 40, 40))
        for row, (sx, sy) in enumerate(spots):
            ca = a[sy:sy + CROP, sx:sx + CROP]
            cb = b[sy:sy + CROP, sx:sx + CROP]
            cd = np.clip(np.abs(ca - cb) * 8, 0, 255)
            for col, c in enumerate((ca, cb, cd)):
                sheet.paste(Image.fromarray(c.astype(np.uint8)), (col * (CROP + 4), row * (CROP + 4)))
        sheet.save(os.path.join(out, base[:-4] + "-diff.png"))
        # the corner alone, old and new, 1:1
        for tag, im in (("old", a), ("new", b)):
            Image.fromarray(im[0:CROP, x0:x0 + CROP].astype(np.uint8)).save(
                os.path.join(out, base[:-4] + "-corner-" + tag + ".png"))


main()
