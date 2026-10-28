from __future__ import annotations

import argparse
import csv
import io
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


COUNTRY_CODES = {
    0: "Japan",
    1: "USA",
    2: "Europe",
    4: "Unknown-4",
}


@dataclass(frozen=True)
class BootInfo:
    game_code: str
    maker_code: str
    disc_number: int
    version: int
    audio_streaming: int
    stream_buffer_size: int
    dol_offset: int
    fst_offset: int
    fst_size: int
    fst_max_size: int
    user_position: int
    user_length: int


@dataclass(frozen=True)
class Bi2Info:
    debug_monitor_size: int
    simulated_mem_size: int
    argument_offset: int
    debug_flag: int
    track_location: int
    track_size: int
    country_code: int
    total_disc: int
    long_file_name_support: int
    dol_limit: int


@dataclass(frozen=True)
class ApploaderInfo:
    version: str
    entry_point: int
    size: int
    trailer_size: int


@dataclass(frozen=True)
class FstEntry:
    index: int
    path: str
    is_dir: bool
    file_offset: int | None
    size: int
    parent_index: int | None
    next_index: int | None


@dataclass(frozen=True)
class RegionSummary:
    region: str
    boot: BootInfo
    bi2: Bi2Info
    apploader: ApploaderInfo
    entries: list[FstEntry]
    extracted_file_paths: set[str]
    extracted_dir_paths: set[str]


@dataclass(frozen=True)
class PairwiseOrderCheck:
    region_a: str
    region_b: str
    common_files: int
    common_dirs: int
    file_order_identical: bool
    dir_order_identical: bool


def parse_boot_bin(path: Path) -> BootInfo:
    data = path.read_bytes()
    if len(data) < 0x43C:
        raise ValueError(f"boot.bin too small: {path}")
    return BootInfo(
        game_code=data[0:4].decode("ascii", "replace"),
        maker_code=data[4:6].decode("ascii", "replace"),
        disc_number=data[6],
        version=data[7],
        audio_streaming=data[8],
        stream_buffer_size=data[9],
        dol_offset=struct.unpack_from(">I", data, 0x420)[0],
        fst_offset=struct.unpack_from(">I", data, 0x424)[0],
        fst_size=struct.unpack_from(">I", data, 0x428)[0],
        fst_max_size=struct.unpack_from(">I", data, 0x42C)[0],
        user_position=struct.unpack_from(">I", data, 0x434)[0],
        user_length=struct.unpack_from(">I", data, 0x438)[0],
    )


def parse_bi2_bin(path: Path) -> Bi2Info:
    data = path.read_bytes()
    if len(data) < 0x28:
        raise ValueError(f"bi2.bin too small: {path}")
    return Bi2Info(
        debug_monitor_size=struct.unpack_from(">I", data, 0x00)[0],
        simulated_mem_size=struct.unpack_from(">I", data, 0x04)[0],
        argument_offset=struct.unpack_from(">I", data, 0x08)[0],
        debug_flag=struct.unpack_from(">I", data, 0x0C)[0],
        track_location=struct.unpack_from(">I", data, 0x10)[0],
        track_size=struct.unpack_from(">I", data, 0x14)[0],
        country_code=struct.unpack_from(">I", data, 0x18)[0],
        total_disc=struct.unpack_from(">I", data, 0x1C)[0],
        long_file_name_support=struct.unpack_from(">I", data, 0x20)[0],
        dol_limit=struct.unpack_from(">I", data, 0x24)[0],
    )


def parse_apploader_img(path: Path) -> ApploaderInfo:
    data = path.read_bytes()
    if len(data) < 0x20:
        raise ValueError(f"apploader.img too small: {path}")
    raw_version = data[0:0x10].split(b"\0", 1)[0]
    return ApploaderInfo(
        version=raw_version.decode("ascii", "replace"),
        entry_point=struct.unpack_from(">I", data, 0x10)[0],
        size=struct.unpack_from(">I", data, 0x14)[0],
        trailer_size=struct.unpack_from(">I", data, 0x18)[0],
    )


