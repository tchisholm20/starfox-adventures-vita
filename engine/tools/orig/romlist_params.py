from __future__ import annotations

import argparse
import csv
import io
import struct
import sys
import zlib
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path


HEADER_SIZE = 0x18
NAME_OFFSET = 0x91
NAME_SIZE = 11
DLL_ID_OFFSET = 0x50
CLASS_ID_OFFSET = 0x52
MAP_ID_OFFSET = 0x78


@dataclass(frozen=True)
class ObjectDefInfo:
    def_id: int
    name: str
    dll_id: int
    class_id: int
    map_id: int
    map_name: str | None


@dataclass(frozen=True)
class PlacementSample:
    size_words: int
    romlist: str
    params: bytes


@dataclass
class MutablePlacementStats:
    placements: int = 0
    romlists: set[str] = field(default_factory=set)
    size_words: Counter[int] = field(default_factory=Counter)
    map_acts1: Counter[int] = field(default_factory=Counter)
    load_flags: Counter[int] = field(default_factory=Counter)
    map_acts2: Counter[int] = field(default_factory=Counter)
    bounds: Counter[int] = field(default_factory=Counter)
    cull_dists: Counter[int] = field(default_factory=Counter)
    samples: dict[int, PlacementSample] = field(default_factory=dict)


@dataclass(frozen=True)
class PlacementSummary:
    def_id: int
    name: str
    dll_id: int
    class_id: int
    map_id: int
    map_name: str | None
    placements: int
    romlist_count: int
    size_words: tuple[tuple[int, int], ...]
    map_acts1: tuple[tuple[int, int], ...]
    load_flags: tuple[tuple[int, int], ...]
    map_acts2: tuple[tuple[int, int], ...]
    bounds: tuple[tuple[int, int], ...]
    cull_dists: tuple[tuple[int, int], ...]
    samples: tuple[PlacementSample, ...]

    @property
    def variable(self) -> bool:
        return len(self.size_words) > 1

    @property
    def primary_size_words(self) -> int:
        return max(self.size_words, key=lambda item: (item[1], -item[0]))[0]


def load_map_names(path: Path) -> list[str]:
    data = path.read_bytes()
    if len(data) % 0x20 != 0:
        raise ValueError(f"Unexpected MAPINFO.bin size: {len(data)}")
    names: list[str] = []
    for offset in range(0, len(data), 0x20):
        raw_name = data[offset : offset + 28]
        names.append(raw_name.split(b"\0", 1)[0].decode("ascii", "replace"))
    return names


def load_object_offsets(path: Path) -> list[int]:
    data = path.read_bytes()
    offsets: list[int] = []
    for offset in range(0, len(data), 4):
        value = struct.unpack_from(">I", data, offset)[0]
        if value == 0xFFFFFFFF:
            break
        offsets.append(value)
    if len(offsets) < 2:
        raise ValueError("OBJECTS.tab did not contain enough offsets to recover live object defs")
    return offsets


def load_object_defs(files_root: Path) -> list[ObjectDefInfo]:
    object_bin = (files_root / "OBJECTS.bin").read_bytes()
    offsets = load_object_offsets(files_root / "OBJECTS.tab")
    map_names = load_map_names(files_root / "MAPINFO.bin")
    records: list[ObjectDefInfo] = []
    for def_id, offset in enumerate(offsets[:-1]):
        name = object_bin[offset + NAME_OFFSET : offset + NAME_OFFSET + NAME_SIZE].split(b"\0", 1)[0].decode(
            "ascii",
            "replace",
        )
        dll_id, class_id = struct.unpack_from(">Hh", object_bin, offset + DLL_ID_OFFSET)
        map_id = struct.unpack_from(">H", object_bin, offset + MAP_ID_OFFSET)[0]
        map_name = None if map_id == 0xFFFF or map_id >= len(map_names) else map_names[map_id]
        records.append(
            ObjectDefInfo(
                def_id=def_id,
                name=name,
                dll_id=dll_id,
                class_id=class_id,
                map_id=map_id,
                map_name=map_name,
            )
        )
    return records


