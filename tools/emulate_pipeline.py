#!/usr/bin/env python3
"""
Multi-frame GOLD STANDARD: run the game's real per-frame vehicle pipeline
under Unicorn, over a full synthetic-but-faithfully-seeded vehicle struct,
for consecutive frames, and capture the struct trajectory.

This is the acceptance oracle for b3_vehicle_step_full() (the C port of the
pipeline).  It executes the REAL functions in FUN_0011BE50's exact call
order for the normal-racing path (byte v+0x210 == 0, racecar mode
+0x1920 == 0 => two substeps at dt/2):

  per frame (glue mirrored line-for-line from the decompiled callers):
    FUN_00104840 mirror   per-frame zeroing of the v+0x160 contact block
    [FUN_0011BC60 STUB]   ground-poly collection -> replaced by refilling
                          the soup at v+0x200 with a flat plane at y=0
                          (the real function queries the loaded world
                          spatial index, which does not exist here; the
                          collision agent owns the real collector)
    driver-input glue     FUN_00104D30 tail: v+0x1400 = v+0x1414*v+0x13BC
    FUN_0011ECF0  (real)  input stage: steering schedule/slew, gear
                          engage, engine update (calls FUN_00121560),
                          drive torque + boost shaping, slide schedule,
                          drift entry state machine
    2 x at dt/2:
      FUN_0011D460 (real) force pass (resistance/tyres/drift/brake/LSDM)
      [FUN_0011AEF0 STUB] ground-collision/crash response - owned by the
                          crash agent; BE50's return-0 path mirrored
      FUN_001239C0 (real) suspension pre-pass (rays vs the soup)
      FUN_00123FD0 (real) suspension force pass + wheel spin/visuals
      stop-check glue     BE50: near-stop zeroing of vel + accF.x/.z
      FUN_00109560 (real) rigid-body integration
    wheel prev-frame restore (BE50 substep tail)
    FUN_0011C720 (real)   HUD/audio export into the ctx objects

  Skipped with evidence (see RE_NOTES section 14): FUN_0018DA00 (slam
  camera wobble, gated off by slam clocks = -1), FUN_0011FFA0 (skid/smoke
  state classifier; writes wheel+0x78 + effect spawner only),
  FUN_0010DCA0/FUN_0010DD20/FUN_00109BB0/FUN_00125100 (rollover/crash-mode
  tails, gated off: frame up.y > 0.5 and DAT_004D5370 == 0),
  FUN_00126520/FUN_00126D40 (crumpled-panel visuals; file numparts = 0).

Seeding mirrors the recovered init chain (FUN_00109270 / FUN_00122830 /
FUN_0011A8F0 / FUN_00134710 / FUN_001214A0 / FUN_001203A0) with COMPCAR1's
real Data/vdb.xml values and the real Car1.bgv extents/wheel positions.

Usage: python3 tools/emulate_pipeline.py [scenario]   (default: all)
"""
import json
import math
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "ev", os.path.join(_here, "emulate_vehicle.py"))
ev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ev)

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED, \
    UC_PROT_ALL, UcError
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP,
                               UC_X86_REG_EAX, UC_X86_REG_EBX,
                               UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

PAGE = 0x1000

# ---------------------------------------------------------------------------
# real function addresses
# ---------------------------------------------------------------------------
F_ECF0   = 0x0011ECF0    # input stage (calls FUN_00121560)
F_D460   = 0x0011D460    # force pass
F_239C0  = 0x001239C0    # suspension pre-pass
F_23FD0  = 0x00123FD0    # suspension force pass
F_09560  = 0x00109560    # rigid-body integrator
F_C720   = 0x0011C720    # ctx export
F_18DA00 = 0x0018DA00    # slam wobble (skipped; ablation-checkable)
F_FFA0   = 0x0011FFA0    # skid classifier (skipped; ablation-checkable)

# globals (all inside the mapped ELF image)
G_DT         = 0x0060EA1C
G_CLOCK      = 0x0060EA20
G_RAND_SCALE = 0x0054F46C
G_LOAD_SCALE = 0x005A8054
G_ALT_DT     = 0x004D617E

RAND_SCALE = 2.3283064365386963e-10
LOAD_SCALE = 23.8164005279541

