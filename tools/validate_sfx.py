#!/usr/bin/env python3
"""Differential test for the recovered crash/impact SOUND EVENT system.

Nothing here trusts the C table. Every assertion re-derives the answer from
the retail image or from executing the retail code, and then compares.

Sections
  1. NAME BINDING      Every event's base-40 wave name must be a name the
                       event's OWN emitter function references: the name is
                       decoded out of build/burnout3.elf at the address the
                       emitter's `MOV EAX, imm32` points at, and the byte
                       range of that immediate must lie inside the emitter.
  2. VARIANT RULE      0x001C99D0 masks the lookup key with `% 40*40`, so the
                       two trailing base-40 characters are (index, count).
                       Each event's file list must be exactly the <base><i><n>
                       family the mask implies, and every file must exist in
                       build/audio.
  3. PARAM TUPLES      The six {min impulse, max impulse, min/max gain,
                       min/max pitch} floats are read back out of a racecar
                       audio object built by EXECUTING the real initialiser
                       FUN_0014A710 under Unicorn, and must equal the C table
                       bit-for-bit (float32).
  4. RESPONSE CURVE    Each emitter is EXECUTED under Unicorn at a sweep of
                       impulses; the gain and playback rate it hands to
                       PlaySound3D (0x001CD8D0) must match what
                       b3_sfx_resolve() computes, including the silence gate
                       below min impulse.
  5. PITCH VARIANCE    FUN_0014A6B0 is 1 + U(-r,+r); with the real RAND_SCALE
                       seeded, every emulated pitch must sit inside the
                       +/-10% band around the C lerp, and the emitters that
                       do not call it must match exactly.
  6. CRASH STATE       The two laws that decide what a crash SOUNDS like:
                       FUN_0011BE50's crashed branch (gear -> neutral, then
                       FUN_00121560 with throttle/wheel/kick = 0, which is
                       what starves the engine of its rev) and FUN_0014D0F0's
                       impact router (crashed cars play the IMPACTFATA
                       emitter at impulse/mass instead of IMPACTWORL).  The
                       retail instructions are read back out of the image and
                       the module's engine law is run head-to-head against
                       the REAL ported FUN_00121560.

Run:  python3 tools/validate_sfx.py
"""
import importlib.util
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

ELF = os.path.join(ROOT, "build", "burnout3.elf")
AUDIO = os.path.join(ROOT, "build", "audio")
SRC = os.path.join(ROOT, "src", "burnout3_sfx.c")
DRIVER = os.path.join(ROOT, "build", "sfx_table")

CS = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"
IDX = {c: i for i, c in enumerate(CS)}

# ELF PT_LOAD map (VA = file offset + delta), from tools/xbe2elf.py's output.
SEGS = [(0x1000, 0x1BAC, 0xF000), (0x2000, 0x2BD200, 0xF000),
        (0x2BD200, 0x2E4F24, 0xF000), (0x2E4F40, 0x2F1A94, 0xF000),
        (0x2F1D00, 0x30BA64, 0xF000), (0x30BA80, 0x32A1DC, 0xF000),
        (0x32A1E0, 0x33D2C8, 0xF000), (0x33D2E0, 0x34E184, 0xF000),
        (0x34EA60, 0x350ACC, 0x12000), (0x350AE0, 0x3597B4, 0x12000),
        (0x3597C0, 0x3A0354, 0x12000), (0x3A0360, 0x4087AC, 0x12000)]

PASS = []
FAIL = []


def check(ok, label, detail=""):
    (PASS if ok else FAIL).append(label)
    print("  %s %s%s" % ("PASS" if ok else "FAIL", label,
                         ("   " + detail) if detail else ""))


def b40(v):
    out = []
    for _ in range(12):
        out.append(CS[v % 40])
        v //= 40
    return ''.join(reversed(out)).rstrip()


def enc(s):
    v = 0
    for c in s.ljust(12, ' '):
        v = v * 40 + IDX[c]
    return v


class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()

    def va2f(self, va):
        for s, e, dl in SEGS:
            if s + dl <= va < e + dl:
                return va - dl
        return None

    def u64(self, va):
        o = self.va2f(va)
        return struct.unpack_from('<Q', self.d, o)[0] if o is not None else None

    def find_u32_in_text(self, val):
        """Every .text byte offset whose little-endian dword equals `val`."""
        pat = struct.pack('<I', val)
        out, off = [], 0x2000
        while True:
            i = self.d.find(pat, off, 0x2BD200)
            if i < 0:
                break
            out.append(i + 0xF000)
            off = i + 1
        return out