def load_objindex(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) % 2 != 0:
        raise ValueError(f"Unexpected OBJINDEX.bin size: {len(data)}")
    return [struct.unpack_from(">h", data, offset)[0] for offset in range(0, len(data), 2)]


def resolve_object_id(objindex: list[int], object_id: int) -> int:
    if 0 <= object_id < len(objindex):
        value = objindex[object_id]
        if value != -1:
            return value
    return object_id


def decompress_zlb(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:4] != b"ZLB\0":
        raise ValueError(f"Unsupported container in {path}")
    version, decompressed_size, compressed_size = struct.unpack_from(">3I", data, 4)
    if version != 1:
        raise ValueError(f"Unexpected ZLB version {version} in {path}")
    payload = zlib.decompress(data[16 : 16 + compressed_size])
    if len(payload) != decompressed_size:
        raise ValueError(f"Bad ZLB size in {path}: expected {decompressed_size}, got {len(payload)}")
    return payload


def collect_placement_stats(files_root: Path, objindex: list[int]) -> tuple[dict[int, MutablePlacementStats], int]:
    stats_by_def: dict[int, MutablePlacementStats] = {}
    romlist_count = 0
    for romlist_path in sorted(files_root.glob("*.romlist.zlb")):
        romlist_count += 1
        payload = decompress_zlb(romlist_path)
        offset = 0
        while offset < len(payload):
            object_id, size_words, map_acts1, load_flags, map_acts2, bound, cull_dist = struct.unpack_from(
                ">HBBBBBB",
                payload,
                offset,
            )
            canonical_id = resolve_object_id(objindex, object_id)
            record_size = size_words * 4
            if record_size < HEADER_SIZE or offset + record_size > len(payload):
                raise ValueError(
                    f"Invalid record in {romlist_path.name}: offset=0x{offset:X} size_words={size_words}"
                )
            params = payload[offset + HEADER_SIZE : offset + record_size]

            stats = stats_by_def.setdefault(canonical_id, MutablePlacementStats())
            stats.placements += 1
            stats.romlists.add(romlist_path.name)
            stats.size_words[size_words] += 1
            stats.map_acts1[map_acts1] += 1
            stats.load_flags[load_flags] += 1
            stats.map_acts2[map_acts2] += 1
            stats.bounds[bound] += 1
            stats.cull_dists[cull_dist] += 1
            if size_words not in stats.samples:
                stats.samples[size_words] = PlacementSample(
                    size_words=size_words,
                    romlist=romlist_path.name,
                    params=params,
                )

            offset += record_size
    return stats_by_def, romlist_count


def build_summaries(files_root: Path) -> tuple[list[PlacementSummary], int]:
    object_defs = load_object_defs(files_root)
    objindex = load_objindex(files_root / "OBJINDEX.bin")
    stats_by_def, romlist_count = collect_placement_stats(files_root, objindex)

    summaries: list[PlacementSummary] = []
    for def_id, stats in stats_by_def.items():
        if def_id < 0 or def_id >= len(object_defs):
            continue
        info = object_defs[def_id]
        samples = tuple(stats.samples[size_words] for size_words in sorted(stats.samples))
        summaries.append(
            PlacementSummary(
                def_id=def_id,
                name=info.name,
                dll_id=info.dll_id,
                class_id=info.class_id,
                map_id=info.map_id,
                map_name=info.map_name,
                placements=stats.placements,
                romlist_count=len(stats.romlists),
                size_words=tuple(sorted(stats.size_words.items())),
                map_acts1=tuple(sorted(stats.map_acts1.items())),
                load_flags=tuple(sorted(stats.load_flags.items())),
                map_acts2=tuple(sorted(stats.map_acts2.items())),
                bounds=tuple(sorted(stats.bounds.items())),
                cull_dists=tuple(sorted(stats.cull_dists.items())),
                samples=samples,
            )
        )

    summaries.sort(key=lambda item: (-item.placements, item.def_id))
    return summaries, romlist_count


