#!/usr/bin/env python3
"""Extract Burnout 3 .bgv car paint textures (all color variants) to PNG.

Texture record reached from .bgv header +0x60 (absolute offset). Layout
(format credit: EdnessP's fmt_Burnout3LRD.py boTexXbox, old revision;
cross-checked against the game's draw path which selects the palette via the
count byte at record+0x69 and the pointer array at record+0x14):

  +0x04 u32 pixel-data offset (relative to record)
  +0x14 u32[palCount] palette record offsets (relative to record)
  +0x34 u32 format (0xB = 8bpp paletted)   +0x38 u32 width  +0x3C u32 height
  +0x40 u32 bit depth (8)
  +0x48 char[] texture name
  +0x69 u8  palette count (one palette per car color variant)

  palette record: {u16 1, u16 3 (or 0xC003)}, u32 data offset rel TEXTURE
  record; data = 256 x BGRA bytes.

Pixels are Morton/Z-order swizzled (Xbox); low bits of x/y interleave, the
larger dimension's high bits follow linearly.

Output: build/cars/<CLASS>_<CarN>_p<K>.png
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import os
import struct
import sys
from PIL import Image

PVEH = (game_path('pveh'))


def unswizzle8(data, w, h):
    out = bytearray(w * h)
    xbits = w.bit_length() - 1
    ybits = h.bit_length() - 1
    shared = min(xbits, ybits)
    for i in range(w * h):
        x = y = 0
        v = i
        for b in range(shared):
            x |= (v & 1) << b; v >>= 1
            y |= (v & 1) << b; v >>= 1
        if xbits > ybits:
            x |= v << shared
        else:
            y |= v << shared
        out[y * w + x] = data[i]
    return bytes(out)


def extract_textures(path):
    data = open(path, 'rb').read()
    def u32(o): return struct.unpack_from('<I', data, o)[0]
    tex = u32(0x60)
    if not (0 < tex < len(data) - 0x70):
        return None, "no texture record"
    fmt, w, h, depth = struct.unpack_from('<4I', data, tex + 0x34)
    if fmt != 0xB or depth != 8:
        return None, "unhandled fmt 0x%X depth %d" % (fmt, depth)
    if not (0 < w <= 2048 and 0 < h <= 2048):
        return None, "bad dims %dx%d" % (w, h)
    bmp = tex + u32(tex + 0x4)
    npal = data[tex + 0x69]
    name_end = data.find(b'\0', tex + 0x48)
    name = data[tex + 0x48:name_end].decode('ascii', 'replace')
    if bmp + w * h > len(data):
        return None, "pixels truncated"
    pixels = unswizzle8(data[bmp:bmp + w * h], w, h)

    images = []
    for k in range(npal):
        prec = tex + u32(tex + 0x14 + k * 4)
        if prec + 12 > len(data):
            continue
        magic = data[prec:prec + 4]
        if magic not in (b'\x01\x00\x03\x00', b'\x01\x00\x03\xc0'):
            continue
        pdata = tex + u32(prec + 4)
        if pdata + 1024 > len(data):
            continue
        pal = data[pdata:pdata + 1024]
        img = Image.new('RGBA', (w, h))
        px = img.load()
        for y in range(h):
            row = y * w
            for x in range(w):
                i = pixels[row + x] * 4
                px[x, y] = (pal[i + 2], pal[i + 1], pal[i], pal[i + 3])
        images.append(img)
    if not images:
        return None, "no valid palettes (%d declared)" % npal
    return (name, images), None


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/cars"
    os.makedirs(outdir, exist_ok=True)
    ok = fail = 0
    for cls in sorted(os.listdir(PVEH)):
        d = os.path.join(PVEH, cls)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith('.bgv'):
                continue
            car = os.path.splitext(fn)[0]
            res, err = extract_textures(os.path.join(d, fn))
            if err:
                print("FAIL %s_%s: %s" % (cls, car, err))
                fail += 1
                continue
            name, images = res
            for k, img in enumerate(images):
                img.save(os.path.join(outdir, "%s_%s_p%d.png" % (cls, car, k)))
            print("ok   %s_%-8s '%s' %dx%d %d palette(s)"
                  % (cls, car, name, images[0].width, images[0].height,
                     len(images)))
            ok += 1
    print("\n%d cars textured, %d failed -> %s" % (ok, fail, outdir))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