# --------------------------------------------------------------------------
def load_table():
    """Compile the module's table driver and read its rows."""
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    r = subprocess.run(["gcc", "-Wall", "-Wextra", "-std=c11", "-O2",
                        "-I" + os.path.join(ROOT, "src"),
                        "-DB3_SFX_TEST_MAIN", "-o", DRIVER, SRC, "-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr)
        raise SystemExit("gcc failed building the sfx table driver")
    out = subprocess.run([DRIVER, "table"], capture_output=True,
                         text=True).stdout
    rows = []
    for line in out.splitlines():
        if not line.startswith("DEF "):
            continue
        f = line[4:].split('|')
        rows.append(dict(
            ev=int(f[0]), name=f[1], wave=f[2], emitter=f[3], dir=f[4],
            param_off=int(f[5]),
            min_imp=float(f[6]), max_imp=float(f[7]),
            min_gain=float(f[8]), max_gain=float(f[9]),
            min_pitch=float(f[10]), max_pitch=float(f[11]),
            gain_scale=float(f[12]), cooldown=int(f[13]),
            cooldown_rnd=int(f[14]), gated=int(f[15]), loop=int(f[16]),
            pitch_var=int(f[17]), conf="CS?"[int(f[18])],
            files=f[19:]))
    return rows


def resolve_c(ev, imp):
    out = subprocess.run([DRIVER, "resolve", str(ev), repr(float(imp))],
                         capture_output=True, text=True).stdout.split()
    return dict(play=int(out[0]), gain=float(out[1]), pitch=float(out[2]),
                t=float(out[3]), file=out[4])


# --------------------------------------------------------------------------
# The DRIVING-TIME loop rows bind their wave somewhere other than the emitter
# that plays it -- the surface beds through a 20-entry TABLE the emitter
# indexes, the skid samples through the ValueDB-backed sample records the
# loader fills, the gear shot through the audio object's one-time init.  Each
# row below names the exact site the name comes from, and section 1 proves
# THAT site instead of "an immediate inside the emitter".  All [C].
#
#   name -> (kind, name_va, site_va)
#     'imm'  the site holds a MOV/PUSH imm32 == name_va
#     'tab'  the site is `lea eax,[esi*8+0x39BBF8]`; name_va = table + id*8
#     'rec'  the name is record i of the 0x48-byte array the site's imm
#            (0x003EC108) starts, walked by `add esi,0x48` @0x0013DC67
DRIVE_BIND = {
    "surface/tar":    ('imm', 0x0039BC00, 0x00136B02),
    "surface/gravel": ('tab', 0x0039BBF8 + 5 * 8, 0x00136B0F),
    "surface/wood":   ('tab', 0x0039BBF8 + 6 * 8, 0x00136B0F),
    "surface/metal":  ('tab', 0x0039BBF8 + 10 * 8, 0x00136B0F),
    "surface/snow":   ('tab', 0x0039BBF8 + 11 * 8, 0x00136B0F),
    "skid/sample1":   ('imm', 0x003EC108, 0x0013DC4E),
    "skid/sample2":   ('rec', 0x003EC108 + 0x48, 0x0013DC4E),
    "skid/sample3":   ('rec', 0x003EC108 + 0x90, 0x0013DC4E),
    "gear change":    ('imm', 0x0039BD30, 0x00136059),
}
# a surface bed is only shipped in the track banks that USE that surface
TRACK_BANK_PREFIXES = ("awd_US_", "awd_EU_", "awd_AS_")


def section1_drive_names(img, rows):
    """The nine driving-loop rows, each proved at its real binding site."""
    for r in rows:
        b = DRIVE_BIND.get(r['name'])
        if not b:
            continue
        kind, name_va, site = b
        o = img.va2f(site)
        imm = name_va if kind != 'tab' else 0x0039BBF8
        if kind == 'rec':
            imm = 0x003EC108
        found = struct.unpack_from('<I', img.d, o + 1)[0]
        if found != imm:                      # LEA has a longer prefix
            found = struct.unpack_from('<I', img.d, o + 3)[0]
        check(found == imm,
              "%-24s %-11s bound at 0x%08X -> 0x%08X"
              % (r['name'], r['wave'], site, imm),
              "found 0x%08X" % found)
        got = b40(img.u64(name_va))
        check(got == r['wave'],
              "%-24s %-11s decodes back at 0x%08X" % (r['name'], r['wave'],
                                                      name_va), got)


def section1_names(img, rows):
    print("\n1. NAME BINDING -- each event's wave name is referenced by its "
          "own emitter")
    import json
    import urllib.request

    def owner(addr):
        try:
            u = ("http://127.0.0.1:8089/get_function_by_address"
                 "?address=0x%08X&program=burnout3.elf" % addr)
            return json.load(urllib.request.urlopen(u)).get('name', '')
        except Exception:
            return None

    for r in rows:
        if r['name'] in DRIVE_BIND:
            continue          # proved at its real binding site, see below
        m = re.match(r'FUN_([0-9A-Fa-f]{8})$', r['emitter'])
        if not m:
            print("  SKIP %-24s emitter %s (no owning function in the DB)"
                  % (r['name'], r['emitter']))
            continue
        fn = int(m.group(1), 16)

        # The base-40 alphabet has no lower case, so a mixed-case `wave` is
        # not a packed constant: it is an ASCII LITERAL that the emitter packs
        # at runtime with FUN_001AEAA0 before the same FUN_001C9E50 bank
        # lookup.  FUN_00136F80 loads all four racecar boost waves that way.
        # Same three-step proof, one level less indirection.
        if any(c.islower() for c in r['wave']):
            lit = r['wave'].encode('ascii') + b'\0'
            hits = []
            for s, e, dl in SEGS:
                off = s
                while True:
                    i = img.d.find(lit, off, e)
                    if i < 0:
                        break
                    # a literal starts on a boundary: preceded by NUL or pad
                    if i == 0 or img.d[i - 1] in (0, 0x20):
                        hits.append(i + dl)
                    off = i + 1
            if not hits:
                check(False, "%-24s %-11s ASCII literal present in image"
                      % (r['name'], r['wave']))
                continue
            sites = []
            for h in hits:
                sites += [(s, h) for s in img.find_u32_in_text(h)]
            owned = [(s, h) for s, h in sites if owner(s) == "FUN_%08x" % fn]
            check(bool(owned),
                  "%-24s %-11s ASCII literal loaded by %s"
                  % (r['name'], r['wave'], r['emitter']),
                  "site %08X -> string %08X" % owned[0] if owned
                  else "sites: " + " ".join("%08X" % s for s, _ in sites[:4]))
            if owned:
                _, sv = owned[0]
                o = img.va2f(sv)
                got = img.d[o:o + 32].split(b'\0')[0].decode('ascii', 'replace')
                check(got == r['wave'],
                      "%-24s %-11s reads back at %08X"
                      % (r['name'], r['wave'], sv), got)
            continue

        # find the data address holding this packed name, then the .text
        # immediate that points at it, then check the immediate is inside
        # the emitter.
        want = enc(r['wave'])
        hits = []
        for s, e, dl in SEGS:
            off = s
            pat = struct.pack('<Q', want)
            while True:
                i = img.d.find(pat, off, e)
                if i < 0:
                    break
                if (i + dl) % 4 == 0:
                    hits.append(i + dl)
                off = i + 1
        if not hits:
            check(False, "%-24s %-11s packed name present in image"
                  % (r['name'], r['wave']))
            continue
        sites = []
        for h in hits:
            sites += [(s, h) for s in img.find_u32_in_text(h)]
        owned = [(s, h) for s, h in sites if owner(s) == "FUN_%08x" % fn]
        check(bool(owned),
              "%-24s %-11s referenced by %s" % (r['name'], r['wave'],
                                                r['emitter']),
              "site %08X -> data %08X" % owned[0] if owned
              else "sites: " + " ".join("%08X" % s for s, _ in sites[:4]))
        if owned:
            _, data_va = owned[0]
            check(b40(img.u64(data_va)) == r['wave'],
                  "%-24s %-11s decodes back at %08X"
                  % (r['name'], r['wave'], data_va))


def section2_variants(rows):
    print("\n2. VARIANT RULE -- <base><index><count> family, all files present")
    for r in rows:
        base = r['wave'].lower()
        files = r['files']
        # infer (count) from the shipped names and require the full family
        ok_family = True
        detail = ""
        if '%' in r['wave']:
            # SPRINTF FAMILY.  A handful of emitters do not use the packed
            # base-40 name + <index><count> rule at all: they sprintf a
            # C format string and look the result up.  The traffic pass
            # (FUN_00146530, "bstpsl0%i" @0x003AE824 / "bstpss0%i"
            # @0x003AE830) picks its variant with rand()&0x80000003
            # sign-fixed to mod 4, +1 -- so the family is simply the
            # format expanded over 1..len(files).                    [C]
            for i, f in enumerate(files):
                want = (base % (i + 1)) + ".wav"
                if f != want:
                    ok_family = False
                    detail = "%s != %s" % (f, want)
                    break
            check(ok_family, "%-24s sprintf family (%d waves)"
                  % (r['name'], len(files)), detail or r['wave'])
            for f in files:
                p = os.path.join(AUDIO, r['dir'], f)
                check(os.path.exists(p), "%-24s file %s/%s"
                      % (r['name'], r['dir'], f))
            continue
        if len(files) == 1 and files[0] == base + ".wav":
            # a name that IS the whole wave -- the surface beds (TAR ->
            # tar.wav) and the three Sound/Skids samples (CONST01 ->
            # const01.wav), which are three distinct names, not a family.
            ok_family = True
            detail = files[0]
        elif len(files) == 1 and not re.search(r'\d\d\.wav$', files[0]):
            ok_family = files[0] == base + ".wav"
            detail = files[0]
        else:
            n = len(files)
            for i, f in enumerate(files):
                want = "%s%d%d.wav" % (base, i + 1, n)
                if f != want:
                    ok_family = False
                    detail = "%s != %s" % (f, want)
                    break
        check(ok_family, "%-24s variant family (%d waves)"
              % (r['name'], len(files)), detail)
        for f in files:
            if r['dir'].startswith(TRACK_BANK_PREFIXES):
                # a road-surface bed lives in the loaded TRACK bank, and a
                # track only ships the surfaces its level actually uses
                # (Silver Lake has no snow.wav) -- so the family is proved
                # against the whole shipped set of track banks.        [C]
                banks = sorted(d for d in os.listdir(AUDIO)
                               if d.startswith(TRACK_BANK_PREFIXES))
                have = [d for d in banks if os.path.exists(
                    os.path.join(AUDIO, d, f))]
                check(bool(have), "%-24s file %s in a track bank"
                      % (r['name'], f), "%d of %d banks" % (len(have),
                                                            len(banks)))
                continue
            p = os.path.join(AUDIO, r['dir'], f)
            check(os.path.exists(p), "%-24s file %s/%s"
                  % (r['name'], r['dir'], f))


def section3_params(rows, offs_by_addr):
    print("\n3. PARAM TUPLES -- vs the real initialiser FUN_0014A710 executed "
          "under Unicorn")
    spec = importlib.util.spec_from_file_location(
        "esp", os.path.join(ROOT, "tools", "emulate_sfx_params.py"))
    esp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(esp)
    blob, _events, _err = esp.run_init(0x0014A710)

    def f32(off):
        return struct.unpack_from('<f', blob, off)[0]

    for r in rows:
        if r['param_off'] < 0:
            continue
        offs = offs_by_addr.get(r['emitter'])
        if not offs:
            continue
        real = [f32(o) for o in offs]
        mine = [r['min_imp'], r['max_imp'], r['min_gain'], r['max_gain'],
                r['min_pitch'], r['max_pitch']]
        same = all(struct.pack('<f', a) == struct.pack('<f', b)
                   for a, b in zip(real, mine))
        check(same, "%-24s tuple @+0x%03X" % (r['name'], offs[0]),
              "real %s" % ["%.6g" % v for v in real] if not same else "")


def section45_curves(rows):
    print("\n4/5. RESPONSE CURVE -- vs the real emitters executed under "
          "Unicorn")
    spec = importlib.util.spec_from_file_location(
        "es", os.path.join(ROOT, "tools", "emulate_sfx.py"))
    es = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(es)

    by_addr = {r['emitter']: r for r in rows}
    sess = es.Sfx()
    # a second session with the real LCG scale, so the pitch variance is
    # actually random rather than pinned at its lower bound by the BSS zero
    rnd = es.Sfx(rand_scale=2.3283064365386963e-10)
    for addr, wave, label, kind, offs in es.EMITTERS:
        key = "FUN_%08X" % addr
        r = by_addr.get(key)
        if r is None or offs is None:
            continue
        lo, hi = r['min_imp'], r['max_imp']
        mags = [lo + (hi - lo) * f for f in (0.0, 0.25, 0.5, 0.75, 1.0)]
        gate_mag = lo * 0.5 if lo > 0 else -1.0
        ratios = []
        ok_all = True
        detail = ""
        for m in mags:
            caps, err = sess.fire(addr, kind, m)
            c = resolve_c(r['ev'], m)
            if not caps:
                ok_all = False
                detail = "real silent at %.6g%s" % (m, " " + (err or ""))
                break
            v = caps[0]
            if v['wave'] != r['wave']:
                ok_all = False
                detail = "wave %s != %s" % (v['wave'], r['wave'])
                break
            if abs(v['gain'] - c['gain']) > 1e-5:
                ok_all = False
                detail = "gain %.6f != %.6f at imp %.6g" % (v['gain'],
                                                            c['gain'], m)
                break
            if c['pitch'] <= 0:
                ok_all = False
                detail = "C pitch %.6g" % c['pitch']
                break
            ratios.append(v['pitch'] / c['pitch'])
        check(ok_all, "%-24s gain curve == emulated (%d points)"
              % (r['name'], len(mags)), detail)
        if ok_all:
            lo_r, hi_r = min(ratios), max(ratios)
            if r['pitch_var']:
                good = 0.899 <= lo_r and hi_r <= 1.101
                check(good, "%-24s pitch inside FUN_0014A6B0 +/-10%% band"
                      % r['name'], "ratio %.4f..%.4f" % (lo_r, hi_r))
            else:
                good = abs(lo_r - 1.0) < 1e-5 and abs(hi_r - 1.0) < 1e-5
                check(good, "%-24s pitch == lerp exactly (no variance call)"
                      % r['name'], "ratio %.6f..%.6f" % (lo_r, hi_r))
        if ok_all and r['pitch_var']:
            band = []
            for m in mags:
                caps, _ = rnd.fire(addr, kind, m)
                c = resolve_c(r['ev'], m)
                if caps and c['pitch'] > 0:
                    band.append(caps[0]['pitch'] / c['pitch'])
            good = bool(band) and min(band) >= 0.899 and max(band) <= 1.101
            check(good, "%-24s pitch variance random in band (real LCG)"
                  % r['name'],
                  "ratio %.4f..%.4f" % (min(band), max(band)) if band
                  else "no voices")
        if r['gated'] and gate_mag >= 0:
            caps, _ = sess.fire(addr, kind, gate_mag)
            c = resolve_c(r['ev'], gate_mag)
            check(not caps and c['play'] == 0,
                  "%-24s silent below min impulse (%.6g)"
                  % (r['name'], gate_mag),
                  "real=%d C=%d" % (len(caps), c['play']))


def section6_crash(img):
    """The crash-state laws: the starved engine, the impact router.

    6.1/6.2 read the retail instructions back out of the image byte for byte.
    6.3 runs the module's crash-engine law against the REAL ported
        FUN_00121560 driven exactly as FUN_0011BE50's crashed branch drives
        it (tools/crash_engine_drv.c).
    6.4 fires FUN_0014D0F0's router through the module itself.
    """
    print("\n6. CRASH STATE -- starved engine (FUN_0011BE50) + impact router "
          "(FUN_0014D0F0)")

    def code(va, blob, label):
        o = img.va2f(va)
        got = img.d[o:o + len(blob)] if o is not None else b""
        check(got == blob, label,
              "%08X: %s != %s" % (va, got.hex(' '), blob.hex(' '))
              if got != blob else "%08X %s" % (va, blob.hex(' ')))

    def f32(va):
        o = img.va2f(va)
        return struct.unpack_from('<f', img.d, o)[0] if o is not None else None

    # ---- 6.1 the crashed branch of the vehicle main path ------------------
    code(0x0011BE75, bytes.fromhex("8a 83 10 02 00 00 84 c0"),
         "crash gate      MOV AL,[EBX+0x210] / TEST AL,AL")
    code(0x0011BEA2, bytes.fromhex("f3 0f 10 05 b0 b2 39 00"),
         "shift timer     MOVSS XMM0,[0x0039B2B0]")
    check(abs(f32(0x0039B2B0) - 0.35) < 1e-6,
          "shift timer     0x0039B2B0 == 0.35", "%r" % f32(0x0039B2B0))
    code(0x0011BEAA, bytes.fromhex("c7 83 c8 14 00 00 00 00 00 00"),
         "gear -> neutral MOV [EBX+0x14C8],0")
    code(0x0011BEB4, bytes.fromhex("c7 83 a4 14 00 00 01 00 00 00"),
         "in-shift flag   MOV [EBX+0x14A4],1")
    code(0x0011BEBE, bytes.fromhex("f3 0f 11 83 a0 14 00 00"),
         "shift timer     MOVSS [EBX+0x14A0],XMM0")
    code(0x0011BEC6, bytes.fromhex("6a 00 6a 00 6a 00"),
         "engine args     PUSH 0 x3 (throttle, wheel omega, kick)")
    code(0x0011BECC, bytes.fromhex("8d b3 48 14 00 00 33 ff"),
         "engine this     LEA ESI,[EBX+0x1448] / XOR EDI,EDI (boost 0)")
    o = img.va2f(0x0011BED4)
    rel = struct.unpack_from('<i', img.d, o + 1)[0]
    check(img.d[o] == 0xE8 and 0x0011BED4 + 5 + rel == 0x00121560,
          "engine call     CALL FUN_00121560",
          "target %08X" % (0x0011BED4 + 5 + rel))

    # ---- 6.2 the world-impact router --------------------------------------
    code(0x0014D17B, bytes.fromhex("8a 86 10 02 00 00 84 c0"),
         "router gate     MOV AL,[ESI+0x210] / TEST AL,AL")
    code(0x0014D223, bytes.fromhex("f3 0f 5e 86 f0 01 00 00"),
         "router impulse  DIVSS XMM0,[ESI+0x1F0] (impulse / mass)")
    code(0x0014D248, bytes.fromhex("f3 0f 59 05 84 16 3b 00"),
         "crash-mode half MULSS XMM0,[0x003B1684]")
    check(abs(f32(0x003B1684) - 0.5) < 1e-9,
          "crash-mode half 0x003B1684 == 0.5", "%r" % f32(0x003B1684))
    for site, target, who in ((0x0014D23C, 0x0014F130, "crashed"),
                              (0x0014D25C, 0x0014F130, "crashed"),
                              (0x0014D293, 0x0014EEA0, "not crashed"),
                              (0x0014D2A7, 0x0014EEA0, "not crashed")):
        o = img.va2f(site)
        rel = struct.unpack_from('<i', img.d, o + 1)[0]
        check(img.d[o] == 0xE8 and site + 5 + rel == target,
              "router %-11s %08X -> FUN_%08X" % (who, site, target),
              "target %08X" % (site + 5 + rel))

    # ---- 6.3 the module's engine law vs the real FUN_00121560 -------------
    ref = os.path.join(ROOT, "build", "crash_engine_drv")
    r = subprocess.run(["gcc", "-Wall", "-Wextra", "-std=c11", "-O2",
                        "-I" + os.path.join(ROOT, "src"), "-o", ref,
                        os.path.join(ROOT, "tools", "crash_engine_drv.c"),
                        os.path.join(ROOT, "src", "burnout3_vehicle_sim.c"),
                        "-lm"], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr)
        check(False, "crash engine    reference driver builds")
        return
    for start, idle, mx in ((5800.0, 1000.0, 6800.0), (7200.0, 1200.0, 7600.0),
                            (3000.0, 900.0, 5200.0)):
        n = 90
        a = subprocess.run([ref, str(start), str(idle), str(mx), str(n)],
                           capture_output=True, text=True).stdout.split()
        b = subprocess.run([DRIVER, "crash", str(start), str(idle), str(mx),
                            str(n)], capture_output=True, text=True
                           ).stdout.split()
        ra = [float(x) for x in a[1::2]]
        rb = [float(x) for x in b[1::2]]
        # The coast-down is deterministic until the idle floor's U(0,16 rad/s)
        # overshoot takes over; that band is idle .. idle + 152.8 rpm, and the
        # two traces draw from different PRNG streams inside it (documented).
        jit = 16.0 * 9.549296
        pre = 0
        while pre < len(ra) and ra[pre] > idle + jit:
            pre += 1
        same = ra[:pre] == rb[:pre]
        check(same and pre > 5,
              "crash engine    coast-down == FUN_00121560 (%.0f->idle, "
              "%d frames)" % (start, pre),
              "%.0f -> %.0f rpm" % (ra[0], ra[pre - 1]) if same
              else "first diff at %d: %r vs %r"
              % (next(i for i in range(pre) if ra[i] != rb[i]),
                 ra[:pre], rb[:pre]))
        band = all(idle - 0.5 <= v <= idle + jit + 0.5 for v in rb[pre:])
        check(band, "crash engine    idle floor inside idle+U(0,16 rad/s)",
              "%.1f..%.1f" % (min(rb[pre:]), max(rb[pre:])) if rb[pre:] else "")
        # and the thing the user actually complained about
        check(rb[-1] <= idle + jit + 0.5,
              "crash engine    no rev left after %d frames (%.0f -> %.0f rpm)"
              % (n, start, rb[-1]))

    # ---- 6.4 the router, through the module --------------------------------
    env = dict(os.environ, B3_AUDIO_DIR=AUDIO)
    out = subprocess.run([DRIVER, "route", "6.0"], capture_output=True,
                         text=True, env=env).stdout
    seg = out.split("route1:")
    check(len(seg) == 2 and "impact/world" in seg[0].split("route0:")[-1]
          and "impact/fatal" not in seg[0],
          "router          not crashed -> IMPACTWORL (FUN_0014EEA0)",
          out.replace("\n", " | ")[:160])
    check(len(seg) == 2 and "impact/fatal(crashmode)" in seg[1],
          "router          crashed     -> IMPACTFATA (FUN_0014F130)",
          out.replace("\n", " | ")[:160])


# --------------------------------------------------------------------------
def section7_boost(img):
    """The racecar boost chain -- FUN_00136F80's four ASCII wave literals, the
    bank they are looked up in, and the volume group that tunes them.
    docs/RE_BOOSTFX.md section 3."""
    print("\n7. BOOST AUDIO -- FUN_00136F80's four waves, byte-read")

    def f32(va):
        o = img.va2f(va)
        return struct.unpack_from('<f', img.d, o)[0] if o is not None else None

    # 1. the four literals sit where the loader says they do
    for va, want, slot in ((0x003AD404, "BoostLoop", 0xA4),
                           (0x003AD3FC, "fire",      0xA8),
                           (0x003AD3F4, "BoostIn",   0xAC),
                           (0x003AD3E8, "BoostOut",  0xB0)):
        o = img.va2f(va)
        got = img.d[o:o + 32].split(b'\0')[0].decode('ascii', 'replace')
        check(got == want, "boost wave      \"%s\" @0x%08X -> obj+0x%02X"
              % (want, va, slot), got)

    # 2. the loader's five instruction pairs: MOV EAX,<literal> then
    #    CALL FUN_001AEAA0 (ASCII -> base-40), and MOV <slot>,EAX after the
    #    FUN_001C9E50 bank lookup.
    for site, va, slot in ((0x00136F8F, 0x003AD404, 0xA4),
                           (0x00136FBF, 0x003AD3FC, 0xA8),
                           (0x00136FE7, 0x003AD3F4, 0xAC),
                           (0x0013700F, 0x003AD3E8, 0xB0)):
        o = img.va2f(site)
        enc_ok = (img.d[o] == 0xB8
                  and struct.unpack_from('<I', img.d, o + 1)[0] == va)
        check(enc_ok, "boost loader    MOV EAX,0x%08X @0x%08X" % (va, site))
        # the CALL follows within a few bytes (the first site has a MOVSS
        # scheduled between the MOV and the CALL) -- find it and check it.
        base = img.va2f(site)
        cva = None
        for k in range(5, 24):
            if img.d[base + k] == 0xE8:
                t = site + k + 5 + struct.unpack_from('<i', img.d, base + k + 1)[0]
                if t == 0x001AEAA0:
                    cva = site + k
                    break
        check(cva is not None,
              "boost loader    CALL FUN_001AEAA0 (ASCII pack) after 0x%08X"
              % site, "" if cva is None else "@0x%08X" % cva)
        o2 = img.va2f(cva if cva is not None else site)
        # ... MOV [ESI+slot],EAX  (89 86 <disp32>)
        win = img.d[o2:o2 + 0x40]
        pat = b'\x89\x86' + struct.pack('<I', slot)
        check(pat in win, "boost loader    MOV [ESI+0x%02X],EAX (the record)"
              % slot)

    # 3. the bank pointer every one of them uses
    for site in (0x00136FAB, 0x00136FD3, 0x00136FFB, 0x00137023):
        o = img.va2f(site)
        check(img.d[o] == 0xB8
              and struct.unpack_from('<I', img.d, o + 1)[0] == 0x0040B7F4,
              "boost loader    bank 0x0040B7F4 @0x%08X" % site)

    # 4. the mode singleton's second path: FUN_00141D20's packed BOOSTLOOP and
    #    FIRE, both at gain 1.0 (0x003B168C) and both looping (|= 0x10)
    for site, name_va, stem in ((0x00141D8A, 0x0039C338, "BOOSTLOOP"),
                                (0x00141EB8, 0x0039C340, "FIRE")):
        o = img.va2f(site)
        check(img.d[o] == 0xBB
              and struct.unpack_from('<I', img.d, o + 1)[0] == name_va,
              "boost mode      MOV EBX,0x%08X @0x%08X" % (name_va, site))
        check(b40(img.u64(name_va)) == stem,
              "boost mode      0x%08X decodes to %s" % (name_va, stem),
              b40(img.u64(name_va)))
    check(abs(f32(0x003B168C) - 1.0) < 1e-9,
          "boost mode      loop gain 1.0 @0x003B168C")
    for site in (0x00141E4D, 0x00141F7B):
        o = img.va2f(site)
        check(img.d[o:o + 4] == b'\x80\x48\x37\x10',
              "boost mode      OR [EAX+0x37],0x10 (LOOP flag) @0x%08X" % site)

    # 5. the "Sound/Boost" ValueDB group -- present, and NOT overridden by the
    #    shipped vdb.xml, which is why the module runs the compiled-in level.
    for va, want in ((0x003AD4C0, "Sound/Boost"),
                     (0x003AD450, "Boost Loop Volume"),
                     (0x003AD48C, "Boost In Volume"),
                     (0x003AD478, "Boost Out Volume"),
                     (0x003AE778, "../export/ValueDB/Sound/Boost.cfg")):
        o = img.va2f(va)
        got = img.d[o:o + 64].split(b'\0')[0].decode('ascii', 'replace')
        check(got == want, "boost volumes   \"%s\" @0x%08X" % (want, va), got)

    # 6. and the module's own state machine, exercised through the driver
    env = dict(os.environ, B3_AUDIO_DIR=AUDIO)
    out = subprocess.run([DRIVER, "boost", "0110001x1000"],
                         capture_output=True, text=True, env=env).stdout
    check("in" in out and "loop" in out and "out" in out,
          "boost chain     In -> Loop -> Out over a boost/release",
          out.replace("\n", " | ")[:200])
    check("crashcut" in out,
          "boost chain     crash cuts the loop with no release",
          out.replace("\n", " | ")[:200])


# ==========================================================================
# 8. THE DRIVING-TIME LOOP EMITTERS
#    FUN_00136610 (road-surface beds), FUN_0013DE10 (tyre skid/squeal) and
#    FUN_00136C50 (gear).  Nothing here trusts the C tables: the id-keyed
#    tables come back out of the image or out of the EXECUTED registrar
#    FUN_00137F50, the emitters' inline constants are read instruction by
#    instruction, and FUN_0013DE10 itself is run under Unicorn head to head
#    with b3_sfx_skid_sample().
# ==========================================================================
SURF_ROWS = ["None", "Tarmac", "Concrete", "Offline Tarmac", "Gravel",
             "Pavement", "Snow", "Offline Gravel", "Metal", "Wood"]
# 0x003EC1xx, the Sound/Skids block: {offset from the sample record, key}
SKID_KEYS = [(0x08, "Slip Volume"), (0x18, "Slip Frequency"),
             (0x28, "Spin Volume"), (0x38, "Spin Frequency")]


def drive_table(driver):
    out = subprocess.run([driver, "surftab"], capture_output=True,
                         text=True).stdout
    t = {'id': {}, 'row': {}, 'w': {}, 'curve': {}}
    for line in out.splitlines():
        f = line.split()
        if f[0] == "SURFID":
            t['id'][int(f[1])] = (int(f[2]), int(f[3]), int(f[4]))
        elif f[0] == "SURFROW":
            t['row'][(int(f[1]), int(f[2]))] = [float(x) for x in f[3:9]]
        elif f[0] == "SKIDW":
            t['w'][int(f[1])] = float(f[2])
        elif f[0] == "SKIDCURVE":
            t['curve'][(int(f[1]), int(f[2]))] = [float(x) for x in f[3:7]]
    return t


def jump_index_table(img, jmp_va, idx_va, n):
    """`jmp [ecx*4+jmp_va]` with `movzx ecx,[eax+idx_va]` -- return the
    per-index TARGET address, straight out of the image."""
    tgt = []
    o = img.va2f(idx_va)
    for i in range(n):
        sel = img.d[o + i]
        j = img.va2f(jmp_va + sel * 4)
        tgt.append(struct.unpack_from('<I', img.d, j)[0])
    return tgt


def section8_drive(img, rows):
    print("\n8. DRIVING LOOPS -- FUN_00136610 / FUN_0013DE10 / FUN_00136C50")
    t = drive_table(DRIVER)

    def f32(va):
        return struct.unpack_from('<f', img.d, img.va2f(va))[0]

    # ---- 8.1 the surface -> WAVE table 0x0039BBF8, decoded from the image
    waves = {"TAR": 0, "GVL": 1, "WOOD": 2, "METAL": 3, "SNOW": 4}
    base = int(rows[0]['ev'])  # unused, keeps flake quiet
    del base
    surf_tar_ev = next(r['ev'] for r in rows if r['name'] == "surface/tar")
    bad = []
    for i in range(40):
        nm = b40(img.u64(0x0039BBF8 + i * 8))
        want = (surf_tar_ev + waves[nm]) if nm in waves else len(rows)
        got = t['id'][i][0]
        if got != want:
            bad.append("%d:%s->%d!=%d" % (i, nm or '-', got, want))
    check(not bad, "surface table   0x0039BBF8[0..39] decodes to the module's "
          "wave choice", " ".join(bad[:6]))

    # ---- 8.2 the surface -> CURVE ROW table and the ten rows, from the
    #      EXECUTED registrar FUN_00137F50 (EDI = 0x0040E130) + the retail
    #      ValueDB, i.e. the values the shipped game runs with.
    spec = importlib.util.spec_from_file_location(
        "esp8", os.path.join(ROOT, "tools", "emulate_sfx_params.py"))
    esp8 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(esp8)
    uc, events, err = run_registrar(esp8, 0x00137F50, 0x0040E130)
    check(err is None, "surface init    FUN_00137F50 executes clean", err or "")
    vdb = esp8.load_vdb()

    rowmap = bytes(uc.mem_read(0x0040E318, 40))
    bad = [("%d:%d!=%d" % (i, t['id'][i][1], rowmap[i]))
           for i in range(40) if t['id'][i][1] != rowmap[i]]
    check(not bad, "surface table   0x0040E318[0..39] row map matches "
          "the module", " ".join(bad[:6]))

    # every registered Sound/Surface key, its retail vdb value, against the
    # module's row.  Key text is "<param><group><cfg>" (the registrar hashes
    # the three back to back), so the group name identifies the row.
    FIELD = {"Min Speed": 0, "Max Speed": 1, "Min Vol": 2, "Max Vol": 3,
             "Min Pitch": 4, "Max Pitch": 5}
    seen = 0
    bad = []
    for e in events:
        s = (e['string'] or b'').decode('latin1')
        if "/Sound/Surface.cfg" not in s:
            continue
        head = s.split("/../export")[0]
        setno = 1 if not head.startswith("Tarmac ") else 0
        if setno == 0:
            head = head[len("Tarmac "):]
        fld = next((k for k in FIELD if head.startswith(k)), None)
        if fld is None:
            continue
        grp = head[len(fld):]
        if grp not in SURF_ROWS:
            continue
        want = vdb.get(e['hash'])
        if want is None:
            continue
        mine = t['row'][(setno, SURF_ROWS.index(grp))][FIELD[fld]]
        seen += 1
        if struct.pack('<f', want) != struct.pack('<f', mine):
            bad.append("%s/%s set%d %.4g!=%.4g" % (fld, grp, setno, want, mine))
    check(seen >= 96 and not bad,
          "surface curves  %d ValueDB Sound/Surface keys match the module's "
          "two 10-row sets" % seen, " ".join(bad[:6]))

    # ---- 8.3 the slip-mute jump table @0x00136C2C/0x00136C34
    tgt = jump_index_table(img, 0x00136C2C, 0x00136C34, 17)
    MUTE, ACTIVE = 0x001368FC, 0x00136831
    bad = []
    for i, a in enumerate(tgt):
        want = 1 if a == ACTIVE else 0
        if a not in (MUTE, ACTIVE):
            bad.append("id%d target %08X" % (i + 1, a))
        elif t['id'][i + 1][2] != want:
            bad.append("id%d %d!=%d" % (i + 1, t['id'][i + 1][2], want))
    check(not bad, "surface slip    the 17-entry mute table @0x00136C2C "
          "matches b3_sfx_surface_slip_active", " ".join(bad[:6]))
    for i in (0, 18, 19, 20, 25, 39):
        check(t['id'][i][2] == 1,
              "surface slip    id %d takes the default (active) branch" % i)

    # ---- 8.4 FUN_00136610's inline constants
    for va, want, label in ((0x003B16B0, 8.0, "|slip| clamp"),
                            (0x003B1728, 0.125, "slip scale"),
                            (0x003B1690, 4.0, "gain k"),
                            (0x003B1BE4, -0.15, "pitch k"),
                            (0x003B1A74, 0.24, "slot 0/3 detune"),
                            (0x003B1BE8, -0.4, "lag min"),
                            (0x00384A80, 0.15, "lag max"),
                            (0x003EC074, 5.5, "emitter offset")):
        check(abs(f32(va) - want) < 1e-6,
              "surface const   %-16s %g @0x%08X" % (label, want, va),
              "%g" % f32(va))

    # ---- 8.5 FUN_0013DE10's surface-weight jump table @0x0013E4D0
    tgt = jump_index_table(img, 0x0013E4D0, 0x0013E4DC, 20)
    W = {0x0013DF4B: 1.0, 0x0013DF55: 0.5, 0x0013DF5F: 0.0}
    bad = []
    for i, a in enumerate(tgt):
        if a not in W:
            bad.append("id%d target %08X" % (i + 1, a))
        elif t['w'][i + 1] != W[a]:
            bad.append("id%d %g!=%g" % (i + 1, t['w'][i + 1], W[a]))
    check(not bad, "skid weights    the 20-entry table @0x0013E4D0 matches "
          "b3_sfx_skid_surface_weight", " ".join(bad[:6]))
    check(t['w'][0] == 0.0 and t['w'][21] == 0.0 and t['w'][38] == 0.0,
          "skid weights    ids outside 1..20 take the 0.0 default")

    # ---- 8.6 the three Sound/Skids sample records: names, and the four
    #      two-point curves each, against the retail ValueDB.
    for i in range(3):
        rec = 0x003EC108 + i * 0x48
        check(b40(img.u64(rec)) == "CONST%02d" % (i + 1),
              "skid sample %d   name at 0x%08X" % (i + 1, rec),
              b40(img.u64(rec)))
    seen, bad = 0, 0
    for e in events:
        s = (e['string'] or b'').decode('latin1')
        if "/Sound/Skids.cfg" not in s or "Sample" not in s:
            continue
        m = re.match(r"Sample (\d) (Slip|Spin) (Volume|Frequency) "
                     r"(Input|Output) Point\s+(\d)", s)
        if not m:
            continue
        want = vdb.get(e['hash'])
        if want is None:
            continue
        sm = int(m.group(1)) - 1
        ci = {("Slip", "Volume"): 0, ("Slip", "Frequency"): 1,
              ("Spin", "Volume"): 2, ("Spin", "Frequency"): 3}[
                  (m.group(2), m.group(3))]
        pi = int(m.group(5)) * 2 + (0 if m.group(4) == "Input" else 1)
        mine = t['curve'][(sm, ci)][pi]
        seen += 1
        if struct.pack('<f', want) != struct.pack('<f', mine):
            bad += 1
            print("      skid curve mismatch %s: %.6g != %.6g" % (s[:44],
                                                                  want, mine))
    check(seen == 48 and bad == 0,
          "skid curves     %d/48 ValueDB Sound/Skids curve points match the "
          "module" % seen)

    # ---- 8.7 the Sound/Skids scalars
    SCAL = [("Maximum Slip", 16.0), ("Maximum Spin", 16.0),
            ("SlipSpin Lag Min", -0.35), ("SlipSpin Lag Max", 0.75),
            ("Slip Speed Multipler", 0.025), ("Crash Volume Multipler", 0.7),
            ("Left Pitch Offset", 1000.0), ("Right Pitch Offset", -1000.0),
            ("Emitter Distance AT", 1.0), ("Emitter Distance RIGHT", 3.0)]
    for key, want in SCAL:
        hit = [vdb.get(e['hash']) for e in events
               if (e['string'] or b'').decode('latin1').startswith(key)
               and "/Sound/Skids.cfg" in (e['string'] or b'').decode('latin1')]
        hit = [h for h in hit if h is not None]
        check(bool(hit) and abs(hit[0] - want) < 1e-6,
              "skid scalar     %-24s = %g" % (key, want),
              "" if hit else "no ValueDB entry")
    # and the addresses the emitter actually reads them from
    for va, want, label in ((0x003EC218, 0.025, "Slip Speed Mul (default)"),
                            (0x003EC210, 1000.0, "Left Pitch Offset"),
                            (0x003EC214, -1000.0, "Right Pitch Offset"),
                            (0x003EC208, 1.0, "Emitter Distance AT"),
                            (0x003EC20C, 3.0, "Emitter Distance RIGHT"),
                            (0x00384208, 1.52587890625e-05, "gain epsilon"),
                            (0x003B1734, 0.15915494, "1/2pi"),
                            (0x003B1738, 6.2831855, "2pi")):
        check(abs(f32(va) - want) < max(1e-6, abs(want) * 1e-6),
              "skid const      %-24s %g @0x%08X" % (label, want, va),
              "%g" % f32(va))

    # ---- 8.8 THE DIFFERENTIAL: execute FUN_0013DE10 and compare
    section8_skid_diff(t)

    # ---- 8.9 FUN_00136C50's upshift rule, read out of the image
    o = img.va2f(0x00136C50)
    check(img.d[o:o + 6] == b'\x8b\x83\x48\x01\x00\x00',
          "gear change     MOV EAX,[EBX+0x148] @0x00136C50")
    out = subprocess.run([DRIVER, "surface", "2", "1", "1", "22", "0"],
                         capture_output=True, text=True).stdout.split()
    check(out[0] == "1" and abs(float(out[1]) - 0.1) < 1e-6,
          "surface law     tarmac at 22 m/s -> gain 0.1 (0.2 * 22/44)",
          " ".join(out))
    out = subprocess.run([DRIVER, "surface", "2", "5", "1", "22", "8"],
                         capture_output=True, text=True).stdout.split()
    check(out[0] == "1" and abs(float(out[1]) - 1.0) < 1e-5,
          "surface law     gravel at 22 m/s, full slip -> 5x the bed "
          "(0.2 * 5)", " ".join(out))
    out = subprocess.run([DRIVER, "surface", "2", "1", "1", "22", "8"],
                         capture_output=True, text=True).stdout.split()
    check(out[0] == "1" and abs(float(out[1]) - 0.1) < 1e-6,
          "surface law     tarmac ignores slip (the mute table)",
          " ".join(out))


def run_registrar(esp8, func, edi):
    """Execute an audio-node registrar with EDI pointing at its table block,
    hooking the ValueDB registrar + hash core the same way
    tools/emulate_sfx_params.py does."""
    from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                         UC_HOOK_CODE, UC_PROT_ALL, UcError)
    from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP,
                                   UC_X86_REG_EAX, UC_X86_REG_ECX,
                                   UC_X86_REG_EDX, UC_X86_REG_EDI)
    PAGE = 0x1000
    MGR, RECORDS, VTAB, STUBS = (0x33000000, 0x34000000, 0x35000000,
                                 0x36000000)
    STACK, STACK_SZ, MAGIC = 0x20000000, 0x100000, 0x50000000
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    esp8.ev.load_elf(uc, ELF)
    for b, n in ((MGR, PAGE), (RECORDS, 0x40000), (VTAB, PAGE), (STUBS, PAGE),
                 (STACK, STACK_SZ), (MAGIC & ~(PAGE - 1), PAGE)):
        uc.mem_map(b, n, UC_PROT_ALL)
    uc.mem_write(0x004A1E94, struct.pack('<I', MGR))
    uc.mem_write(MGR + 0x10, struct.pack('<IIIII', VTAB, 0, 0, RECORDS,
                                         0x2000))
    uc.mem_write(STUBS, b"\x31\xC0\xC2\x04\x00")
    uc.mem_write(STUBS + 0x10, b"\xB0\x01\xC2\x04\x00")
    uc.mem_write(VTAB, struct.pack('<IIII', STUBS, STUBS, STUBS, STUBS + 0x10))
    events, pending = [], {'string': None}

    def on_unmapped(mu, access, address, size, value, user):
        try:
            mu.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    def on_hash(mu, address, size, user):
        try:
            pending['string'] = bytes(mu.mem_read(mu.reg_read(UC_X86_REG_ECX),
                                                  mu.reg_read(UC_X86_REG_EDX)))
        except UcError:
            pending['string'] = None

    def on_hret(mu, address, size, user):
        events.append({'string': pending['string'],
                       'hash': mu.reg_read(UC_X86_REG_EAX)})
        pending['string'] = None

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_CODE, on_hash, begin=0x001AF250, end=0x001AF250)
    uc.hook_add(UC_HOOK_CODE, on_hret, begin=0x001AF27F, end=0x001AF27F)
    sp = STACK + STACK_SZ - 0x8000
    uc.mem_write(sp, struct.pack('<I', MAGIC))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EDI, edi)
    err = None
    try:
        uc.emu_start(func, MAGIC, count=80_000_000)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    return uc, events, err


