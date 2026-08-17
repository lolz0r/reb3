#!/usr/bin/env python3
"""
xbe2elf - convert an original Xbox XBE into an ELF32/x86 image with correct
virtual address mapping, so Ghidra (or any ELF-aware tool) loads it properly.

The prior analysis loaded default.xbe as a flat raw binary at address 0, which
makes every address a file offset. Section deltas differ per section, so all
absolute data references (float constants, strings, vtables, jump tables)
resolve to the wrong bytes. This rebuilds the real VA space instead.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import struct, sys, os

XOR_EP = {'retail': 0xA8FC57AB, 'debug': 0x94859D4B, 'chihiro': 0x40B5C16E}
XOR_KT = {'retail': 0x5B6D40B6, 'debug': 0xEFB1F152, 'chihiro': 0x2290059D}

SEC_W, SEC_P, SEC_X = 1, 2, 4
PF_X, PF_W, PF_R = 1, 2, 4


class Section:
    __slots__ = ('name', 'vaddr', 'vsize', 'raw_off', 'raw_size', 'flags')

    def __init__(self, name, vaddr, vsize, raw_off, raw_size, flags):
        self.name, self.vaddr, self.vsize = name, vaddr, vsize
        self.raw_off, self.raw_size, self.flags = raw_off, raw_size, flags

    @property
    def elf_flags(self):
        f = PF_R
        if self.flags & SEC_X:
            f |= PF_X
        if self.flags & SEC_W:
            f |= PF_W
        return f


class Xbe:
    def __init__(self, data):
        if data[:4] != b'XBEH':
            raise ValueError('not an XBE (bad magic)')
        self.d = data
        u = self.u32
        self.base = u(0x104)
        self.hdr_size = u(0x108)
        self.image_size = u(0x10C)
        self.cert_addr = u(0x118)
        self.nsections = u(0x11C)
        self.sec_hdr_addr = u(0x120)
        self.ep_raw = u(0x128)
        self.tls_addr = u(0x12C)
        self.kt_raw = u(0x158)
        self.kind, self.entry = self._decode(self.ep_raw, XOR_EP)
        self.kernel_thunk = self.kt_raw ^ XOR_KT[self.kind] if self.kind else 0
        self.sections = self._sections()
        self._cert()

    def u32(self, off):
        return struct.unpack_from('<I', self.d, off)[0]

    def _decode(self, val, table):
        """Pick the XOR key whose result lands inside the image."""
        for kind, key in table.items():
            v = val ^ key
            if self.base <= v < self.base + self.image_size:
                return kind, v
        return None, val

    def _cstr(self, vaddr):
        o = vaddr - self.base
        if not (0 <= o < len(self.d)):
            return '?'
        end = self.d.index(b'\0', o)
        return self.d[o:end].decode('ascii', 'replace')

    def _sections(self):
        out, o = [], self.sec_hdr_addr - self.base
        for i in range(self.nsections):
            b = o + i * 0x38
            flags, va, vs, ro, rs, nameaddr = struct.unpack_from('<IIIIII', self.d, b)
            out.append(Section(self._cstr(nameaddr), va, vs, ro, rs, flags))
        return out

    def _cert(self):
        c = self.cert_addr - self.base
        self.title_id = self.u32(c + 8)
        self.title = self.d[c + 12:c + 92].decode('utf-16-le').rstrip('\0')


def build_elf(xbe, include_headers=True):
    """Emit ET_EXEC ELF32/EM_386 with one PT_LOAD per XBE section."""
    secs = sorted(xbe.sections, key=lambda s: s.vaddr)

    segs = []  # (vaddr, filesz, memsz, flags, payload, name)
    if include_headers:
        # Map the XBE header itself read-only so header structures stay visible.
        segs.append((xbe.base, xbe.hdr_size, xbe.hdr_size, PF_R,
                     xbe.d[0:xbe.hdr_size], 'XBEHDR'))
    for s in secs:
        payload = xbe.d[s.raw_off:s.raw_off + s.raw_size]
        # vsize > raw_size means trailing BSS; ELF p_memsz handles it natively.
        segs.append((s.vaddr, len(payload), max(s.vsize, len(payload)),
                     s.elf_flags, payload, s.name))

    EHDR, PHDR = 52, 32
    nph = len(segs)
    data_off = EHDR + PHDR * nph
    # Keep file offset congruent to vaddr mod 4096 (ELF loader requirement,
    # and Ghidra's ELF loader validates it).
    blobs, phdrs = [], []
    cur = data_off
    for vaddr, filesz, memsz, flags, payload, name in segs:
        pad = (vaddr - cur) % 0x1000
        cur += pad
        blobs.append((b'\0' * pad, payload))
        phdrs.append((cur, vaddr, filesz, memsz, flags))
        cur += filesz

    out = bytearray()
    e_shoff = 0
    out += b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8      # e_ident
    out += struct.pack('<HHIIIIIHHHHHH',
                       2, 3, 1,          # ET_EXEC, EM_386, EV_CURRENT
                       xbe.entry,        # e_entry  <- real entry point
                       EHDR,             # e_phoff
                       e_shoff, 0,
                       EHDR, PHDR, nph,
                       40, 0, 0)
    for off, vaddr, filesz, memsz, flags in phdrs:
        out += struct.pack('<IIIIIIII', 1, off, vaddr, vaddr,
                           filesz, memsz, flags, 0x1000)
    for pad, payload in blobs:
        out += pad + payload
    return bytes(out), segs


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else \
        game_path('default.xbe')
    dst = sys.argv[2] if len(sys.argv) > 2 else 'default.elf'

    xbe = Xbe(open(src, 'rb').read())
    print(f'title      : {xbe.title!r}  id=0x{xbe.title_id:08X}')
    print(f'base       : 0x{xbe.base:08X}   image size 0x{xbe.image_size:X}')
    print(f'entry      : 0x{xbe.entry:08X}  ({xbe.kind} key)')
    print(f'kernthunk  : 0x{xbe.kernel_thunk:08X}')
    print(f'sections   : {xbe.nsections}')

    elf, segs = build_elf(xbe)
    open(dst, 'wb').write(elf)

    print(f'\n{"name":<12} {"vaddr":<12} {"filesz":<10} {"memsz":<10} bss')
    for vaddr, filesz, memsz, flags, _, name in segs:
        bss = memsz - filesz
        print(f'{name:<12} 0x{vaddr:08X}   0x{filesz:<8X} 0x{memsz:<8X} '
              f'{("+0x%X" % bss) if bss else "-"}')
    print(f'\nwrote {dst} ({len(elf):,} bytes)')


if __name__ == '__main__':
    main()