# ---------------------------------------------------------------------------
# synthetic memory layout
# ---------------------------------------------------------------------------
VEHICLE   = 0x30000000
RACECAR   = 0x30010000
CONFIG    = 0x30014000
CTX0      = 0x30018000   # frame object (v+0x204 == v+0xCC0): 4x4 + file ptr
FILEIMG   = 0x3001C000   # .bgv header stub (+0xC/+0xD/+0x14/+0x18/+0xE80..)
CTX1      = 0x30020000   # damage/visual ctx (v+0xCC4)
WHEELF    = 0x30024000   # 4 x 0x40 wheel frame matrices (v+0xCC8..)
SOUP_HDR  = 0x30028000   # {count, records*, types*}
SOUP_REC  = 0x30028040
SOUP_TYPE = 0x30028800
REGION_LO = 0x30000000
REGION_SZ = 0x2A000

STACK_BASE = 0x20000000
STACK_SIZE = 0x100000
MAGIC_RET  = 0x50000000

# COMPCAR1 (pveh/COMP/Car1.bgv + Data/vdb.xml), all [C] recovered values
CAR = dict(
    mass=800.0,
    front_attach=0.11, front_k=54000.0, front_c=5300.0, front_len=0.18,
    rear_k=56000.0, rear_c=5300.0, rear_len=0.18, rear_attach=0.11,
    gears=[-4.0, 0.0, 3.8, 2.3, 1.7, 1.37, 1.16, 1.11, 3.2],
    idle=1000.0, up=6000.0, down=5200.0, maxrpm=7600.0, torque=570.0,
    peak=5400.0, falloff=6800.0, kick_t=4.0, kick_time=2.0,
    drag=1.0, downforce=0.0,
    brake_h=-0.21, accel_h=-0.23, steer_h=-0.23, drift_h=-0.23,
    smin_ang=None, smax_ang=None, smin_vel=None, smax_vel=None, sresp=None,
    # filled from the VDB table below
    radius=0.3117, dim=2.6764,
    ext=(1.0157, 1.1222, 2.0636, 0.0),
    ext2=(-1.0157, -0.1505, -2.0866, 2.0636),
    # wheel local positions from Car1.bgv +0xB80 (front, front, rear, rear)
    wheels_xz=[(-0.7600, 1.2379), (0.7600, 1.2379),
               (-0.7600, -1.3067), (0.7600, -1.3067)],
    inv_inertia=(0.0008, 0.0011, 0.0013),   # FUN_001203A0 default class
)


def _load_car_vdb():
    """Pull the full COMPCAR1 64-param set out of burnout3_car_physics.h so
    the seeded config is the real one, not a hand-copied subset."""
    path = os.path.join(_here, '..', 'src', 'burnout3_car_physics.h')
    params = {}
    with open(path) as f:
        active = False
        for line in f:
            if 'B3_CARPARAMS_COMPCAR1[]' in line:
                active = True
                continue
            if active:
                if line.strip().startswith('};'):
                    break
                line = line.strip()
                if line.startswith('{'):
                    body = line[1:line.index('}')]
                    off_s, val_s = body.split(',')[:2]
                    params[int(off_s.strip().rstrip('u'), 0)] = \
                        float(val_s.strip().rstrip('f'))
    return params

CFG = _load_car_vdb()          # offset -> value, the real 0x1D0 struct image


