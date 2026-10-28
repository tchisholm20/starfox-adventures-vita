from __future__ import annotations

import argparse
import io
import re
import sys
import struct
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path


SOURCE_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_./-])([A-Za-z0-9_./-]+\.(?:c|h))(?![A-Za-z0-9_./-])")
FILE_TOKEN_RE = re.compile(
    r"(?<![A-Za-z0-9_./-])([A-Za-z0-9_./-]+\.(?:bin|tab|zlb|romlist|thp|sam|sdi|poo|pro|wad|img))(?![A-Za-z0-9_./-])"
)
TAGGED_SOURCE_RE = re.compile(r"<([A-Za-z0-9_./-]+\.(?:c|h))>")
ERROR_HINT_RE = re.compile(r"(?:failed|error|warning|assert|panic|NULL|invalid)", re.IGNORECASE)


@dataclass(frozen=True)
class DolString:
    text: str
    ram_address: int
    file_offset: int
    section_index: int


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
            if section_sizes[index]
        ]

    def iter_strings(self, min_length: int = 4) -> list[DolString]:
        strings: list[DolString] = []
        for section_index, section in enumerate(self.sections):
            offset = int(section["offset"])
            size = int(section["size"])
            addr = int(section["addr"])
            section_data = self.data[offset : offset + size]
            cursor = 0
            while cursor < len(section_data):
                if 0x20 <= section_data[cursor] <= 0x7E:
                    start = cursor
                    while cursor < len(section_data) and 0x20 <= section_data[cursor] <= 0x7E:
                        cursor += 1
                    if cursor - start >= min_length:
                        text = section_data[start:cursor].decode("ascii", "ignore")
                        strings.append(
                            DolString(
                                text=text,
                                ram_address=addr + start,
                                file_offset=offset + start,
                                section_index=section_index,
                            )
                        )
                else:
                    cursor += 1
        return strings


def load_disc_paths(files_root: Path) -> tuple[set[str], set[str]]:
    basenames: set[str] = set()
    relative_paths: set[str] = set()
    for path in files_root.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(files_root).as_posix()
        relative_paths.add(rel)
        basenames.add(path.name)
    return basenames, relative_paths


def unique_ordered(values: list[str]) -> list[str]:
    seen: set[str] = set()
    result: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return result


def keep_file_token(token: str) -> bool:
    stem = Path(token).stem
    return len(stem) >= 2


def looks_like_internal_alias(token: str) -> bool:
    stem = Path(token).stem
    return sum(1 for char in stem if char.isupper()) >= 2


def build_summary(strings: list[DolString], disc_basenames: set[str]) -> dict[str, object]:
    source_hits: defaultdict[str, list[DolString]] = defaultdict(list)
    tagged_source_strings: list[DolString] = []
    file_hits: defaultdict[str, list[DolString]] = defaultdict(list)
    error_strings: list[DolString] = []

    for item in strings:
        source_tokens = SOURCE_TOKEN_RE.findall(item.text)
        for token in source_tokens:
            source_hits[token].append(item)
        if TAGGED_SOURCE_RE.search(item.text):
            tagged_source_strings.append(item)
        for token in FILE_TOKEN_RE.findall(item.text):
            if not keep_file_token(token):
                continue
            file_hits[token].append(item)
        if ERROR_HINT_RE.search(item.text):
            error_strings.append(item)

    dol_only_files = [
        token
        for token in sorted(file_hits)
        if "/" not in token and token not in disc_basenames and looks_like_internal_alias(token)
    ]
    return {
        "source_hits": source_hits,
        "tagged_source_strings": unique_ordered([item.text for item in tagged_source_strings]),
        "file_hits": file_hits,
        "dol_only_files": dol_only_files,
        "error_strings": unique_ordered([item.text for item in error_strings]),
    }


