from __future__ import annotations

import argparse
import struct
import zlib
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class ObjectStats:
    canonical_id: int
    name: str
    placements: int = 0
    romlists: set[str] = field(default_factory=set)
    size_words: Counter[int] = field(default_factory=Counter)
    sample_params: bytes | None = None


def load_object_names(files_root: Path) -> dict[int, str]:
    tab_path = files_root / "OBJECTS.tab"
    bin_path = files_root / "OBJECTS.bin"
    offsets: list[int] = []
    with tab_path.open("rb") as tab_file:
        while True:
            data = tab_file.read(4)
            if len(data) < 4:
                break
            offset = struct.unpack(">I", data)[0]
            if offset == 0xFFFFFFFF:
                break
            offsets.append(offset)

    names: dict[int, str] = {}
    with bin_path.open("rb") as bin_file:
        for object_id, offset in enumerate(offsets):
            bin_file.seek(offset + 0x91)
            raw = bin_file.read(11)
            name = raw.replace(b"\0", b" ").decode("ascii", "replace").strip() or f"obj_{object_id:04X}"
            names[object_id] = name
    return names


def load_object_remap(files_root: Path) -> dict[int, int]:
    remap: dict[int, int] = {}
    data = (files_root / "OBJINDEX.bin").read_bytes()
    for index in range(0, len(data), 2):
        value = struct.unpack_from(">h", data, index)[0]
        if value != -1:
            remap[index // 2] = value
    return remap


def decompress_zlb(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:4] != b"ZLB\0":
        raise ValueError(f"Unsupported romlist container in {path}")
    version, decompressed_size, compressed_size = struct.unpack_from(">3I", data, 4)
    if version != 1:
        raise ValueError(f"Unexpected ZLB version {version} in {path}")
    payload = zlib.decompress(data[16 : 16 + compressed_size])
    if len(payload) != decompressed_size:
        raise ValueError(f"Bad ZLB size in {path}: expected {decompressed_size}, got {len(payload)}")
    return payload


def format_words_hex(data: bytes) -> str:
    return " ".join(data[index : index + 4].hex().upper() for index in range(0, len(data), 4))


def audit_romlists(files_root: Path) -> tuple[dict[int, ObjectStats], list[dict[str, object]]]:
    object_names = load_object_names(files_root)
    object_remap = load_object_remap(files_root)
    object_stats: dict[int, ObjectStats] = {}
    romlist_summaries: list[dict[str, object]] = []

    for romlist_path in sorted(files_root.glob("*.romlist.zlb")):
        payload = decompress_zlb(romlist_path)
        offset = 0
        placements = 0
        unique_objects: set[int] = set()
        while offset < len(payload):
            object_id, size_words, flags = struct.unpack_from(">hBB", payload, offset)
            canonical_id = object_remap.get(object_id, object_id)
            record_size = size_words * 4
            if record_size <= 0 or offset + record_size > len(payload):
                raise ValueError(
                    f"Invalid record in {romlist_path.name}: offset=0x{offset:X} size_words={size_words}"
                )
            param_bytes = payload[offset + 0x18 : offset + record_size]
            stats = object_stats.setdefault(
                canonical_id,
                ObjectStats(
                    canonical_id=canonical_id,
                    name=object_names.get(canonical_id, f"obj_{canonical_id:04X}"),
                ),
            )
            stats.placements += 1
            stats.romlists.add(romlist_path.name)
            stats.size_words[size_words] += 1
            if stats.sample_params is None and param_bytes:
                stats.sample_params = param_bytes
            unique_objects.add(canonical_id)
            placements += 1
            offset += record_size

        romlist_summaries.append(
            {
                "name": romlist_path.name,
                "compressed_size": romlist_path.stat().st_size,
                "decompressed_size": len(payload),
                "placements": placements,
                "unique_objects": len(unique_objects),
            }
        )

    return object_stats, romlist_summaries


def markdown_report(files_root: Path) -> str:
    object_stats, romlist_summaries = audit_romlists(files_root)
    varying = [
        stats
        for stats in object_stats.values()
        if len(stats.size_words) > 1
    ]
    top_objects = sorted(
        object_stats.values(),
        key=lambda stats: (-stats.placements, stats.canonical_id),
    )
    top_romlists = sorted(
        romlist_summaries,
        key=lambda item: (-int(item["placements"]), -int(item["decompressed_size"]), str(item["name"])),
    )
    single_record_romlists = [item for item in romlist_summaries if int(item["placements"]) == 1]

    lines: list[str] = []
    lines.append("# `orig/GSAE01/files/*.romlist.zlb` audit")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- Romlists parsed: {len(romlist_summaries)}")
    lines.append(f"- Unique canonical object IDs seen: {len(object_stats)}")
    lines.append(f"- Single-record romlists: {len(single_record_romlists)}")
    lines.append(f"- Objects with varying record sizes: {len(varying)}")

    lines.append("")
    lines.append("## Busiest romlists")
    for item in top_romlists[:15]:
        lines.append(
            f"- `{item['name']}`: placements={item['placements']}, "
            f"unique_objects={item['unique_objects']}, "
            f"zlb={item['compressed_size']} bytes, raw={item['decompressed_size']} bytes"
        )

    lines.append("")
    lines.append("## Most-used object definitions")
    for stats in top_objects[:20]:
        size_summary = ", ".join(f"{size_words}w x{count}" for size_words, count in sorted(stats.size_words.items()))
        example = ""
        if stats.sample_params is not None:
            example = f", example_params=`{format_words_hex(stats.sample_params[:32])}`"
        lines.append(
            f"- `0x{stats.canonical_id:04X}` `{stats.name}`: placements={stats.placements}, "
            f"romlists={len(stats.romlists)}, sizes={size_summary}{example}"
        )

    lines.append("")
    lines.append("## Varying-size objects")
    if varying:
        for stats in sorted(varying, key=lambda item: (-item.placements, item.canonical_id))[:20]:
            size_summary = ", ".join(f"{size_words}w x{count}" for size_words, count in sorted(stats.size_words.items()))
            lines.append(
                f"- `0x{stats.canonical_id:04X}` `{stats.name}`: placements={stats.placements}, sizes={size_summary}"
            )
    else:
        lines.append("- None")

    lines.append("")
    lines.append("## Minimal romlists")
    for item in sorted(single_record_romlists, key=lambda entry: (int(entry["decompressed_size"]), str(entry["name"])))[:15]:
        lines.append(
            f"- `{item['name']}`: raw={item['decompressed_size']} bytes, zlb={item['compressed_size']} bytes"
        )

    return "\n".join(lines)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Mine object placement metadata from orig romlists.")
    parser.add_argument(
        "--files-root",
        type=Path,
        default=Path("orig/GSAE01/files"),
        help="Path to the extracted files/ directory.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    print(markdown_report(args.files_root))


if __name__ == "__main__":
    main()