def section8_skid_diff(t):
    """Execute FUN_0013DE10 over a sweep of slip/spin values and compare the
    gain + playback rate it hands to PlaySound3D against b3_sfx_skid_sample.

    The emulator's Sound/Skids block is first seeded with the retail ValueDB
    values (the same ones section 8.6 proved), because that is what the
    shipped game's registrar leaves there."""
    import struct as _s
    spec = importlib.util.spec_from_file_location(
        "es8", os.path.join(ROOT, "tools", "emulate_sfx.py"))
    es8 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(es8)
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX,
                                   UC_X86_REG_EDX, UC_X86_REG_EDI,
                                   UC_X86_REG_ESP)

    SKID = 0x0013DE10
    sfx = es8.Sfx()
    uc = sfx.uc

    # The skid emitter does NOT go through PlaySound3D: its six voices are
    # pre-allocated by FUN_0013DBC0 out of the 0x0040B848 pool, and each
    # frame it either RESTARTS one (FUN_001CC910, block = stack arg 1,
    # @0x0013E47E) or UPDATES it in place (FUN_001CC3E0, block = EDI,
    # @0x0013E48F).  Both carry the same params block, so both are hooked.
    def _grab(mu, block, pop):
        blk = bytes(mu.mem_read(block, 0x30))
        sfx.captured.append({
            'wave': None,
            'gain': _s.unpack_from('<f', blk, 0x1C)[0],
            'freq': _s.unpack_from('<f', blk, 0x24)[0],
            'pos': _s.unpack_from('<3f', blk, 0)})
        mu.reg_write(UC_X86_REG_EAX, es8.VOICE)
        sfx._ret(mu, pop)

    def _on_start(mu, address, size, user):
        esp = mu.reg_read(UC_X86_REG_ESP)
        _grab(mu, _s.unpack('<I', mu.mem_read(esp + 8, 4))[0], 8)

    def _on_update(mu, address, size, user):
        _grab(mu, mu.reg_read(UC_X86_REG_EDI), 0)

    uc.hook_add(UC_HOOK_CODE, _on_start, begin=0x001CC910, end=0x001CC910)
    uc.hook_add(UC_HOOK_CODE, _on_update, begin=0x001CC3E0, end=0x001CC3E0)
    OBJ = es8.OBJ
    SKIDOBJ, WH0, WH1, PTRS = OBJ + 0x2000, OBJ + 0x3000, OBJ + 0x3200, \
        OBJ + 0x3400

    # the three sample records, seeded with the SHIPPED (ValueDB) curves
    for i in range(3):
        rec = 0x003EC108 + i * 0x48
        for ci in range(4):
            uc.mem_write(rec + 8 + ci * 16,
                         _s.pack('<4f', *t['curve'][(i, ci)]))
    # and the shipped scalars
    for va, v in ((0x003EC1E0, -0.35), (0x003EC1E4, 0.75), (0x003EC1E8, 0.7),
                  (0x003EC218, 0.025), (0x003EC21C, 16.0), (0x003EC220, 16.0),
                  (0x003EC210, 1000.0), (0x003EC214, -1000.0)):
        uc.mem_write(va, _s.pack('<f', v))
    for m in range(8):                       # every wheel-mode multiplier
        uc.mem_write(0x003EC1EC + m * 4, _s.pack('<f', 1.0))

    uc.mem_write(SKIDOBJ, _s.pack('<III', es8.REC, es8.REC, es8.REC))
    uc.mem_write(PTRS, _s.pack('<II', WH0, WH1))
    # the voice-start branch @0x0013E425 copies seven dwords out of the wave
    # descriptor and picks a random start offset from
    #   (desc+0x18 * 8 / desc+0x1C) with the object's own LCG at +0x50/+0x54
    # (seeded by FUN_0013B480 @0x0013B4A9/@0x0013B4B0) -- give it a real
    # 16-bit descriptor so that path does not divide by zero.
    uc.mem_write(es8.DESC + 0x18, _s.pack('<I', 48000))
    uc.mem_write(es8.DESC + 0x1C, _s.pack('<I', 16))
    uc.mem_write(SKIDOBJ + 0x50, _s.pack('<II', 0xFD462907, 0x02B9D6F8))

    bad, n = [], 0
    for speed, omega in ((40.0, 0.0), (40.0, 60.0), (40.0, 130.0),
                         (20.0, 0.0), (20.0, 55.0), (60.0, 0.0),
                         (5.0, 0.0), (30.0, 200.0)):
        for left in (0, 1):
            # both wheels identical, on tarmac (weight 1.0), radius 0.34
            for w in (WH0, WH1):
                uc.mem_write(w, b'\0' * 0x100)
                uc.mem_write(w + 0x50, _s.pack('<f', 0.34))
                uc.mem_write(w + 0x5C, _s.pack('<f', omega))
                uc.mem_write(w + 0x78, _s.pack('<I', 1))
                uc.mem_write(w + 0xB0, b'\x01')
                uc.mem_write(w + 0xB3, b'\x01')
            uc.mem_write(SKIDOBJ + 0x0C, b'\0' * 0x40)   # voices + variants
            uc.mem_write(SKIDOBJ + 0x0C,
                         _s.pack('<III', es8.VOICE, es8.VOICE, es8.VOICE))
            uc.mem_write(SKIDOBJ + 0x24, _s.pack('<ff', 0.0, 0.0))
            sfx.captured = []
            err = sfx._run(SKID, {UC_X86_REG_EAX: PTRS, UC_X86_REG_EDX: 0,
                                  UC_X86_REG_ECX: 0},
                           stack=(SKIDOBJ,
                                  _s.unpack('<I', _s.pack('<f', speed))[0],
                                  SKIDOBJ + 0x0C, es8.POS, left,
                                  SKIDOBJ + 0x24, 0, 0))
            if err:
                bad.append("speed %g omega %g: %s" % (speed, omega, err))
                continue
            # the module, driven with the same two wheels
            acc = 0.0
            out = subprocess.run(
                [DRIVER, "skidwheel", repr(speed), repr(omega), "0.34",
                 "1", "1", "0"], capture_output=True, text=True).stdout
            acc = float(out.split()[0])
            total = 2.0 * acc                 # both wheels, weight 1.0
            total = max(-16.0, min(16.0, total))
            # the module is driven sample-by-sample with the SAME running
            # total, so the retail spin-branch rewrite (@0x0013E20B) is
            # exercised too.
            want = []
            for s in range(3):
                o = subprocess.run(
                    [DRIVER, "skid", str(s), repr(total), str(left), "0",
                     repr(es8.RATE)], capture_output=True, text=True
                ).stdout.split()
                total = float(o[3])
                if int(o[0]):
                    want.append((float(o[1]), float(o[2])))
            got = [(c['gain'], c['freq']) for c in sfx.captured]
            n += 1
            if len(got) != len(want) or any(
                    abs(a[0] - b[0]) > 1e-4 or abs(a[1] - b[1]) > 0.05
                    for a, b in zip(sorted(got), sorted(want))):
                bad.append("speed %g omega %g L%d: real %s vs module %s"
                           % (speed, omega, left,
                              ["%.4f@%.0f" % g for g in got],
                              ["%.4f@%.0f" % g for g in want]))
    check(not bad, "skid emitter    FUN_0013DE10 executed at %d slip/spin "
          "points == b3_sfx_skid_sample" % n, " | ".join(bad[:3]))