def summary_markdown(strings: list[DolString], summary: dict[str, object]) -> str:
    source_hits: dict[str, list[DolString]] = summary["source_hits"]
    tagged_source_strings: list[str] = summary["tagged_source_strings"]
    file_hits: dict[str, list[DolString]] = summary["file_hits"]
    dol_only_files: list[str] = summary["dol_only_files"]
    error_strings: list[str] = summary["error_strings"]

    source_counts = Counter({name: len(items) for name, items in source_hits.items()})
    common_files = Counter({name: len(items) for name, items in file_hits.items()})

    lines: list[str] = []
    lines.append("# `orig/GSAE01/sys/main.dol` string audit")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- Printable strings recovered: {len(strings)}")
    lines.append(f"- Unique source-like tokens: {len(source_hits)}")
    lines.append(f"- Unique file-like tokens: {len(file_hits)}")
    lines.append(f"- DOL-only file-family aliases absent from extracted disc filenames: {len(dol_only_files)}")
    lines.append("")

    lines.append("## Source-like names")
    for name, count in source_counts.most_common(20):
        first_hit = source_hits[name][0]
        lines.append(f"- `{name}`: {count} hits, first at RAM `0x{first_hit.ram_address:08X}`")
    lines.append("")

    lines.append("## Tagged source strings")
    if tagged_source_strings:
        for text in tagged_source_strings[:15]:
            lines.append(f"- `{text}`")
    else:
        lines.append("- None")
    lines.append("")

    lines.append("## DOL-only file aliases")
    for name in dol_only_files[:20]:
        first_hit = file_hits[name][0]
        lines.append(f"- `{name}`: first at RAM `0x{first_hit.ram_address:08X}`")
    lines.append("")

    lines.append("## Frequent file-family strings")
    for name, count in common_files.most_common(20):
        first_hit = file_hits[name][0]
        lines.append(f"- `{name}`: {count} hits, first at RAM `0x{first_hit.ram_address:08X}`")
    lines.append("")

    lines.append("## Error / warning style strings")
    for text in error_strings[:20]:
        lines.append(f"- `{text}`")
    lines.append("")

    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/dol_strings.py`")
    lines.append("- Search by substring with addresses: `python tools/orig/dol_strings.py --search camcontrol BLOCKS.bin`")
    return "\n".join(lines)


def search_markdown(strings: list[DolString], patterns: list[str]) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    matches = [
        item
        for item in strings
        if any(pattern in item.text.lower() for pattern in lowered)
    ]
    lines: list[str] = []
    lines.append("# DOL string search")
    lines.append("")
    if not matches:
        lines.append("- No matching strings.")
        return "\n".join(lines)
    for item in matches[:200]:
        lines.append(
            f"- RAM `0x{item.ram_address:08X}`, DOL `0x{item.file_offset:06X}`: `{item.text}`"
        )
    if len(matches) > 200:
        lines.append(f"- ... {len(matches) - 200} more matches omitted")
    return "\n".join(lines)


def rows_to_tsv(strings: list[DolString], patterns: list[str] | None) -> str:
    matches = strings
    if patterns:
        lowered = [pattern.lower() for pattern in patterns]
        matches = [
            item
            for item in strings
            if any(pattern in item.text.lower() for pattern in lowered)
        ]
    buffer = io.StringIO()
    buffer.write("ram_address\tdol_offset\tsection_index\ttext\n")
    for item in matches:
        buffer.write(f"0x{item.ram_address:08X}\t0x{item.file_offset:06X}\t{item.section_index}\t{item.text}\n")
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Extract address-aware printable strings from retail main.dol.")
    parser.add_argument(
        "--dol",
        type=Path,
        default=Path("orig/GSAE01/sys/main.dol"),
        help="Path to the EN main.dol.",
    )
    parser.add_argument(
        "--files-root",
        type=Path,
        default=Path("orig/GSAE01/files"),
        help="Path to the extracted EN files/ tree for filename comparison.",
    )
    parser.add_argument(
        "--search",
        nargs="+",
        help="Substring search against extracted DOL strings.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "tsv"),
        default="markdown",
        help="Output format.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    dol = DolFile(args.dol)
    strings = dol.iter_strings()
    disc_basenames, _disc_paths = load_disc_paths(args.files_root)
    try:
        if args.format == "tsv":
            sys.stdout.write(rows_to_tsv(strings, args.search))
            return
        if args.search:
            sys.stdout.write(search_markdown(strings, args.search))
            sys.stdout.write("\n")
            return
        summary = build_summary(strings, disc_basenames)
        sys.stdout.write(summary_markdown(strings, summary))
        sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
