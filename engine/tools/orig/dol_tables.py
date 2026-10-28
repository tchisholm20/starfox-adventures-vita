from __future__ import annotations

import argparse
import csv
import io
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


FILE_TOKEN_RE = re.compile(r"\.(?:bin|tab)$", re.IGNORECASE)
SYMBOL_RE = re.compile(r"^(\S+)\s*=\s*\.\S+:0x([0-9A-Fa-f]+);")


@dataclass(frozen=True)
class DolSection:
    index: int
    offset: int
    address: int
    size: int


@dataclass(frozen=True)
class FileTableEntry:
    file_id: int
    table_address: int
    string_address: int
    name: str
    duplicate_of: int | None
    disc_paths: tuple[str, ...]
    match_kind: str


@dataclass(frozen=True)
class InitTableEntry:
    table_name: str
    table_address: int
    index: int
    target: int
    symbol: str | None


@dataclass(frozen=True)
class FstEntry:
    index: int
    path: str


class DolFile:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        section_offsets = [struct.unpack_from(">I", self.data, index * 4)[0] for index in range(18)]
        section_addrs = [struct.unpack_from(">I", self.data, 0x48 + index * 4)[0] for index in range(18)]
        section_sizes = [struct.unpack_from(">I", self.data, 0x90 + index * 4)[0] for index in range(18)]
        self.sections = [
            DolSection(
                index=index,
                offset=section_offsets[index],
                address=section_addrs[index],
                size=section_sizes[index],
            )
            for index in range(18)
            if section_sizes[index]
        ]
        self.text_sections = [section for section in self.sections if section.index <= 6]

    def addr_to_offset(self, addr: int) -> int | None:
        for section in self.sections:
            if section.address <= addr < section.address + section.size:
                return section.offset + (addr - section.address)
        return None

    def offset_to_addr(self, offset: int) -> int | None:
        for section in self.sections:
            if section.offset <= offset < section.offset + section.size:
                return section.address + (offset - section.offset)
        return None

    def is_text_pointer(self, addr: int) -> bool:
        for section in self.text_sections:
            if section.address <= addr < section.address + section.size:
                return True
        return False

    def read_u32(self, addr: int) -> int:
        offset = self.addr_to_offset(addr)
        if offset is None:
            raise ValueError(f"Address 0x{addr:08X} is not inside a loaded DOL section")
        return struct.unpack_from(">I", self.data, offset)[0]

    def read_c_string(self, addr: int, max_length: int = 64) -> str | None:
        offset = self.addr_to_offset(addr)
        if offset is None:
            return None
        end = self.data.find(b"\0", offset)
        if end < 0 or end == offset or end - offset > max_length:
            return None
        raw = self.data[offset:end]
        if any(byte < 0x20 or byte > 0x7E for byte in raw):
            return None
        return raw.decode("ascii")


