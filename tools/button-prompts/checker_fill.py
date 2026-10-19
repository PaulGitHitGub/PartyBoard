"""checker_fill.py <in.png> <out.png> <x0> <y0> <x1> <y1>: rebuilds the
Practice/Back banner's 8x8 green checker inside the box (inclusive pixel
coordinates), erasing the painted Z button. Light squares sit where
((x - 155) // 8 + (y - 1) // 8) is even, measured from the clean rows."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0])) + "/../hd-textures")
import texconv
w, h, px = texconv.read_png_rgba(sys.argv[1]); px = bytearray(px)
x0, y0, x1, y1 = map(int, sys.argv[3:7])
LIGHT, DARK = (148, 214, 8), (41, 173, 0)
for y in range(y0, y1 + 1):
    for x in range(x0, x1 + 1):
        c = LIGHT if ((x - 155) // 8 + (y - 1) // 8) % 2 == 0 else DARK
        i = (y * w + x) * 4; px[i:i + 3] = bytes(c); px[i + 3] = 255
texconv.write_png(sys.argv[2], w, h, bytes(px))
