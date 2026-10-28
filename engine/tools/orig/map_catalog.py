from __future__ import annotations

import argparse
import csv
import io
import re
import struct
import sys
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class PointerRun:
    file_offset: int
    ram_address: int
    values: list[str]


@dataclass(frozen=True)
class MapInfoRecord:
    map_id: int
    name: str
    map_type: int
    mapinfo_unk1d: int
    player_obj: int


@dataclass(frozen=True)
class MapsBinRecord:
    size_x: int | None
    size_z: int | None
    origin_x: int | None
    origin_z: int | None
    unk08: int
    unk0c: tuple[int, int, int, int]
    n_blocks: int
    unk1e: int
    facefeed: bool
    unique_mods: tuple[int, ...]
    nonempty_blocks: int


@dataclass(frozen=True)
class GlobalMapRecord:
    x: int
    z: int
    layer: int
    link0: int
    link1: int


class DolFile:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        section_offsets = [struct.unpack_from(">I", self.data, index * 4)[0] for index in range(18)]
        section_addrs = [struct.unpack_from(">I", self.data, 0x48 + index * 4)[0] for index in range(18)]
        section_sizes = [struct.unpack_from(">I", self.data, 0x90 + index * 4)[0] for index in range(18)]
        self.sections = [
            {
                "offset": section_offsets[index],
                "addr": section_addrs[index],
                "size": section_sizes[index],
            }
            for index in range(18)
        ]

    def addr_to_offset(self, addr: int) -> int | None:
        for section in self.sections:
            start = int(section["addr"])
            size = int(section["size"])
            if start <= addr < start + size:
                return int(section["offset"]) + (addr - start)
        return None

    def offset_to_addr(self, offset: int) -> int | None:
        for section in self.sections:
            start = int(section["offset"])
            size = int(section["size"])
            if start <= offset < start + size:
                return int(section["addr"]) + (offset - start)
        return None

    def read_c_string(self, addr: int) -> str | None:
        offset = self.addr_to_offset(addr)
        if offset is None:
            return None
        end = self.data.find(b"\0", offset)
        if end < 0 or end == offset or end - offset > 64:
            return None
        raw = self.data[offset:end]
        if any(byte < 0x20 or byte > 0x7E for byte in raw):
            return None
        return raw.decode("ascii")


def load_mapinfo(files_root: Path) -> list[MapInfoRecord]:
    data = (files_root / "MAPINFO.bin").read_bytes()
    if len(data) % 0x20 != 0:
        raise ValueError(f"Unexpected MAPINFO.bin size: {len(data)}")

    records: list[MapInfoRecord] = []
    for map_id, offset in enumerate(range(0, len(data), 0x20)):
        name_raw, map_type, mapinfo_unk1d, player_obj = struct.unpack_from(">28sBBH", data, offset)
        name = name_raw.split(b"\0", 1)[0].decode("ascii", "replace")
        records.append(
            MapInfoRecord(
                map_id=map_id,
                name=name,
                map_type=map_type,
                mapinfo_unk1d=mapinfo_unk1d,
                player_obj=player_obj,
            )
        )
    return records


def decode_block_mods(map_bin: bytes, offset: int, count: int) -> tuple[tuple[int, ...], int]:
    mods: Counter[int] = Counter()
    nonempty_blocks = 0
    for index in range(count):
        value = struct.unpack_from(">I", map_bin, offset + (index * 4))[0]
        mod = (value >> 23) & 0xFF
        if mod == 0xFF:
            continue
        if mod >= 5:
            mod += 1
        mods[mod] += 1
        nonempty_blocks += 1
    return tuple(sorted(mods)), nonempty_blocks


def load_maps_bin(files_root: Path, map_count: int) -> list[MapsBinRecord]:
    map_bin = (files_root / "MAPS.bin").read_bytes()
    map_tab = (files_root / "MAPS.tab").read_bytes()
    expected_tab_size = map_count * 0x1C
    if len(map_tab) < expected_tab_size:
        raise ValueError(f"MAPS.tab too small: expected at least {expected_tab_size}, got {len(map_tab)}")

    records: list[MapsBinRecord] = []
    for map_id in range(map_count):
        offsets = struct.unpack_from(">7I", map_tab, map_id * 0x1C)
        info = struct.unpack_from(">hhhhIIIIIhH", map_bin, offsets[0])
        size_x, size_z, origin_x, origin_z = info[:4]
        facefeed = (size_x & 0xFFFF) == 0xFACE and (size_z & 0xFFFF) == 0xFEED
        unique_mods: tuple[int, ...] = ()
        nonempty_blocks = 0
        if not facefeed:
            block_count = size_x * size_z
            unique_mods, nonempty_blocks = decode_block_mods(map_bin, offsets[1], block_count)
        records.append(
            MapsBinRecord(
                size_x=None if facefeed else size_x,
                size_z=None if facefeed else size_z,
                origin_x=None if facefeed else origin_x,
                origin_z=None if facefeed else origin_z,
                unk08=info[4],
                unk0c=(info[5], info[6], info[7], info[8]),
                n_blocks=info[9],
                unk1e=info[10],
                facefeed=facefeed,
                unique_mods=unique_mods,
                nonempty_blocks=nonempty_blocks,
            )
        )
    return records


