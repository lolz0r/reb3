#!/usr/bin/env python3
"""
Recover Burnout 3's vehicle physics parameter table.

FUN_00132D10 registers every tunable physics value with the game's ValueDB.
Each registration compiles to:

    LEA <reg>,[ESI + 0xNNN]     ; address of the field inside the physics struct
    ...
    MOV ECX, <group string VA>  ; e.g. "Physics/Transmission/Engine"
    MOV EDX, <name string VA>   ; e.g. "Peak Torque Revs"
    CALL 0x001AEE20             ; register(name, group, &field, ...)

Walking the disassembly backwards from each CALL yields name -> group -> struct
offset for the whole model. Strings are read straight out of the XBE .rdata.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import json, re, struct, sys, urllib.request

MCP = "http://127.0.0.1:8089"
REGISTER_FN = "0x001aee20"

# Two registrars, two different config structs.
#   FUN_00132D10 -- full race-car config: key at +0xB0/+0xB4, params from +0xB8,
#                   stride 0x1D0. Defaults in FUN_00132950.
#   FUN_00134AC0 -- reduced config: key at +0x80/+0x84, params from +0x88. Only
#                   mass and the two suspension sets, i.e. no gearbox, engine or
#                   drift model -- consistent with traffic vehicles.
FUNC = "0x00132d10"
REGISTRARS = [("race", "0x00132d10", "0x00132950"),
              ("simple", "0x00134ac0", None)]

XBE = (game_path('default.xbe'))
RDATA_VA, RDATA_RAW, RDATA_SIZE = 0x0036B7C0, 0x0035C000, 0x00046B94


def load_strings():
    d = open(XBE, 'rb').read()

    def cstr(va):
        if not (RDATA_VA <= va < RDATA_VA + RDATA_SIZE):
            return None
        o = RDATA_RAW + (va - RDATA_VA)
        e = d.index(b'\0', o)
        if e - o > 96:
            return None
        try:
            return d[o:e].decode('ascii')
        except UnicodeDecodeError:
            return None
    return cstr


DEFAULTS_FN = "0x00132950"

# In the defaults constructor Ghidra renders each store as
#   *(undefined4 *)(param_1 + 0x130) = 0x3ff33333;
# and occasionally mistakes a float immediate for a pointer:
#   *(undefined1 **)(param_1 + 0xd4) = &DAT_3e4ccccd;
# Both forms carry the same 32-bit pattern, which is the IEEE-754 float.
STORE = re.compile(
    r'\(param_1 \+ (0x[0-9a-f]+|\d+)\) = (?:&DAT_([0-9a-f]{8})|(0x[0-9a-f]+|\d+));')


def disassemble():
    url = "%s/disassemble_function?address=%s&limit=8000" % (MCP, FUNC)
    return json.loads(urllib.request.urlopen(url, timeout=300).read())['instructions']


def decompile(addr):
    url = "%s/decompile_function?address=%s" % (MCP, addr)
    return json.loads(urllib.request.urlopen(url, timeout=300).read()).get('decompiled', '')


def extract_defaults():
    """offset -> float default, from the constructor at FUN_00132950."""
    out = {}
    for m in STORE.finditer(decompile(DEFAULTS_FN)):
        off = int(m.group(1), 0)
        bits = int(m.group(2), 16) if m.group(2) else int(m.group(3), 0)
        out[off] = struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]
    return out


IMM = re.compile(r'^MOV (E[A-D]X),(0x[0-9a-f]+)$')
LEA = re.compile(r'^LEA (E[A-D]I?X?|E[SD]I),\[ESI \+ (0x[0-9a-f]+)\]$')
# The final registration in the function folds the displacement into ESI itself
# (ADD ESI,0x1cc / PUSH ESI) rather than using LEA, since ESI is dead after it.
ADDESI = re.compile(r'^ADD ESI,(0x[0-9a-f]+)$')


def extract():
    cstr = load_strings()
    ins = disassemble()
    rows, seen = [], set()

    for i, x in enumerate(ins):
        if not x['instruction'].startswith('CALL ' + REGISTER_FN):
            continue
        name = group = offset = None
        # Walk back to the operands feeding this call. 40 instructions is well
        # clear of the longest observed setup block (~26).
        for y in ins[max(0, i - 40):i][::-1]:
            t = y['instruction']
            m = IMM.match(t)
            if m:
                reg, va = m.group(1), int(m.group(2), 16)
                if reg == 'EDX' and name is None:
                    name = cstr(va)
                elif reg == 'ECX' and group is None:
                    group = cstr(va)
                continue
            m = LEA.match(t)
            if m and offset is None:
                offset = int(m.group(2), 16)
                continue
            m = ADDESI.match(t)
            if m and offset is None:
                offset = int(m.group(1), 16)
        if name and group and offset is not None:
            key = (group, name, offset)
            if key not in seen:
                seen.add(key)
                rows.append({'group': group, 'name': name, 'offset': offset})
    return rows


def cfloat(v):
    """Format as a C float literal. %g alone yields '1000f' for whole numbers."""
    s = '%.9g' % v
    if not any(c in s for c in '.eEnN'):
        s += '.0'
    return s + 'f'


def ident(s):
    s = re.sub(r'[^0-9A-Za-z]+', '_', s).strip('_').lower()
    return 'p_' + s if s[:1].isdigit() else s


def main():
    rows = extract()
    defaults = extract_defaults()
    if not rows:
        print('no registrations recovered', file=sys.stderr)
        return 1

    rows.sort(key=lambda r: r['offset'])
    groups = {}
    for r in rows:
        groups.setdefault(r['group'], []).append(r)

    for g in sorted(groups):
        print('\n%s' % g)
        for r in groups[g]:
            d = defaults.get(r['offset'])
            print('    +0x%03X  %-30s %s' % (
                r['offset'], r['name'],
                ('%.6g' % d) if d is not None else '(no default)'))
    have = sum(1 for r in rows if r['offset'] in defaults)
    print('\n%d parameters across %d groups; struct span +0x%03X..+0x%03X (stride 0x1D0)'
          % (len(rows), len(groups), rows[0]['offset'], rows[-1]['offset']))
    print('%d/%d have a compiled-in default from FUN_00132950' % (have, len(rows)))

    out = sys.argv[1] if len(sys.argv) > 1 else 'src/burnout3_physics_params.h'
    lines = [
        '// Generated by tools/extract_physics_params.py -- do not edit by hand.',
        '// Burnout 3 vehicle physics parameter table, recovered from the',
        '// registration function at 0x00132D10. Offsets are byte offsets into the',
        '// game\'s per-vehicle physics struct; all fields are f32.',
        '#ifndef BURNOUT3_PHYSICS_PARAMS_H',
        '#define BURNOUT3_PHYSICS_PARAMS_H', '',
        'typedef struct {',
        '    const char* group;',
        '    const char* name;',
        '    unsigned    offset;   // byte offset into the 0x1D0-byte physics struct',
        '    float       def;      // compiled-in default (FUN_00132950)',
        '    int         has_def;',
        '} PhysicsParam;', '',
        '#define PHYSICS_PARAM_COUNT %d' % len(rows), '',
        'static const PhysicsParam PHYSICS_PARAMS[PHYSICS_PARAM_COUNT] = {',
    ]
    for g in sorted(groups):
        lines.append('    /* %s */' % g)
        for r in groups[g]:
            d = defaults.get(r['offset'])
            lines.append('    { "%s", "%s", 0x%03Xu, %s, %d },'
                         % (g, r['name'], r['offset'], cfloat(d if d is not None else 0.0),
                            1 if d is not None else 0))
    lines += ['};', '',
              '#define B3_PHYSICS_STRUCT_SIZE 0x1D0u', '']

    lines.append('// Field offsets, for direct access into the physics struct.')
    for r in rows:
        lines.append('#define B3_PHYS_%-34s 0x%03Xu'
                     % (ident(r['group'].split('/')[-1] + '_' + r['name']).upper(),
                        r['offset']))
    lines += ['', '#endif // BURNOUT3_PHYSICS_PARAMS_H', '']
    open(out, 'w').write('\n'.join(lines))
    print('wrote %s' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