# ==========================================================================
# 9. THE TRAFFIC-PASS WHOOSH's MASTER VOLUME
#    The user report was "I hear it, but it's quiet".  The emitter takes
#    max(masterVol, 0.09) * 2.0 and masterVol is the traffic manager's own
#    +0xD8, which FUN_00145F60 ramps to 1.0 -- so the factor is 2.0, not the
#    0.18 floor.  Both halves are read back out of the image here.
# ==========================================================================
def section9_pass_master(img):
    print("\n9. TRAFFIC-PASS WHOOSH -- the master volume and the per-frame "
          "re-position")

    def f32(va):
        return struct.unpack_from('<f', img.d, img.va2f(va))[0]

    o = img.va2f(0x0014654C)
    check(img.d[o:o + 8] == b'\xf3\x0f\x10\x0d\x38\x1d\x3b\x00',
          "whoosh master   MOVSS XMM1,[0x003B1D38] @0x0014654C")
    check(img.d[img.va2f(0x00146559):img.va2f(0x00146559) + 5]
          == b'\x0f\x2f\xc8\x76\x03',
          "whoosh master   COMISS XMM1,XMM0 / JBE  (= max) @0x00146559")
    check(img.d[img.va2f(0x00146566):img.va2f(0x00146566) + 8]
          == b'\xf3\x0f\x59\x05\x88\x16\x3b\x00',
          "whoosh master   MULSS XMM0,[0x003B1688] @0x00146566")
    check(abs(f32(0x003B1D38) - 0.09) < 1e-7,
          "whoosh master   floor 0.09 @0x003B1D38", "%g" % f32(0x003B1D38))
    check(abs(f32(0x003B1688) - 2.0) < 1e-7,
          "whoosh master   scale 2.0 @0x003B1688", "%g" % f32(0x003B1688))
    # the final gain multiply by that master, @0x001469BE
    check(img.d[img.va2f(0x001469BE):img.va2f(0x001469BE) + 5]
          == b'\xf3\x0f\x59\x45\x0c',
          "whoosh master   MULSS XMM0,[EBP+0xC] (the master) @0x001469BE")
    # the ramp in the traffic-audio manager
    check(img.d[img.va2f(0x00146013):img.va2f(0x00146013) + 8]
          == b'\xf3\x0f\x58\x05\xd8\x7e\x3a\x00',
          "whoosh master   ADDSS XMM0,[0x003A7ED8] (ramp up) @0x00146013")
    check(img.d[img.va2f(0x0014602D):img.va2f(0x0014602D) + 8]
          == b'\xf3\x0f\x5c\x05\xc4\x69\x3a\x00',
          "whoosh master   SUBSS XMM0,[0x003A69C4] (ramp down) @0x0014602D")
    check(abs(f32(0x003A7ED8) - 0.01) < 1e-7,
          "whoosh master   ramp up 0.01 @0x003A7ED8", "%g" % f32(0x003A7ED8))
    check(abs(f32(0x003A69C4) - 0.1) < 1e-7,
          "whoosh master   ramp down 0.1 @0x003A69C4", "%g" % f32(0x003A69C4))
    check(img.d[img.va2f(0x00146008):img.va2f(0x00146008) + 4]
          == b'\x0f\x2f\x4d\x00',
          "whoosh master   the ramp CEILING is 1.0 (COMISS against "
          "[0x003B168C]) @0x00146008")
    # and the module's own ramp reaches that ceiling
    out = subprocess.run([DRIVER, "passmaster", "500"],
                         capture_output=True, text=True).stdout.strip()
    check(abs(float(out) - 1.0) < 1e-6,
          "whoosh master   b3_sfx_pass_master() ramps to 1.0", out)
    out = subprocess.run([DRIVER, "passmaster", "1"],
                         capture_output=True, text=True).stdout.strip()
    check(abs(float(out) - 0.01) < 1e-6,
          "whoosh master   ... at +0.01 per tick", out)
    # the per-frame re-position retail does and the port now does
    check(img.d[img.va2f(0x0014663C)] == 0xE8
          and (0x0014663C + 5 + struct.unpack_from(
              '<i', img.d, img.va2f(0x0014663C) + 1)[0]) == 0x001CC3E0,
          "whoosh voice    CALL FUN_001CC3E0 (re-position) @0x0014663C")
    check(img.d[img.va2f(0x001CD1A8):img.va2f(0x001CD1A8) + 13]
          == b'\xf3\x0f\x10\x05\xb8\x16\x3b\x00'
             b'\xf3\x0f\x11\x40\x38',
          "whoosh voice    the block's max distance is [0x003B16B8] -> +0x38 "
          "(FUN_001CD180 @0x001CD1A8)")
    check(abs(f32(0x003B16B8) - 50.0) < 1e-7 and abs(f32(0x003B16B4) - 15.0)
          < 1e-7,
          "whoosh voice    ... = the same 50/15 roll-off every emitter uses")


def main():
    if not os.path.exists(ELF):
        raise SystemExit("build/burnout3.elf missing (tools/xbe2elf.py)")
    img = Image(ELF)
    rows = load_table()
    print("sfx event table: %d events" % len(rows))

    spec = importlib.util.spec_from_file_location(
        "es0", os.path.join(ROOT, "tools", "emulate_sfx.py"))
    es0 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(es0)
    offs_by_addr = {"FUN_%08X" % a: o for a, _w, _l, _k, o in es0.EMITTERS if o}

    section1_names(img, rows)
    section1_drive_names(img, rows)
    section2_variants(rows)
    section3_params(rows, offs_by_addr)
    section45_curves(rows)
    section6_crash(img)
    section7_boost(img)
    section8_drive(img, rows)
    section9_pass_master(img)

    n = len(PASS) + len(FAIL)
    print("\n%d/%d checks pass" % (len(PASS), n))
    if FAIL:
        print("FAILURES:")
        for f in FAIL:
            print("  -", f)
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
