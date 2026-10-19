"""5P-8P player tags for the 8-player party, built from the game's own 1P-4P art (PREVIEW).

Inputs (decoded from the game, see README): tags.png (the 200x64 cursor-tag atlas: 1P-4P and COM,
star bank on top, dome bank below), label.png (32x72 small 1P-4P labels), frame1.png (96x192 red
portrait frame), font.png (16x176 white digit font, the one the 5th-8th rank labels use).
Each new tag keeps the 1P tag's base (star or dome), outline and "P", with the numeral redrawn
from the game's digit font and the colours moved to a new hue.
    python3 gen.py <in_dir> <out_dir>
"""
import colorsys, os, struct, sys, zlib

def read_png(p):
    b = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(b):
        n, t = struct.unpack('>I4s', b[i:i + 8]); c = b[i + 8:i + 8 + n]; i += 12 + n
        if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT': idat += c
    raw = zlib.decompress(idat); st = w * 4; prev = bytearray(st); rows = []; o = 0
    for y in range(h):
        f = raw[o]; line = bytearray(raw[o + 1:o + 1 + st]); o += 1 + st
        for x in range(st):
            a = line[x - 4] if x >= 4 else 0; up = prev[x]; c = prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + up) & 255
            elif f == 3: line[x] = (line[x] + (a + up) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(up - c), abs(a - c), abs(a + up - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else up if pb <= pc else c)) & 255
        rows.append([tuple(line[x * 4:x * 4 + 4]) for x in range(w)]); prev = line
    return w, h, rows

def write_png(p, rows):
    h = len(rows); w = len(rows[0])
    raw = b''.join(b'\0' + bytes(c for px in r for c in px) for r in rows)
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                        ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))

# New colours: (hue in degrees, saturation factor, value factor), applied to the 1P (red) colours.
# Chosen not to clash with 1P red, 2P blue, 3P yellow, 4P green, COM cyan or the purple base.
TARGETS = {5: ('orange', 28, 1.0, 1.05), 6: ('pink', 325, 0.55, 1.12), 7: ('silver', 0, 0.0, 1.1), 8: ('brown', 24, 0.7, 0.8)}

def recolor(px, target):
    _, hue, sf, vf = target
    r, g, b, a = px
    h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
    s = min(1, s * sf); v = min(1, v * vf)
    r, g, b = colorsys.hsv_to_rgb(hue / 360, s, v)
    return (int(r * 255 + .5), int(g * 255 + .5), int(b * 255 + .5), a)

def is_red(px):
    r, g, b, a = px
    if a < 128: return False
    h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
    return s > 0.35 and (h < 0.06 or h > 0.92)

def glyph(font, n):
    """White body of digit n of the game font (16x16 cell), as a 0/1 mask, plus its bbox."""
    m = [[1 if font[16 * n + y][x][3] > 16 and sum(font[16 * n + y][x][:3]) > 400 else 0 for x in range(16)] for y in range(16)]
    xs = [x for y in range(16) for x in range(16) if m[y][x]]; ys = [y for y in range(16) for x in range(16) if m[y][x]]
    return m, (min(xs), min(ys), max(xs) + 1, max(ys) + 1)

