#!/usr/bin/env python3
"""
Recover every car's real physics tuning from the game's ValueDB, keyed by
hashes recovered by running the game's own parameter registrar under Unicorn.

KEY CORRECTION found by this tool (2026-08-10): the per-car physics VDB is NOT
embedded in each .bgv (HANDOFF 6A's assumption). It is the single retail file
    Data/vdb.xml            (binary, despite the extension)
byte-identical to the community dump burnout-data-tool/data/vdb/
vdb_xbox_bo3_release.xml. A full scan of every shipped file for the recovered
hash dwords finds them ONLY in Data/vdb.xml (the .bgv hits were noise-level).

The registration key, captured from the real code (NOT guessed):
    "<param name><group>/<cfg path>"
e.g. "Mass (Kg)Physics/Vehicle/../Export/ValueDB/VehiclePhysics/COMPCAR1.cfg"
-- param name FIRST. Earlier direct-CRC attempts composed cfg-path-first and
therefore matched nothing. The hash core (0x001AF250) is table-CRC over
0x003F7700 with an ARITHMETIC shift (SAR, not SHR) and no final inversion.
The car name is the base-40 decode (FUN_001AECC0) of the packed 8-byte
vehicle ID in vlist.bin.

Evidence chain, each stage verified before the next is trusted:
  1. Emulate FUN_00132D10 (64 params) with hooks at:
       0x001AEE20  registration entry -> capture destination field pointer
       0x001AF250  hash core entry    -> capture the composed string
       0x001AF27F  hash core RET      -> capture EAX = hash
     The 64 captured offsets equal the 64 offsets in
     src/burnout3_physics_params.h exactly. FUN_00134AC0 (9 traffic params
     at +0x88..+0xA8) is run the same way; its keys are the same 9 strings
     (mass + suspension), so traffic lookups need no extra table.
  2. A pure-Python mirror of the hash reproduces every emulated hash
     (64/64 on three different car names). Independently, the community's
     bo3_vdb_definitions.yaml lists the same hash for COMPCAR1 Mass (Kg)
     (1803659936 == 0x6B81AAA0) -- a second derivation that agrees.
  3. Data/vdb.xml contains 64/64 computed hashes for every drivable (.bgv)
     vlist car and exactly the 9-param traffic subset for .btv cars --
     100 of 107 cars have VDB overrides (see generate output for the misses).
  4. Values are the raw dwords of the 8-byte {u32 value, i32 hash} default
     records, reinterpreted as f32 (RwReal; all 64 physics params are reals).
     Gear ordering / mass / torque sanity gates run over every car.

Usage:
  python3 tools/extract_car_vdb.py probe          # stages 1+2 (emulation)
  python3 tools/extract_car_vdb.py scan           # stage 3 coverage report
  python3 tools/extract_car_vdb.py generate       # full run -> header
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import importlib.util
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED, \
    UC_HOOK_CODE, UC_PROT_ALL, UcError
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX,
                               UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX)

_spec = importlib.util.spec_from_file_location(
    "ev", os.path.join(os.path.dirname(__file__), "emulate_vehicle.py"))
ev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ev)

GAME_DIR = (game_root())
PVEH = os.path.join(GAME_DIR, "pveh")
VDB_FILE = os.path.join(GAME_DIR, "Data", "vdb.xml")

REGISTRAR_64 = 0x00132D10      # player-car physics config, 64 params
REGISTRAR_9 = 0x00134AC0       # reduced (traffic) config, 9 params
REG_ENTRY = 0x001AEE20         # per-parameter registration entry
HASH_ENTRY = 0x001AF250        # CRC core: ECX = string, EDX = length
HASH_RET = 0x001AF27F          # its RET: EAX = hash
HASH_TABLE_VA = 0x003F7700     # 256 dwords used by the CRC core
MANAGER_GLOBAL = 0x004A1E94    # holds pointer to the comms/registry manager

PAGE = 0x1000

# Scratch layout for the registrar run (clear of the image and of
# emulate_vehicle's own constants, which we do not use here).
VEH = 0x30000000       # minimal vehicle: only +0x13F4 is read
OBJ = 0x32000000       # *(VEH+0x13F4): +0x1970/+0x1974 = packed base-40 ID
CFG = 0x31000000       # the physics parameter struct being registered
MGR = 0x33000000       # *(MANAGER_GLOBAL); registry object lives at +0x10
RECORDS = 0x34000000   # registry record array (0x20-byte records)
VTAB = 0x35000000      # fake vtable for the registry's virtual calls
STUBS = 0x36000000     # stub code the vtable points at
STACK = 0x20000000
STACK_SZ = 0x100000
MAGIC_RET = 0x50000000

OUT_HEADER = os.path.join(os.path.dirname(__file__), "..", "src",
                          "burnout3_car_physics.h")


# --------------------------------------------------------------------------
# vlist.bin: the packed 8-byte base-40 vehicle IDs, straight from the file
# (no re-encoding step that could drift from the game's own bytes).
# --------------------------------------------------------------------------
B40 = {0: ' ', 1: '-', 2: '/', 39: '_'}


def b40_decode(v):
    out = []
    for _ in range(12):
        v, r = divmod(v, 40)
        if r in B40:
            c = B40[r]
        elif 3 <= r <= 12:
            c = chr(r + 0x2D)       # '0'..'9'
        elif 13 <= r <= 38:
            c = chr(r + 0x34)       # 'A'..'Z'
        else:
            c = '?'
        out.append(c)
    return ''.join(reversed(out)).rstrip(' ')


def read_vlist():
    """[(name, packed_id, drivable)] in vlist order (107 entries)."""
    data = open(os.path.join(PVEH, "vlist.bin"), 'rb').read()
    version, count = struct.unpack_from('<II', data, 0)
    assert version == 6, version
    out = []
    for i in range(count):
        drivable = struct.unpack_from('<I', data, 8 + i * 4)[0]
        pid = struct.unpack_from('<Q', data, 0x408 + i * 8)[0]
        out.append((b40_decode(pid), pid, drivable))
    return out


def vehicle_file(name, drivable):
    """pveh filename for a vlist entry, e.g. COMPCAR10 -> Car10.bgv."""
    cls, num = name[:4], name[7:]
    assert name[4:7] == 'CAR', name
    return cls, "Car%s.%s" % (num, 'bgv' if drivable else 'btv')


# --------------------------------------------------------------------------
# Stage 1: run a registrar under Unicorn and capture {offset, string, hash}.
# --------------------------------------------------------------------------
def run_registrar(func_addr, packed_id, max_steps=20_000_000):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)

    for base, size in ((VEH, 0x2000), (OBJ, 0x2000), (CFG, 0x2000),
                       (MGR, PAGE), (RECORDS, 0x10000), (VTAB, PAGE),
                       (STUBS, PAGE), (STACK, STACK_SZ),
                       (MAGIC_RET & ~(PAGE - 1), PAGE)):
        uc.mem_map(base, size, UC_PROT_ALL)

    # vehicle -> object holding the packed ID (read by both registrars'
    # FUN_001aecc0 call and copied to the struct at +0xB0 / +0x80)
    uc.mem_write(VEH + 0x13F4, struct.pack('<I', OBJ))
    uc.mem_write(OBJ + 0x1970, struct.pack('<Q', packed_id))

    # registry manager: *(0x4A1E94) -> MGR; the registry object is MGR+0x10
    # with layout {+0 vtable, +4 next index, +8 count, +C records, +10 cap}
    # (fields from FUN_001aed70/FUN_001aee20). The vtable's virtual calls are
    # __thiscall with one pushed arg (disasm at 0x001aee7a/82/8e), so the
    # stubs RET 4. Slot +0xC is a validator whose AL==1 keeps the record.
    uc.mem_write(MANAGER_GLOBAL, struct.pack('<I', MGR))
    reg = MGR + 0x10
    uc.mem_write(reg, struct.pack('<IIIII', VTAB, 0, 0, RECORDS, 0x200))
    stub_ret4 = STUBS            # xor eax,eax ; ret 4
    stub_al1 = STUBS + 0x10      # mov al,1   ; ret 4
    uc.mem_write(stub_ret4, b"\x31\xC0\xC2\x04\x00")
    uc.mem_write(stub_al1, b"\xB0\x01\xC2\x04\x00")
    uc.mem_write(VTAB, struct.pack('<IIII', stub_ret4, stub_ret4,
                                   stub_ret4, stub_al1))

    events = []                  # completed {off, string, hash} triples
    pending = {'dest': None, 'string': None}
    faulted_pages = []

    def on_unmapped(mu, access, address, size, value, user):
        page = address & ~(PAGE - 1)
        try:
            mu.mem_map(page, PAGE, UC_PROT_ALL)
            faulted_pages.append(page)
        except UcError:
            return False
        return True

    def on_reg_entry(mu, address, size, user):
        esp = mu.reg_read(UC_X86_REG_ESP)
        dest = struct.unpack('<I', mu.mem_read(esp + 8, 4))[0]
        pending['dest'] = dest

    def on_hash_entry(mu, address, size, user):
        ptr = mu.reg_read(UC_X86_REG_ECX)
        ln = mu.reg_read(UC_X86_REG_EDX)
        pending['string'] = bytes(mu.mem_read(ptr, ln))

    def on_hash_ret(mu, address, size, user):
        h = mu.reg_read(UC_X86_REG_EAX)
        events.append({'dest': pending['dest'],
                       'string': pending['string'], 'hash': h})
        pending['dest'] = pending['string'] = None

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_CODE, on_reg_entry, begin=REG_ENTRY, end=REG_ENTRY)
    uc.hook_add(UC_HOOK_CODE, on_hash_entry, begin=HASH_ENTRY, end=HASH_ENTRY)
    uc.hook_add(UC_HOOK_CODE, on_hash_ret, begin=HASH_RET, end=HASH_RET)

    sp = STACK + STACK_SZ - 0x2000
    uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
    if func_addr == REGISTRAR_9:
        # FUN_00134AC0(EAX=struct, stack: id_lo, id_hi)
        uc.mem_write(sp + 4, struct.pack('<Q', packed_id))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EAX, CFG)   # struct base
    uc.reg_write(UC_X86_REG_EBX, VEH)   # vehicle (REGISTRAR_64 reads +0x13F4)

    err = None
    try:
        uc.emu_start(func_addr, MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))

    cfg_path = bytes(uc.mem_read(CFG, 0x60)).split(b'\0')[0]
    return events, cfg_path, faulted_pages, err


# --------------------------------------------------------------------------
# Stage 2: pure-Python mirror of the hash core at 0x001AF250.
#   or eax,-1 ; for each byte: idx = (eax & 0xFF) ^ sext(byte)
#                              eax = (eax SAR 8) ^ table[idx]
# SAR (arithmetic) not SHR, and no final inversion. Table at 0x003F7700.
# --------------------------------------------------------------------------
_table = None


def hash_table():
    global _table
    if _table is None:
        data = open(ev.ELF, 'rb').read()
        ph_off = struct.unpack_from('<I', data, 0x1C)[0]
        ph_num = struct.unpack_from('<H', data, 0x2C)[0]
        raw = None
        for i in range(ph_num):
            p_type, off, va, _, fsz, msz, _, _ = struct.unpack_from(
                '<IIIIIIII', data, ph_off + i * 32)
            if p_type == 1 and va <= HASH_TABLE_VA < va + fsz:
                raw = data[off + HASH_TABLE_VA - va:
                           off + HASH_TABLE_VA - va + 0x400]
                break
        assert raw is not None
        _table = list(struct.unpack('<256I', raw))
    return _table


def gt_hash(data):
    table = hash_table()
    h = 0xFFFFFFFF
    for b in data:
        idx = (h & 0xFF) ^ b          # bytes here are ASCII < 0x80, so the
        # MOVSX sign extension never sets high bits and idx stays in-table
        sh = h >> 8
        if h & 0x80000000:            # SAR: arithmetic shift fills with 1s
            sh |= 0xFF000000
        h = sh ^ table[idx]
    return h


def compose(name, group, param):
    # Order verified by emulation (stage 1 capture): the registration key is
    #   "<param name><group>/<cfg path>"
    # e.g. "Mass (Kg)Physics/Vehicle/../Export/ValueDB/VehiclePhysics/COMPCAR1.cfg"
    # -- NOT cfgpath-first, which is why the earlier direct-CRC attempts failed.
    return ("%s%s/../Export/ValueDB/VehiclePhysics/%s.cfg"
            % (param, group, name)).encode('ascii')


# --------------------------------------------------------------------------
# The 64 registered params, from the generated header (offset -> group, name).
# --------------------------------------------------------------------------
def known_param_offsets():
    hdr = os.path.join(os.path.dirname(__file__), "..", "src",
                       "burnout3_physics_params.h")
    offs = {}
    import re
    for line in open(hdr):
        m = re.match(r'\s*\{ "([^"]+)", "([^"]+)", (0x[0-9A-Fa-f]+)u,', line)
        if m:
            offs[int(m.group(3), 16)] = (m.group(1), m.group(2))
    return offs


# --------------------------------------------------------------------------
# Stage 3/4: the retail VDB (Data/vdb.xml).
# Header {i32 type=2, i32 defaultCount, i32 unk, i32 fileDefCount,
#         i32 fileDefOffset}; then defaultCount x {u32 rawValue, i32 hash};
# fileDefs are {u32 active, i32 pathHash}. Layout per burnout-data-tool's
# VDBParser.cs, confirmed against the file itself.
# --------------------------------------------------------------------------
def read_vdb():
    data = open(VDB_FILE, 'rb').read()
    t, dvc, unk1, fdc, fdo = struct.unpack_from('<IIIII', data, 0)
    assert t == 2, t
    values = {}
    for i in range(dvc):
        raw, nh = struct.unpack_from('<Ii', data, 20 + i * 8)
        h = nh & 0xFFFFFFFF
        assert h not in values, "duplicate hash %08X in vdb" % h
        values[h] = raw
    filedefs = set()
    for i in range(fdc):
        act, h = struct.unpack_from('<Ii', data, fdo + i * 8)
        filedefs.add(h & 0xFFFFFFFF)
    return values, filedefs


def f32(raw):
    return struct.unpack('<f', struct.pack('<I', raw))[0]


def extract_all():
    """{car name: {offset: float}} for all vlist cars, plus bookkeeping."""
    vdb, filedefs = read_vdb()
    known = known_param_offsets()
    cars = {}
    for name, pid, drv in read_vlist():
        vals = {}
        for off, (grp, prm) in known.items():
            h = gt_hash(compose(name, grp, prm))
            if h in vdb:
                vals[off] = f32(vdb[h])
        cfg_in_filedefs = gt_hash(
            b"../Export/ValueDB/VehiclePhysics/%s.cfg" % name.encode()) \
            in filedefs
        cars[name] = (vals, drv, cfg_in_filedefs)
    return cars


# --------------------------------------------------------------------------
# Sanity gates. A car failing a gate is reported and EXCLUDED from the header
# rather than silently emitted.
# --------------------------------------------------------------------------
def sane(name, vals, drv):
    problems = []
    if not vals:
        return ["no VDB overrides"]
    m = vals.get(0x0B8)
    if m is not None and not (400.0 <= m <= 20000.0):
        problems.append("mass %.0f out of range" % m)
    gears = [vals.get(0x0E8 + i * 4) for i in range(6)]
    fwd = [g for g in gears if g is not None and g > 0.0]
    for a, b in zip(fwd, fwd[1:]):
        if b >= a:
            problems.append("gear ratios not descending: %r" % (gears,))
            break
    fin = vals.get(0x100)
    if fin is not None and not (1.0 <= fin <= 8.0):
        problems.append("final drive %.2f out of range" % fin)
    tq = vals.get(0x11C)
    if tq is not None and not (50.0 <= tq <= 5000.0):
        problems.append("torque %.0f out of range" % tq)
    rev = vals.get(0x0E0)
    if rev is not None and rev >= 0.0:
        problems.append("reverse ratio %.2f not negative" % rev)
    return problems


# --------------------------------------------------------------------------
# Modes
# --------------------------------------------------------------------------
def probe():
    known = known_param_offsets()
    ok = True
    for name, pid, drv in read_vlist():
        if name not in ("COMPCAR1", "SUPRCAR5", "TSPCCAR3"):
            continue
        events, cfg_path, faults, err = run_registrar(REGISTRAR_64, pid)
        print("== %s  (registrar 0x%X)" % (name, REGISTRAR_64))
        print("   cfg path built by the game: %r" % cfg_path)
        print("   events: %d, lazily faulted pages: %d, err: %s"
              % (len(events), len(faults), err))
        if err or len(events) != 64:
            ok = False
            continue
        offsets = sorted(e['dest'] - CFG for e in events)
        if offsets != sorted(known):
            print("   OFFSET MISMATCH vs burnout3_physics_params.h")
            ok = False
        mismatch = 0
        for e in events:
            if gt_hash(e['string']) != e['hash']:
                mismatch += 1
                print("   HASH MIRROR FAIL %r emu=%08X py=%08X"
                      % (e['string'], e['hash'], gt_hash(e['string'])))
        # compose() must rebuild the captured string byte for byte
        rebuilt = 0
        for e in events:
            off = e['dest'] - CFG
            grp, prm = known[off]
            if compose(name, grp, prm) == e['string']:
                rebuilt += 1
        print("   python hash mirror: %d/64 match; compose(): %d/64 exact"
              % (64 - mismatch, rebuilt))
        ok = ok and mismatch == 0 and rebuilt == 64

    name, pid, drv = read_vlist()[0]
    events, cfg_path, faults, err = run_registrar(REGISTRAR_9, pid)
    print("== %s  (registrar 0x%X)" % (name, REGISTRAR_9))
    print("   events: %d, err: %s" % (len(events), err))
    ok = ok and not err and len(events) == 9
    for e in events:
        py = gt_hash(e['string'])
        mark = "OK" if py == e['hash'] else "MIRROR FAIL"
        print("   +0x%03X %08X %s  %r"
              % (e['dest'] - CFG, e['hash'], mark, e['string']))
        ok = ok and py == e['hash']
    print("\nprobe:", "ALL OK" if ok else "FAILED")
    return ok


def scan():
    cars = extract_all()
    full = partial = none = 0
    for name, (vals, drv, in_fd) in cars.items():
        n = len(vals)
        if n == 64:
            full += 1
        elif n:
            partial += 1
        else:
            none += 1
        tag = "drivable" if drv else "traffic"
        print("%-10s %-8s %2d/64 params, cfg in fileDefs: %s"
              % (name, tag, n, in_fd))
    print("\n%d cars full 64, %d partial (traffic 9), %d without overrides"
          % (full, partial, none))
    return True


def generate():
    # gate on the emulation self-check first: the header must never be
    # regenerated from an unverified hash pipeline
    name0, pid0, _ = read_vlist()[0]
    events, _, _, err = run_registrar(REGISTRAR_64, pid0)
    known = known_param_offsets()
    assert not err and len(events) == 64, "registrar emulation failed"
    for e in events:
        off = e['dest'] - CFG
        grp, prm = known[off]
        assert gt_hash(e['string']) == e['hash'], "hash mirror diverged"
        assert compose(name0, grp, prm) == e['string'], "compose() diverged"

    cars = extract_all()
    order = [n for n, _, _ in read_vlist()]

    excluded = {}
    for name in order:
        vals, drv, in_fd = cars[name]
        problems = sane(name, vals, drv)
        if problems:
            excluded[name] = problems

    lines = []
    a = lines.append
    a("// Generated by tools/extract_car_vdb.py -- do not edit by hand.")
    a("//")
    a("// Per-car physics overrides from the retail Data/vdb.xml, keyed by")
    a("// hashes recovered by executing the game's own registrar FUN_00132D10")
    a("// under Unicorn (key = \"<param><group>/../Export/ValueDB/")
    a("// VehiclePhysics/<VLIST-ID>.cfg\", table-CRC at 0x001AF250 with SAR")
    a("// semantics). Offsets are the same 0x1D0-struct offsets as")
    a("// burnout3_physics_params.h; apply with b3_config_set_by_offset().")
    a("// Drivable cars carry all 64 params; traffic (.btv) cars carry the")
    a("// 9-param reduced set (FUN_00134AC0: mass + suspension). Cars with no")
    a("// VDB overrides fall back to b3_physics_defaults().")
    a("#ifndef BURNOUT3_CAR_PHYSICS_H")
    a("#define BURNOUT3_CAR_PHYSICS_H")
    a("")
    a("typedef struct {")
    a("    unsigned short offset;   // into the game's 0x1D0 physics struct")
    a("    float          value;")
    a("} B3CarParam;")
    a("")
    a("typedef struct {")
    a("    const char*       id;         // vlist vehicle ID (base-40 decoded)")
    a("    const char*       class_code; // pveh/<class>/")
    a("    const char*       file;       // matches VehicleInfo.file")
    a("    const B3CarParam* params;")
    a("    int               n_params;   // 64 drivable / 9 traffic")
    a("} B3CarPhysics;")
    a("")

    emitted = []
    for name in order:
        vals, drv, in_fd = cars[name]
        if name in excluded:
            continue
        cls, fn = vehicle_file(name, drv)
        a("static const B3CarParam B3_CARPARAMS_%s[] = {" % name)
        for off in sorted(vals):
            grp, prm = known[off]
            lit = "%.9g" % vals[off]
            if not any(c in lit for c in ".e"):
                lit += ".0"          # 800 -> 800.0 so the f suffix is legal
            a("    { 0x%03Xu, %sf },  // %s/%s" % (off, lit, grp, prm))
        a("};")
        emitted.append((name, cls, fn, len(vals)))
    a("")
    a("#define B3_CAR_PHYSICS_COUNT %d" % len(emitted))
    a("")
    a("static const B3CarPhysics B3_CAR_PHYSICS[B3_CAR_PHYSICS_COUNT] = {")
    for name, cls, fn, n in emitted:
        a('    { "%s", "%s", "%s", B3_CARPARAMS_%s, %d },'
          % (name, cls, fn, name, n))
    a("};")
    a("")
    for name, problems in excluded.items():
        a("// EXCLUDED %s: %s" % (name, "; ".join(problems)))
    a("")
    a("#endif // BURNOUT3_CAR_PHYSICS_H")

    with open(OUT_HEADER, 'w') as f:
        f.write("\n".join(lines) + "\n")

    # human-readable per-car summary table
    print("emitted %d cars to %s (excluded %d: %s)"
          % (len(emitted), os.path.normpath(OUT_HEADER), len(excluded),
             ", ".join(excluded) or "none"))
    print("\n%-10s %5s %6s | %5s %5s %5s %5s %5s %5s %5s | %6s %6s"
          % ("car", "mass", "torque", "1st", "2nd", "3rd", "4th", "5th",
             "6th", "final", "maxrpm", "boost"))
    for name in order:
        vals, drv, in_fd = cars[name]
        if not vals:
            continue
        g = lambda off: vals.get(off)
        fmt = lambda v, w=5: ("%*.2f" % (w, v)) if v is not None else " " * w
        print("%-10s %5s %6s | %s %s %s %s %s %s %s | %6s %6s"
              % (name, fmt(g(0x0B8)), fmt(g(0x11C), 6),
                 fmt(g(0x0E8)), fmt(g(0x0EC)), fmt(g(0x0F0)), fmt(g(0x0F4)),
                 fmt(g(0x0F8)), fmt(g(0x0FC)), fmt(g(0x100)),
                 fmt(g(0x118), 6), fmt(g(0x1A4), 6)))
    return True


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "probe"
    if mode == "probe":
        sys.exit(0 if probe() else 1)
    if mode == "scan":
        sys.exit(0 if scan() else 1)
    if mode == "generate":
        sys.exit(0 if generate() else 1)
    print("unknown mode", mode)
    sys.exit(2)