def format_counter(counter_items: tuple[tuple[int, int], ...], *, hex_width: int = 2) -> str:
    return ", ".join(f"0x{value:0{hex_width}X} x{count}" for value, count in counter_items)


def format_counter_limited(counter_items: tuple[tuple[int, int], ...], *, hex_width: int = 2, limit: int = 12) -> str:
    if len(counter_items) <= limit:
        return format_counter(counter_items, hex_width=hex_width)
    visible = counter_items[:limit]
    hidden = len(counter_items) - limit
    text = format_counter(visible, hex_width=hex_width)
    return f"{text}, ... (+{hidden} more)"


def format_size_words(counter_items: tuple[tuple[int, int], ...]) -> str:
    return ", ".join(f"{size_words}w x{count}" for size_words, count in counter_items)


def format_param_bytes(counter_items: tuple[tuple[int, int], ...]) -> str:
    return ", ".join(f"0x{(size_words * 4) - HEADER_SIZE:X} x{count}" for size_words, count in counter_items)


def format_sample(sample: PlacementSample, limit_words: int = 8) -> str:
    words = [sample.params[index : index + 4].hex().upper() for index in range(0, min(len(sample.params), limit_words * 4), 4)]
    return " ".join(words)


def size_def_histogram(summaries: list[PlacementSummary]) -> Counter[int]:
    histogram: Counter[int] = Counter()
    for summary in summaries:
        if summary.variable:
            continue
        histogram[summary.primary_size_words] += 1
    return histogram


def size_placement_histogram(summaries: list[PlacementSummary]) -> Counter[int]:
    histogram: Counter[int] = Counter()
    for summary in summaries:
        for size_words, count in summary.size_words:
            histogram[size_words] += count
    return histogram


def summary_markdown(summaries: list[PlacementSummary], romlist_count: int) -> str:
    fixed = [summary for summary in summaries if not summary.variable]
    variable = [summary for summary in summaries if summary.variable]
    by_def_size = size_def_histogram(summaries)
    by_placement_size = size_placement_histogram(summaries)
    top_fixed = sorted(fixed, key=lambda item: (-item.placements, item.def_id))

    lines: list[str] = []
    lines.append("# `orig/GSAE01/files/*.romlist.zlb` parameter catalog")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- Root romlists parsed: {romlist_count}")
    lines.append(f"- Canonical object defs seen in placements: {len(summaries)}")
    lines.append(f"- Fixed-size placement structs: {len(fixed)}")
    lines.append(f"- Variable-size placement structs: {len(variable)}")
    lines.append(
        "- Most common fixed object sizes by canonical def count: "
        + ", ".join(f"`{size_words}w`={count}" for size_words, count in by_def_size.most_common(8))
    )
    lines.append(
        "- Most common placed record sizes by total placements: "
        + ", ".join(f"`{size_words}w`={count}" for size_words, count in by_placement_size.most_common(8))
    )
    lines.append("")

    lines.append("## High-value findings")
    lines.append(
        "- Retail romlists already give real per-object placement sizes: for fixed-size defs the parameter payload is simply `(size_words * 4) - 0x18` bytes."
    )
    lines.append(
        "- Only one canonical object def varies in retail placements: `0x0491 curve`, which is a strong first target for variable-length object decoding."
    )
    lines.append(
        "- Retail placements go down to `6w` records, so the loader accepts header-only objects with no trailing param bytes; many common families start at `8w` with one extra `0x08`-byte param tail."
    )
    lines.append(
        "- High-usage families like `TrigPln`, `TrickyWarp`, `CmbSrc*`, and `HitAnimator` already have stable retail parameter widths that can drive struct definitions before code is named."
    )
    lines.append("")

    lines.append("## Highest-Usage Fixed-Size Objects")
    for summary in top_fixed[:15]:
        sample = summary.samples[0] if summary.samples else None
        sample_text = ""
        if sample is not None and sample.params:
            sample_text = f", sample=`{format_sample(sample)}`"
        map_text = "" if summary.map_name is None else f", map=`{summary.map_name}`"
        lines.append(
            f"- `0x{summary.def_id:04X}` `{summary.name}`: placements={summary.placements}, romlists={summary.romlist_count}, "
            f"dll=`0x{summary.dll_id:04X}`, class=`0x{summary.class_id & 0xFFFF:04X}`, "
            f"sizes={format_size_words(summary.size_words)}, params={format_param_bytes(summary.size_words)}{map_text}{sample_text}"
        )
    lines.append("")

    lines.append("## Variable-Size Objects")
    if variable:
        for summary in variable:
            sample_texts = [
                f"{sample.size_words}w from `{sample.romlist}` -> `{format_sample(sample)}`"
                for sample in summary.samples
            ]
            lines.append(
                f"- `0x{summary.def_id:04X}` `{summary.name}`: placements={summary.placements}, romlists={summary.romlist_count}, "
                f"dll=`0x{summary.dll_id:04X}`, class=`0x{summary.class_id & 0xFFFF:04X}`, "
                f"sizes={format_size_words(summary.size_words)}"
            )
            for sample_text in sample_texts[:4]:
                lines.append(f"  - {sample_text}")
    else:
        lines.append("- None")
    lines.append("")

    lines.append("## Practical Use")
    lines.append("- Summary: `python tools/orig/romlist_params.py`")
    lines.append("- CSV dump: `python tools/orig/romlist_params.py --format csv`")
    lines.append("- Search by object, DLL, class, map, or size:")
    lines.append("  - `python tools/orig/romlist_params.py --search curve`")
    lines.append("  - `python tools/orig/romlist_params.py --search dll:0x0126 size:20w`")
    lines.append("  - `python tools/orig/romlist_params.py --search variable`")
    return "\n".join(lines)