def build(cells, font, num, target, digit_cell=0):
    """cells: the four 1P-4P cells (2D pixel lists) of one bank. Returns the new cell: the shared
    base, the template's "P" in the new colour, and the new numeral."""
    H = len(cells[0]); W = len(cells[0][0])
    tmpl = cells[digit_cell]
    # the base all four cells share (where at least three agree)
    base = [[None] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            vals = [c[y][x] for c in cells]
            best = max(set(vals), key=vals.count)
            if vals.count(best) >= 3: base[y][x] = best
    cols = [x for x in range(W) if any(is_red(tmpl[y][x]) for y in range(H))]
    counts = [sum(is_red(tmpl[y][x]) for y in range(H)) for x in range(W)]
    lo = min(cols) + 3; hi = max(cols) - 3
    split = min(range(lo, hi), key=lambda x: (counts[x], abs(x - (lo + hi) // 2)))
    differs = [[base[y][x] is None or tmpl[y][x] != base[y][x] for x in range(W)] for y in range(H)]
    # numeral box (with its outline): where the cells' numerals are, left of the P
    pts = [(x, y) for y in range(H) for x in range(split) if differs[y][x] or any(c[y][x] != tmpl[y][x] for c in cells)]
    bx0 = min(x for x, y in pts); bx1 = max(x for x, y in pts) + 1
    by0 = min(y for x, y in pts); by1 = max(y for x, y in pts) + 1
    out = [[None] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if x >= split and differs[y][x]:
                p = tmpl[y][x]
                out[y][x] = recolor(p, target) if p[3] > 0 and (is_red(p) or sum(p[:3]) < 200) else p
            elif x < split and (by0 <= y < by1 and bx0 <= x < bx1):
                out[y][x] = None            # repainted below
            else:
                out[y][x] = base[y][x] if base[y][x] is not None else tmpl[y][x]
    # background inside the numeral box: nearest shared-base pixel in the same column (the bases
    # are vertical gradients), else in the row
    for y in range(by0, by1):
        for x in range(bx0, min(bx1, split)):
            got = None
            for d in range(1, H):
                for yy in (y - d, y + d):
                    if 0 <= yy < H and not (by0 <= yy < by1) and base[yy][x] is not None:
                        got = base[yy][x]; break
                if got: break
            out[y][x] = got or (0, 0, 0, 0)
    # fill colour per row (the template numeral's brightest red), outline = its darkest pixel
    row_col = {}
    for y in range(H):
        reds = [tmpl[y][x] for x in range(split) if is_red(tmpl[y][x])]
        if reds: row_col[y] = max(reds, key=lambda p: sum(p[:3]))
    outline = min((tmpl[y][x] for y in range(H) for x in range(split) if differs[y][x] and tmpl[y][x][3] > 200), key=lambda p: sum(p[:3]))
    # numeral body: the game font's white digit, supersampled into the box less a 2-pixel outline
    g, (gx0, gy0, gx1, gy1) = glyph(font, num)
    ox0, oy0, ox1, oy1 = bx0 + 2, by0 + 2, bx1 - 2, by1 - 2
    body = [[0] * W for _ in range(H)]
    for y in range(oy0, oy1):
        for x in range(ox0, ox1):
            hit = 0
            for sy in range(4):
                for sx in range(4):
                    u = gx0 + ((x - ox0) + (sx + .5) / 4) * (gx1 - gx0) / (ox1 - ox0)
                    v = gy0 + ((y - oy0) + (sy + .5) / 4) * (gy1 - gy0) / (oy1 - oy0)
                    hit += g[min(15, int(v))][min(15, int(u))]
            body[y][x] = hit >= 8
    lit = lambda c: recolor(c, target)
    for y in range(by0, by1):
        for x in range(bx0, min(bx1, split)):
            if body[y][x]:
                c = row_col.get(y) or row_col[min(row_col, key=lambda r: abs(r - y))]
                out[y][x] = lit(c)
            elif any(0 <= y + dy < H and 0 <= x + dx < W and body[y + dy][x + dx] for dy in range(-2, 3) for dx in range(-2, 3) if dx * dx + dy * dy <= 5):
                out[y][x] = lit(outline) if target[2] > 0 else (48, 48, 56, 255)
    return out

def build_label(cell, font, num, target):
    """Small seat label (26x18): the 1P label in the new colour, numeral redrawn."""
    H = len(cell); W = len(cell[0])
    hue = lambda p: colorsys.rgb_to_hsv(p[0] / 255, p[1] / 255, p[2] / 255)
    reddish = lambda p: p[3] > 128 and hue(p)[1] > 0.25 and (hue(p)[0] < 0.08 or hue(p)[0] > 0.9)
    cols = [x for x in range(W) if any(is_red(cell[y][x]) and hue(cell[y][x])[2] > 0.6 for y in range(H))]
    counts = [sum(is_red(cell[y][x]) and hue(cell[y][x])[2] > 0.6 for y in range(H)) for x in range(W)]
    lo = min(cols) + 2; hi = max(cols) - 2
    split = min(range(lo, hi), key=lambda x: (counts[x], abs(x - (lo + hi) // 2)))
    out = [[recolor(p, target) if reddish(p) else p for p in row] for row in cell]
    inner = [(x, y) for y in range(H) for x in range(split) if reddish(cell[y][x])]
    others = [cell[y][x] for x, y in inner if hue(cell[y][x])[2] <= 0.6]
    bg = max(set(others), key=others.count)
    fills = {}
    for x, y in inner:
        if hue(cell[y][x])[2] > 0.6:
            fills[y] = max(fills.get(y, cell[y][x]), cell[y][x], key=lambda p: sum(p[:3]))
    dark = min((cell[y][x] for x, y in inner), key=lambda p: sum(p[:3]))
    bx0 = min(x for x, y in inner); bx1 = max(x for x, y in inner) + 1
    by0 = min(y for x, y in inner); by1 = max(y for x, y in inner) + 1
    for x, y in inner:
        out[y][x] = recolor(bg, target)
    g, (gx0, gy0, gx1, gy1) = glyph(font, num)
    ox0, oy0, ox1, oy1 = bx0 + 1, by0 + 1, bx1 - 1, by1 - 1
    body = {}
    for y in range(oy0, oy1):
        for x in range(ox0, ox1):
            hit = sum(g[min(15, int(gy0 + ((y - oy0) + (sy + .5) / 4) * (gy1 - gy0) / (oy1 - oy0)))]
                      [min(15, int(gx0 + ((x - ox0) + (sx + .5) / 4) * (gx1 - gx0) / (ox1 - ox0)))] for sy in range(4) for sx in range(4))
            if hit >= 8: body[(x, y)] = 1
    for x, y in inner:
        if (x, y) in body:
            c = fills.get(y) or fills[min(fills, key=lambda r: abs(r - y))]
            out[y][x] = recolor(c, target)
        elif any((x + dx, y + dy) in body for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
            out[y][x] = recolor(dark, target) if target[2] > 0 else (48, 48, 56, 255)
    return out

def colorful(p):
    r, g, b, a = p
    if a < 128: return False
    h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
    return s > 0.45 and v > 0.45 and not (0.68 < h < 0.80)   # not the purple base

def recolor_back(p): return p

def cells_of(img, x0s, y0, cw, ch):
    return [[img[y][x0:x0 + cw] for y in range(y0, y0 + ch)] for x0 in x0s]

def main(ind, outd):
    os.makedirs(outd, exist_ok=True)
    _, _, font = read_png(os.path.join(ind, 'font.png'))
    _, _, tags = read_png(os.path.join(ind, 'tags.png'))
    _, _, label = read_png(os.path.join(ind, 'label.png'))
    _, _, frame = read_png(os.path.join(ind, 'frame1.png'))
    new_tags = [[(0, 0, 0, 0)] * 160 for _ in range(64)]
    for bank_y in (0, 32):
        cells = cells_of(tags, (0, 40, 80, 120), bank_y, 40, 32)
        for k, n in enumerate((5, 6, 7, 8)):
            cell = build(cells, font, n, TARGETS[n])
            for y in range(32):
                new_tags[bank_y + y][40 * k:40 * k + 40] = cell[y]
    write_png(os.path.join(outd, 'tags_5p8p_1x.png'), new_tags)
    lcells = [[label[18 * k + y][0:26] for y in range(18)] for k in range(4)]
    new_labels = []
    for n in (5, 6, 7, 8):
        cell = build_label(lcells[0], font, n, TARGETS[n])
        new_labels += [r + [(0, 0, 0, 0)] * 6 for r in cell]
    write_png(os.path.join(outd, 'labels_5p8p_1x.png'), new_labels)
    for n in (5, 6, 7, 8):
        write_png(os.path.join(outd, 'frame_%dp_1x.png' % n), [[recolor(p, TARGETS[n]) if p[3] and is_red(p) or (p[3] and sum(p[:3]) > 0 and colorsys.rgb_to_hsv(p[0]/255, p[1]/255, p[2]/255)[1] > 0.2) else p for p in row] for row in frame])

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