def f2u(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


class Pipeline:
    """Persistent emulation session: one Uc across frames."""

    def __init__(self, dt=1.0 / 60.0):
        self.dt = dt
        self.clock = 0.0
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        ev.load_elf(uc, os.path.join(_here, '..', ev.ELF)
                    if not os.path.exists(ev.ELF) else ev.ELF)
        uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        uc.mem_map(REGION_LO, REGION_SZ, UC_PROT_ALL)
        uc.mem_map(MAGIC_RET & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        self.uc = uc
        self._seed_globals()
        self._seed_objects()
        self._seed_vehicle()

    # -- memory helpers ----------------------------------------------------
    def _on_unmapped(self, uc, access, address, size, value, user):
        page = address & ~(PAGE - 1)
        try:
            uc.mem_map(page, PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    def wf(self, addr, val):
        self.uc.mem_write(addr, struct.pack('<f', float(val)))

    def wu(self, addr, val):
        self.uc.mem_write(addr, struct.pack('<I', val & 0xFFFFFFFF))

    def wb(self, addr, val):
        self.uc.mem_write(addr, bytes([val & 0xFF]))

    def rf(self, addr):
        return struct.unpack('<f', self.uc.mem_read(addr, 4))[0]

    def ru(self, addr):
        return struct.unpack('<I', self.uc.mem_read(addr, 4))[0]

    def rb(self, addr):
        return self.uc.mem_read(addr, 1)[0]

    def vf(self, off):          # vehicle float
        return self.rf(VEHICLE + off)

    def vu(self, off):
        return self.ru(VEHICLE + off)

    def vb(self, off):
        return self.rb(VEHICLE + off)

    # -- seeding -----------------------------------------------------------
    def _seed_globals(self):
        self.wf(G_DT, self.dt)
        self.wf(G_CLOCK, self.clock)
        self.wf(G_RAND_SCALE, RAND_SCALE)
        self.wf(G_LOAD_SCALE, LOAD_SCALE)
        self.wb(G_ALT_DT, 0)

    def _seed_objects(self):
        V = self
        # racecar/owner object (v+0x13F4)
        V.wb(RACECAR + 0x19A8, 1)          # pipeline gate
        V.wu(RACECAR + 0x1920, 0)          # mode 0: normal (2 x dt/2)
        V.wu(RACECAR + 0x134C, 1)          # not traffic
        V.wu(RACECAR + 0x179C, 1)          # racing driver path in ECF0
        V.wu(RACECAR + 0x1198, RACECAR)    # slam-clock object -> itself
        V.wf(RACECAR + 0x1598, -1.0)       # slam time: never
        V.wf(RACECAR + 0x1690, -1.0)       # second slam clock: never
        V.wf(RACECAR + 0x10DC, 0.0)        # its clock
        V.wf(RACECAR + 0x1350, -100.0)     # last crash long past
        # config object (v+0x13F8): full real 0x1D0 image
        for off, val in CFG.items():
            V.wf(CONFIG + off, val)
        # frame object (v+0x204 == v+0xCC0)
        self._write_matrix(CTX0, [[1, 0, 0, 0], [0, 1, 0, 0],
                                  [0, 0, 1, 0], [0, 0.31, 0, 0]])
        V.wu(CTX0 + 0x40, FILEIMG)
        # .bgv header stub
        V.wb(FILEIMG + 0xC, 0)             # numBodyParts: none
        V.wb(FILEIMG + 0xD, 4)             # numWheels
        V.wf(FILEIMG + 0x14, CAR['dim'])
        V.wf(FILEIMG + 0x18, CAR['radius'])
        for i, x in enumerate(CAR['ext']):
            V.wf(FILEIMG + 0xE80 + 4 * i, x)
        for i, x in enumerate(CAR['ext2']):
            V.wf(FILEIMG + 0xE90 + 4 * i, x)
        # damage ctx (v+0xCC4)
        V.wf(CTX1 + 0x324, 1.2)            # grip scalar
        V.wf(CTX1 + 0x49C, 10000.0)        # ground clearance sentinel
        for i in range(4):
            V.wb(CTX1 + 0x4AC + i, 0)      # wheel states pristine
            V.wf(CTX1 + 0x4D0 + 4 * i, CAR['radius'])
        # wheel frames
        for i, (x, z) in enumerate(CAR['wheels_xz']):
            self._write_matrix(WHEELF + 0x40 * i,
                               [[1, 0, 0, 0], [0, 1, 0, 0],
                                [0, 0, 1, 0], [x, 0.0, z, 0]])
        # ground soup: flat plane y=0, two triangles, surface type 0
        S = 5000.0
        V.wu(SOUP_HDR + 0, 2)
        V.wu(SOUP_HDR + 4, SOUP_REC)
        V.wu(SOUP_HDR + 8, SOUP_TYPE)
        tris = [((-S, 0, -S), (-S, 0, S), (S, 0, -S)),
                ((S, 0, S), (S, 0, -S), (-S, 0, S))]
        for t, (p0, p1, p2) in enumerate(tris):
            base = SOUP_REC + 0x40 * t
            for j, p in enumerate((p0, p1, p2)):
                for k in range(3):
                    V.wf(base + 0x10 * j + 4 * k, p[k])
                V.wf(base + 0x10 * j + 12, 0.0)
            V.wf(base + 0x30, 0.0)
            V.wf(base + 0x34, 1.0)
            V.wf(base + 0x38, 0.0)
            V.wf(base + 0x3C, 0.0)
            self.uc.mem_write(SOUP_TYPE + 2 * t, struct.pack('<H', 0))

    def _write_matrix(self, addr, rows):
        for r in range(4):
            for c in range(4):
                self.wf(addr + 16 * r + 4 * c, rows[r][c])

    def _seed_vehicle(self):
        V = self
        base = VEHICLE
        # rigid body (FUN_00109270 + FUN_00109190 default class diag)
        ix, iy, iz = CAR['inv_inertia']
        self._write_matrix(base + 0x10, [[ix, 0, 0, 0], [0, iy, 0, 0],
                                         [0, 0, iz, 0], [0, 0, 0, 0]])
        self._write_matrix(base + 0x40, [[ix, 0, 0, 0], [0, iy, 0, 0],
                                         [0, 0, iz, 0], [0, 0, 0, 0]])
        self._write_matrix(base + 0x70, [[1, 0, 0, 0], [0, 1, 0, 0],
                                         [0, 0, 1, 0], [0, -0.31, 0, 0]])
        for off in range(0xB0, 0x140, 4):
            V.wf(base + off, 0.0)
        V.wf(base + 0xC8, 1.0)             # dir = +z (FUN_00109270)
        V.wf(base + 0x140, 1.0)
        # pointers
        V.wu(base + 0x200, SOUP_HDR)
        V.wu(base + 0x204, CTX0)
        V.wu(base + 0xCC0, CTX0)
        V.wu(base + 0xCC4, CTX1)
        for i in range(4):
            V.wu(base + 0xCC8 + 4 * i, WHEELF + 0x40 * i)
        V.wu(base + 0x13F4, RACECAR)
        V.wu(base + 0x13F8, CONFIG)
        V.wu(base + 0x1568, RACECAR)
        V.wu(base + 0x14D8, VEHICLE)
        # geometry/mass (FUN_00122830 / FUN_0011A8F0)
        V.wf(base + 0x1CC, CAR['dim'])
        for i, x in enumerate(CAR['ext']):
            V.wf(base + 0x1D0 + 4 * i, x)
        for i, x in enumerate(CAR['ext2']):
            V.wf(base + 0x1E0 + 4 * i, x)
        V.wf(base + 0x1F0, CAR['mass'])
        V.wf(base + 0x1F4, (CAR['ext'][1] - CAR['ext2'][1]) * 0.1)
        V.wf(base + 0x1F8, 0.1)
        V.wf(base + 0x1FC, -0.75)
        V.wu(base + 0x208, VEHICLE + 0x220)
        for off in range(0x20C, 0x220):
            V.wb(base + off, 0)
        V.wb(base + 0x20C, 2)
        V.wb(base + 0x215, 3)              # normal race state
        V.wb(base + 0x218, 0xFF)
        # wheels
        V.wb(base + 0x1169, 4)
        V.wb(base + 0x116A, 4)
        V.wb(base + 0x116B, 0)
        cfg = CFG
        for i in range(4):
            w = base + 0x820 + 0xC0 * i
            self.uc.mem_write(w, b'\0' * 0xC0)
            attach = cfg[0xBC] if i < 2 else cfg[0xD8]
            length = cfg[0xC8] if i < 2 else cfg[0xD4]
            V.wf(w + 0x50, CAR['radius'])
            V.wf(w + 0x60, attach - 0.75 * length)   # droop spawn
            V.wf(w + 0x64, attach - 0.75 * length)
            V.wf(w + 0x6C, 1.0)
            V.wf(w + 0x70, -1.0 if (i & 1) else 1.0)
            V.wf(w + 0x74, attach)
        # config -> live copy (FUN_00134710), full mirror
        V.wf(base + 0x1F0, cfg[0xB8])
        V.wf(base + 0xCA0, cfg[0xBC]); V.wf(base + 0xCA8, cfg[0xC0])
        V.wf(base + 0xCA4, cfg[0xC4]); V.wf(base + 0xCAC, cfg[0xC8])
        V.wf(base + 0xCB8, cfg[0xCC]); V.wf(base + 0xCB4, cfg[0xD0])
        V.wf(base + 0xCBC, cfg[0xD4]); V.wf(base + 0xCB0, cfg[0xD8])
        for i in range(9):
            V.wf(base + 0x1448 + 4 * i, cfg[0xE0 + 4 * i])
        V.wf(base + 0x146C, cfg[0x104]); V.wf(base + 0x1470, cfg[0x108])
        V.wf(base + 0x1474, cfg[0x10C]); V.wf(base + 0x1480, cfg[0x118])
        V.wf(base + 0x1484, cfg[0x11C])
        V.wf(base + 0x148C, cfg[0x120] * 0.10471976)
        V.wf(base + 0x1490, cfg[0x124] * 0.10471976)
        V.wf(base + 0x1494, cfg[0x128]); V.wf(base + 0x1498, cfg[0x12C])
        live_map = {0x1360: 0x130, 0x1364: 0x134, 0x1368: 0x138,
                    0x136C: 0x13C, 0x1370: 0x140, 0x1374: 0x144,
                    0x1378: 0x148, 0x137C: 0x14C, 0x1380: 0x150,
                    0x1384: 0x154, 0x1388: 0x158, 0x138C: 0x15C,
                    0x1390: 0x160, 0x1394: 0x164, 0x1398: 0x168,
                    0x139C: 0x16C, 0x13A0: 0x170, 0x13A4: 0x174,
                    0x13A8: 0x178, 0x13AC: 0x17C, 0x13B0: 0x180,
                    0x13B4: 0x184, 0x13B8: 0x188, 0x13BC: 0x18C,
                    0x13C0: 0x190, 0x13C4: 0x194, 0x13C8: 0x198,
                    0x13CC: 0x19C, 0x13D8: 0x1B0,
                    0x13E0: 0x1BC, 0x13E4: 0x1C0, 0x13E8: 0x1C4,
                    0x13EC: 0x1C8, 0x13F0: 0x1CC}
        for live, coff in live_map.items():
            V.wf(base + live, cfg.get(coff, 0.0))
        V.wf(base + 0x13D0, math.cos(cfg[0x1A0] * 0.017453292))
        V.wf(base + 0x13DC, math.cos((90.0 - cfg[0x1B4]) * 0.017453292))
        V.wf(base + 0x13D4, cfg[0x1A4])
        # input/drift state (FUN_0011A8F0)
        for off in range(0x13FC, 0x1448, 4):
            V.wu(base + off, 0)
        V.wf(base + 0x1430, cfg[0x160]); V.wf(base + 0x1440, cfg[0x160])
        V.wu(base + 0x1524, 0)
        V.wf(base + 0x152C, -1.0)
        V.wf(base + 0x1534, 1.0)
        V.wf(base + 0x1520, 0.0)
        for off in (0x1530, 0x1538):
            V.wf(base + off, 0.0)
        for off in (0x153C, 0x153D, 0x153E, 0x153F, 0x1540,
                    0x1550, 0x1551, 0x1552, 0x1553):
            V.wb(base + off, 0)
        V.wb(base + 0x1550, 1)
        self._write_matrix(base + 0x14E0, [[1, 0, 0, 0], [0, 1, 0, 0],
                                           [0, 0, 1, 0], [0, 0, 0, 0]])
        V.wu(base + 0x1160, 0)
        V.wb(base + 0x1164, 0); V.wf(base + 0x1164, 0.0)
        V.wb(base + 0x1168, 0)
        V.wf(base + 0x1414, 0.0)
        # transmission reset (FUN_001214A0)
        V.wf(base + 0x1488, cfg[0x118])
        V.wu(base + 0x14B0, 0xFD462907)
        V.wu(base + 0x14B4, 0x02B9D6F8)
        for off in (0x14A0, 0x14A4, 0x14A8, 0x14AC, 0x14B8, 0x14BC,
                    0x14C0, 0x14C4, 0x14D0, 0x14D4):
            V.wu(base + off, 0)
        V.wf(base + 0x149C, cfg[0x104] * 0.10471976)   # idle
        V.wu(base + 0x14C8, 0)
        ngears = 0
        while ngears < 6 and cfg[0xE8 + 4 * ngears] > 0.0:
            ngears += 1
        V.wu(base + 0x14CC, ngears)

    # -- calling real functions -------------------------------------------
    def call(self, addr, stack_args=(), regs=None, max_steps=8_000_000):
        uc = self.uc
        sp = STACK_BASE + STACK_SIZE - 0x2000
        # determinism: the retail code reads a few uninitialised stack
        # slots (see RE_NOTES 8.2/14); zero the frame region every call
        uc.mem_write(sp - 0x1800, b'\0' * 0x2000)
        uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
        for i, a in enumerate(stack_args):
            uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
        uc.reg_write(UC_X86_REG_ESP, sp)
        for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                  UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI):
            uc.reg_write(r, VEHICLE)
        for r, val in (regs or {}).items():
            uc.reg_write(r, val)
        try:
            uc.emu_start(addr, MAGIC_RET, count=max_steps)
        except UcError as e:
            raise RuntimeError("0x%08X faulted: %s @ 0x%08X"
                               % (addr, e, uc.reg_read(UC_X86_REG_EIP)))

    # -- one frame ---------------------------------------------------------
    def frame(self, throttle=0.0, brake=0.0, steer=0.0, boost=0,
              run_skipped=False):
        V = self
        base = VEHICLE
        dt = self.dt
        # DAT_0060EA20 is a FLOAT the game advances by dt every frame, and so
        # is the port's mirror; accumulating it in Python double drifts ~1e-6
        # away from both, which is enough to flip a strictly-compared window
        # boundary (the steer-away phase edges land exactly on a frame).
        self.clock = struct.unpack('<f', struct.pack('<f', self.clock + dt))[0]
        self.wf(G_DT, dt)
        self.wf(G_CLOCK, self.clock)
        self.wf(RACECAR + 0x10DC, self.clock)

        # FUN_00104840 mirror: contact scratch zeroing
        V.wb(base + 0x212, 0)
        self.uc.mem_write(base + 0x160, b'\0' * 0x40)

        # [FUN_0011BC60 STUB] ground collection -> flat plane refill
        V.wu(SOUP_HDR + 0, 2)

        # driver inputs (FUN_00104D30): raw throttle 0x1414, brake 0x1404,
        # steer 0x1408, boost input bit 0x13FC&4
        V.wf(base + 0x1414, throttle)
        V.wf(base + 0x1404, brake)
        V.wf(base + 0x1408, steer)
        V.wb(base + 0x13FC, 4 if boost else 0)
        if V.vf(0xBC) < 0.1:
            V.wf(base + 0x1408, 0.0)
        V.wf(base + 0x1438, V.vf(0x1438))
        th = throttle * V.vf(0x13BC)
        V.wf(base + 0x1400, min(th, 1.0))

        # FUN_0011ECF0 (real): ECX = vehicle
        self.call(F_ECF0, regs={UC_X86_REG_ECX: VEHICLE,
                                UC_X86_REG_EBX: 0, UC_X86_REG_EDI: 0,
                                UC_X86_REG_ESI: 0})

        # BE50 main path, mode 0: two substeps at dt/2, wheel snapshot
        snap = bytes(self.uc.mem_read(base + 0x820, 0x30 * 16))
        snaps = [bytes(self.uc.mem_read(base + 0x820 + 0xC0 * i, 0x20))
                 for i in range(4)]
        dt2 = dt * 0.5
        for _ in range(2):
            self.call(F_D460, stack_args=[VEHICLE, f2u(dt2)])
            V.wb(base + 0x212, 0)
            V.wb(base + 0x213, 0)
            # [FUN_0011AEF0 STUB] returns 0 -> BE50 clears crash state 3
            if V.vu(0x1524) == 3:
                V.wu(base + 0x1524, 0)
            self.call(F_239C0, stack_args=[VEHICLE])
            self.call(F_23FD0, stack_args=[VEHICLE, f2u(dt2)])
            # BE50 stop-check
            spd = V.vf(0xBC)
            thl = V.vf(0x1400)
            r179c = self.ru(RACECAR + 0x179C)
            if (spd < 0.5 and thl <= 0.1) or r179c in (0, 2):
                for off in (0xB0, 0xB4, 0xB8, 0xBC, 0xF0, 0xF8):
                    V.wf(base + off, 0.0)
            self.call(F_09560, stack_args=[f2u(dt2)],
                      regs={UC_X86_REG_ECX: VEHICLE})
        # substep tail: wheel prev-frame records = pre-frame values
        for i in range(4):
            self.uc.mem_write(base + 0x850 + 0xC0 * i, snaps[i])
        # timer 0x152C
        t = V.vf(0x152C)
        if t >= -0.0001:
            V.wf(base + 0x152C, t - dt)
        if run_skipped:
            self.call(F_18DA00, regs={UC_X86_REG_ESI: RACECAR})
            self.call(F_FFA0, regs={UC_X86_REG_ESI: VEHICLE})
        self.call(F_C720, regs={UC_X86_REG_EAX: VEHICLE})
        return self.capture()

    # -- state capture -----------------------------------------------------
    def capture(self):
        V = self
        pos = [self.rf(CTX0 + 0x30 + 4 * i) for i in range(3)]
        cap = dict(
            pos=pos,
            vel=[V.vf(0xB0 + 4 * i) for i in range(3)],
            speed=V.vf(0xBC),
            dir=[V.vf(0xC0 + 4 * i) for i in range(3)],
            omega=[V.vf(0xD0 + 4 * i) for i in range(3)],
            angmom=[V.vf(0xE0 + 4 * i) for i in range(3)],
            at=[self.rf(CTX0 + 0x20 + 4 * i) for i in range(3)],
            up=[self.rf(CTX0 + 0x10 + 4 * i) for i in range(3)],
            rpm=V.vf(0x149C) * 9.549296,
            gear=struct.unpack('<i', struct.pack('<I', V.vu(0x14C8)))[0],
            torque=V.vf(0x1520),
            steer_deg=V.vf(0x1164),
            drift=V.vu(0x1524),
            slide=V.vf(0x1440),
            airborne=V.vb(0x1168),
            wheels=[dict(cur=V.vf(0x820 + 0xC0 * i + 0x64),
                         prev=V.vf(0x820 + 0xC0 * i + 0x60),
                         omega=V.vf(0x820 + 0xC0 * i + 0x5C),
                         spin=V.vf(0x820 + 0xC0 * i + 0x58),
                         contact=V.vb(0x820 + 0xC0 * i + 0xB3))
                    for i in range(4)],
        )
        return cap


    # -- full state export (for reseeding the C port at a checkpoint) ------
    def export_state(self):
        """Everything b3_vehicle_step_full tracks, as a flat name->value
        dict (floats/ints). Written as 'key value' lines for dump_traj.c."""
        V = self
        st = {}
        for r in range(4):
            for c in range(4):
                st['frame_%d_%d' % (r, c)] = self.rf(CTX0 + 16 * r + 4 * c)
        for i, name in enumerate(('vel_x', 'vel_y', 'vel_z', 'speed')):
            st[name] = V.vf(0xB0 + 4 * i)
        for i in range(3):
            st['dir_%d' % i] = V.vf(0xC0 + 4 * i)
            st['omega_%d' % i] = V.vf(0xD0 + 4 * i)
            st['angmom_%d' % i] = V.vf(0xE0 + 4 * i)
            st['defl_%d' % i] = V.vf(0x130 + 4 * i)
        for i in range(4):
            b = 0x820 + 0xC0 * i
            for j, nm in enumerate(('wp', 'cp', 'pp', 'pc')):
                for k in range(3):
                    st['w%d_%s_%d' % (i, nm, k)] = V.vf(b + 0x10 * j + 4 * k)
            st['w%d_n_0' % i] = V.vf(b + 0x20)
            st['w%d_n_1' % i] = V.vf(b + 0x24)
            st['w%d_n_2' % i] = V.vf(b + 0x28)
            st['w%d_torque' % i] = V.vf(b + 0x54)
            st['w%d_spin' % i] = V.vf(b + 0x58)
            st['w%d_omega' % i] = V.vf(b + 0x5C)
            st['w%d_prev' % i] = V.vf(b + 0x60)
            st['w%d_cur' % i] = V.vf(b + 0x64)
            st['w%d_contact' % i] = V.vb(b + 0xB3)
        # transmission
        st['t_omega'] = V.vf(0x149C)
        st['t_timer'] = V.vf(0x14A0)
        st['t_shifting'] = V.vu(0x14A4)
        st['t_noupshift'] = V.vu(0x14A8)
        st['t_limit'] = V.vf(0x1488)
        st['t_rng_a'] = V.vu(0x14B0)
        st['t_rng_c'] = V.vu(0x14B4)
        st['t_gear'] = struct.unpack('<i', struct.pack('<I',
                                                       V.vu(0x14C8)))[0]
        st['t_upblk'] = V.vf(0x14D0)
        st['t_dnblk'] = V.vf(0x14D4)
        # input/drift state
        st['thr_prev'] = V.vf(0x141C)
        st['brake_prev'] = V.vf(0x1420)
        st['steer_prev'] = V.vf(0x1424)
        st['drift_time'] = V.vf(0x142C)
        st['slide_prev'] = V.vf(0x1430)
        st['drift_timer'] = V.vf(0x1438)
        st['airtime'] = V.vf(0x143C)
        st['slide'] = V.vf(0x1440)
        st['drift_state'] = V.vu(0x1524)
        st['f1168'] = V.vb(0x1168)
        st['timer_152C'] = V.vf(0x152C)
        st['clock'] = self.clock
        st['grip'] = self.rf(CTX1 + 0x324)
        # control-state bytes + the aggressive-driving-reaction clocks the
        # steer-away envelope in FUN_0011ECF0 keys off
        st['class_215'] = V.vb(0x215)
        st['contact_212'] = V.vb(0x212)
        st['hit_side_153C'] = V.vb(0x153C)
        st['ooc_slam_1598'] = self.rf(RACECAR + 0x1598)
        st['ooc_wall_1690'] = self.rf(RACECAR + 0x1690)
        st['launch_1350'] = self.rf(RACECAR + 0x1350)
        st['flag_b_1446'] = V.vb(0x1446)
        return st

    # -- adversarial state edits (applied AT a checkpoint) -----------------
    def mutate(self, mut):
        """Edit the emulated body so the exported state hands the C port an
        IDENTICAL starting condition. Used by validate_port.py's adversarial
        trajectories (lateral kicks, spins, airborne re-entry, OOC windows).
        Keys: angmom_add, vel_set, pos_add, frame_rows, wheels_airborne,
        airtime, f1168, class_215, hit_side, slam_1598, wall_1690."""
        V = self
        base = VEHICLE
        if 'pos_add' in mut:
            for i, d in enumerate(mut['pos_add']):
                self.wf(CTX0 + 0x30 + 4 * i, self.rf(CTX0 + 0x30 + 4 * i) + d)
        if 'frame_rows' in mut:
            for r, row in mut['frame_rows'].items():
                for c, val in enumerate(row):
                    self.wf(CTX0 + 16 * r + 4 * c, val)
        if 'vel_set' in mut:
            v3 = mut['vel_set']
            for i in range(3):
                V.wf(base + 0xB0 + 4 * i, v3[i])
            sp = math.sqrt(sum(x * x for x in v3))
            V.wf(base + 0xBC, sp)
            if sp > 1e-6:
                for i in range(3):
                    V.wf(base + 0xC0 + 4 * i, v3[i] / sp)
        if 'angmom_add' in mut:
            L = [V.vf(0xE0 + 4 * i) + mut['angmom_add'][i] for i in range(3)]
            for i in range(3):
                V.wf(base + 0xE0 + 4 * i, L[i])
            # omega = L . M, the same product FUN_00109560 forms from the
            # world inverse-inertia rows at +0x40
            for j in range(3):
                w = sum(L[i] * V.vf(0x40 + 16 * i + 4 * j) for i in range(3))
                V.wf(base + 0xD0 + 4 * j, w)
        if mut.get('wheels_airborne'):
            for i in range(4):
                V.wb(base + 0x820 + 0xC0 * i + 0xB3, 0)
        if 'airtime' in mut:
            V.wf(base + 0x143C, mut['airtime'])
        if 'f1168' in mut:
            V.wb(base + 0x1168, mut['f1168'])
        if 'class_215' in mut:
            V.wb(base + 0x215, mut['class_215'])
        if 'hit_side' in mut:
            V.wb(base + 0x153C, mut['hit_side'])
        if 'slam_1598' in mut:
            self.wf(RACECAR + 0x1598, mut['slam_1598'])
        if 'wall_1690' in mut:
            self.wf(RACECAR + 0x1690, mut['wall_1690'])

    def write_state(self, path):
        """State file for dump_traj.c --state. Integers (PRNG words, gear,
        flags) must be written exactly -- %.9g on a 32-bit PRNG word loses
        low bits and desyncs the torque flutter stream."""
        with open(path, 'w') as f:
            for k, v in self.export_state().items():
                if isinstance(v, int):
                    f.write('%s %d\n' % (k, v))
                else:
                    f.write('%s %.9g\n' % (k, v))


SCENARIOS = {
    # name: list of (frames, throttle, brake, steer, boost)
    'accelerate': [(60, 0.0, 0.0, 0.0, 0), (240, 1.0, 0.0, 0.0, 0)],
    'corner':     [(60, 0.0, 0.0, 0.0, 0), (180, 1.0, 0.0, 0.0, 0),
                   (120, 0.6, 0.0, 0.5, 0)],
    'brake':      [(60, 0.0, 0.0, 0.0, 0), (180, 1.0, 0.0, 0.0, 0),
                   (150, 0.0, 1.0, 0.0, 0)],
}


def scenario_inputs(name):
    """Flat per-frame input list [(th, br, st, boost), ...]."""
    out = []
    for frames, th, br, st, bo in SCENARIOS[name]:
        out += [(th, br, st, bo)] * frames
    return out


def run_scenario(name, dt=1.0 / 60.0, run_skipped=False):
    p = Pipeline(dt=dt)
    traj = []
    for frames, th, br, st, bo in SCENARIOS[name]:
        for _ in range(frames):
            traj.append(p.frame(th, br, st, bo, run_skipped=run_skipped))
    return traj


def main():
    which = sys.argv[1:] or list(SCENARIOS)
    for name in which:
        traj = run_scenario(name)
        out = os.path.join(_here, '..', 'build', 'traj_%s.json' % name)
        with open(out, 'w') as f:
            json.dump(traj, f)
        last = traj[-1]
        print("%-12s %d frames -> pos (%.2f, %.2f, %.2f)  speed %6.2f m/s "
              "(%5.1f mph)  gear %d  rpm %5.0f  drift %d" %
              (name, len(traj), last['pos'][0], last['pos'][1],
               last['pos'][2], last['speed'],
               last['speed'] * 2.2374146, last['gear'], last['rpm'],
               last['drift']))
        for i in (0, 30, 60, 120, 180, 240, len(traj) - 1):
            if i < len(traj):
                t = traj[i]
                print("  f%3d pos(%7.2f,%6.3f,%8.2f) v(%6.2f,%6.2f,%6.2f) "
                      "sp %6.2f g %d rpm %5.0f w0(cur %.3f om %6.1f c%d)" %
                      (i, t['pos'][0], t['pos'][1], t['pos'][2],
                       t['vel'][0], t['vel'][1], t['vel'][2], t['speed'],
                       t['gear'], t['rpm'], t['wheels'][0]['cur'],
                       t['wheels'][0]['omega'], t['wheels'][0]['contact']))


if __name__ == '__main__':
    main()