def _read_c_string(blob: bytes, offset: int) -> str:
    end = blob.find(b"\0", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("ascii", "replace")


def parse_fst(path: Path) -> list[FstEntry]:
    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError(f"fst.bin too small: {path}")

    root_word0, root_word1, root_word2 = struct.unpack_from(">III", data, 0)
    if (root_word0 >> 24) != 1:
        raise ValueError("FST root entry is not a directory")

    entry_count = root_word2
    names_base = entry_count * 12
    if len(data) < names_base:
        raise ValueError("FST name table starts past end of file")

    raw_entries: list[tuple[bool, str, int, int]] = []
    for index in range(entry_count):
        word0, word1, word2 = struct.unpack_from(">III", data, index * 12)
        is_dir = bool(word0 >> 24)
        name_offset = word0 & 0x00FFFFFF
        raw_entries.append((is_dir, _read_c_string(data, names_base + name_offset), word1, word2))

    entries: list[FstEntry] = [
        FstEntry(
            index=0,
            path="",
            is_dir=True,
            file_offset=None,
            size=0,
            parent_index=0,
            next_index=entry_count,
        )
    ]

    def walk(dir_index: int, parent_path: str) -> None:
        cursor = dir_index + 1
        _, _, _, end_index = raw_entries[dir_index]
        while cursor < end_index:
            is_dir, name, word1, word2 = raw_entries[cursor]
            path_text = f"{parent_path}/{name}" if parent_path else name
            if is_dir:
                entries.append(
                    FstEntry(
                        index=cursor,
                        path=path_text,
                        is_dir=True,
                        file_offset=None,
                        size=0,
                        parent_index=word1,
                        next_index=word2,
                    )
                )
                walk(cursor, path_text)
                cursor = word2
            else:
                entries.append(
                    FstEntry(
                        index=cursor,
                        path=path_text,
                        is_dir=False,
                        file_offset=word1,
                        size=word2,
                        parent_index=None,
                        next_index=None,
                    )
                )
                cursor += 1

    walk(0, "")
    entries.sort(key=lambda entry: entry.index)
    return entries


def collect_extracted_paths(files_root: Path) -> tuple[set[str], set[str]]:
    file_paths: set[str] = set()
    dir_paths: set[str] = set()
    for path in files_root.rglob("*"):
        rel = path.relative_to(files_root).as_posix()
        if path.is_dir():
            dir_paths.add(rel)
        elif path.is_file():
            file_paths.add(rel)
    return file_paths, dir_paths


def load_region_summary(orig_root: Path, region: str) -> RegionSummary:
    region_root = orig_root / region
    sys_root = region_root / "sys"
    files_root = region_root / "files"
    extracted_files, extracted_dirs = collect_extracted_paths(files_root)
    return RegionSummary(
        region=region,
        boot=parse_boot_bin(sys_root / "boot.bin"),
        bi2=parse_bi2_bin(sys_root / "bi2.bin"),
        apploader=parse_apploader_img(sys_root / "apploader.img"),
        entries=parse_fst(sys_root / "fst.bin"),
        extracted_file_paths=extracted_files,
        extracted_dir_paths=extracted_dirs,
    )


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


def file_entries(entries: list[FstEntry]) -> list[FstEntry]:
    return [entry for entry in entries if not entry.is_dir]


def dir_entries(entries: list[FstEntry]) -> list[FstEntry]:
    return [entry for entry in entries if entry.is_dir and entry.index != 0]


def compare_regions(summaries: list[RegionSummary]) -> dict[str, object]:
    file_lists = [[entry.path for entry in file_entries(summary.entries)] for summary in summaries]
    dir_lists = [[entry.path for entry in dir_entries(summary.entries)] for summary in summaries]
    file_order_identical = all(paths == file_lists[0] for paths in file_lists[1:])
    dir_order_identical = all(paths == dir_lists[0] for paths in dir_lists[1:])

    by_region = {
        summary.region: {entry.path: entry for entry in file_entries(summary.entries)}
        for summary in summaries
    }
    common_paths = sorted(set.intersection(*(set(mapping) for mapping in by_region.values())))
    missing_paths = sorted(set.union(*(set(mapping) for mapping in by_region.values())) - set(common_paths))

    size_status = Counter()
    size_diffs: list[tuple[str, tuple[int | None, ...]]] = []
    regions = [summary.region for summary in summaries]
    for path_text in common_paths:
        sizes = tuple(by_region[region][path_text].size for region in regions)
        if len(set(sizes)) == 1:
            size_status["identical-size"] += 1
        else:
            size_status["different-size"] += 1
            size_diffs.append((path_text, sizes))

    size_diffs.sort(
        key=lambda item: (
            -max(item[1]) + min(item[1]),
            item[0],
        )
    )

    pairwise_checks: list[PairwiseOrderCheck] = []
    for index, summary_a in enumerate(summaries):
        files_a = [entry.path for entry in file_entries(summary_a.entries)]
        dirs_a = [entry.path for entry in dir_entries(summary_a.entries)]
        set_files_a = set(files_a)
        set_dirs_a = set(dirs_a)
        for summary_b in summaries[index + 1 :]:
            files_b = [entry.path for entry in file_entries(summary_b.entries)]
            dirs_b = [entry.path for entry in dir_entries(summary_b.entries)]
            common_files = set_files_a & set(files_b)
            common_dirs = set_dirs_a & set(dirs_b)
            pairwise_checks.append(
                PairwiseOrderCheck(
                    region_a=summary_a.region,
                    region_b=summary_b.region,
                    common_files=len(common_files),
                    common_dirs=len(common_dirs),
                    file_order_identical=[path for path in files_a if path in common_files]
                    == [path for path in files_b if path in common_files],
                    dir_order_identical=[path for path in dirs_a if path in common_dirs]
                    == [path for path in dirs_b if path in common_dirs],
                )
            )

    extracted_checks: dict[str, dict[str, object]] = {}
    for summary in summaries:
        fst_file_paths = {entry.path for entry in file_entries(summary.entries)}
        fst_dir_paths = {entry.path for entry in dir_entries(summary.entries)}
        extracted_checks[summary.region] = {
            "missing_files": sorted(fst_file_paths - summary.extracted_file_paths),
            "extra_files": sorted(summary.extracted_file_paths - fst_file_paths),
            "missing_dirs": sorted(fst_dir_paths - summary.extracted_dir_paths),
            "extra_dirs": sorted(summary.extracted_dir_paths - fst_dir_paths),
        }

    return {
        "file_order_identical": file_order_identical,
        "dir_order_identical": dir_order_identical,
        "size_status": size_status,
        "size_diffs": size_diffs,
        "missing_paths": missing_paths,
        "pairwise_checks": pairwise_checks,
        "extracted_checks": extracted_checks,
    }


def lookup_entries(entries: list[FstEntry], patterns: list[str]) -> list[FstEntry]:
    lowered = [pattern.lower() for pattern in patterns]
    return [
        entry
        for entry in entries
        if any(pattern in entry.path.lower() for pattern in lowered)
    ]


def summary_markdown(summaries: list[RegionSummary], comparison: dict[str, object]) -> str:
    lines: list[str] = []
    lines.append("# `orig/*/sys` audit")
    lines.append("")

    target = summaries[0]
    target_boot = target.boot
    target_bi2 = target.bi2
    target_apploader = target.apploader
    lines.append(f"## EN retail header facts (`{target.region}`)")
    lines.append(
        f"- `boot.bin`: game=`{target_boot.game_code}`, maker=`{target_boot.maker_code}`, "
        f"disc={target_boot.disc_number}, revision byte=`0x{target_boot.version:02X}`"
    )
    lines.append(
        f"- `boot.bin`: DOL offset=`0x{target_boot.dol_offset:08X}`, "
        f"FST offset=`0x{target_boot.fst_offset:08X}`, FST size=`0x{target_boot.fst_size:X}`"
    )
    lines.append(
        f"- `bi2.bin`: country=`{COUNTRY_CODES.get(target_bi2.country_code, str(target_bi2.country_code))}`, "
        f"total discs={target_bi2.total_disc}, debug flag=`0x{target_bi2.debug_flag:X}`"
    )
    lines.append(
        f"- `apploader.img`: version=`{target_apploader.version}`, entry=`0x{target_apploader.entry_point:08X}`, "
        f"loader=`{format_size(target_apploader.size)}`, trailer=`{format_size(target_apploader.trailer_size)}`"
    )
    lines.append("")

    lines.append("## FST structure")
    lines.append(
        f"- `{target.region}` FST entries: {len(target.entries)} total, "
        f"{len(file_entries(target.entries))} files, {len(dir_entries(target.entries))} directories"
    )
    root_files = [entry for entry in target.entries if not entry.is_dir and "/" not in entry.path]
    lines.append(
        "- First root files by FST index: "
        + ", ".join(
            f"`0x{entry.index:04X}` `{entry.path}`"
            for entry in root_files[:12]
        )
    )
    lines.append(
        "- Large root files: "
        + ", ".join(
            f"`{entry.path}`={format_size(entry.size)}"
            for entry in sorted(root_files, key=lambda entry: (-entry.size, entry.path))[:10]
        )
    )
    lines.append("")

    lines.append("## Cross-region comparison")
    lines.append(
        f"- File-order stability across `{', '.join(summary.region for summary in summaries)}`: "
        f"{'identical' if comparison['file_order_identical'] else 'different'}"
    )
    lines.append(
        f"- Directory-order stability across `{', '.join(summary.region for summary in summaries)}`: "
        f"{'identical' if comparison['dir_order_identical'] else 'different'}"
    )
    size_status = comparison["size_status"]
    lines.append(
        "- Shared file-path size status: "
        + ", ".join(
            f"`{status}`={size_status[status]}"
            for status in ("identical-size", "different-size")
            if size_status[status]
        )
    )
    if comparison["missing_paths"]:
        lines.append(f"- File paths missing in at least one region: {len(comparison['missing_paths'])}")
    else:
        lines.append("- Every FST file path exists in all listed regions.")
    for check in comparison["pairwise_checks"]:
        lines.append(
            f"  - Shared-path order `{check.region_a}` vs `{check.region_b}`: "
            f"files={check.common_files} ({'stable' if check.file_order_identical else 'different'}), "
            f"dirs={check.common_dirs} ({'stable' if check.dir_order_identical else 'different'})"
        )
    for path_text, sizes in comparison["size_diffs"][:12]:
        size_summary = ", ".join(
            f"`{summary.region}`={format_size(size)}"
            for summary, size in zip(summaries, sizes)
        )
        lines.append(f"  - `{path_text}`: {size_summary}")
    lines.append("")

    lines.append("## Extracted-tree verification")
    extracted_checks = comparison["extracted_checks"]
    for summary in summaries:
        check = extracted_checks[summary.region]
        missing_files = check["missing_files"]
        extra_files = check["extra_files"]
        missing_dirs = check["missing_dirs"]
        extra_dirs = check["extra_dirs"]
        lines.append(
            f"- `{summary.region}` vs extracted `files/`: missing files={len(missing_files)}, "
            f"extra files={len(extra_files)}, missing dirs={len(missing_dirs)}, extra dirs={len(extra_dirs)}"
        )
    lines.append("")

    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/sys_audit.py`")
    lines.append("- Full CSV of EN FST entries: `python tools/orig/sys_audit.py --format csv`")
    lines.append("- Lookup specific retail file IDs: `python tools/orig/sys_audit.py --path GAMETEXT.bin frontend.romlist.zlb`")
    return "\n".join(lines)


def rows_to_csv(entries: list[FstEntry]) -> str:
    fieldnames = [
        "index",
        "index_hex",
        "type",
        "path",
        "file_offset_hex",
        "size",
        "parent_index_hex",
        "next_index_hex",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for entry in entries:
        writer.writerow(
            {
                "index": entry.index,
                "index_hex": f"0x{entry.index:04X}",
                "type": "dir" if entry.is_dir else "file",
                "path": entry.path,
                "file_offset_hex": "" if entry.file_offset is None else f"0x{entry.file_offset:08X}",
                "size": entry.size,
                "parent_index_hex": "" if entry.parent_index is None else f"0x{entry.parent_index:04X}",
                "next_index_hex": "" if entry.next_index is None else f"0x{entry.next_index:04X}",
            }
        )
    return buffer.getvalue()


def lookup_markdown(entries: list[FstEntry], patterns: list[str]) -> str:
    matches = lookup_entries(entries, patterns)
    lines: list[str] = []
    lines.append("# FST lookup")
    lines.append("")
    if not matches:
        lines.append("- No matching FST entries.")
        return "\n".join(lines)
    for entry in matches:
        detail = (
            f"offset=`0x{entry.file_offset:08X}`, size={entry.size}"
            if not entry.is_dir
            else f"parent=`0x{entry.parent_index:04X}`, next=`0x{entry.next_index:04X}`"
        )
        lines.append(f"- `0x{entry.index:04X}` `{entry.path}` ({'dir' if entry.is_dir else 'file'}, {detail})")
    return "\n".join(lines)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Audit retail sys/FST metadata from orig/.")
    parser.add_argument(
        "--orig-root",
        type=Path,
        default=Path("orig"),
        help="Path to the repo orig/ root.",
    )
    parser.add_argument(
        "--regions",
        nargs="+",
        default=["GSAE01", "GSAP01", "GSAJ01"],
        help="Regions to load. First region is the primary summary target.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "csv"),
        default="markdown",
        help="Output format.",
    )
    parser.add_argument(
        "--path",
        nargs="+",
        help="Substring lookup against the primary region FST paths.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    summaries = [load_region_summary(args.orig_root, region) for region in args.regions]
    primary_entries = summaries[0].entries
    try:
        if args.path:
            if args.format == "csv":
                sys.stdout.write(rows_to_csv(lookup_entries(primary_entries, args.path)))
            else:
                sys.stdout.write(lookup_markdown(primary_entries, args.path))
                sys.stdout.write("\n")
            return

        if args.format == "csv":
            sys.stdout.write(rows_to_csv(primary_entries))
        else:
            comparison = compare_regions(summaries)
            sys.stdout.write(summary_markdown(summaries, comparison))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
