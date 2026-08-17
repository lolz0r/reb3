#!/usr/bin/env python3
"""
Apply the recovered Burnout 3 struct layouts to the Ghidra database.

The Ghidra project is not in the repo, so this makes the typing reproducible:
run it against a fresh import of build/burnout3.elf and the physics functions
become readable enough to port.

Requires the ghidra-mcp bridge on 127.0.0.1:8089 with burnout3.elf open.

IMPORTANT /create_struct behaviour: it packs fields sequentially and IGNORES the
`offset` key wherever there is a hole. Explicit padding must be supplied for
every gap or the tail of the struct silently lands at the wrong offsets -- the
first attempt at B3PhysicsConfig came out 440 bytes instead of 464 with the last
five fields 24 bytes low, and reported success. Every struct built here is
gap-filled and then verified.
"""
import json
import re
import sys
import urllib.request

MCP = "http://127.0.0.1:8089"
PARAMS_HEADER = "src/burnout3_physics_params.h"

TYPE_SIZES = {"float": 4, "int": 4, "void *": 4, "byte": 1, "char": 1,
              "undefined4": 4, "B3Wheel[4]": 4 * 0xC0, "float[9]": 36}


def post(endpoint, body):
    req = urllib.request.Request(MCP + "/" + endpoint,
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=300).read())


def get(endpoint):
    return json.loads(urllib.request.urlopen(MCP + "/" + endpoint, timeout=300).read())


def gap_fill(known, size, pad_type="undefined1"):
    """Turn (offset, type, name) triples into a contiguous field list."""
    fields, cur = [], 0
    for off, ty, nm in sorted(known):
        if off < cur:
            raise SystemExit("overlap: 0x%X starts before 0x%X" % (off, cur))
        if off > cur:
            fields.append({"name": "pad_%04x" % cur,
                           "type": "%s[%d]" % (pad_type, off - cur),
                           "offset": cur})
        fields.append({"name": nm, "type": ty, "offset": off})
        cur = off + TYPE_SIZES[ty]
    if cur < size:
        fields.append({"name": "pad_%04x" % cur,
                       "type": "%s[%d]" % (pad_type, size - cur), "offset": cur})
    return fields


def make_struct(name, fields, expect_size):
    body = {"name": name, "fields": fields, "replace_placeholder": True}
    r = post("create_struct", body)
    if "error" in r and "already exists" in r["error"]:
        body["force"] = True
        r = post("recreate_struct", body)
    got = r.get("size")
    ok = got == expect_size
    print("  %-18s %s  size 0x%X (want 0x%X)" % (
        name, "OK  " if ok else "FAIL", got or 0, expect_size))
    return ok


def physics_config_fields():
    """64 registered params from the generated header, plus explicit gap pads."""
    src = open(PARAMS_HEADER).read()
    rows = re.findall(r'\{ "([^"]+)", "([^"]+)", 0x([0-9A-F]+)u', src)
    by_off = {int(o, 16): (g, n) for g, n, o in rows}

    def ident(group, name):
        s = re.sub(r'[^0-9A-Za-z]+', '_', group.split('/')[-1] + '_' + name)
        s = s.strip('_').lower()
        return ('f_' + s) if s[0].isdigit() else s

    fields = [{"name": "hdr", "type": "byte[184]", "offset": 0}]
    seen = set()
    for off in range(0xB8, 0x1D0, 4):
        if off in by_off:
            nm = ident(*by_off[off])
            while nm in seen:
                nm += "_2"
            seen.add(nm)
            fields.append({"name": nm, "type": "float", "offset": off})
        else:
            fields.append({"name": "pad_%03x" % off, "type": "undefined4",
                           "offset": off})
    return fields


