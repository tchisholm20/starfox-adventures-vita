from __future__ import annotations

import argparse
import hashlib
import struct
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
import re


PRINTABLE_RE = re.compile(rb"[ -~]{4,}")
SOURCE_NAME_RE = re.compile(r"([A-Za-z0-9_./<>-]+\.(?:c|h))")
COMPARE_SKIP_SUFFIXES = {".adp", ".iso", ".sam", ".thp"}


@dataclass(frozen=True)
class RegionComparison:
    relative_path: str
    status: str
    sizes: tuple[int | None, int | None, int | None]


def file_sha1(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def iter_files(path: Path) -> list[Path]:
    return sorted(file for file in path.rglob("*") if file.is_file())


def format_size(size: int) -> str:
    units = ["B", "KiB", "MiB", "GiB"]
    value = float(size)
    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            if unit == "B":
                return f"{int(value)} {unit}"
            return f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{size} B"


def relative_files_by_region(orig_root: Path, regions: list[str]) -> dict[str, dict[str, Path]]:
    result: dict[str, dict[str, Path]] = {}
    for region in regions:
        region_root = orig_root / region
        files: dict[str, Path] = {}
        for file_path in iter_files(region_root):
            rel = file_path.relative_to(region_root).as_posix()
            files[rel] = file_path
        result[region] = files
    return result


def extension_counter(files: list[Path]) -> Counter[str]:
    counter: Counter[str] = Counter()
    for file_path in files:
        suffix = file_path.suffix or "(none)"
        counter[suffix] += 1
    return counter


def tiny_romlists(files_root: Path) -> list[tuple[str, int]]:
    result: list[tuple[str, int]] = []
    for file_path in sorted(files_root.glob("*.romlist.zlb")):
        size = file_path.stat().st_size
        if size <= 128:
            result.append((file_path.name, size))
    return result


def duplicate_groups(region_root: Path) -> list[tuple[int, str, list[str]]]:
    files = [
        path
        for path in iter_files(region_root)
        if path.suffix.lower() != ".iso"
    ]
    by_size: dict[int, list[Path]] = defaultdict(list)
    for file_path in files:
        by_size[file_path.stat().st_size].append(file_path)

    groups: list[tuple[int, str, list[str]]] = []
    for size, paths in by_size.items():
        if len(paths) < 2:
            continue
        by_hash: dict[str, list[Path]] = defaultdict(list)
        for path in paths:
            by_hash[file_sha1(path)].append(path)
        for digest, digest_paths in by_hash.items():
            if len(digest_paths) < 2:
                continue
            rel_paths = sorted(path.relative_to(region_root).as_posix() for path in digest_paths)
            groups.append((size, digest, rel_paths))

    groups.sort(key=lambda item: (-len(item[2]), -item[0], item[2][0]))
    return groups


def compare_regions(orig_root: Path, regions: list[str]) -> list[RegionComparison]:
    by_region = relative_files_by_region(orig_root, regions)
    all_paths = sorted(
        {
            path
            for files in by_region.values()
            for path in files
            if Path(path).suffix.lower() not in COMPARE_SKIP_SUFFIXES
        }
    )
    comparisons: list[RegionComparison] = []
    hash_cache: dict[Path, str] = {}

    def cached_sha1(path: Path) -> str:
        digest = hash_cache.get(path)
        if digest is None:
            digest = file_sha1(path)
            hash_cache[path] = digest
        return digest

    for relative_path in all_paths:
        paths = [by_region[region].get(relative_path) for region in regions]
        sizes = tuple(path.stat().st_size if path is not None else None for path in paths)
        if any(path is None for path in paths):
            status = "missing"
        else:
            digests = {cached_sha1(path) for path in paths if path is not None}
            if len(digests) == 1:
                status = "identical"
            elif len({size for size in sizes if size is not None}) == 1:
                status = "same-size-diff"
            else:
                status = "different-size"
        comparisons.append(
            RegionComparison(
                relative_path=relative_path,
                status=status,
                sizes=sizes,
            )
        )
    return comparisons


def parse_map_info(path: Path) -> list[dict[str, int | str]]:
    record_size = 0x20
    data = path.read_bytes()
    if len(data) % record_size != 0:
        raise ValueError(f"Unexpected MAPINFO.bin size: {len(data)}")
    records: list[dict[str, int | str]] = []
    for offset in range(0, len(data), record_size):
        name_raw, map_type, param2, param3 = struct.unpack_from(">28s2bh", data, offset)
        name = name_raw.split(b"\0", 1)[0].decode("ascii", "replace")
        records.append(
            {
                "index": offset // record_size,
                "name": name,
                "type": map_type,
                "param2": param2,
                "param3": param3,
            }
        )
    return records


def parse_warp_tab(path: Path) -> list[dict[str, float | int]]:
    record_size = 0x10
    data = path.read_bytes()
    if len(data) % record_size != 0:
        raise ValueError(f"Unexpected WARPTAB.bin size: {len(data)}")
    records: list[dict[str, float | int]] = []
    for offset in range(0, len(data), record_size):
        x_pos, y_pos, z_pos, layer, angle = struct.unpack_from(">3f2h", data, offset)
        records.append(
            {
                "index": offset // record_size,
                "x": x_pos,
                "y": y_pos,
                "z": z_pos,
                "layer": layer,
                "angle_raw": angle,
            }
        )
    return records


def parse_main_dol_source_names(path: Path) -> list[str]:
    data = path.read_bytes()
    matches: Counter[str] = Counter()
    for raw in PRINTABLE_RE.findall(data):
        text = raw.decode("ascii", "ignore")
        for match in SOURCE_NAME_RE.finditer(text):
            matches[match.group(1)] += 1
    return [f"{count}x {name}" for name, count in matches.most_common()]


def parse_main_dol_file_table_strings(path: Path) -> list[str]:
    data = path.read_bytes()
    raw_strings = [item.decode("ascii", "ignore") for item in PRINTABLE_RE.findall(data)]
    interesting = [
        item
        for item in raw_strings
        if any(token in item for token in (".bin", ".tab", ".zlb", ".romlist"))
    ]
    filtered: list[str] = []
    seen: set[str] = set()
    for item in interesting:
        if item in seen:
            continue
        seen.add(item)
        filtered.append(item)
    return filtered[:20]


def summary_markdown(orig_root: Path, version: str, regions: list[str]) -> str:
    region_root = orig_root / version
    files_root = region_root / "files"
    files = iter_files(region_root)
    extensions = extension_counter(files)
    duplicates = duplicate_groups(region_root)
    comparisons = compare_regions(orig_root, regions)
    map_info = parse_map_info(files_root / "MAPINFO.bin")
    warp_tab = parse_warp_tab(files_root / "WARPTAB.bin")
    source_names = parse_main_dol_source_names(region_root / "sys" / "main.dol")
    file_table_strings = parse_main_dol_file_table_strings(region_root / "sys" / "main.dol")

    comparison_counter = Counter(item.status for item in comparisons)
    root_files = sorted(path for path in files_root.iterdir() if path.is_file())
    zero_byte_root = [path.name for path in root_files if path.stat().st_size == 0]
    tiny_root_romlists = tiny_romlists(files_root)
    map_type_counts = Counter(int(item["type"]) for item in map_info)
    warp_layer_counts = Counter(int(item["layer"]) for item in warp_tab)

    lines: list[str] = []
    lines.append(f"# `orig/{version}` audit")
    lines.append("")
    lines.append("## Inventory")
    lines.append(f"- Files in region tree: {len(files)}")
    lines.append(f"- Map directories under `files/`: {len([path for path in files_root.iterdir() if path.is_dir()])}")
    lines.append(f"- Root files under `files/`: {len(root_files)}")
    lines.append("- Most common extensions:")
    for suffix, count in extensions.most_common(10):
        lines.append(f"  - `{suffix}`: {count}")

    lines.append("")
    lines.append("## Root file oddities")
    lines.append(f"- Zero-byte root files: {', '.join(f'`{name}`' for name in zero_byte_root)}")
    lines.append(f"- Tiny root romlists (<= 128 bytes): {len(tiny_root_romlists)}")
    for name, size in tiny_root_romlists[:12]:
        lines.append(f"  - `{name}`: {size} bytes")
    lines.append(f"- `MAPINFO.bin`: {len(map_info)} fixed 0x20-byte records")
    lines.append(
        "- `MAPINFO.bin` type histogram: "
        + ", ".join(f"`{map_type}`={count}" for map_type, count in sorted(map_type_counts.items()))
    )
    for item in map_info[:8]:
        lines.append(
            f"  - `{item['index']:02X}` `{item['name']}` type={item['type']} "
            f"param2={item['param2']} param3={item['param3']}"
        )
    lines.append(f"- `WARPTAB.bin`: {len(warp_tab)} fixed 0x10-byte records")
    lines.append(
        "- `WARPTAB.bin` layer histogram: "
        + ", ".join(f"`{layer}`={count}" for layer, count in sorted(warp_layer_counts.items()))
    )

    lines.append("")
    lines.append("## Duplicate content inside the EN tree")
    if duplicates:
        for size, _digest, rel_paths in duplicates[:12]:
            joined = ", ".join(f"`{rel_path}`" for rel_path in rel_paths[:5])
            more = "" if len(rel_paths) <= 5 else f" ... (+{len(rel_paths) - 5} more)"
            lines.append(f"- {format_size(size)}: {joined}{more}")
    else:
        lines.append("- No duplicate groups found.")

    lines.append("")
    lines.append("## Cross-region comparison")
    lines.append(
        "- Relative-path status counts: "
        + ", ".join(
            f"`{status}`={comparison_counter[status]}"
            for status in ("identical", "same-size-diff", "different-size", "missing")
            if comparison_counter[status]
        )
    )
    interesting = [
        item
        for item in comparisons
        if item.relative_path.startswith("files/")
        and item.status != "identical"
    ]
    for item in interesting[:15]:
        size_text = ", ".join("missing" if size is None else str(size) for size in item.sizes)
        lines.append(f"  - `{item.relative_path}`: {item.status} ({size_text})")

    lines.append("")
    lines.append("## `main.dol` strings")
    lines.append("- Source-like names in DOL strings:")
    for item in source_names[:12]:
        lines.append(f"  - `{item}`")
    lines.append("- File-table-like strings in DOL strings:")
    for item in file_table_strings[:10]:
        lines.append(f"  - `{item}`")

    return "\n".join(lines)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Audit bundled orig/ assets for decomp leads.")
    parser.add_argument(
        "--orig-root",
        type=Path,
        default=Path("orig"),
        help="Path to the repo orig/ root.",
    )
    parser.add_argument(
        "--version",
        default="GSAE01",
        help="Target region to summarize. Default: GSAE01",
    )
    parser.add_argument(
        "--regions",
        nargs="+",
        default=["GSAE01", "GSAP01", "GSAJ01"],
        help="Regions to include in cross-region comparison.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    print(summary_markdown(args.orig_root, args.version, args.regions))


if __name__ == "__main__":
    main()