def matches_numeric(value: int, pattern: str) -> bool:
    if pattern.startswith("0x"):
        try:
            return value == int(pattern, 16)
        except ValueError:
            return False
    if pattern.isdigit():
        return value == int(pattern, 10)
    return pattern in (f"{value:x}", f"{value:04x}")


def matches_pattern(summary: PlacementSummary, pattern: str) -> bool:
    if pattern == "variable":
        return summary.variable
    if pattern == "fixed":
        return not summary.variable
    if pattern.startswith("def:"):
        return matches_numeric(summary.def_id, pattern[4:])
    if pattern.startswith("dll:"):
        return matches_numeric(summary.dll_id, pattern[4:])
    if pattern.startswith("class:"):
        return matches_numeric(summary.class_id & 0xFFFF, pattern[6:])
    if pattern.startswith("map:"):
        target = pattern[4:]
        if summary.map_name is None:
            return matches_numeric(summary.map_id, target)
        return target in summary.map_name.lower() or matches_numeric(summary.map_id, target)
    if pattern.startswith("size:"):
        target = pattern[5:]
        if target.endswith("w"):
            try:
                size_words = int(target[:-1], 0)
            except ValueError:
                return False
            return any(item_size == size_words for item_size, _count in summary.size_words)
        if target.endswith("b"):
            try:
                size_bytes = int(target[:-1], 0)
            except ValueError:
                return False
            return any((item_size * 4) - HEADER_SIZE == size_bytes for item_size, _count in summary.size_words)
        return False

    haystacks = [
        summary.name.lower(),
        f"{summary.def_id:04x}",
        f"0x{summary.def_id:04x}",
        f"{summary.dll_id:04x}",
        f"0x{summary.dll_id:04x}",
        f"{summary.class_id & 0xFFFF:04x}",
        f"0x{summary.class_id & 0xFFFF:04x}",
        "variable" if summary.variable else "fixed",
    ]
    if summary.map_name is not None:
        haystacks.append(summary.map_name.lower())
    for size_words, _count in summary.size_words:
        haystacks.extend([f"{size_words}w", f"0x{(size_words * 4) - HEADER_SIZE:x}"])
    return any(pattern in value for value in haystacks)


