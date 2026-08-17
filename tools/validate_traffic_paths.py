#!/usr/bin/env python3
"""Validate the RIDX traffic-path data used by the persistent cursor."""

import math
import os
import pathlib
import struct


def retail_branch_select(path_rows, branch, cursor, selector_bit, direction_bit,
                         lookahead_count):
    """FUN_001A0750's path/cursor link selection over 0x12-byte rows."""
    row = int(cursor)
    start = row + 1 if direction_bit else row + (3 if lookahead_count < 2 else 1)
    start = min(start, path_rows - 1)
    column = (3 if not direction_bit else 1) if not selector_bit else \
             (2 if not direction_bit else 0)
    for source_row in range(start, row - 1, -1):
        link = branch[source_row * 0x12:(source_row + 1) * 0x12]
        target_path = link[0x0c + column]
        if target_path == 0xff:
            continue
        target_row = struct.unpack_from('<H', link, column * 2)[0]
        if not direction_bit:
            if source_row < path_rows - 1:
                return target_path, target_row, source_row
            back = min(source_row - row - 1, 4)
            return target_path, max(target_row - back, 0), source_row - back
        if target_row:
            return target_path, target_row - 1, source_row
        return target_path, 0, source_row + 1
    return None


def test_retail_branch_selector():
    branch = bytearray(6 * 0x12)

    def link(row, column, target_path, target_row):
        struct.pack_into('<H', branch, row * 0x12 + column * 2, target_row)
        branch[row * 0x12 + 0x0c + column] = target_path

    link(4, 3, 7, 9)
    link(4, 2, 6, 2)
    link(4, 1, 5, 2)
    assert retail_branch_select(6, branch, 1.2, 0, 0, 1) == (7, 9, 4)
    assert retail_branch_select(6, branch, 1.2, 1, 0, 1) == (6, 2, 4)
    assert retail_branch_select(6, branch, 3.2, 0, 1, 1) == (5, 1, 4)

    branch = bytearray(6 * 0x12)
    link(5, 3, 4, 4)
    assert retail_branch_select(6, branch, 2.2, 0, 0, 1) == (4, 2, 3)


def retail_pool_windows(windows, progress):
    """FUN_001A28B0's current-and-two-prior circular window selection."""
    for current, window in enumerate(windows):
        if window[0] <= progress <= window[1]:
            count = len(windows)
            return [current, (current - 1) % count, (current - 2) % count]
    return None


def test_retail_pool_windows():
    windows = [(0, 9), (10, 19), (20, 29), (30, 39)]
    assert retail_pool_windows(windows, 3) == [0, 3, 2]
    assert retail_pool_windows(windows, 12) == [1, 0, 3]
    assert retail_pool_windows(windows, 29) == [2, 1, 0]
    assert retail_pool_windows(windows, 40) is None


def main():
    test_retail_branch_selector()
    test_retail_pool_windows()
    track = os.environ.get('B3_TRACK', os.environ.get('B3_POSTFX_TRACK',
                                                        'US_C3_V1'))
    path = pathlib.Path('build/tracks') / track / 'traffic_paths.bin'
    data = path.read_bytes()
    magic, version, point_count, path_count = struct.unpack_from('<4sIII', data)
    assert magic == b'B3TP' and version in (1, 2, 3, 4)
    assert point_count and path_count
    window_count = request_count = 0
    offset = 16
    if version >= 3:
        window_count, request_count = struct.unpack_from('<II', data, offset)
        offset += 8
    offset += point_count * 12
    rows = 0
    links = 0
    path_rows = []
    branch_rows = []
    for path_id in range(path_count):
        (count,) = struct.unpack_from('<I', data, offset)
        offset += 4
        assert count >= 2
        path_rows.append(count)
        pairs = struct.unpack_from('<%dH' % (count * 2), data, offset)
        offset += count * 4
        assert all(point < point_count for point in pairs)
        distances = struct.unpack_from('<%df' % (count * 2), data, offset)
        offset += count * 8
        previous = distances[0]
        assert math.isfinite(previous)
        for row in range(count):
            distance, width = distances[row * 2:row * 2 + 2]
            assert math.isfinite(distance) and math.isfinite(width)
            assert distance >= previous, 'path %d distance row %d' % (path_id, row)
            previous = distance
        if version >= 2:
            branch = data[offset:offset + count * 0x12]
            assert len(branch) == count * 0x12
            branch_rows.append(branch)
            offset += count * 0x12
        rows += count
    pool_windows = []
    if version >= 3:
        for _ in range(window_count):
            first, last, request_base, count, refresh, pad = struct.unpack_from(
                '<IIIBBH', data, offset)
            offset += 16
            assert first <= last and pad == 0
            assert request_base + count <= request_count
            pool_windows.append((first, last, request_base, count, refresh))
        pool_requests = []
        for _ in range(request_count):
            first_row, last_row, path_id, direction = struct.unpack_from(
                '<HHBB', data, offset)
            offset += 6
            assert path_id < path_count
            assert first_row < path_rows[path_id]
            assert last_row < path_rows[path_id]
            assert direction in (0, 1, 2)
            pool_requests.append((first_row, last_row, path_id, direction))
        assert pool_windows
        for window in pool_windows:
            assert window[3] > 0
        assert retail_pool_windows(pool_windows, pool_windows[0][0])
    mix = (0, 0, 0, 0)
    if version >= 4:
        # the spawn-policy section: FUN_001A5E30's class lists, FUN_0019E5B0's
        # (path,row) -> (record,slot) bindings and the per-road speed/rate
        # tables.  tools/validate_traffic_mix.py checks their CONTENT against
        # the retail functions; here we only bound the layout.
        mix = struct.unpack_from('<IIII', data, offset)
        offset += 16 + mix[0] * 16 + mix[1] * 32 + mix[2] * 8 + mix[3] * 32
    assert offset == len(data)
    for path_id, branch in enumerate(branch_rows):
        for row in range(path_rows[path_id]):
            link = branch[row * 0x12:(row + 1) * 0x12]
            for column, target in enumerate(link[0x0c:0x10]):
                if target == 0xff:
                    continue
                target_row = struct.unpack_from('<H', link, column * 2)[0]
                assert target < path_count
                assert target_row < path_rows[target], (
                    'path %d row %d column %d -> path %d row %d'
                    % (path_id, row, column, target, target_row))
                links += 1
    print('traffic path cursor/link/pool data and retail selectors: OK '
          '(%d paths, %d rows, %d links, %d windows, %d requests, '
          '%d classes/%d models/%d bindings/%d roads)'
          % (path_count, rows, links, window_count, request_count,
             mix[0], mix[1], mix[2], mix[3]))


if __name__ == '__main__':
    main()
