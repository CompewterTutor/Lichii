# SPDX-License-Identifier: GPL-3.0-or-later

import struct
import tempfile
import unittest
from pathlib import Path

from keylist_tool import (ComputeRow, GraphicsRow, KeyList, KeyListError, MAGIC,
                          describe, diff, read, write)


def graphics(key_hash=1, hashes=(0x11, 0x22, 0, 0, 0, 0), state=b"\x01\x02\x03\x04"):
    return GraphicsRow(key_hash, hashes, state)


def compute(key_hash=2, unique_hash=0x33):
    return ComputeRow(key_hash, unique_hash, 0x1000, (8, 4, 1))


class RoundTrip(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "keylist.bin"

    def tearDown(self):
        self.directory.cleanup()

    def test_round_trip_keeps_rows(self):
        keys = KeyList()
        keys.add(graphics())
        keys.add(compute())
        write(self.path, keys)
        loaded = read(self.path)
        self.assertEqual(loaded.graphics, keys.graphics)
        self.assertEqual(loaded.compute, keys.compute)
        self.assertEqual(loaded.rejected, 0)

    def test_save_orders_rows_by_hash(self):
        keys = KeyList()
        keys.add(graphics(key_hash=9))
        keys.add(graphics(key_hash=3))
        write(self.path, keys)
        body = self.path.read_bytes()[12:]
        first, = struct.unpack_from("<Q", body, 4)
        self.assertEqual(first, 3)

    def test_rejects_foreign_file(self):
        self.path.write_bytes(b"NOPE" + bytes(32))
        with self.assertRaises(KeyListError):
            read(self.path)

    def test_rejects_unknown_version(self):
        self.path.write_bytes(MAGIC + struct.pack("<I", 99))
        with self.assertRaises(KeyListError):
            read(self.path)

    def test_truncated_row_is_reported(self):
        keys = KeyList()
        keys.add(graphics())
        self.path.write_bytes(keys.pack()[:-2])
        loaded = read(self.path)
        self.assertEqual(len(loaded.graphics), 0)
        self.assertEqual(loaded.rejected, 1)

    def test_oversized_state_is_reported(self):
        body = (struct.pack("<BBHQ", 0, 0, 0, 1) + struct.pack("<6Q", *([0] * 6)) +
                struct.pack("<I", 0x10000) + bytes(16))
        self.path.write_bytes(MAGIC + struct.pack("<I", 1) + body)
        loaded = read(self.path)
        self.assertEqual(loaded.rejected, 1)


class Merge(unittest.TestCase):
    def test_merge_deduplicates_rows(self):
        first, second = KeyList(), KeyList()
        first.add(graphics(key_hash=1))
        second.add(graphics(key_hash=1))
        second.add(graphics(key_hash=2, hashes=(0x11, 0x44, 0, 0, 0, 0)))
        second.add(compute())
        added, rejected = first.merge(second)
        self.assertEqual((added, rejected), (2, 0))
        self.assertEqual(sorted(first.graphics), [1, 2])
        self.assertEqual(len(first.compute), 1)


class Reports(unittest.TestCase):
    def test_program_hashes_skip_empty_stages(self):
        keys = KeyList()
        keys.add(graphics(hashes=(0x11, 0, 0x22, 0, 0, 0)))
        keys.add(compute())
        self.assertEqual(keys.program_hashes(), {0x11, 0x22, 0x33})

    def test_shader_sets_count_states(self):
        keys = KeyList()
        keys.add(graphics(key_hash=1))
        keys.add(graphics(key_hash=2))
        keys.add(graphics(key_hash=3, hashes=(0x55, 0, 0, 0, 0, 0)))
        counts = keys.shader_sets()
        self.assertEqual(counts[(0x11, 0x22)], 2)
        self.assertEqual(counts[(0x55,)], 1)

    def test_describe_mentions_counts(self):
        keys = KeyList()
        keys.add(graphics())
        lines = describe(Path("x.bin"), keys)
        self.assertIn("1 graphics", lines[0])
        self.assertIn("distinct programs", lines[0])

    def test_diff_reports_the_splitting_byte(self):
        keys = KeyList()
        keys.add(graphics(key_hash=1, state=b"\x00\x01\x02"))
        keys.add(graphics(key_hash=2, state=b"\x00\x09\x02"))
        keys.add(graphics(key_hash=3, state=b"\x00\x03\x02"))
        lines = diff(keys, 2)
        self.assertEqual(len(lines), 1)
        self.assertIn("+0x01", lines[0])
        self.assertIn("2 states", lines[0])

    def test_diff_ignores_repeated_states(self):
        keys = KeyList()
        keys.add(graphics(key_hash=1, state=b"\x01"))
        keys.add(graphics(key_hash=2, state=b"\x01"))
        self.assertEqual(diff(keys, 2), [])


if __name__ == "__main__":
    unittest.main()
