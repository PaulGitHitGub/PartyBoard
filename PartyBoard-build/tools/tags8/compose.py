"""Preview mock-ups: paste the new 5P-8P art where it would appear on full-size screenshots.
    python3 compose.py <tags_dir> <screenshot.png> <out.png> <kind>
kind: charsel (5P-8P cursor tags + frames on the bottom-row portraits), summary (5P-8P labels in
the right-hand party strip), rules (5P-8P labels over the bottom-row Teams faces).
Sprite coordinates are 576x480, scaled to the screenshot."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from gen import read_png, write_png

def paste(dst, src, sx0, sy0, sw, sh, X, Y, scale):
    """Draw src[sy0:sy0+sh][sx0:sx0+sw] with its top-left at game point (X, Y), scaled."""
    H = len(dst); W = len(dst[0]); kx = W / 576.0; ky = H / 480.0
    px0 = X * kx; py0 = Y * ky; pw = sw * scale * kx; ph = sh * scale * ky
    for py in range(int(py0), int(py0 + ph) + 1):
        for px in range(int(px0), int(px0 + pw) + 1):
            if not (0 <= px < W and 0 <= py < H): continue
            u = (px + .5 - px0) / pw * sw - .5; v = (py + .5 - py0) / ph * sh - .5
            if u < -.5 or v < -.5 or u > sw - .5 or v > sh - .5: continue
            x0 = max(0, min(sw - 1, int(u // 1))); y0 = max(0, min(sh - 1, int(v // 1)))
            x1 = min(sw - 1, x0 + 1); y1 = min(sh - 1, y0 + 1); fx = min(1, max(0, u - x0)); fy = min(1, max(0, v - y0))
            acc = [0.0] * 4
            for (xx, yy, w) in ((x0, y0, (1 - fx) * (1 - fy)), (x1, y0, fx * (1 - fy)), (x0, y1, (1 - fx) * fy), (x1, y1, fx * fy)):
                p = src[sy0 + yy][sx0 + xx]; a = p[3] / 255 * w
                for c in range(3): acc[c] += p[c] * a
                acc[3] += a
            if acc[3] <= 0: continue
            a = acc[3]; d = dst[py][px]
            dst[py][px] = tuple(int(acc[c] / a * a + d[c] * (1 - a)) if c < 3 else 255 for c in range(4))

def main(tagdir, shot, out, kind):
    _, _, img = read_png(shot)
    _, _, tags = read_png(os.path.join(tagdir, 'tags_5p8p_1x.png'))
    _, _, labels = read_png(os.path.join(tagdir, 'labels_5p8p_1x.png'))
    if kind == 'charsel':
        gy = 2.06
        for k in range(4):
            x = (168, 248, 328, 408)[k]; y = 190 + gy
            _, _, fr = read_png(os.path.join(tagdir, 'frame_%dp_1x.png' % (k + 5)))
            paste(img, fr, 0, 0, 96, 96, x - 48, y - 48, 1.0)
            paste(img, tags, 40 * k, 32, 40, 32, x - 20, y - 52, 1.0)
    elif kind == 'summary':
        s = 0.82; cx = (576 - 17 - 16) - 95 * s
        for k in range(4):
            x = 16 + cx + (-69 + 46 * k) * s; y = 40 + 32 - 8 * s + 25 * s
            paste(img, labels, 0, 18 * k, 26, 18, x - 13 * s, y - 9 * s, s)
    elif kind == 'rules':
        s = 0.6
        for k in range(4):
            x = 340 + (k - 1.5) * 42 - 8 * s; y = 116 + 15 - 21 * s
            paste(img, labels, 0, 18 * k, 26, 18, x - 13 * s, y - 9 * s, s)
    write_png(out, img)

if __name__ == '__main__':
    main(*sys.argv[1:5])
