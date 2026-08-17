#!/usr/bin/env python3
"""
Extract Burnout 3's three compiled-in HUD/menu fonts from the XBE image.

DISCOVERY (2026-08-10, session notes in docs/RE_FRONTEND.md section 6):
docs previously recorded "no font/digit texture exists in Frontend.txd or
Global.txd [C]" and the glyph store as open [?]. Resolved: the fonts are
COMPILED INTO THE EXECUTABLE (.data), each as a font object followed by its
texture record + DXT5 bitmap, in the exact texture-record layout used by
static.dat / .txd / .bgv (name at +0x48, fmt at +0x34).

Evidence chain (burnout3.elf VAs):
  * FUN_0002ef90 (font-system init, called from FUN_0003c8a0) stores three
    font-object pointers from .data and binds each to its texture by NAME
    via FUN_0002ddf0 (a __stricmp scan of a loaded texture container's
    records at +0x48 -- the same +0x48 name field as every other texture
    record in the game).  The names -- "GlobalFont", "HeadFont",
    "SmallFont" (.rdata 0x3aaed0/0x3aaec4/0x3aaeb8) -- exist nowhere in any
    shipped data file; the only records carrying them are in the XBE .data:
    the lookup resolves to the compiled-in records below.
  * Pointer slots:  [0x3e7b98] -> GlobalFont object, [0x3e7bb0] -> HeadFont
    object, [0x3e7ba4] -> SmallFont object (assignment order read off the
    FUN_0002ef90 decompile: obj+0x10 <- [0x3e7b98] gets "GlobalFont",
    obj+0x14 <- [0x3e7bb0] gets "HeadFont", obj+0x18 <- [0x3e7ba4] gets
    "SmallFont").
  * Font object layout (proven by the glyph-walk in FUN_001c1060 /
    FUN_001c0f50 and the relocation loop in FUN_0002ef90):
      +0x00  char[4] "3rev"          +0x04  u32 texture handle (runtime)
      +0x08  f32,f32 layout scale    +0x10  f32,f32 (unresolved scalars)
      +0x18  u32 relocated flag      +0x1C  ptr default glyph record
      +0x20  ptr[128] charmap: glyph record for (c & 0x7F)
    Before relocation +0x1C/+0x20[] hold OFFSETS from the object base;
    FUN_0002ef90 adds the base once (+0x18 guards).
  * Glyph record, 0x20 bytes (layout confirmed two ways: decoded fields
    below are self-consistent for every mapped char, AND FUN_001c1060
    reads +0x1C as a u16 charcode to verify the charmap hit, walking
    0x20-stride sibling records on mismatch):
      +0x00 f32 u        +0x04 f32 v         (top-left, / texture size)
      +0x08 f32 w        +0x0C f32 h         (extent,   / texture size)
      +0x10 f32 xoffset  +0x14 f32 yoffset   (pen offset, same units)
      +0x18 f32 advance  +0x1C u32 charcode
  * Texture record at a fixed offset from each object (records are packed
    directly after the glyph pool): name/fmt/dims verified below at
    runtime; all three are fmt 0xF (DXT5), bitmap at +0x80, no palettes.

      object      VA        texture rec  name        dims     coverage
      GlobalFont  0x3c84d8  0x3c9c38     GlobalFont  256x256  ASCII 32..126
      HeadFont    0x3d9cb8  0x3da338     HeadFont    128x256  space, '-',
                                                              A-Z minus W
      SmallFont   0x3e23b8  0x3e3b18     SmallFont   128x128  ASCII 32..126

  All three atlases are WHITE glyphs + DXT5 alpha; colour (the HUD's gold,
  the menus' white/blue) is applied by the 2D pipeline's vertex colours at
  draw time (text scene-graph nodes carry an RGBA each, FUN_001c15a0 /
  FUN_001c1670).

Outputs:
  build/frontend/GlobalFont.png / HeadFont.png / SmallFont.png  (RGBA)
  src/burnout3_font.h   glyph metrics tables for the harness HUD

Exits non-zero if any structural check fails (name mismatch, charcode
mismatch, bad UVs).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_textures import decode_dxt                     # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(REPO, "build", "burnout3.elf")
OUT_DIR = os.path.join(REPO, "build", "frontend")
OUT_HDR = os.path.join(REPO, "src", "burnout3_font.h")

# (name, object VA, texture record VA)  -- see header comment for provenance
FONTS = [
    ("GlobalFont", 0x3C84D8, 0x3C9C38),
    ("HeadFont",   0x3D9CB8, 0x3DA338),
    ("SmallFont",  0x3E23B8, 0x3E3B18),
]
PTR_SLOTS = {"GlobalFont": 0x3E7B98, "HeadFont": 0x3E7BB0,
             "SmallFont": 0x3E7BA4}


class Image32:
    """Map the ELF's PT_LOAD segments so VAs read the true bytes."""

    def __init__(self, path):
        self.data = open(path, "rb").read()
        assert self.data[:4] == b"\x7fELF", "not an ELF"
        e_phoff, = struct.unpack_from("<I", self.data, 0x1C)
        e_phnum, = struct.unpack_from("<H", self.data, 0x2C)
        self.segs = []
        for i in range(e_phnum):
            p_type, p_off, p_va, _, p_filesz, p_memsz = struct.unpack_from(
                "<IIIIII", self.data, e_phoff + i * 32)[:6]
            if p_type == 1:
                self.segs.append((p_va, p_off, p_filesz, p_memsz))

    def read(self, va, n):
        for v, o, fs, ms in self.segs:
            if v <= va < v + ms:
                off = va - v
                if off + n <= fs:
                    return self.data[o + off:o + off + n]
                got = self.data[o + off:o + fs]
                return got + b"\0" * (n - len(got))
        raise ValueError("VA %#x unmapped" % va)

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]


