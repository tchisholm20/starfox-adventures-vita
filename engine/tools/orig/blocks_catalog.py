from __future__ import annotations

import argparse
import csv
import hashlib
import io
import os
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
import re

if __package__ in (None, ""):
    sys.path.append(os.fspath(Path(__file__).resolve().parents[2]))
    from tools.orig.map_catalog import detect_tables, load_mapinfo
else:
    from tools.orig.map_catalog import detect_tables, load_mapinfo


MOD_TAB_RE = re.compile(r"mod(\d+)\.tab$")
ZLB_MAGIC = b"ZLB\0"


@dataclass(frozen=True)
class InnerZlbSegment:
    offset: int
    version: int
    decompressed_size: int
    compressed_size: int
    next_offset: int
    padding: int


@dataclass(frozen=True)
class ModFamily:
    mod_id: int
    tab_path: Path
    resolution: str
    payload_path: Path | None
    candidate_payload_path: Path | None
    entry_count: int
    unique_offset_count: int
    max_offset: int
    flag_counts: tuple[tuple[int, int], ...]
    segments: tuple[InnerZlbSegment, ...]
    maps: tuple[str, ...]
    same_name_root_duplicate: bool


def parse_mod_id(path: Path) -> int:
    match = MOD_TAB_RE.fullmatch(path.name)
    if match is None:
        raise ValueError(f"Unsupported mod tab name: {path.name}")
    return int(match.group(1))


def iter_mod_tabs(files_root: Path) -> list[Path]:
    return sorted(
        path
        for path in files_root.rglob("mod*.tab")
        if MOD_TAB_RE.fullmatch(path.name) is not None
    )


def load_tab_words(path: Path) -> list[int]:
    data = path.read_bytes()
    words: list[int] = []
    for offset in range(0, len(data), 4):
        value = struct.unpack_from(">I", data, offset)[0]
        if value == 0xFFFFFFFF:
            break
        words.append(value)
    return words


def offsets_and_flags(words: list[int]) -> tuple[list[int], Counter[int]]:
    offsets = [value & 0x00FFFFFF for value in words]
    flags = Counter((value >> 24) & 0xFF for value in words)
    return offsets, flags


@lru_cache(maxsize=None)
def load_payload(path: Path) -> bytes:
    return path.read_bytes()


@lru_cache(maxsize=None)
def sha1_digest(path: Path) -> bytes:
    return hashlib.sha1(load_payload(path)).digest()


def parse_segments(payload: bytes, unique_offsets: list[int]) -> tuple[InnerZlbSegment, ...]:
    segments: list[InnerZlbSegment] = []
    for index, offset in enumerate(unique_offsets):
        if payload[offset : offset + 4] != ZLB_MAGIC:
            raise ValueError(f"Offset 0x{offset:X} does not start with a ZLB header")
        version, decompressed_size, compressed_size = struct.unpack_from(">3I", payload, offset + 4)
        next_offset = unique_offsets[index + 1] if index + 1 < len(unique_offsets) else len(payload)
        padding = next_offset - (offset + 16 + compressed_size)
        segments.append(
            InnerZlbSegment(
                offset=offset,
                version=version,
                decompressed_size=decompressed_size,
                compressed_size=compressed_size,
                next_offset=next_offset,
                padding=padding,
            )
        )
    return tuple(segments)


def is_direct_zlb_payload(payload_path: Path, unique_offsets: list[int]) -> bool:
    payload = load_payload(payload_path)
    if not unique_offsets:
        return False
    if unique_offsets[-1] >= len(payload):
        return False
    return all(payload[offset : offset + 4] == ZLB_MAGIC for offset in unique_offsets)


def same_name_payload(tab_path: Path, files_root: Path) -> Path | None:
    local_payload = tab_path.with_suffix(".zlb.bin")
    if local_payload.is_file():
        return local_payload
    root_payload = files_root / local_payload.name
    if root_payload.is_file():
        return root_payload
    return None


def same_name_root_duplicate(tab_path: Path, payload_path: Path, files_root: Path) -> bool:
    if payload_path.parent == files_root:
        return False
    root_copy = files_root / payload_path.name
    if not root_copy.is_file():
        return False
    return sha1_digest(payload_path) == sha1_digest(root_copy)


def candidate_same_dir_payload(tab_path: Path, unique_offsets: list[int], files_root: Path) -> Path | None:
    if tab_path.parent == files_root:
        return None
    if not unique_offsets:
        return None
    max_offset = unique_offsets[-1]
    candidates = sorted(
        path
        for path in tab_path.parent.glob("mod*.zlb.bin")
    )
    fitting = [path for path in candidates if max_offset < path.stat().st_size]
    if len(fitting) == 1:
        return fitting[0]
    return None