def load_global_map(files_root: Path) -> dict[int, GlobalMapRecord]:
    data = (files_root / "globalma.bin").read_bytes()
    if len(data) % 0x0C != 0:
        raise ValueError(f"Unexpected globalma.bin size: {len(data)}")

    result: dict[int, GlobalMapRecord] = {}
    for offset in range(0, len(data), 0x0C):
        x, z, layer, map_id, link0, link1 = struct.unpack_from(">hhhhhh", data, offset)
        if map_id < 0:
            break
        result[map_id] = GlobalMapRecord(x=x, z=z, layer=layer, link0=link0, link1=link1)
    return result


def load_trkblk(files_root: Path) -> list[int]:
    data = (files_root / "TRKBLK.tab").read_bytes()
    if len(data) % 2 != 0:
        raise ValueError(f"Unexpected TRKBLK.tab size: {len(data)}")
    return [struct.unpack_from(">h", data, offset)[0] for offset in range(0, len(data), 2)]


def decompress_romlist_size(path: Path) -> int:
    data = path.read_bytes()
    if data[:4] != b"ZLB\0":
        raise ValueError(f"Unsupported romlist format: {path}")
    version, decompressed_size, compressed_size = struct.unpack_from(">3I", data, 4)
    if version != 1:
        raise ValueError(f"Unexpected ZLB version {version} in {path}")
    payload = zlib.decompress(data[16 : 16 + compressed_size])
    if len(payload) != decompressed_size:
        raise ValueError(f"Bad ZLB size in {path}: expected {decompressed_size}, got {len(payload)}")
    return decompressed_size


def find_pointer_run(
    dol: DolFile,
    allowed_names: set[str],
    start_offset: int = 0x100,
    end_offset: int | None = None,
    min_length: int = 1,
) -> PointerRun:
    if end_offset is None:
        end_offset = len(dol.data) - 4

    best_offset: int | None = None
    best_values: list[str] = []
    for file_offset in range(start_offset, end_offset, 4):
        values: list[str] = []
        cursor = file_offset
        while cursor + 4 <= len(dol.data):
            ptr = struct.unpack_from(">I", dol.data, cursor)[0]
            value = dol.read_c_string(ptr)
            if value not in allowed_names:
                break
            values.append(value)
            cursor += 4
        if len(values) > len(best_values):
            best_offset = file_offset
            best_values = values

    if best_offset is None or len(best_values) < min_length:
        raise ValueError("Failed to locate pointer table in main.dol")

    ram_address = dol.offset_to_addr(best_offset)
    if ram_address is None:
        raise ValueError(f"Unable to translate DOL file offset 0x{best_offset:X} back to a RAM address")
    return PointerRun(file_offset=best_offset, ram_address=ram_address, values=best_values)


def load_map_dir_ids(dol: DolFile, dir_table: PointerRun, map_count: int) -> PointerRun:
    start = dir_table.file_offset + (len(dir_table.values) * 4)
    values: list[str] = []
    cursor = start
    while cursor + 4 <= len(dol.data) and len(values) < map_count:
        value = struct.unpack_from(">I", dol.data, cursor)[0]
        if value >= len(dir_table.values):
            break
        values.append(str(value))
        cursor += 4

    ram_address = dol.offset_to_addr(start)
    if ram_address is None:
        raise ValueError(f"Unable to translate map dir-id table offset 0x{start:X}")
    return PointerRun(file_offset=start, ram_address=ram_address, values=values)


def detect_tables(files_root: Path, dol_path: Path, map_count: int) -> tuple[PointerRun, PointerRun, PointerRun]:
    dol = DolFile(dol_path)
    romlist_names = {path.name[:-12] for path in files_root.glob("*.romlist.zlb")}
    map_name_table = find_pointer_run(dol, romlist_names, min_length=map_count)
    if len(map_name_table.values) != map_count:
        raise ValueError(
            f"Expected {map_count} map-name entries from main.dol, found {len(map_name_table.values)}"
        )

    map_like_names = romlist_names | {path.name for path in files_root.iterdir() if path.is_dir()}
    dir_table = find_pointer_run(
        dol,
        map_like_names,
        start_offset=map_name_table.file_offset + (len(map_name_table.values) * 4),
        end_offset=min(len(dol.data), map_name_table.file_offset + 0x2000),
        min_length=40,
    )
    map_dir_ids = load_map_dir_ids(dol, dir_table, map_count)
    return map_name_table, dir_table, map_dir_ids


