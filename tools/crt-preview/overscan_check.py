#!/usr/bin/env python3
"""Overscan check (issue #36): a 640x480 frame with the black margins most
N64 games leave (8 left, 7 right, 0 top, 6 bottom) and a mid-grey picture is
drawn through each filter at 1920x1080, 4:3. Prints the black margin left in
the 1440x1080 picture, in TV pixels, per side: a column or row counts as
black when its brightest pixel is under 8 of 255. Off keeps the margins; the
flat filters must show 0 on every side. CRT Curved is measured along the
middle row and column (its corners are the tube's)."""
import subprocess, sys, os, tempfile
W, H = 1920, 1080
def ppm_write(path, w, h, px):
    with open(path, 'wb') as f:
        f.write(b'P6 %d %d 255\n' % (w, h)); f.write(px)
def ppm_read(path):
    d = open(path, 'rb').read()
    parts = d.split(None, 4)
    return int(parts[1]), int(parts[2]), parts[4]
frame = bytearray(640 * 480 * 3)
for y in range(0, 480 - 6):
    for x in range(8, 640 - 7):
        i = (y * 640 + x) * 3
        frame[i:i+3] = b'\xa0\xa0\xa0'
tmp = tempfile.mkdtemp()
src = os.path.join(tmp, 'in.ppm'); ppm_write(src, 640, 480, bytes(frame))
bad = 0
for shader, name in enumerate(['Off', 'Scanlines', 'CRT', 'CRT Curved']):
    out = os.path.join(tmp, 'out%d.ppm' % shader)
    subprocess.check_call([os.path.join(os.path.dirname(__file__) or '.', 'crt-preview'), str(shader), src, out, str(W), str(H), '43'])
    w, h, px = ppm_read(out)
    x0, pw = (W - 1440) // 2, 1440
    def lit(x, y): return max(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) >= 8
    mid = shader == 3
    def col(x): return any(lit(x, y) for y in ([h // 2 - 1, h // 2, h // 2 + 1] if mid else range(0, h, 3)))
    def row(y): return any(lit(x, y) for x in ([w // 2 - 1, w // 2, w // 2 + 1] if mid else range(x0, x0 + pw, 3)))
    left = next(i for i in range(pw) if col(x0 + i))
    right = next(i for i in range(pw) if col(x0 + pw - 1 - i))
    top = next(i for i in range(h) if row(i))
    bottom = next(i for i in range(h) if row(h - 1 - i))
    ok = shader == 0 or max(left, right, top, bottom) <= 1
    bad += not ok
    print('%-10s black margin, TV pixels: left %d right %d top %d bottom %d%s' % (name, left, right, top, bottom, '' if ok else '  FAIL'))
sys.exit(1 if bad else 0)