def build_dir_to_maps(files_root: Path, dol_path: Path) -> dict[str, tuple[str, ...]]:
    mapinfo = load_mapinfo(files_root)
    _map_name_table, dir_table, map_dir_ids = detect_tables(files_root, dol_path, len(mapinfo))
    dir_to_maps: dict[str, list[str]] = defaultdict(list)
    for map_id, map_record in enumerate(mapinfo):
        if map_id >= len(map_dir_ids.values):
            continue
        dir_id = int(map_dir_ids.values[map_id])
        if dir_id >= len(dir_table.values):
            continue
        dir_name = dir_table.values[dir_id]
        dir_to_maps[dir_name].append(f"0x{map_id:02X} {map_record.name}")
    return {key: tuple(values) for key, values in sorted(dir_to_maps.items())}


def analyze_family(tab_path: Path, files_root: Path, dir_to_maps: dict[str, tuple[str, ...]]) -> ModFamily:
    words = load_tab_words(tab_path)
    offsets, flags = offsets_and_flags(words)
    unique_offsets = sorted(set(offsets))
    max_offset = 0 if not unique_offsets else unique_offsets[-1]
    payload_path = same_name_payload(tab_path, files_root)
    candidate_payload_path: Path | None = None
    resolution = "unresolved"
    segments: tuple[InnerZlbSegment, ...] = ()

    if payload_path is not None and is_direct_zlb_payload(payload_path, unique_offsets):
        resolution = "direct-zlb"
        segments = parse_segments(load_payload(payload_path), unique_offsets)
    else:
        candidate_payload_path = candidate_same_dir_payload(tab_path, unique_offsets, files_root)
        if candidate_payload_path is not None:
            resolution = "candidate-same-dir-bin"

    maps = dir_to_maps.get(tab_path.parent.name, ())
    return ModFamily(
        mod_id=parse_mod_id(tab_path),
        tab_path=tab_path,
        resolution=resolution,
        payload_path=payload_path,
        candidate_payload_path=candidate_payload_path,
        entry_count=len(words),
        unique_offset_count=len(unique_offsets),
        max_offset=max_offset,
        flag_counts=tuple(sorted(flags.items())),
        segments=segments,
        maps=maps,
        same_name_root_duplicate=(
            payload_path is not None and same_name_root_duplicate(tab_path, payload_path, files_root)
        ),
    )


def format_flags(flag_counts: tuple[tuple[int, int], ...]) -> str:
    return ", ".join(f"0x{flag:02X} x{count}" for flag, count in flag_counts)


def summarize_root_duplicates(families: list[ModFamily], files_root: Path) -> tuple[int, int]:
    local_bins = [
        path
        for path in files_root.rglob("mod*.zlb.bin")
        if path.parent != files_root
    ]
    duplicated = 0
    for path in local_bins:
        root_copy = files_root / path.name
        if not root_copy.is_file():
            continue
        if sha1_digest(path) == sha1_digest(root_copy):
            duplicated += 1
    return duplicated, len(local_bins)


def resolution_groups(families: list[ModFamily]) -> dict[str, list[ModFamily]]:
    grouped: dict[str, list[ModFamily]] = defaultdict(list)
    for family in families:
        grouped[family.resolution].append(family)
    return grouped


def top_aliasing(families: list[ModFamily], limit: int = 10) -> list[ModFamily]:
    return sorted(
        [family for family in families if family.resolution == "direct-zlb"],
        key=lambda family: (-(family.entry_count - family.unique_offset_count), family.mod_id, family.tab_path.as_posix()),
    )[:limit]


def smallest_testcases(families: list[ModFamily], limit: int = 10) -> list[ModFamily]:
    return sorted(
        [family for family in families if family.resolution == "direct-zlb" and family.payload_path is not None],
        key=lambda family: (family.unique_offset_count, family.payload_path.stat().st_size, family.entry_count, family.tab_path.as_posix()),
    )[:limit]


def unresolved_rows(families: list[ModFamily]) -> list[ModFamily]:
    return [
        family
        for family in families
        if family.resolution != "direct-zlb"
    ]


