#!/usr/bin/env python3
"""
Recover the game's Camera.cfg parameter set by EXECUTING its two registrars
under Unicorn, then resolve every key in the retail Data/vdb.xml.

Same pipeline that recovered the per-car physics (RE_NOTES 10 /
tools/extract_car_vdb.py): the registrar is run for real, the CRC core at
0x001AF250 is hooked to capture the composed key string and its hash, and
the parameter's storage pointer is taken from the registration entry.  No
key is guessed and no value is transcribed.

  FUN_00160840   "Camera/Follow/", "Camera/Look Back/", "Camera/Bumper/"
                 -- the chase / look-back / bumper camera geometry
  FUN_00160B90   "Crash/HandyCam/", "InGame/Effect/{Burnout,Shunt,Shunted,
                 Slam,Shake}/{Position,Angular}/" -- the camera-shake and
                 crash-handycam spring system (cfg path
                 "../Export/ValueDB/Camera/Camera.cfg")

Usage: python3 tools/emulate_tdfx_camera.py [--json out.json]
"""
import importlib.util
import json
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED, \
    UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_PROT_ALL, UcError
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX, \
    UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBP, \
    UC_X86_REG_EDI, UC_X86_REG_XMM2

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "cvdb", os.path.join(_here, "extract_car_vdb.py"))
cvdb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cvdb)
ev = cvdb.ev

REGISTRARS = {
    0x00160840: "camera geometry (Follow / Look Back / Bumper)",
    0x00160B90: "camera shake (Crash HandyCam + InGame effects)",
}

REG_ENTRY = cvdb.REG_ENTRY        # 0x001AEE20 scalar/vector registration
ALLOC = 0x001AEDB0                # record allocator (vector params)
HASH_ENTRY = cvdb.HASH_ENTRY
HASH_RET = cvdb.HASH_RET
MANAGER_GLOBAL = cvdb.MANAGER_GLOBAL

PAGE = 0x1000
CFG = 0x31000000       # the camera parameter struct being registered
MGR = 0x33000000
RECORDS = 0x34000000
VTAB = 0x35000000
STUBS = 0x36000000
STACK = 0x20000000
STACK_SZ = 0x100000
MAGIC_RET = 0x50000000
CFG_SZ = 0x8000


