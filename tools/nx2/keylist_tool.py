#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect, merge and export pipeline key lists (`keylist.bin`).

A key list holds pipeline identifiers only: the shader content hashes of each stage plus the fixed
pipeline state, never code, drivers or resources. It is therefore safe to share between players of
the same title and build. The format is written by `PipelineKeyList::Save`
(`src/video_core/renderer_vulkan/vk_pipeline_keylist.cpp`):

    "lchkeyls", u32 version
    per graphics row: u8 kind=0, u8 reserved, u16 reserved, u64 hash,
                      6*u64 unique_hashes, u32 state_size, state bytes
    per compute row:  u8 kind=1, u8 reserved, u16 reserved, u64 hash,
                      u64 unique_hash, u32 shared_memory_size, 3*u32 workgroup_size

Rows carry the hash the emulator computed for them; this tool keeps that hash and never invents
one, so the emulator re-verifies every row it loads. A row whose state layout differs from the
current build is dropped there, not here.
"""

import argparse
from collections import Counter
import dataclasses
import struct
import sys
from pathlib import Path

MAGIC = b"lchkeyls"
VERSION = 1
KIND_GRAPHICS = 0
KIND_COMPUTE = 1
MAX_ROWS = 1_000_000
# FixedPipelineState is a few hundred bytes; anything far larger is a damaged or foreign file.
MAX_STATE_SIZE = 0x4000

GRAPHICS_HEADER = struct.Struct("<BBHQ")
COMPUTE_HEADER = struct.Struct("<BBHQ")
COMPUTE_BODY = struct.Struct("<QIIII")

# Serialized FixedPipelineState prefix (offsets from this build's fixed_pipeline_state.h; the
# stored size is 0x70, so vertex attributes and later fields are not part of a key row).
STATE_FIELDS = (
    (0x00, 0x04, "raw1 flags (topology, msaa)"),
    (0x04, 0x08, "raw2 flags (depth, alpha test)"),
    (0x08, 0x10, "color_formats"),
    (0x10, 0x18, "driver id/version"),
    (0x18, 0x1c, "alpha_test_ref"),
    (0x1c, 0x20, "point_size"),
    (0x20, 0x40, "viewport_swizzles"),
    (0x40, 0x48, "attribute_types"),
    (0x48, 0x50, "dynamic_state"),
    (0x50, 0x70, "attachments"),
)


def state_field(offset: int) -> str:
    for start, end, name in STATE_FIELDS:
        if start <= offset < end:
            if name == "attachments":
                return f"attachments[{(offset - start) // 4}]"
            return name
    return "outside the stored prefix"


class KeyListError(ValueError):
    """A file that is not a key list of a supported version."""


@dataclasses.dataclass(frozen=True)
class GraphicsRow:
    key_hash: int
    unique_hashes: tuple
    state: bytes

    def pack(self) -> bytes:
        return (GRAPHICS_HEADER.pack(KIND_GRAPHICS, 0, 0, self.key_hash) +
                struct.pack("<6Q", *self.unique_hashes) +
                struct.pack("<I", len(self.state)) + self.state)


@dataclasses.dataclass(frozen=True)
class ComputeRow:
    key_hash: int
    unique_hash: int
    shared_memory_size: int
    workgroup_size: tuple

    def pack(self) -> bytes:
        return (COMPUTE_HEADER.pack(KIND_COMPUTE, 0, 0, self.key_hash) +
                COMPUTE_BODY.pack(self.unique_hash, self.shared_memory_size,
                                  *self.workgroup_size))


@dataclasses.dataclass
class KeyList:
    graphics: dict = dataclasses.field(default_factory=dict)
    compute: dict = dataclasses.field(default_factory=dict)
    rejected: int = 0

    def add(self, row) -> bool:
        target = self.graphics if isinstance(row, GraphicsRow) else self.compute
        if row.key_hash in target:
            return False
        target[row.key_hash] = row
        return True

    def merge(self, other: "KeyList") -> tuple:
        added = sum(self.add(row) for row in other.graphics.values())
        added += sum(self.add(row) for row in other.compute.values())
        return added, other.rejected

    def pack(self) -> bytes:
        rows = [self.graphics[hash] for hash in sorted(self.graphics)]
        rows += [self.compute[hash] for hash in sorted(self.compute)]
        return MAGIC + struct.pack("<I", VERSION) + b"".join(row.pack() for row in rows)

    def program_hashes(self) -> set:
        hashes = set()
        for row in self.graphics.values():
            hashes.update(value for value in row.unique_hashes if value)
        hashes.update(row.unique_hash for row in self.compute.values() if row.unique_hash)
        return hashes

    def shader_sets(self) -> Counter:
        """How many recorded states share one exact set of stage shaders."""
        return Counter(tuple(value for value in row.unique_hashes if value)
                       for row in self.graphics.values())

    def state_sizes(self) -> set:
        return {len(row.state) for row in self.graphics.values()}


def read(path: Path) -> KeyList:
    data = path.read_bytes()
    if len(data) < len(MAGIC) + 4 or data[:len(MAGIC)] != MAGIC:
        raise KeyListError(f"{path}: not a key list")
    version, = struct.unpack_from("<I", data, len(MAGIC))
    if version != VERSION:
        raise KeyListError(f"{path}: unsupported key list version {version}")
    keys = KeyList()
    offset = len(MAGIC) + 4
    while offset < len(data) and len(keys.graphics) + len(keys.compute) < MAX_ROWS:
        if offset + GRAPHICS_HEADER.size > len(data):
            keys.rejected += 1
            break
        kind, _, _, key_hash = GRAPHICS_HEADER.unpack_from(data, offset)
        offset += GRAPHICS_HEADER.size
        if kind == KIND_GRAPHICS:
            if offset + 6 * 8 + 4 > len(data):
                keys.rejected += 1
                break
            unique_hashes = struct.unpack_from("<6Q", data, offset)
            offset += 6 * 8
            state_size, = struct.unpack_from("<I", data, offset)
            offset += 4
            if state_size > MAX_STATE_SIZE or offset + state_size > len(data):
                keys.rejected += 1
                break
            keys.add(GraphicsRow(key_hash, unique_hashes, data[offset:offset + state_size]))
            offset += state_size
        elif kind == KIND_COMPUTE:
            if offset + COMPUTE_BODY.size > len(data):
                keys.rejected += 1
                break
            unique_hash, shared, x, y, z = COMPUTE_BODY.unpack_from(data, offset)
            offset += COMPUTE_BODY.size
            keys.add(ComputeRow(key_hash, unique_hash, shared, (x, y, z)))
        else:
            keys.rejected += 1
            break
    return keys


def write(path: Path, keys: KeyList) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(keys.pack())
    temporary.replace(path)


def diff(keys: KeyList, minimum: int) -> list:
    """Which state bytes split one shader set into several pipelines.

    A set of stage shaders that the game draws with many fixed states costs one pipeline build per
    state. Reporting the offsets that differ points at the register that is really changing.
    """
    groups = {}
    for row in keys.graphics.values():
        groups.setdefault(tuple(value for value in row.unique_hashes if value), []).append(row.state)
    offsets = Counter()
    values = {}
    for states in groups.values():
        if len(states) < minimum or len({state for state in states}) < 2:
            continue
        width = min(len(state) for state in states)
        for offset in range(width):
            column = {state[offset] for state in states}
            if len(column) > 1:
                offsets[offset] += len(states) - 1
                values.setdefault(offset, set()).update(column)
    lines = []
    for offset, count in offsets.most_common(24):
        lines.append(f"  +0x{offset:02x} {state_field(offset)} changes in {count} states, "
                     f"{len(values[offset])} values")
    return lines


def describe(path: Path, keys: KeyList) -> list:
    sets = keys.shader_sets()
    repeated = sum(count for count in sets.values() if count > 1)
    lines = [
        f"{path}: {len(keys.graphics)} graphics, {len(keys.compute)} compute, "
        f"{len(keys.program_hashes())} distinct programs",
        f"  shader sets: {len(sets)}, states on shared sets: {repeated}, "
        f"state sizes: {sorted(keys.state_sizes()) or '-'}",
    ]
    if keys.rejected:
        lines.append(f"  rejected rows: {keys.rejected}")
    for members, count in sets.most_common(5):
        lines.append(f"  {count:5} states for {len(members)} stages")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect", help="report what a key list holds")
    inspect.add_argument("files", nargs="+", type=Path)
    merge = commands.add_parser("merge", help="union several key lists into one")
    merge.add_argument("output", type=Path)
    merge.add_argument("inputs", nargs="+", type=Path)
    programs = commands.add_parser("programs", help="print every shader hash, one per line")
    programs.add_argument("file", type=Path)
    diff_cmd = commands.add_parser(
        "diff", help="state bytes that split one shader set into several pipelines")
    diff_cmd.add_argument("file", type=Path)
    diff_cmd.add_argument("--minimum", type=int, default=2,
                          help="ignore sets with fewer states (default 2)")
    args = parser.parse_args()

    try:
        if args.command == "inspect":
            for path in args.files:
                print("\n".join(describe(path, read(path))))
            return 0
        if args.command == "merge":
            merged = KeyList()
            for path in args.inputs:
                added, rejected = merged.merge(read(path))
                print(f"{path}: +{added} rows" + (f", {rejected} rejected" if rejected else ""))
            write(args.output, merged)
            print(f"{args.output}: {len(merged.graphics)} graphics, "
                  f"{len(merged.compute)} compute")
            return 0
        keys = read(args.file)
        if args.command == "diff":
            print(f"{args.file}: {len(keys.graphics)} graphics rows")
            lines = diff(keys, args.minimum)
            print("\n".join(lines) if lines else "  no shader set has more than one state")
            return 0
        for value in sorted(keys.program_hashes()):
            print(f"{value:016x}")
        return 0
    except (OSError, KeyListError) as exc:
        print(exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