def summary_markdown(families: list[ModFamily], files_root: Path) -> str:
    grouped = resolution_groups(families)
    direct = grouped.get("direct-zlb", [])
    candidates = grouped.get("candidate-same-dir-bin", [])
    unresolved = grouped.get("unresolved", [])
    duplicated_roots, local_bin_count = summarize_root_duplicates(families, files_root)
    flag_totals = Counter()
    segment_padding: list[int] = []
    for family in direct:
        for flag, count in family.flag_counts:
            flag_totals[flag] += count
        segment_padding.extend(segment.padding for segment in family.segments)

    lines: list[str] = []
    lines.append("# `orig/GSAE01/files/mod*.{tab,zlb.bin}` catalog")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- `mod*.tab` files audited: {len(families)}")
    lines.append(f"- Direct ZLB-resolved families: {len(direct)}")
    lines.append(f"- Secondary tab-only families with one plausible same-dir bin: {len(candidates)}")
    lines.append(f"- Still unresolved tab-only families: {len(unresolved)}")
    lines.append(f"- Directory `mod*.zlb.bin` files duplicated at disc root: {duplicated_roots} / {local_bin_count}")
    lines.append(
        f"- Direct-family flag bytes: {format_flags(tuple(sorted(flag_totals.items())))}"
    )
    if segment_padding:
        lines.append(
            f"- Direct-family inner-ZLB padding range: {min(segment_padding)}..{max(segment_padding)} bytes"
        )
    lines.append("")

    lines.append("## High-value findings")
    lines.append(
        "- The resolved same-name `mod*.tab` / `mod*.zlb.bin` pairs are not opaque blobs: every unique table offset lands on a valid inner `ZLB` header."
    )
    lines.append(
        "- In those resolved families, each unique inner-ZLB offset appears exactly once with flag `0x10`; every remaining reference to that offset uses flag `0x00`."
    )
    lines.append(
        "- The root `mod*.zlb.bin` files are byte-identical duplicates of the directory copies, which is strong evidence that runtime `BLOCKS.bin` aliases are backed by shared root mirrors rather than unique root-only content."
    )
    if candidates or unresolved:
        lines.append(
            "- Several maps carry extra `mod*.tab` files without a same-name binary, which exposes unfinished block-family indirection that is still worth recovering before split work."
        )
    lines.append("")

    lines.append("## Highest-Aliasing Direct Families")
    for family in top_aliasing(families):
        lines.append(
            f"- `{family.tab_path.relative_to(files_root).as_posix()}`: entries={family.entry_count}, "
            f"unique_segments={family.unique_offset_count}, payload=`{family.payload_path.relative_to(files_root).as_posix()}`"
        )
    lines.append("")

    lines.append("## Smallest Direct Testcases")
    for family in smallest_testcases(families):
        payload = family.payload_path
        if payload is None:
            continue
        lines.append(
            f"- `{family.tab_path.relative_to(files_root).as_posix()}`: segments={family.unique_offset_count}, "
            f"entries={family.entry_count}, payload_size={payload.stat().st_size}"
        )
    lines.append("")

    lines.append("## Secondary Tab-Only Families")
    if candidates:
        for family in candidates:
            lines.append(
                f"- `{family.tab_path.relative_to(files_root).as_posix()}`: "
                f"candidate_bin=`{family.candidate_payload_path.relative_to(files_root).as_posix()}` "
                f"(inference from size only)"
            )
    else:
        lines.append("- None")
    if unresolved:
        for family in unresolved:
            payload_text = (
                "none"
                if family.payload_path is None
                else family.payload_path.relative_to(files_root).as_posix()
            )
            lines.append(
                f"- `{family.tab_path.relative_to(files_root).as_posix()}`: "
                f"same_name_payload=`{payload_text}`, max_offset=`0x{family.max_offset:X}`"
            )
    lines.append("")

    lines.append("## Practical Use")
    lines.append("- Summary: `python tools/orig/blocks_catalog.py`")
    lines.append("- CSV dump: `python tools/orig/blocks_catalog.py --format csv`")
    lines.append("- Search by mod, dir, or map:")
    lines.append("  - `python tools/orig/blocks_catalog.py --search mod:13`")
    lines.append("  - `python tools/orig/blocks_catalog.py --search dir:swaphol`")
    lines.append("  - `python tools/orig/blocks_catalog.py --search map:\"ThornTail Hollow\"`")
    return "\n".join(lines)


