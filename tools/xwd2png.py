"""xwd2png.py in.xwd out.png: a 24/32-bit TrueColor XWD dump (xwd -id ...) as a PNG."""
import struct
import sys

from PIL import Image

data = open(sys.argv[1], "rb").read()
f = struct.unpack(">25I", data[:100])
header_size, depth, width, height = f[0], f[3], f[4], f[5]
byte_order, bpp, bpl = f[7], f[11], f[12]
masks = f[14:17]
ncolors = f[19]
off = header_size + ncolors * 12
if bpp != 32:
    sys.exit(f"{bpp} bits per pixel: not handled")
order = "<" if byte_order == 0 else ">"
img = Image.new("RGB", (width, height))
px = img.load()


def shift(mask):
    return (mask & -mask).bit_length() - 1


shifts = [shift(m) for m in masks]
for y in range(height):
    row = data[off + y * bpl: off + y * bpl + width * 4]
    for x, (v,) in enumerate(struct.iter_unpack(order + "I", row)):
        px[x, y] = tuple((v & m) >> s for m, s in zip(masks, shifts))
img.save(sys.argv[2])
print(width, height)