def run(func_addr, max_steps=40_000_000):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    for base, size in ((CFG, CFG_SZ), (MGR, PAGE), (RECORDS, 0x20000),
                       (VTAB, PAGE), (STUBS, PAGE), (STACK, STACK_SZ),
                       (MAGIC_RET & ~(PAGE - 1), PAGE)):
        uc.mem_map(base, size, UC_PROT_ALL)

    uc.mem_write(MANAGER_GLOBAL, struct.pack('<I', MGR))
    reg = MGR + 0x10
    uc.mem_write(reg, struct.pack('<IIIII', VTAB, 0, 0, RECORDS, 0x400))
    stub_ret4 = STUBS
    stub_al1 = STUBS + 0x10
    uc.mem_write(stub_ret4, b"\x31\xC0\xC2\x04\x00")
    uc.mem_write(stub_al1, b"\xB0\x01\xC2\x04\x00")
    uc.mem_write(VTAB, struct.pack('<IIII', stub_ret4, stub_ret4,
                                   stub_ret4, stub_al1))

    events = []
    pending = {'dest': None, 'string': None}

    def on_unmapped(mu, access, address, size, value, user):
        try:
            mu.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    def on_reg_entry(mu, address, size, user):
        # FUN_001AEE20(registry, storage, group, name, ...) -- storage at +8
        esp = mu.reg_read(UC_X86_REG_ESP)
        pending['dest'] = struct.unpack('<I', mu.mem_read(esp + 8, 4))[0]

    def on_alloc(mu, address, size, user):
        # vector params allocate their record first and the storage pointer is
        # written into *record afterwards; the record-write hook below picks
        # that up, so just clear whatever a previous scalar left.
        pending['dest'] = None

    def on_record_write(mu, access, address, size, value, user):
        # registry records are 0x20 bytes at [registry+0xC] (FUN_001AEDB0:
        # record = base + index*0x20) and record+0x00 is the parameter's
        # storage address (`*record = &field` in every registrar).  Only
        # accept aligned record+0x00 writes, so stack spills of the same
        # pointer cannot be mistaken for the registration.
        if (size == 4 and RECORDS <= address < RECORDS + 0x20000
                and (address - RECORDS) % 0x20 == 0
                and CFG <= value < CFG + CFG_SZ):
            pending['dest'] = value

    def on_hash_entry(mu, address, size, user):
        ptr = mu.reg_read(UC_X86_REG_ECX)
        ln = mu.reg_read(UC_X86_REG_EDX)
        pending['string'] = bytes(mu.mem_read(ptr, ln))

    def on_hash_ret(mu, address, size, user):
        h = mu.reg_read(UC_X86_REG_EAX)
        events.append({'dest': pending['dest'],
                       'string': pending['string'], 'hash': h})
        pending['string'] = None

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_record_write,
                begin=RECORDS, end=RECORDS + 0x20000)
    uc.hook_add(UC_HOOK_CODE, on_reg_entry, begin=REG_ENTRY, end=REG_ENTRY)
    uc.hook_add(UC_HOOK_CODE, on_alloc, begin=ALLOC, end=ALLOC)
    uc.hook_add(UC_HOOK_CODE, on_hash_entry, begin=HASH_ENTRY, end=HASH_ENTRY)
    uc.hook_add(UC_HOOK_CODE, on_hash_ret, begin=HASH_RET, end=HASH_RET)

    sp = STACK + STACK_SZ - 0x4000
    uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EAX, CFG)
    err = None
    try:
        uc.emu_start(func_addr, MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    defaults = bytes(uc.mem_read(CFG, CFG_SZ))
    return events, defaults, err


CFG_SUFFIX = "/../Export/ValueDB/Camera/Camera.cfg"


def split_key(s):
    """'<param><group>/../Export/ValueDB/Camera/Camera.cfg' -> (group, param)"""
    t = s.decode('ascii', 'replace')
    # the composed key is "<param><group>/<cfg path>"; cut at the cfg path
    # wherever it starts, so a struct whose path buffer was not fully built
    # in isolation still yields the right (group, param) pair.
    cut = t.find("/../Export/ValueDB/")
    head = t[:cut] if cut >= 0 else t
    # the group always ends with '/', and every group in these two registrars
    # starts with a capital after either the param name or a '/'
    for g in ("Crash/HandyCam", "Camera/Follow", "Camera/Look Back",
              "Camera/Bumper", "InGame/Effect/Burnout/Position",
              "InGame/Effect/Burnout/Angular", "InGame/Effect/Shunt/Position",
              "InGame/Effect/Shunt/Angular", "InGame/Effect/Shunted/Position",
              "InGame/Effect/Shunted/Angular", "InGame/Effect/Slam/Position",
              "InGame/Effect/Slam/Angular", "InGame/Effect/Shake/Position",
              "InGame/Effect/Shake/Angular"):
        if head.endswith(g):
            return g, head[:-len(g)]
    return None, head


def canonical_hash(group, param):
    key = ("%s%s/../Export/ValueDB/Camera/Camera.cfg"
           % (param, group)).encode('ascii')
    return cvdb.gt_hash(key)


def vdb_value(raw, n, blob, pool_lo, pool_hi):
    """A VDB rawValue is the float itself for scalars and a FILE OFFSET into
    the element pool for arrays (RE_GAMEPLAY section 1)."""
    if n == 1:
        return [cvdb.f32(raw)]
    if pool_lo <= raw < pool_hi - 4 * n:
        return list(struct.unpack_from('<%df' % n, blob, raw))
    return None


def main():
    vdb, _filedefs = cvdb.read_vdb()
    blob = open(cvdb.VDB_FILE, 'rb').read()
    _t, dvc, _u, _fdc, fdo = struct.unpack_from('<IIIII', blob, 0)
    pool_lo, pool_hi = 20 + dvc * 8, fdo

    out = []
    for addr, what in REGISTRARS.items():
        events, defaults, err = run(addr)
        print("== FUN_%08X  (%s) ==" % (addr, what))
        if err:
            print("   emulation ended:", err)
        print("   %d hashed keys captured" % len(events))
        for e in events:
            group, param = split_key(e['string'])
            param = param.strip()
            dest = e['dest']
            off = dest - CFG if (dest is not None
                                 and CFG <= dest < CFG + CFG_SZ) else None
            # a parameter is a vec3 iff the registrar left three consecutive
            # non-overlapping slots for it; the compiled defaults tell us
            # directly (the registrars write x/y/z before registering).
            h = e['hash']
            src = "emulated"
            if h not in vdb and group:
                h2 = canonical_hash(group, param)
                if h2 in vdb:
                    h, src = h2, "canonical"
            raw = vdb.get(h)
            n3 = vdb_value(raw, 3, blob, pool_lo, pool_hi) if raw else None
            n1 = vdb_value(raw, 1, blob, pool_lo, pool_hi) if raw else None
            # scalars land in a sane float range; array offsets decode to
            # denormals as floats, which is the tell.
            if n1 and abs(n1[0]) > 1e-30:
                vals, n = n1, 1
            elif n3 is not None:
                vals, n = n3, 3
            else:
                vals, n = None, 0
            # NOTE: the storage offset is only reliable for scalar params
            # (captured at FUN_001AEE20's entry); vector params write their
            # storage pointer into a record the allocator hands out from a
            # lazily-faulted heap, which this run does not track.  The
            # compiled-in default is therefore reported only when the offset
            # is trustworthy, and is [?] otherwise.
            defv = None
            if off is not None and n == 1:
                defv = list(struct.unpack_from('<f', defaults, off))
            out.append(dict(registrar="0x%08X" % addr, group=group,
                            param=param, offset=off, hash="0x%08X" % h,
                            key_source=src, n=n, default=defv, vdb=vals))
            print("   %-32s %-18s off=%-8s %08X(%-9s) default=%-22s vdb=%s"
                  % (group, param,
                     ("+0x%X" % off) if off is not None else "?", h, src,
                     ("%s" % ["%g" % v for v in defv]) if defv else "?",
                     ("%s" % ["%g" % v for v in vals]) if vals else "-"))
    if '--json' in sys.argv:
        p = sys.argv[sys.argv.index('--json') + 1]
        json.dump(out, open(p, 'w'), indent=1)
        print("wrote", p)


if __name__ == '__main__':
    main()


# ======================================================================
# The chase-camera smoothing law -- executing the REAL span of
# FUN_0015E550 (the "Camera/Follow" mode's per-frame update, mode 2/3 of
# the camera director; vtable 0x003A9C28 entry +0x10).
#
# Span emulated: 0x0015E5B6 .. 0x0015E734.  It is straight-line apart from
# the two integer-power loops, and reads exactly three inputs:
#
#   [ecx + 0xBC]   the vehicle's speed in m/s   (RE_NOTES 10)
#   [ebp + 0x0C]   the frame delta handed to the mode update
#   [edi + 0x20]   Camera/Follow "Spring Coeff" .x  (cfg + 0x80)
#   [edi + 0x24]   Camera/Follow "Spring Coeff" .y  (cfg + 0x84)
#
# and produces the two blend factors the camera then lerps with, left in
# [esp+0x24] (yaw / lateral) and [esp+0x28] (pitch / vertical).
#
# edi is loaded from [ebx+8] at 0x0015E622 (the mode object's config
# pointer, wired by FUN_001674B0), so ebx is seeded too.
# ======================================================================

FOLLOW_UPDATE_BEGIN = 0x0015E5B6
FOLLOW_UPDATE_END   = 0x0015E734

_VEH = 0x40000000
_CFG2 = 0x41000000
_MODE = 0x42000000


def follow_blend(speed_ms, dt, spring_x, spring_y):
    """Run the real x86 and return (blend_x, blend_y)."""
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    for base, size in ((_VEH, PAGE), (_CFG2, PAGE), (_MODE, PAGE),
                       (STACK, STACK_SZ)):
        uc.mem_map(base, size, UC_PROT_ALL)

    def on_unmapped(mu, access, address, size, value, user):
        try:
            mu.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    uc.mem_write(_VEH + 0xBC, struct.pack('<f', speed_ms))
    uc.mem_write(_CFG2 + 0x20, struct.pack('<ff', spring_x, spring_y))
    uc.mem_write(_MODE + 0x08, struct.pack('<I', _CFG2))

    # a 16-byte-aligned frame; the prologue (which we skip) did
    # `and esp,0xFFFFFFF0; sub esp,0x1E4` then pushed ebx/esi/edi.
    ebp = STACK + STACK_SZ - 0x1000
    esp = (ebp - 0x1E4 - 0x10) & ~0xF
    uc.mem_write(ebp + 0x0C, struct.pack('<f', dt))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.reg_write(UC_X86_REG_EBP, ebp)
    uc.reg_write(UC_X86_REG_ECX, _VEH)
    uc.reg_write(UC_X86_REG_EBX, _MODE)
    # xmm2 = 1.0, loaded at 0x0015E55C from 0x003B168C
    uc.reg_write(UC_X86_REG_XMM2, struct.unpack('<Q', struct.pack('<ff', 1.0, 0.0))[0])
    uc.emu_start(FOLLOW_UPDATE_BEGIN, FOLLOW_UPDATE_END, count=2_000_000)
    esp2 = uc.reg_read(UC_X86_REG_ESP)
    bx = struct.unpack('<f', uc.mem_read(esp2 + 0x24, 4))[0]
    by = struct.unpack('<f', uc.mem_read(esp2 + 0x28, 4))[0]
    return bx, by


# ======================================================================
# THE WHOLE chase-camera update -- executing FUN_0015E550 end to end.
#
# This is the camera the player is looking through when their own car
# wrecks: nothing in the retail image switches the director's mode on a
# crash (RE_TAKEDOWN_FX section 9), so mode 2's update keeps running over
# the wreck's own transform.  Running the whole function (not just the
# smoothing span above) recovers the parts the earlier pass left [?]: the
# pitch target, the yaw target, the focus anchor, the eye composition,
# the Down Angle's role and the boost FOV.
#
# Seeded objects:
#   mode  (ECX)      +0x08 cfg, +0x18 yaw deg, +0x1C pitch deg,
#                    +0x20 slot, +0x24 yaw-track gate, +0x26 look-back
#   cfg              +0x00 Camera Offset, +0x10 Focus Offset,
#                    +0x20 Spring Coeff, +0x30 Down Angle
#   DAT_0073A1A8[slot]  racecar   (+0x11AC boost FOV ramp, +0x27D0 player)
#   DAT_0064B38C[slot*0x30] vehicle (+0x0BC speed m/s, +0x204 -> matrix)
#   [ebp+8]          camera state (out: +0x10 FOV, +0x20 quat, +0x30 eye)
#   [ebp+0xC]        dt
#
# The only call that is stubbed is the low-speed camera-collision probe
# FUN_00162A90 at 0x0015EEEF (it walks the world's collision data, which
# is not part of this module); everything else -- FUN_00011900 (axis /
# angle -> matrix, DEGREES), FUN_000116E0 (matrix multiply),
# FUN_00011B10, FUN_00013D10 -- runs for real.
# ======================================================================

FOLLOW_FULL_ENTRY = 0x0015E550
RACECAR_TABLE = 0x0073A1A8          # 0x0015E56A  dword[slot]
VEHICLE_TABLE = 0x0064B38C          # 0x0015E57B  dword[slot*0x30]
CAM_COLLIDE   = 0x00162A90          # stubbed

_STATE = 0x43000000
_RC    = 0x44000000
_VEH2  = 0x45000000
_MAT   = 0x46000000
_CFG3  = 0x47000000
_MODE2 = 0x48000000
_RET   = 0x51000000


def _fv(*v):
    return struct.pack('<%df' % len(v), *v)


def follow_update(car_rows, speed_ms, dt, yaw_deg, pitch_deg,
                  boost_ramp=0.0, look_back=0, yaw_gate=1,
                  cam_offset=(0.0, 0.95, -6.8),
                  focus_offset=(0.0, 0.35, 2.0),
                  spring=(0.06, 0.1, 5.0), down_angle=1.8, slot=0):
    """Execute FUN_0015E550 over a seeded car transform.

    car_rows: 4 x (x,y,z) -- right, up, forward, position (the vehicle's
    own matrix at *(veh+0x204), RE_NOTES section 16).
    Returns dict(eye, basis(3x3 rows), fov, pitch, yaw).
    """
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    for base, size in ((_STATE, PAGE), (_RC, 0x8000), (_VEH2, 0x1000),
                       (_MAT, PAGE), (_CFG3, PAGE), (_MODE2, PAGE),
                       (STACK, STACK_SZ), (_RET & ~(PAGE - 1), PAGE)):
        uc.mem_map(base, size, UC_PROT_ALL)

    def on_unmapped(mu, access, address, size, value, user):
        try:
            mu.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    # skip the low-speed camera-collision probe (needs the world)
    def on_code(mu, address, size, user):
        if address == 0x0015EEEF:
            mu.reg_write(UC_X86_REG_EIP, 0x0015EEF4)
    uc.hook_add(UC_HOOK_CODE, on_code, begin=0x0015EEEF, end=0x0015EEEF)

    uc.mem_write(RACECAR_TABLE + 4 * slot, struct.pack('<I', _RC))
    uc.mem_write(VEHICLE_TABLE + 0x30 * slot, struct.pack('<I', _VEH2))
    uc.mem_write(_RC + 0x11AC, _fv(boost_ramp))
    uc.mem_write(_RC + 0x27D0, struct.pack('<I', 0))
    uc.mem_write(_VEH2 + 0x0BC, _fv(speed_ms))
    uc.mem_write(_VEH2 + 0x204, struct.pack('<I', _MAT))
    for i, r in enumerate(car_rows):
        uc.mem_write(_MAT + 0x10 * i, _fv(r[0], r[1], r[2],
                                          1.0 if i == 3 else 0.0))
    uc.mem_write(_CFG3 + 0x00, _fv(*cam_offset))
    uc.mem_write(_CFG3 + 0x10, _fv(*focus_offset))
    uc.mem_write(_CFG3 + 0x20, _fv(*spring))
    uc.mem_write(_CFG3 + 0x30, _fv(down_angle))
    uc.mem_write(_MODE2 + 0x08, struct.pack('<I', _CFG3))
    uc.mem_write(_MODE2 + 0x18, _fv(yaw_deg, pitch_deg))
    uc.mem_write(_MODE2 + 0x20, struct.pack('<I', slot))
    uc.mem_write(_MODE2 + 0x24, bytes([yaw_gate, 0, look_back, 0]))
    uc.mem_write(_MODE2 + 0x40, struct.pack('<I', 0))
    uc.mem_write(0x0045B9C0, b"\x00")          # the pow() path is off

    esp = STACK + STACK_SZ - 0x2000
    uc.mem_write(esp, struct.pack('<I', _RET))
    uc.mem_write(esp + 4, struct.pack('<I', _STATE))
    uc.mem_write(esp + 8, _fv(dt))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.reg_write(UC_X86_REG_ECX, _MODE2)
    uc.emu_start(FOLLOW_FULL_ENTRY, _RET, count=20_000_000)

    # camera state layout, proved by this run: +0x10 FOV (degrees),
    # +0x20 orientation QUATERNION (FUN_00011B10 is matrix -> quat),
    # +0x30 eye position.
    fov = struct.unpack('<f', uc.mem_read(_STATE + 0x10, 4))[0]
    quat = list(struct.unpack('<4f', uc.mem_read(_STATE + 0x20, 16)))
    eye = list(struct.unpack('<3f', uc.mem_read(_STATE + 0x30, 12)))
    pitch = struct.unpack('<f', uc.mem_read(_MODE2 + 0x1C, 4))[0]
    yaw = struct.unpack('<f', uc.mem_read(_MODE2 + 0x18, 4))[0]
    return dict(eye=eye, quat=quat, fov=fov, pitch=pitch, yaw=yaw)