# Live vehicle object. Only fields with evidence are named; see
# src/burnout3_vehicle_struct.h for the per-field provenance and confidence.
VEHICLE_FIELDS = [
    # +0xBC is compared against speed_mph * 0.44704, so this quad is a velocity
    # direction vector plus a speed magnitude -- not a force vector.
    (0x0B0, "float", "vel_dir_x"), (0x0B4, "float", "vel_dir_y"),
    (0x0B8, "float", "vel_dir_z"), (0x0BC, "float", "speed_ms"),
    (0x130, "float", "acc0"), (0x134, "float", "acc1"),
    (0x138, "float", "acc2"), (0x13C, "float", "acc3"),
    (0x1F0, "float", "mass_kg"),
    (0x204, "void *", "p_frame"),
    (0x820, "B3Wheel[4]", "wheels"),
    (0xCA0, "float", "front_attach_height"), (0xCA4, "float", "front_spring_damping"),
    (0xCA8, "float", "front_spring_force"), (0xCAC, "float", "front_spring_length"),
    (0xCB0, "float", "rear_attach_height"), (0xCB4, "float", "rear_spring_damping"),
    (0xCB8, "float", "rear_spring_force"), (0xCBC, "float", "rear_spring_length"),
    (0x1164, "float", "steer_angle_deg"),
    (0x1169, "byte", "wheel_count"),
    (0x1360, "float", "resist_coef"),
    (0x13D4, "float", "speed_mph"),
    (0x13F4, "void *", "p_owner"), (0x13F8, "void *", "p_cfg_obj"),
    (0x1444, "byte", "drivetrain_flag_a"), (0x1446, "byte", "drivetrain_flag_b"),
    (0x1448, "float[9]", "gear"),
    (0x146C, "float", "idle_rpm"), (0x1470, "float", "change_up_rpm"),
    (0x1474, "float", "change_down_rpm"),
    (0x1480, "float", "max_rpm"), (0x1484, "float", "torque"),
    (0x1494, "float", "boost_kick_torque"), (0x1498, "float", "boost_kick_time"),
    (0x149C, "float", "engine_omega_rads"),
    (0x14C8, "int", "gear_current"), (0x14CC, "int", "gear_target"),
]

WHEEL_FIELDS = [
    {"name": "unk_00", "type": "byte[116]", "offset": 0x00},
    {"name": "attach_height", "type": "float", "offset": 0x74},
    {"name": "unk_78", "type": "byte[60]", "offset": 0x78},
    {"name": "active", "type": "byte", "offset": 0xB4},
    {"name": "unk_b5", "type": "byte[11]", "offset": 0xB5},
]

# The vehicle pointer arrives on the stack in these, not in a register --
# __regparm1 produces `in_stack_00000004` and no field naming at all.
PROTOTYPES = [
    ("0x00132950", "void b3_physics_config_defaults(B3PhysicsConfig * cfg)", "__regparm1"),
    ("0x0011d460", "void b3_vehicle_drivetrain_update(B3Vehicle * v)", "__cdecl"),
    ("0x00123fd0", "void b3_vehicle_suspension_update(B3Vehicle * v)", "__cdecl"),
]


def verify_config():
    layout = get("get_struct_layout?struct_name=B3PhysicsConfig")
    present = {f["offset"] for f in layout.get("fields", [])}
    want = {int(o, 16) for _, _, o in
            re.findall(r'\{ "([^"]+)", "([^"]+)", 0x([0-9A-F]+)u',
                       open(PARAMS_HEADER).read())}
    missing = sorted(want - present)
    print("  params at exact offset: %d/%d%s" % (
        len(want) - len(missing), len(want),
        "" if not missing else "  MISSING " + " ".join("0x%X" % m for m in missing)))
    return not missing


def main():
    print("structs:")
    ok = make_struct("B3Wheel", WHEEL_FIELDS, 0xC0)
    ok &= make_struct("B3PhysicsConfig", physics_config_fields(), 0x1D0)
    ok &= make_struct("B3Vehicle", gap_fill(VEHICLE_FIELDS, 0x14D0), 0x14D0)
    print("verify:")
    ok &= verify_config()

    print("prototypes:")
    for addr, proto, cc in PROTOTYPES:
        r = post("set_function_prototype", {"function_address": addr,
                                            "prototype": proto,
                                            "calling_convention": cc})
        good = r.get("status") == "success"
        ok &= good
        print("  %-12s %s  %s" % (addr, "OK  " if good else "FAIL",
                                  proto.split('(')[0].split()[-1]))

    # A typed apply should produce a large number of `v->field` references.
    d = get("decompile_function?address=0x0011d460").get("decompiled", "")
    n = sum(1 for l in d.splitlines() if "v->" in l)
    print("drivetrain decompile: %d lines using named fields %s" % (
        n, "" if n > 100 else "(LOW -- check calling convention)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