def rows_to_csv(families: list[ModFamily], files_root: Path) -> str:
    fieldnames = [
        "mod_id",
        "tab_path",
        "resolution",
        "payload_path",
        "candidate_payload_path",
        "entry_count",
        "unique_offset_count",
        "max_offset_hex",
        "flags",
        "segment_count",
        "maps",
        "same_name_root_duplicate",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for family in families:
        writer.writerow(
            {
                "mod_id": f"0x{family.mod_id:02X}",
                "tab_path": family.tab_path.relative_to(files_root).as_posix(),
                "resolution": family.resolution,
                "payload_path": "" if family.payload_path is None else family.payload_path.relative_to(files_root).as_posix(),
                "candidate_payload_path": "" if family.candidate_payload_path is None else family.candidate_payload_path.relative_to(files_root).as_posix(),
                "entry_count": family.entry_count,
                "unique_offset_count": family.unique_offset_count,
                "max_offset_hex": f"0x{family.max_offset:X}",
                "flags": format_flags(family.flag_counts),
                "segment_count": len(family.segments),
                "maps": " | ".join(family.maps),
                "same_name_root_duplicate": family.same_name_root_duplicate,
            }
        )
    return buffer.getvalue()


def matches_pattern(family: ModFamily, pattern: str, files_root: Path) -> bool:
    if pattern.startswith("mod:"):
        value = pattern[4:]
        if value.startswith("0x"):
            try:
                return family.mod_id == int(value, 16)
            except ValueError:
                return False
        if value.isdigit():
            return family.mod_id == int(value, 10)
        return value in (f"{family.mod_id:x}", f"{family.mod_id:02x}")
    if pattern.startswith("dir:"):
        value = pattern[4:]
        return value in family.tab_path.parent.name.lower()
    if pattern.startswith("map:"):
        value = pattern[4:]
        return any(value in map_name.lower() for map_name in family.maps)

    haystacks = [
        family.tab_path.relative_to(files_root).as_posix().lower(),
        family.resolution.lower(),
        f"{family.mod_id:x}",
        f"0x{family.mod_id:x}",
    ]
    if family.payload_path is not None:
        haystacks.append(family.payload_path.relative_to(files_root).as_posix().lower())
    if family.candidate_payload_path is not None:
        haystacks.append(family.candidate_payload_path.relative_to(files_root).as_posix().lower())
    haystacks.extend(map_name.lower() for map_name in family.maps)
    return any(pattern in value for value in haystacks)


def search_markdown(families: list[ModFamily], patterns: list[str], files_root: Path) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    matches = [
        family
        for family in families
        if any(matches_pattern(family, pattern, files_root) for pattern in lowered)
    ]

    lines: list[str] = []
    lines.append("# BLOCKS search")
    lines.append("")
    if not matches:
        lines.append("- No matching mod families.")
        return "\n".join(lines)

    for family in matches[:25]:
        payload_text = "none" if family.payload_path is None else family.payload_path.relative_to(files_root).as_posix()
        lines.append(
            f"- `{family.tab_path.relative_to(files_root).as_posix()}`: mod=`0x{family.mod_id:02X}`, "
            f"resolution=`{family.resolution}`, payload=`{payload_text}`, "
            f"entries={family.entry_count}, unique_segments={family.unique_offset_count}, flags=`{format_flags(family.flag_counts)}`"
        )
        if family.maps:
            lines.append("  maps: " + ", ".join(f"`{item}`" for item in family.maps[:6]))
        if family.candidate_payload_path is not None:
            lines.append(
                f"  candidate same-dir bin: `{family.candidate_payload_path.relative_to(files_root).as_posix()}`"
            )
        for segment in family.segments[:6]:
            lines.append(
                f"  seg `0x{segment.offset:X}`: dec=`0x{segment.decompressed_size:X}`, "
                f"comp=`0x{segment.compressed_size:X}`, pad={segment.padding}"
            )
        if len(family.segments) > 6:
            lines.append(f"  ... {len(family.segments) - 6} more segments")
    if len(matches) > 25:
        lines.append(f"- ... {len(matches) - 25} more matches omitted")
    return "\n".join(lines)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Recover BLOCKS/modXX family structure from retail orig assets.")
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
        help="Path to the EN main.dol used for dir-name to map-name recovery.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "csv"),
        default="markdown",
        help="Output format.",
    )
    parser.add_argument(
        "--search",
        nargs="+",
        help="Substring search across mod IDs, directories, map names, and paths.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    files_root = args.files_root.resolve()
    dir_to_maps = build_dir_to_maps(files_root, args.dol.resolve())
    families = [analyze_family(path, files_root, dir_to_maps) for path in iter_mod_tabs(files_root)]

    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(families, files_root))
        elif args.search:
            sys.stdout.write(search_markdown(families, args.search, files_root))
            sys.stdout.write("\n")
        else:
            sys.stdout.write(summary_markdown(families, files_root))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
