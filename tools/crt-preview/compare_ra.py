#!/usr/bin/env python3
"""Our64's TV filters against RetroArch's CRT shaders, on the same frames,
by numbers. Usage: compare_ra.py <out dir> [game frame 640x480 png/ppm]

Ours (crt-preview) get the 640x480 frame, as the console's TV pass does.
RetroArch's (ra-preview, the system's slang-shaders at their defaults) get
the same frame at 640x240, each pair of rows averaged: one row a game line,
as a core at the game's own resolution gives them. All are drawn at
1920x1080, 4:3 (a 1440x1080 picture).

Measured, in linear light, on test frames (the middle of the picture):
  light      mean light of a flat grey (128, then 230) over the plain picture's
  lines      how deep the scanlines are on that grey: (max - min) / (max + min)
             of the rows' means; 0 none, 1 black between lines
  period     TV rows per scanline (1080 / 240 = 4.5)
  mask       the same depth across the columns, per color channel: a phosphor mask
  edge       TV pixels a black-to-white vertical edge takes from 10% to 90%
  tint       largest shift of a flat color's (230, 200, 90) channel shares
  corner     how far the picture's corner is pulled in by a curved tube, as a
  side       share of the picture's height; and its sides' middles
Then the game frame through all of them: sheet-whole.png, sheet-crop.png
(1:1) and sheet-crop-4x.png. Needs Pillow and numpy."""
import os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
RA = '/usr/share/libretro/shaders/shaders_slang/crt/shaders/'
W, H, PW = 1920, 1080, 1440
X0 = (W - PW) // 2
RA_ACROSS = 1  # frame columns per column given to RetroArch's: 1 (640 wide) or 2 (320 wide, most N64 games)
FILTERS = [  # name, kind, shader, linear (RetroArch: the preset's filter_linear0)
    ('Off', 'ours', 0, 0), ('Our Scanlines', 'ours', 1, 0), ('Our CRT', 'ours', 2, 0), ('Our CRT Curved', 'ours', 3, 0),
    ('RA zfast-crt', 'ra', 'zfast_crt/zfast_crt_finemask.slang', 1), ('RA crt-pi', 'ra', 'crt-pi.slang', 1),
    ('RA crt-lottes', 'ra', 'crt-lottes.slang', 0), ('RA crt-geom', 'ra', 'crt-geom.slang', 0),
]

def lin(a):
    a = a.astype(np.float64) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)

def lum(a):
    return lin(a) @ np.array([0.2126, 0.7152, 0.0722])