def build_rows(files_root: Path, dol_path: Path) -> tuple[list[dict[str, object]], dict[str, object]]:
    mapinfo = load_mapinfo(files_root)
    maps_bin = load_maps_bin(files_root, len(mapinfo))
    global_map = load_global_map(files_root)
    trkblk = load_trkblk(files_root)
    map_name_table, dir_table, map_dir_ids = detect_tables(files_root, dol_path, len(mapinfo))

    root_romlists = {path.name[:-12] for path in files_root.glob("*.romlist.zlb")}
    rows: list[dict[str, object]] = []
    for record in mapinfo:
        map_id = record.map_id
        maps_bin_record = maps_bin[map_id]
        romlist_name = map_name_table.values[map_id]
        global_record = global_map.get(map_id)
        dir_id: int | None = None
        dir_name: str | None = None
        if map_id < len(map_dir_ids.values):
            dir_id = int(map_dir_ids.values[map_id])
            dir_name = dir_table.values[dir_id]

        romlist_path = files_root / f"{romlist_name}.romlist.zlb"
        romlist_size_raw = decompress_romlist_size(romlist_path)
        row = {
            "map_id": map_id,
            "map_id_hex": f"0x{map_id:02X}",
            "name": record.name,
            "map_type": record.map_type,
            "mapinfo_unk1d": record.mapinfo_unk1d,
            "player_obj": record.player_obj,
            "player_obj_hex": f"0x{record.player_obj:04X}",
            "romlist": romlist_name,
            "romlist_compressed": romlist_path.stat().st_size,
            "romlist_raw": romlist_size_raw,
            "dir_id": dir_id,
            "dir_id_hex": None if dir_id is None else f"0x{dir_id:02X}",
            "dir_name": dir_name,
            "size_x": maps_bin_record.size_x,
            "size_z": maps_bin_record.size_z,
            "origin_x": maps_bin_record.origin_x,
            "origin_z": maps_bin_record.origin_z,
            "unk08_hex": f"0x{maps_bin_record.unk08:08X}",
            "unk0c_hex": " ".join(f"0x{value:08X}" for value in maps_bin_record.unk0c),
            "n_blocks": maps_bin_record.n_blocks,
            "unk1e_hex": f"0x{maps_bin_record.unk1e:04X}",
            "facefeed": maps_bin_record.facefeed,
            "nonempty_blocks": maps_bin_record.nonempty_blocks,
            "unique_mods": " ".join(f"{value:02d}" for value in maps_bin_record.unique_mods),
            "x": None if global_record is None else global_record.x,
            "z": None if global_record is None else global_record.z,
            "layer": None if global_record is None else global_record.layer,
            "link0": None if global_record is None else global_record.link0,
            "link1": None if global_record is None else global_record.link1,
        }
        rows.append(row)

    extra_root_romlists = sorted(root_romlists - set(map_name_table.values))
    alias_rows = [row for row in rows if row["dir_name"] is not None and row["dir_name"] != row["romlist"]]
    facefeed_rows = [row for row in rows if row["facefeed"]]
    type_counts = Counter(int(row["map_type"]) for row in rows)
    root_only_rows = [row for row in rows if row["dir_name"] is None]
    summary = {
        "map_name_table": map_name_table,
        "dir_table": dir_table,
        "map_dir_ids": map_dir_ids,
        "trkblk": trkblk,
        "extra_root_romlists": extra_root_romlists,
        "alias_rows": alias_rows,
        "facefeed_rows": facefeed_rows,
        "type_counts": type_counts,
        "root_only_rows": root_only_rows,
    }
    return rows, summary


def rows_to_csv(rows: list[dict[str, object]]) -> str:
    fieldnames = [
        "map_id",
        "map_id_hex",
        "name",
        "map_type",
        "mapinfo_unk1d",
        "player_obj_hex",
        "romlist",
        "romlist_compressed",
        "romlist_raw",
        "dir_id_hex",
        "dir_name",
        "size_x",
        "size_z",
        "origin_x",
        "origin_z",
        "n_blocks",
        "nonempty_blocks",
        "unique_mods",
        "unk08_hex",
        "unk1e_hex",
        "facefeed",
        "x",
        "z",
        "layer",
        "link0",
        "link1",
    ]
    with_rows = [{name: row.get(name) for name in fieldnames} for row in rows]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(with_rows)
    return buffer.getvalue()