def extract(img):
    from PIL import Image
    os.makedirs(OUT_DIR, exist_ok=True)
    fonts_meta = []
    for name, obj, rec in FONTS:
        # --- structural verification -----------------------------------
        slot = img.u32(PTR_SLOTS[name])
        assert slot == obj, \
            "%s: pointer slot %#x -> %#x, expected %#x" % (
                name, PTR_SLOTS[name], slot, obj)
        assert img.read(obj, 4) == b"3rev", "%s: bad object magic" % name
        rec_name = img.read(rec + 0x48, 16).split(b"\0")[0].decode()
        assert rec_name == name, \
            "%s: texture record name is %r" % (name, rec_name)
        fmt = img.u32(rec + 0x34)
        w = img.u32(rec + 0x38)
        h = img.u32(rec + 0x3C)
        bmp_off = img.u32(rec + 0x04)
        assert fmt == 0xF and bmp_off == 0x80, \
            "%s: unexpected fmt %#x / bmp off %#x" % (name, fmt, bmp_off)

        # --- bitmap (DXT5 = 1 byte/pixel) ------------------------------
        rgba = decode_dxt(img.read(rec + 0x80, w * h), 0, w, h, 0xF)
        assert rgba is not None
        Image.frombytes("RGBA", (w, h), bytes(rgba)).save(
            os.path.join(OUT_DIR, name + ".png"))

        # --- glyph table ----------------------------------------------
        default_off = img.u32(obj + 0x1C)
        offs = struct.unpack("<128I", img.read(obj + 0x20, 512))
        glyphs = {}
        for c in range(0x20, 0x7F):
            # FUN_001c1060's exact walk: charmap[c & 0x7F] is the FIRST
            # record whose (charcode & 0x7F) == c; on charcode mismatch
            # step 0x20-stride sibling records until the default record.
            o = offs[c]
            while o != default_off:
                cc, = struct.unpack("<I", img.read(obj + o + 28, 4))
                if cc == c:
                    break
                o += 0x20
            if o == default_off:
                continue
            u, v, gw, gh, xo, yo, adv = struct.unpack(
                "<7f", img.read(obj + o, 28))
            if u < 0.0 or gh == 0.0:
                # blank glyph (space uses u = v = -1): advance only
                u = v = gw = gh = 0.0
            assert 0.0 <= u <= 1.0 and 0.0 <= v <= 1.0 and \
                0.0 <= u + gw <= 1.001 and 0.0 <= v + gh <= 1.001, \
                "%s: glyph %r bad UVs" % (name, chr(c))
            glyphs[c] = (u, v, gw, gh, xo, yo, adv)
        # ' ' has a record in all three fonts; require it plus digits for
        # the two full fonts
        assert 0x20 in glyphs, "%s: no space glyph" % name
        if name != "HeadFont":
            for d in range(0x30, 0x3A):
                assert d in glyphs, "%s: digit %r missing" % (name, chr(d))
        fonts_meta.append((name, w, h, glyphs))
        print("%s: %dx%d DXT5, %d glyphs mapped" % (name, w, h, len(glyphs)))
    return fonts_meta


