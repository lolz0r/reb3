#!/usr/bin/env python3
"""Regression cases for FUN_00174050 / navigator ribbon progression."""

import importlib.util
import pathlib
import struct


MODULE_PATH = pathlib.Path(__file__).with_name("extract_bgd_paths.py")
SPEC = importlib.util.spec_from_file_location("extract_bgd_paths", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def build_graph():
    reader = MODULE.BGD.__new__(MODULE.BGD)
    reader.d = bytearray(512)
    points = 0
    pairs = 96
    links = 160
    for index, (x, z) in enumerate((
            (0.0, -1.0), (0.0, 1.0),
            (10.0, -1.0), (10.0, 1.0),
            (20.0, -1.0), (20.0, 1.0))):
        struct.pack_into('<3f', reader.d, points + index * 16, x, 0.0, z)
    struct.pack_into('<HHHH', reader.d, pairs, 0, 1, 2, 3)
    struct.pack_into('<HHHH', reader.d, pairs + 4, 2, 3, 4, 5)
    rows = []
    for section in range(3):
        row_links = links + section * 32
        rows.append(dict(section=section, node_count=2, flags=0,
                         pairs=pairs, links=row_links))
        for node in range(2):
            struct.pack_into('<HHBBHH', reader.d, row_links + node * 10,
                             0, 0, 0xff, 0xff, 0xffff, 0xffff)

    # Section 0's first node has both direct-link targets.  Its final node
    # deliberately lacks a forward link, exposing FUN_00174EE0's fallback.
    struct.pack_into('<BBHH', reader.d, links + 4, 1, 2, 1, 0)
    struct.pack_into('<BBHH', reader.d, links + 10 + 4, 0xff, 2, 0xffff, 1)
    return reader, dict(points=points, rows=rows)


def walk_flags(reader, graph, section, node, flags):
    values = iter(flags + [0, 0])
    reader.nav_step_flags = lambda *_: next(values)
    return reader.nav_walk(graph, section, node, (0.0, 0.0, 0.0))


def main():
    reader, graph = build_graph()
    assert MODULE.BGD.nav_step_flags(reader, graph, 0, 0, (15.0, 0.0, 0.0)) & 4

    priority = walk_flags(reader, graph, 0, 0, [4 | 1])
    assert priority['section'] == 0 and priority['node'] == 1
    assert priority['steps'] == 1

    direct_forward = walk_flags(reader, graph, 0, 0, [1])
    assert direct_forward['section'] == 1 and direct_forward['node'] == 1
    assert direct_forward['steps'] == 1

    direct_reverse = walk_flags(reader, graph, 0, 0, [2])
    assert direct_reverse['section'] == 2 and direct_reverse['node'] == 0
    assert direct_reverse['steps'] == 1

    reverse_terminal = walk_flags(reader, graph, 0, 0, [8])
    assert reverse_terminal['section'] == 1 and reverse_terminal['node'] == 1
    assert reverse_terminal['steps'] == 1

    fallback_reverse = walk_flags(reader, graph, 0, 1, [4])
    assert fallback_reverse['section'] == 2 and fallback_reverse['node'] == 1
    assert fallback_reverse['steps'] == 1
    print('nav ribbon walker: OK')


if __name__ == '__main__':
    main()