def summary_markdown(rows: list[dict[str, object]], summary: dict[str, object]) -> str:
    map_name_table = summary["map_name_table"]
    dir_table = summary["dir_table"]
    map_dir_ids = summary["map_dir_ids"]
    trkblk = summary["trkblk"]
    extra_root_romlists = summary["extra_root_romlists"]
    alias_rows = summary["alias_rows"]
    facefeed_rows = summary["facefeed_rows"]
    type_counts = summary["type_counts"]
    root_only_rows = summary["root_only_rows"]

    lines: list[str] = []
    lines.append("# `orig/GSAE01` map catalog")
    lines.append("")
    lines.append("## DOL tables recovered directly from EN `main.dol`")
    lines.append(
        f"- Map-ID to romlist table: `{len(map_name_table.values)}` entries at RAM `0x{map_name_table.ram_address:08X}`"
    )
    lines.append(
        f"- Asset-dir name table: `{len(dir_table.values)}` entries at RAM `0x{dir_table.ram_address:08X}`"
    )
    lines.append(
        f"- Map-ID to dir-ID table: `{len(map_dir_ids.values)}` entries at RAM `0x{map_dir_ids.ram_address:08X}`"
    )
    lines.append(
        "- First dir-backed maps: "
        + ", ".join(
            f"`0x{row['map_id']:02X}` `{row['romlist']}` -> `0x{int(row['dir_id']):02X}` `{row['dir_name']}`"
            for row in rows[:6]
        )
    )
    lines.append("")
    lines.append("## Structure summary")
    lines.append(
        "- Map type histogram: "
        + ", ".join(f"`{map_type}`={count}" for map_type, count in sorted(type_counts.items()))
    )
    lines.append(f"- Root-only map IDs without a dir mapping: {len(root_only_rows)}")
    lines.append(f"- `TRKBLK.tab` entries: {len(trkblk)}")
    lines.append(
        "- First `TRKBLK.tab` values: "
        + ", ".join(f"`{value}`" for value in trkblk[:12])
    )
    lines.append("")
    lines.append("## High-value findings")
    lines.append(
        f"- The map-ID table stops at `0x{len(map_dir_ids.values) - 1:02X}`; every later map (`0x{len(map_dir_ids.values):02X}` and up) is root-only."
    )
    lines.append(
        "- `MAPINFO.bin` is confirmed as `>28s B B H`, so the last field is a real `playerObj`/spawn object ID, not an anonymous short."
    )
    lines.append(
        "- `MAPS.bin` info records are confirmed as `0x20` bytes: `sizeX`, `sizeZ`, `originX`, `originZ`, `unk08`, four `unk0C` words, `nBlocks`, `unk1E`."
    )
    if facefeed_rows:
        lines.append(
            "- `FACEFEED` sentinel geometry records: "
            + ", ".join(f"`{row['map_id_hex']}` `{row['romlist']}`" for row in facefeed_rows)
        )
    lines.append(
        "- Root romlists not referenced by the map-ID table: "
        + ", ".join(f"`{name}`" for name in extra_root_romlists)
    )
    lines.append("")
    lines.append("## Alias examples")
    for row in alias_rows[:12]:
        lines.append(
            f"- `{row['map_id_hex']}` `{row['name']}`: romlist=`{row['romlist']}`, dir=`{row['dir_name']}`"
        )
    lines.append("")
    lines.append("## Root-only object maps")
    for row in root_only_rows[:12]:
        size_text = "FACEFEED" if row["facefeed"] else f"{row['size_x']}x{row['size_z']}"
        lines.append(
            f"- `{row['map_id_hex']}` `{row['romlist']}`: type={row['map_type']}, "
            f"playerObj=`{row['player_obj_hex']}`, size={size_text}, raw_romlist={row['romlist_raw']}"
        )
    lines.append("")
    lines.append("## Usage")
    lines.append("- Markdown summary: `python tools/orig/map_catalog.py`")
    lines.append("- CSV dump: `python tools/orig/map_catalog.py --format csv`")
    return "\n".join(lines)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Catalog EN map metadata straight from orig/ retail assets.")
    parser.add_argument(
        "--files-root",
        type=Path,
        default=Path("orig/GSAE01/files"),
        help="Path to the extracted EN files/ directory.",
    )
    parser.add_argument(
        "--dol",
        type=Path,
        default=Path("orig/GSAE01/sys/main.dol"),
        help="Path to the EN main.dol.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "csv"),
        default="markdown",
        help="Output format.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    rows, summary = build_rows(args.files_root, args.dol)
    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(rows))
        else:
            sys.stdout.write(summary_markdown(rows, summary))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