def _read_c_string(blob: bytes, offset: int) -> str:
    end = blob.find(b"\0", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("ascii", "replace")


def parse_fst(path: Path) -> list[FstEntry]:
    data = path.read_bytes()
    root_word0, _, root_word2 = struct.unpack_from(">III", data, 0)
    if (root_word0 >> 24) != 1:
        raise ValueError("FST root entry is not a directory")

    entry_count = root_word2
    names_base = entry_count * 12
    raw_entries: list[tuple[bool, str, int, int]] = []
    for index in range(entry_count):
        word0, word1, word2 = struct.unpack_from(">III", data, index * 12)
        is_dir = bool(word0 >> 24)
        name_offset = word0 & 0x00FFFFFF
        raw_entries.append((is_dir, _read_c_string(data, names_base + name_offset), word1, word2))

    entries: list[FstEntry] = []

    def walk(dir_index: int, parent_path: str) -> None:
        cursor = dir_index + 1
        _, _, end_index = raw_entries[dir_index][1:]
        while cursor < end_index:
            is_dir, name, word1, word2 = raw_entries[cursor]
            path_text = f"{parent_path}/{name}" if parent_path else name
            if is_dir:
                walk(cursor, path_text)
                cursor = word2
            else:
                entries.append(FstEntry(index=cursor, path=path_text))
                cursor += 1

    walk(0, "")
    return entries


def load_symbols(path: Path | None) -> dict[int, list[str]]:
    if path is None or not path.is_file():
        return {}
    symbols: dict[int, list[str]] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = SYMBOL_RE.match(line)
        if match is None:
            continue
        name = match.group(1)
        address = int(match.group(2), 16)
        symbols.setdefault(address, []).append(name)
    return symbols


def is_file_token(text: str | None) -> bool:
    return text is not None and bool(FILE_TOKEN_RE.search(text))


def find_file_table(dol: DolFile, min_length: int = 16) -> tuple[int, list[tuple[int, str]]]:
    best_start: int | None = None
    best_values: list[tuple[int, str]] = []

    for section in dol.sections:
        if section.index <= 6:
            continue
        values: list[tuple[int, str]] = []
        value_start: int | None = None
        for rel in range(0, section.size, 4):
            table_addr = section.address + rel
            ptr = dol.read_u32(table_addr)
            text = dol.read_c_string(ptr)
            if is_file_token(text):
                if value_start is None:
                    value_start = table_addr
                values.append((ptr, text))
            else:
                if len(values) > len(best_values):
                    best_start = value_start
                    best_values = values
                values = []
                value_start = None
        if len(values) > len(best_values):
            best_start = value_start
            best_values = values

    if best_start is None or len(best_values) < min_length:
        raise ValueError("Failed to locate a plausible runtime file-ID table in the DOL")
    return best_start, best_values


def build_disc_lookup(entries: list[FstEntry]) -> tuple[dict[str, list[FstEntry]], dict[str, list[FstEntry]]]:
    exact: dict[str, list[FstEntry]] = {}
    folded: dict[str, list[FstEntry]] = {}
    for entry in entries:
        basename = Path(entry.path).name
        exact.setdefault(basename, []).append(entry)
        folded.setdefault(basename.lower(), []).append(entry)
    return exact, folded


def match_disc_paths(
    name: str,
    exact_lookup: dict[str, list[FstEntry]],
    folded_lookup: dict[str, list[FstEntry]],
) -> tuple[tuple[str, ...], str]:
    exact_matches = exact_lookup.get(name, [])
    if exact_matches:
        return tuple(entry.path for entry in exact_matches), "exact"

    folded_matches = folded_lookup.get(name.lower(), [])
    if folded_matches:
        return tuple(entry.path for entry in folded_matches), "case-folded"

    return (), "none"


def build_file_entries(dol: DolFile, fst_entries: list[FstEntry]) -> tuple[int, list[FileTableEntry]]:
    table_address, raw_entries = find_file_table(dol)
    exact_lookup, folded_lookup = build_disc_lookup(fst_entries)
    first_seen: dict[int, int] = {}
    entries: list[FileTableEntry] = []
    for file_id, (string_address, name) in enumerate(raw_entries):
        duplicate_of = first_seen.get(string_address)
        if duplicate_of is None:
            first_seen[string_address] = file_id
        disc_paths, match_kind = match_disc_paths(name, exact_lookup, folded_lookup)
        entries.append(
            FileTableEntry(
                file_id=file_id,
                table_address=table_address + (file_id * 4),
                string_address=string_address,
                name=name,
                duplicate_of=duplicate_of,
                disc_paths=disc_paths,
                match_kind=match_kind,
            )
        )
    return table_address, entries


def build_init_tables(dol: DolFile, symbols: dict[int, list[str]]) -> list[InitTableEntry]:
    candidates: list[DolSection] = []
    for section in dol.sections:
        if section.index <= 6 or section.size > 0x40 or section.size % 4 != 0:
            continue
        words = [dol.read_u32(section.address + rel) for rel in range(0, section.size, 4)]
        nonzero = [word for word in words if word != 0]
        if not nonzero:
            continue
        if all(dol.is_text_pointer(word) for word in nonzero):
            candidates.append(section)

    candidates.sort(key=lambda section: section.address)
    table_names = [".ctors", ".dtors"]
    results: list[InitTableEntry] = []
    for table_index, section in enumerate(candidates[:2]):
        table_name = table_names[table_index] if table_index < len(table_names) else f".init_table_{table_index}"
        for index, rel in enumerate(range(0, section.size, 4)):
            target = dol.read_u32(section.address + rel)
            if target == 0:
                continue
            names = symbols.get(target, [])
            results.append(
                InitTableEntry(
                    table_name=table_name,
                    table_address=section.address,
                    index=index,
                    target=target,
                    symbol=names[0] if names else None,
                )
            )
    return results


def summary_markdown(table_address: int, entries: list[FileTableEntry], init_entries: list[InitTableEntry]) -> str:
    unique_names = len({entry.string_address for entry in entries})
    duplicate_entries = [entry for entry in entries if entry.duplicate_of is not None]
    no_match_entries = [entry for entry in entries if entry.match_kind == "none"]
    case_folded_entries = [entry for entry in entries if entry.match_kind == "case-folded"]

    lines: list[str] = []
    lines.append("# `orig/GSAE01/sys/main.dol` runtime table audit")
    lines.append("")
    lines.append("## Runtime file-ID table")
    lines.append(
        f"- Recovered straight from EN `main.dol` at RAM `0x{table_address:08X}` with `{len(entries)}` file IDs (`0x00`..`0x{len(entries) - 1:02X}`)"
    )
    lines.append(f"- Unique string targets: {unique_names}")
    lines.append(f"- Duplicate IDs reusing an earlier filename pointer: {len(duplicate_entries)}")
    lines.append(f"- IDs with an exact FST basename match: {len([entry for entry in entries if entry.match_kind == 'exact'])}")
    lines.append(f"- IDs with only a case-folded basename match: {len(case_folded_entries)}")
    lines.append(f"- IDs with no retail basename match at all: {len(no_match_entries)}")
    lines.append("")
    lines.append("## High-value runtime IDs")
    interesting_names = {
        "BLOCKS.bin",
        "BLOCKS.tab",
        "DLLS.bin",
        "DLLS.tab",
        "DLLSIMPO.bin",
        "PREANIM.bin",
        "PREANIM.tab",
        "TEXPRE.bin",
        "TEXPRE.tab",
    }
    for entry in [item for item in entries if item.name in interesting_names]:
        duplicate_text = "" if entry.duplicate_of is None else f", duplicate_of=`0x{entry.duplicate_of:02X}`"
        if entry.disc_paths:
            disc_text = ", ".join(f"`{path}`" for path in entry.disc_paths[:3])
        else:
            disc_text = "no basename match in FST"
        lines.append(
            f"- `0x{entry.file_id:02X}` `{entry.name}` at table slot `0x{entry.table_address:08X}`"
            f"{duplicate_text}: {disc_text}"
        )
    lines.append("")
    lines.append("## Duplicate filename pointers")
    for entry in duplicate_entries[:16]:
        lines.append(
            f"- `0x{entry.file_id:02X}` reuses `0x{entry.duplicate_of:02X}` for `{entry.name}`"
        )
    lines.append("")
    lines.append("## Alias-only / missing-basename entries")
    for entry in no_match_entries[:20]:
        lines.append(f"- `0x{entry.file_id:02X}` `{entry.name}`")
    lines.append("")
    lines.append("## Case-folded FST matches")
    for entry in case_folded_entries[:12]:
        lines.append(
            f"- `0x{entry.file_id:02X}` `{entry.name}` -> " + ", ".join(f"`{path}`" for path in entry.disc_paths)
        )
    lines.append("")
    lines.append("## `.ctors` / `.dtors`")
    if init_entries:
        for entry in init_entries:
            symbol_text = entry.symbol if entry.symbol is not None else "unknown"
            lines.append(
                f"- `{entry.table_name}`[`{entry.index}`] at `0x{entry.table_address + (entry.index * 4):08X}`"
                f" -> `0x{entry.target:08X}` `{symbol_text}`"
            )
    else:
        lines.append("- No init tables recovered")
    lines.append("")
    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/dol_tables.py`")
    lines.append("- CSV dump of file IDs: `python tools/orig/dol_tables.py --format csv`")
    lines.append("- Search specific aliases: `python tools/orig/dol_tables.py --search BLOCKS DLLS PREANIM`")
    return "\n".join(lines)


def search_markdown(entries: list[FileTableEntry], init_entries: list[InitTableEntry], patterns: list[str]) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    file_matches = [
        entry
        for entry in entries
        if any(
            pattern in entry.name.lower()
            or any(pattern in path.lower() for path in entry.disc_paths)
            for pattern in lowered
        )
    ]
    init_matches = [
        entry
        for entry in init_entries
        if any(
            pattern in entry.table_name.lower()
            or pattern in f"{entry.target:08x}"
            or (entry.symbol is not None and pattern in entry.symbol.lower())
            for pattern in lowered
        )
    ]

    lines: list[str] = []
    lines.append("# DOL table search")
    lines.append("")
    if not file_matches and not init_matches:
        lines.append("- No matching table entries.")
        return "\n".join(lines)

    for entry in file_matches:
        duplicate_text = "" if entry.duplicate_of is None else f", duplicate_of=0x{entry.duplicate_of:02X}"
        if entry.disc_paths:
            disc_text = ", disc=" + ", ".join(entry.disc_paths)
        else:
            disc_text = ""
        lines.append(
            f"- file `0x{entry.file_id:02X}` `{entry.name}` at `0x{entry.table_address:08X}`"
            f"{duplicate_text}{disc_text}"
        )
    for entry in init_matches:
        symbol_text = entry.symbol or "unknown"
        lines.append(
            f"- init `{entry.table_name}`[`{entry.index}`] at `0x{entry.table_address + (entry.index * 4):08X}`"
            f" -> `0x{entry.target:08X}` `{symbol_text}`"
        )
    return "\n".join(lines)


def rows_to_csv(entries: list[FileTableEntry]) -> str:
    fieldnames = [
        "file_id",
        "file_id_hex",
        "table_address",
        "string_address",
        "name",
        "duplicate_of_hex",
        "match_kind",
        "disc_paths",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for entry in entries:
        writer.writerow(
            {
                "file_id": entry.file_id,
                "file_id_hex": f"0x{entry.file_id:02X}",
                "table_address": f"0x{entry.table_address:08X}",
                "string_address": f"0x{entry.string_address:08X}",
                "name": entry.name,
                "duplicate_of_hex": "" if entry.duplicate_of is None else f"0x{entry.duplicate_of:02X}",
                "match_kind": entry.match_kind,
                "disc_paths": " | ".join(entry.disc_paths),
            }
        )
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Recover runtime file IDs and init tables from retail main.dol.")
    parser.add_argument(
        "--dol",
        type=Path,
        default=Path("orig/GSAE01/sys/main.dol"),
        help="Path to the EN main.dol.",
    )
    parser.add_argument(
        "--fst",
        type=Path,
        default=Path("orig/GSAE01/sys/fst.bin"),
        help="Path to the EN fst.bin.",
    )
    parser.add_argument(
        "--symbols",
        type=Path,
        default=Path("config/GSAE01/symbols.txt"),
        help="Optional symbols.txt used only for naming `.ctors` / `.dtors` targets.",
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
        help="Substring search over runtime file aliases, disc paths, and init-table symbols.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()

    dol = DolFile(args.dol)
    fst_entries = parse_fst(args.fst)
    symbols = load_symbols(args.symbols)
    table_address, entries = build_file_entries(dol, fst_entries)
    init_entries = build_init_tables(dol, symbols)

    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(entries))
        elif args.search:
            sys.stdout.write(search_markdown(entries, init_entries, args.search))
            sys.stdout.write("\n")
        else:
            sys.stdout.write(summary_markdown(table_address, entries, init_entries))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