def render(out, tag, frame):
    """frame: 640x480 uint8 array. Returns each filter's 1920x1080 picture."""
    f480 = os.path.join(out, 'tmp-480.ppm'); f240 = os.path.join(out, 'tmp-240.ppm')
    Image.fromarray(frame).save(f480)
    m = lin(frame).reshape(240, 2, 640 // RA_ACROSS, RA_ACROSS, 3).mean((1, 3))  # averaged as light
    half = np.where(m <= 0.0031308, m * 12.92, 1.055 * m ** (1 / 2.4) - 0.055) * 255
    Image.fromarray(np.clip(np.round(half), 0, 255).astype(np.uint8)).save(f240)
    res = {}
    for name, kind, shader, linear in FILTERS:
        o = os.path.join(out, 'tmp-out.ppm')
        if kind == 'ours':
            subprocess.check_call([os.path.join(HERE, 'crt-preview'), str(shader), f480, o, str(W), str(H), '43'])
        else:
            subprocess.check_call([os.path.join(HERE, 'ra-preview'), RA + shader, str(linear), f240, o, str(W), str(H), '43'])
        res[name] = np.asarray(Image.open(o)).copy()
    for f in (f480, f240, os.path.join(out, 'tmp-out.ppm')):
        os.remove(f)
    return res

def flat(rgb):
    return np.full((480, 640, 3), rgb, np.uint8)

def depth(v):
    return (v.max() - v.min()) / (v.max() + v.min() + 1e-12)

def measure(out):
    rows = {n[0]: {} for n in FILTERS}
    mid = (slice(H // 2 - 135, H // 2 + 135), slice(W // 2 - 180, W // 2 + 180))  # 60 lines, clear of any bend
    for level in (128, 230):
        r = render(out, 'grey', flat((level,) * 3))
        base = lum(r['Off'][mid]).mean()
        for n, img in r.items():
            l = lum(img[mid])
            rows[n]['light %d' % level] = l.mean() / base
            rows[n]['lines %d' % level] = depth(l.mean(1))
            rows[n]['mask %d' % level] = max(depth(lin(img[mid])[:, :, c].mean(0)) for c in range(3)) if level == 128 else None
            if level == 128:
                spec = np.abs(np.fft.rfft(l.mean(1) - l.mean()))
                k = spec[1:].argmax() + 1
                rows[n]['period'] = 270.0 / k if spec[k] > 1e-4 * 270 else 0.0
    edge = np.zeros((480, 640, 3), np.uint8); edge[:, 320:] = 255
    r = render(out, 'edge', edge)
    for n, img in r.items():
        p = lum(img[H // 2 - 135:H // 2 + 135, W // 2 - 60:W // 2 + 60]).mean(0)
        p = (p - p[:20].mean()) / (p[-20:].mean() - p[:20].mean())
        lo = np.argmax(p >= 0.1); hi = np.argmax(p >= 0.9)
        # between samples, for a width under a pixel
        f = lambda t, i: i - 1 + (t - p[i - 1]) / (p[i] - p[i - 1]) if i > 0 else 0.0
        rows[n]['edge'] = f(0.9, hi) - f(0.1, lo)
    color = (230, 200, 90)
    r = render(out, 'color', flat(color))
    want = lin(np.array(color, np.uint8)); want = want / want.sum()
    for n, img in r.items():
        got = lin(img[mid]).reshape(-1, 3).mean(0); got = got / got.sum()
        rows[n]['tint'] = np.abs(got - want).max()
    r = render(out, 'white', flat((255, 255, 255)))
    for n, img in r.items():
        l = lum(img) > 0.02
        diag = next((i for i in range(400) if l[i, X0 + i]), 400) / H
        top = next((i for i in range(400) if l[i, W // 2]), 400) / H
        left = next((i for i in range(400) if l[H // 2, X0 + i]), 400) / H
        rows[n]['corner'] = diag; rows[n]['side'] = max(top, left)
    return rows

def table(rows):
    cols = ['light 128', 'light 230', 'lines 128', 'lines 230', 'period', 'mask 128', 'edge', 'tint', 'corner', 'side']
    head = ['filter', 'light@128', 'light@230', 'lines@128', 'lines@230', 'period', 'mask', 'edge px', 'tint', 'corner', 'side']
    fmt = {'period': '%.2f', 'edge': '%.1f', 'tint': '%.3f', 'corner': '%.3f', 'side': '%.3f'}
    out = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    for n, v in rows.items():
        out.append('| ' + n + ' | ' + ' | '.join((fmt.get(c, '%.2f') % v[c]) for c in cols) + ' |')
    return '\n'.join(out)

def sheets(out, path):
    frame = np.asarray(Image.open(path).convert('RGB').resize((640, 480), Image.BOX))
    r = render(out, 'game', frame)
    names = [n[0] for n in FILTERS]
    whole = Image.new('RGB', (4 * 484, 2 * 384), 'black')
    crop = Image.new('RGB', (4 * 364, 2 * 294), 'black')
    big = Image.new('RGB', (4 * 364, 2 * 294), 'black')
    for k, n in enumerate(names):
        im = Image.fromarray(r[n]); x, y = k % 4, k // 4
        im.save(os.path.join(out, 'game-' + n.lower().replace(' ', '-') + '.png'))
        whole.paste(im.crop((X0, 0, X0 + PW, H)).resize((480, 360), Image.LANCZOS), (x * 484, y * 384 + 20))
        crop.paste(im.crop((780, 380, 1140, 650)), (x * 364, y * 294 + 20))
        big.paste(im.crop((915, 480, 1005, 547)).resize((360, 268), Image.NEAREST), (x * 364, y * 294 + 20))
        for img, wd, ht in ((whole, 484, 384), (crop, 364, 294), (big, 364, 294)):
            ImageDraw.Draw(img).text((x * wd + 4, y * ht + 4), n, fill='white')
    whole.save(os.path.join(out, 'sheet-whole.png')); crop.save(os.path.join(out, 'sheet-crop.png')); big.save(os.path.join(out, 'sheet-crop-4x.png'))

def main():
    global RA_ACROSS
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    text = ''
    for RA_ACROSS in (2, 1):
        text += "RetroArch's shaders given the frame %d wide:\n\n" % (640 // RA_ACROSS) + table(measure(out)) + '\n\n'
    print(text)
    open(os.path.join(out, 'table.md'), 'w').write(text)
    RA_ACROSS = 2
    if len(sys.argv) > 2:
        sheets(out, sys.argv[2])

main()