def emit_header(fonts_meta):
    lines = []
    a = lines.append
    a("/* GENERATED by tools/extract_font.py -- do not edit.")
    a(" *")
    a(" * Burnout 3's three fonts, recovered from the XBE .data section")
    a(" * (font objects 0x3c84d8/0x3d9cb8/0x3e23b8 + embedded DXT5 texture")
    a(" * records 0x3c9c38/0x3da338/0x3e3b18; glyph record layout proven by")
    a(" * FUN_001c1060's charmap walk -- see tools/extract_font.py and")
    a(" * docs/RE_FRONTEND.md section 6).  Pixel-space metrics; the atlases")
    a(" * (build/frontend/<name>.png) are white + alpha, colour is applied")
    a(" * at draw time exactly as the game's 2D pipeline does.")
    a(" */")
    a("#ifndef BURNOUT3_FONT_H")
    a("#define BURNOUT3_FONT_H")
    a("")
    a("typedef struct B3Glyph {")
    a("    float u0, v0, u1, v1;   /* normalized atlas rect */")
    a("    float w, h;             /* glyph size, atlas px  */")
    a("    float xoff, yoff;       /* pen offset, atlas px  */")
    a("    float advance;          /* pen advance, atlas px */")
    a("    unsigned char present;  /* 0 = font has no glyph */")
    a("} B3Glyph;")
    a("")
    a("typedef struct B3Font {")
    a("    const char *name;       /* atlas basename in build/frontend */")
    a("    int tex_w, tex_h;")
    a("    float line_h;           /* max glyph yoff+h, atlas px */")
    a("    B3Glyph glyph[95];      /* chars 0x20..0x7E */")
    a("} B3Font;")
    a("")
    for name, w, h, glyphs in fonts_meta:
        line_h = max(g[5] * h + g[3] * h for g in glyphs.values())
        a("static const B3Font b3_font_%s = {" % name.lower())
        a('    "%s", %d, %d, %.3ff,' % (name, w, h, line_h))
        a("    {")
        for c in range(0x20, 0x7F):
            g = glyphs.get(c)
            cs = chr(c) if chr(c) not in "\\'" else "\\" + chr(c)
            if g is None:
                a("    {0,0,0,0, 0,0, 0,0, 0, 0}, /* '%s' absent */" % cs)
                continue
            u, v, gw, gh, xo, yo, adv = g
            a("    {%.6ff,%.6ff,%.6ff,%.6ff, %.1ff,%.1ff, %.1ff,%.1ff, "
              "%.1ff, 1}, /* '%s' */"
              % (u, v, u + gw, v + gh, gw * w, gh * h, xo * w, yo * h,
                 adv * w, cs))
        a("    }")
        a("};")
        a("")
    a("#endif /* BURNOUT3_FONT_H */")
    with open(OUT_HDR, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s" % OUT_HDR)


def main():
    if not os.path.exists(ELF):
        sys.exit("missing %s -- run tools/xbe2elf.py first" % ELF)
    img = Image32(ELF)
    fonts_meta = extract(img)
    emit_header(fonts_meta)


if __name__ == "__main__":
    main()