def search_markdown(summaries: list[PlacementSummary], patterns: list[str]) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    matches = [summary for summary in summaries if all(matches_pattern(summary, pattern) for pattern in lowered)]

    lines: list[str] = []
    lines.append("# Romlist parameter search")
    lines.append("")
    if not matches:
        lines.append("- No matching object defs.")
        return "\n".join(lines)

    for summary in matches[:25]:
        map_text = "" if summary.map_name is None else f", map=`{summary.map_name}`"
        lines.append(
            f"- `0x{summary.def_id:04X}` `{summary.name}`: "
            f"{'variable' if summary.variable else 'fixed'}, placements={summary.placements}, romlists={summary.romlist_count}, "
            f"dll=`0x{summary.dll_id:04X}`, class=`0x{summary.class_id & 0xFFFF:04X}`{map_text}"
        )
        lines.append(
            f"  sizes=`{format_size_words(summary.size_words)}`, params=`{format_param_bytes(summary.size_words)}`"
        )
        lines.append(
            f"  load_flags=`{format_counter_limited(summary.load_flags)}`, bounds=`{format_counter_limited(summary.bounds)}`, cull=`{format_counter_limited(summary.cull_dists)}`"
        )
        for sample in summary.samples[:4]:
            sample_text = format_sample(sample)
            lines.append(f"  sample {sample.size_words}w `{sample.romlist}`: `{sample_text}`")
    if len(matches) > 25:
        lines.append(f"- ... {len(matches) - 25} more matches omitted")
    return "\n".join(lines)


def rows_to_csv(summaries: list[PlacementSummary]) -> str:
    fieldnames = [
        "def_id",
        "def_id_hex",
        "name",
        "dll_id_hex",
        "class_id_hex",
        "map_id_hex",
        "map_name",
        "placements",
        "romlist_count",
        "variable",
        "sizes",
        "param_bytes",
        "load_flags",
        "bounds",
        "cull_dists",
        "samples",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for summary in summaries:
        writer.writerow(
            {
                "def_id": summary.def_id,
                "def_id_hex": f"0x{summary.def_id:04X}",
                "name": summary.name,
                "dll_id_hex": f"0x{summary.dll_id:04X}",
                "class_id_hex": f"0x{summary.class_id & 0xFFFF:04X}",
                "map_id_hex": "" if summary.map_id == 0xFFFF else f"0x{summary.map_id:04X}",
                "map_name": "" if summary.map_name is None else summary.map_name,
                "placements": summary.placements,
                "romlist_count": summary.romlist_count,
                "variable": str(summary.variable).lower(),
                "sizes": format_size_words(summary.size_words),
                "param_bytes": format_param_bytes(summary.size_words),
                "load_flags": format_counter(summary.load_flags),
                "bounds": format_counter(summary.bounds),
                "cull_dists": format_counter(summary.cull_dists),
                "samples": " | ".join(
                    f"{sample.size_words}w:{sample.romlist}:{format_sample(sample)}"
                    for sample in summary.samples
                ),
            }
        )
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Recover per-object retail romlist parameter sizes from orig/ placements.")
    parser.add_argument(
        "--files-root",
        type=Path,
        default=Path("orig/GSAE01/files"),
        help="Path to the extracted EN files/ directory.",
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
        help="Substring search across object names and IDs, plus `def:`, `dll:`, `class:`, `map:`, `size:`, `variable`, or `fixed`.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    summaries, romlist_count = build_summaries(args.files_root.resolve())
    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(summaries))
        elif args.search:
            sys.stdout.write(search_markdown(summaries, args.search))
            sys.stdout.write("\n")
        else:
            sys.stdout.write(summary_markdown(summaries, romlist_count))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
