from __future__ import annotations

import argparse
import csv
import io
import re
import struct
import sys
import zlib
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path


MAX_DLL_ID = 0x2C1
SYMBOL_RE = re.compile(
    r"^(\S+)\s*=\s*\.(\S+):0x([0-9A-Fa-f]+); // type:(\S+)(?: size:0x([0-9A-Fa-f]+))?"
)


@dataclass(frozen=True)
class SymbolInfo:
    name: str
    section: str
    kind: str
    size: int | None


@dataclass(frozen=True)
class ObjectDef:
    def_id: int
    name: str
    dll_id: int
    class_id: int


@dataclass(frozen=True)
class FunctionSlot:
    index: int
    address: int
    symbol: str | None
    stub_kind: str | None


@dataclass(frozen=True)
class DllEntry:
    dll_id: int
    table_slot_address: int
    target_address: int
    target_symbol: str | None
    target_symbol_size: int | None
    status: str
    issue: str | None
    prev: int | None
    next: int | None
    unk08: int | None
    unk0e: int | None
    function_count: int | None
    slot_mask: str
    slots: tuple[FunctionSlot, ...]
    object_defs: tuple[ObjectDef, ...]
    placements: int
    sample_words: tuple[int, ...]


class DolFile:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        section_offsets = [struct.unpack_from(">I", self.data, index * 4)[0] for index in range(18)]
        section_addrs = [struct.unpack_from(">I", self.data, 0x48 + index * 4)[0] for index in range(18)]
        section_sizes = [struct.unpack_from(">I", self.data, 0x90 + index * 4)[0] for index in range(18)]
        self.sections = [
            {
                "index": index,
                "offset": section_offsets[index],
                "address": section_addrs[index],
                "size": section_sizes[index],
            }
            for index in range(18)
            if section_sizes[index]
        ]
        self.text_sections = [section for section in self.sections if int(section["index"]) <= 6]

    def addr_to_offset(self, addr: int) -> int | None:
        for section in self.sections:
            start = int(section["address"])
            end = start + int(section["size"])
            if start <= addr < end:
                return int(section["offset"]) + (addr - start)
        return None

    def is_text_pointer(self, addr: int) -> bool:
        for section in self.text_sections:
            start = int(section["address"])
            end = start + int(section["size"])
            if start <= addr < end:
                return True
        return False

    def read_u32(self, addr: int) -> int:
        offset = self.addr_to_offset(addr)
        if offset is None:
            raise ValueError(f"Address 0x{addr:08X} is not inside a loaded DOL section")
        return struct.unpack_from(">I", self.data, offset)[0]

    def read_words(self, addr: int, count: int) -> tuple[int, ...]:
        offset = self.addr_to_offset(addr)
        if offset is None:
            raise ValueError(f"Address 0x{addr:08X} is not inside a loaded DOL section")
        return struct.unpack_from(f">{count}I", self.data, offset)


def load_symbols(path: Path | None) -> dict[int, SymbolInfo]:
    if path is None or not path.is_file():
        return {}
    symbols: dict[int, SymbolInfo] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = SYMBOL_RE.match(line)
        if match is None:
            continue
        address = int(match.group(3), 16)
        size = int(match.group(5), 16) if match.group(5) else None
        symbols[address] = SymbolInfo(
            name=match.group(1),
            section=match.group(2),
            kind=match.group(4),
            size=size,
        )
    return symbols


def guess_dll_table_address(symbols: dict[int, SymbolInfo], max_dll_id: int) -> int:
    expected_size = (max_dll_id + 1) * 4
    candidates = [
        address
        for address, symbol in symbols.items()
        if symbol.section == "data" and symbol.size == expected_size
    ]
    if len(candidates) == 1:
        return candidates[0]
    raise ValueError(
        f"Failed to infer the DLL pointer table. Expected one .data symbol of size 0x{expected_size:X}, "
        f"found {len(candidates)}."
    )


def load_object_offsets(tab_path: Path) -> list[int]:
    data = tab_path.read_bytes()
    offsets: list[int] = []
    for offset in range(0, len(data), 4):
        value = struct.unpack_from(">I", data, offset)[0]
        if value == 0xFFFFFFFF:
            break
        offsets.append(value)
    if len(offsets) < 2:
        raise ValueError("OBJECTS.tab did not contain enough offsets to recover object definitions")
    return offsets


def load_objindex(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) % 2 != 0:
        raise ValueError(f"Unexpected OBJINDEX.bin size: {len(data)}")
    return [struct.unpack_from(">h", data, offset)[0] for offset in range(0, len(data), 2)]


def resolve_object_id(objindex: list[int], object_id: int) -> int:
    if 0 <= object_id < len(objindex) and objindex[object_id] != -1:
        return objindex[object_id]
    return object_id


def load_object_defs(files_root: Path) -> tuple[list[ObjectDef], list[int]]:
    object_bin = (files_root / "OBJECTS.bin").read_bytes()
    offsets = load_object_offsets(files_root / "OBJECTS.tab")
    object_defs: list[ObjectDef] = []
    for def_id, offset in enumerate(offsets[:-1]):
        name = object_bin[offset + 0x91 : offset + 0x9C].split(b"\0", 1)[0].decode("ascii", "replace")
        dll_id, class_id = struct.unpack_from(">Hh", object_bin, offset + 0x50)
        object_defs.append(
            ObjectDef(
                def_id=def_id,
                name=name,
                dll_id=dll_id,
                class_id=class_id,
            )
        )
    return object_defs, load_objindex(files_root / "OBJINDEX.bin")


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


def load_dll_placement_counts(
    files_root: Path,
    object_defs: list[ObjectDef],
    objindex: list[int],
) -> Counter[int]:
    placements: Counter[int] = Counter()
    for romlist_path in sorted(files_root.glob("*.romlist.zlb")):
        payload = decompress_zlb(romlist_path)
        offset = 0
        while offset < len(payload):
            object_id, size_words, _flags = struct.unpack_from(">hBB", payload, offset)
            record_size = size_words * 4
            if record_size <= 0 or offset + record_size > len(payload):
                raise ValueError(
                    f"Invalid record in {romlist_path.name}: offset=0x{offset:X} size_words={size_words}"
                )
            canonical_id = resolve_object_id(objindex, object_id)
            if 0 <= canonical_id < len(object_defs):
                dll_id = object_defs[canonical_id].dll_id
                placements[dll_id] += 1
            offset += record_size
    return placements


def detect_stub_kind(dol: DolFile, function_address: int) -> str | None:
    offset = dol.addr_to_offset(function_address)
    if offset is None or offset + 8 > len(dol.data):
        return None
    first_word, second_word = struct.unpack_from(">2I", dol.data, offset)
    if first_word == 0x4E800020:
        return "blr"
    if (first_word & 0xFFFF0000) == 0x38600000 and second_word == 0x4E800020:
        value = first_word & 0xFFFF
        if value & 0x8000:
            value -= 0x10000
        return f"const {value}"
    return None


def symbol_name(symbols: dict[int, SymbolInfo], address: int) -> str | None:
    symbol = symbols.get(address)
    return None if symbol is None else symbol.name


def parse_dll_entries(
    dol: DolFile,
    table_address: int,
    max_dll_id: int,
    symbols: dict[int, SymbolInfo],
    objects_by_dll: dict[int, list[ObjectDef]],
    placements_by_dll: Counter[int],
) -> list[DllEntry]:
    entries: list[DllEntry] = []
    for dll_id in range(max_dll_id + 1):
        slot_address = table_address + (dll_id * 4)
        target_address = dol.read_u32(slot_address)
        target_symbol = symbols.get(target_address)
        object_defs = tuple(objects_by_dll.get(dll_id, []))
        placements = placements_by_dll.get(dll_id, 0)
        sample_words: tuple[int, ...] = ()

        if target_address == 0:
            entries.append(
                DllEntry(
                    dll_id=dll_id,
                    table_slot_address=slot_address,
                    target_address=target_address,
                    target_symbol=None,
                    target_symbol_size=None,
                    status="null",
                    issue=None,
                    prev=None,
                    next=None,
                    unk08=None,
                    unk0e=None,
                    function_count=None,
                    slot_mask="",
                    slots=(),
                    object_defs=object_defs,
                    placements=placements,
                    sample_words=sample_words,
                )
            )
            continue

        target_offset = dol.addr_to_offset(target_address)
        if target_offset is None or target_offset + 16 > len(dol.data):
            entries.append(
                DllEntry(
                    dll_id=dll_id,
                    table_slot_address=slot_address,
                    target_address=target_address,
                    target_symbol=None if target_symbol is None else target_symbol.name,
                    target_symbol_size=None if target_symbol is None else target_symbol.size,
                    status="unmapped",
                    issue="target outside DOL sections",
                    prev=None,
                    next=None,
                    unk08=None,
                    unk0e=None,
                    function_count=None,
                    slot_mask="",
                    slots=(),
                    object_defs=object_defs,
                    placements=placements,
                    sample_words=sample_words,
                )
            )
            continue

        sample_words = dol.read_words(target_address, 5)
        prev, next_ptr, unk08, function_count_minus_one, unk0e = struct.unpack_from(">IIIHH", dol.data, target_offset)
        function_count = function_count_minus_one + 1
        descriptor_size = 16 + (function_count * 4)

        issue: str | None = None
        slots: tuple[FunctionSlot, ...] = ()
        slot_mask = ""

        if function_count <= 0 or function_count > 64 or target_offset + descriptor_size > len(dol.data):
            issue = "implausible function count"
        else:
            addresses = [
                struct.unpack_from(">I", dol.data, target_offset + 16 + (index * 4))[0]
                for index in range(function_count)
            ]
            if any(address != 0 and not dol.is_text_pointer(address) for address in addresses):
                issue = "non-text slot addresses"
            elif target_symbol is not None and target_symbol.size is not None and target_symbol.size < descriptor_size:
                issue = "symbol smaller than descriptor"
            else:
                slot_mask = "".join("1" if address != 0 else "0" for address in addresses)
                slots = tuple(
                    FunctionSlot(
                        index=index,
                        address=address,
                        symbol=symbol_name(symbols, address),
                        stub_kind=None if address == 0 else detect_stub_kind(dol, address),
                    )
                    for index, address in enumerate(addresses)
                )

        entries.append(
            DllEntry(
                dll_id=dll_id,
                table_slot_address=slot_address,
                target_address=target_address,
                target_symbol=None if target_symbol is None else target_symbol.name,
                target_symbol_size=None if target_symbol is None else target_symbol.size,
                status="descriptor" if issue is None else "non_descriptor",
                issue=issue,
                prev=prev,
                next=next_ptr,
                unk08=unk08,
                unk0e=unk0e,
                function_count=function_count,
                slot_mask=slot_mask,
                slots=slots,
                object_defs=object_defs,
                placements=placements,
                sample_words=sample_words,
            )
        )
    return entries


def summarize_shape_counts(entries: list[DllEntry]) -> Counter[tuple[int, str]]:
    counter: Counter[tuple[int, str]] = Counter()
    for entry in entries:
        if entry.status != "descriptor" or entry.function_count is None:
            continue
        counter[(entry.function_count, entry.slot_mask)] += 1
    return counter


def format_sample_objects(entry: DllEntry, limit: int = 4) -> str:
    return ", ".join(f"`0x{obj.def_id:04X}` `{obj.name}`" for obj in entry.object_defs[:limit])


def summary_markdown(
    entries: list[DllEntry],
    table_address: int,
    max_dll_id: int,
) -> str:
    descriptors = [entry for entry in entries if entry.status == "descriptor"]
    null_entries = [entry for entry in entries if entry.status == "null"]
    non_descriptors = [entry for entry in entries if entry.status == "non_descriptor"]
    unmapped = [entry for entry in entries if entry.status == "unmapped"]
    issue_counter = Counter(entry.issue for entry in non_descriptors if entry.issue is not None)
    shape_counter = summarize_shape_counts(entries)

    object_dll_ids = {entry.dll_id for entry in entries if entry.object_defs}
    resolved_object_dll_ids = {entry.dll_id for entry in descriptors if entry.object_defs}
    unresolved_object_entries = [
        entry
        for entry in entries
        if entry.object_defs and entry.status != "descriptor"
    ]

    top_by_placements = sorted(
        [entry for entry in descriptors if entry.object_defs],
        key=lambda entry: (-entry.placements, entry.dll_id),
    )

    lines: list[str] = []
    lines.append("# `orig/GSAE01/sys/main.dol` DLL catalog")
    lines.append("")
    lines.append("## Summary")
    lines.append(
        f"- Recovered DLL pointer table at `0x{table_address:08X}` with `{max_dll_id + 1}` slots (`0x0000`..`0x{max_dll_id:04X}`)"
    )
    lines.append(f"- Descriptor-like entries: {len(descriptors)}")
    lines.append(f"- Null table slots: {len(null_entries)}")
    lines.append(f"- Non-descriptor in-DOL targets: {len(non_descriptors)}")
    lines.append(f"- Unmapped / external targets: {len(unmapped)}")
    lines.append(
        f"- Object DLL IDs referenced by `OBJECTS.bin`: {len(object_dll_ids)} "
        f"({len(resolved_object_dll_ids)} resolve cleanly to descriptor tables)"
    )
    lines.append(
        f"- Descriptor tables not referenced by any current object def: "
        f"{len([entry for entry in descriptors if not entry.object_defs])}"
    )
    lines.append("")

    lines.append("## Common descriptor shapes")
    for (function_count, mask), count in shape_counter.most_common(10):
        lines.append(f"- `{function_count}` slots, mask `{mask}`: {count} DLLs")
    lines.append("")

    lines.append("## High-usage object DLL families")
    for entry in top_by_placements[:15]:
        lines.append(
            f"- `0x{entry.dll_id:04X}`: placements={entry.placements}, "
            f"defs={len(entry.object_defs)}, slots={entry.function_count}, mask=`{entry.slot_mask}`, "
            f"samples={format_sample_objects(entry)}"
        )
    lines.append("")

    lines.append("## Object DLL IDs without descriptor-like targets")
    if unresolved_object_entries:
        for entry in sorted(unresolved_object_entries, key=lambda item: item.dll_id):
            target_text = f"`0x{entry.target_address:08X}`"
            if entry.target_symbol is not None:
                target_text += f" `{entry.target_symbol}`"
            lines.append(
                f"- `0x{entry.dll_id:04X}` -> {target_text}: {entry.issue or entry.status}; "
                f"samples={format_sample_objects(entry)}"
            )
    else:
        lines.append("- None")
    lines.append("")

    lines.append("## Non-descriptor target classes")
    for issue, count in issue_counter.most_common():
        lines.append(f"- `{issue}`: {count}")
    lines.append("")

    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/dll_catalog.py`")
    lines.append("- CSV dump: `python tools/orig/dll_catalog.py --format csv`")
    lines.append("- Search DLL IDs, object names, symbols, or addresses:")
    lines.append("  - `python tools/orig/dll_catalog.py --search dll:0x0126 curve fn_8019AFF4`")
    return "\n".join(lines)


def matches_pattern(entry: DllEntry, pattern: str) -> bool:
    if pattern.startswith("dll:"):
        value = pattern[4:]
        return value in (f"{entry.dll_id:04x}", f"0x{entry.dll_id:04x}")

    haystacks = [
        f"{entry.dll_id:04x}",
        f"0x{entry.dll_id:04x}",
        f"{entry.target_address:08x}",
        f"0x{entry.target_address:08x}",
        entry.status.lower(),
    ]
    if entry.issue is not None:
        haystacks.append(entry.issue.lower())
    if entry.target_symbol is not None:
        haystacks.append(entry.target_symbol.lower())
    for obj in entry.object_defs:
        haystacks.extend(
            [
                obj.name.lower(),
                f"{obj.def_id:04x}",
                f"0x{obj.def_id:04x}",
                f"{obj.class_id & 0xFFFF:04x}",
                f"0x{obj.class_id & 0xFFFF:04x}",
            ]
        )
    for slot in entry.slots:
        haystacks.extend(
            [
                f"{slot.address:08x}",
                f"0x{slot.address:08x}",
                f"{slot.index}",
                f"slot:{slot.index}",
            ]
        )
        if slot.symbol is not None:
            haystacks.append(slot.symbol.lower())
        if slot.stub_kind is not None:
            haystacks.append(slot.stub_kind.lower())
    return any(pattern in value for value in haystacks)


def search_entries(entries: list[DllEntry], patterns: list[str]) -> list[DllEntry]:
    lowered = [pattern.lower() for pattern in patterns]
    matches: list[DllEntry] = []
    for entry in entries:
        if any(matches_pattern(entry, pattern) for pattern in lowered):
            matches.append(entry)
    return matches


def search_markdown(entries: list[DllEntry], patterns: list[str]) -> str:
    matches = search_entries(entries, patterns)
    lines: list[str] = []
    lines.append("# DLL search")
    lines.append("")
    if not matches:
        lines.append("- No matching DLL entries.")
        return "\n".join(lines)

    for entry in matches[:25]:
        target_text = f"`0x{entry.target_address:08X}`"
        if entry.target_symbol is not None:
            target_text += f" `{entry.target_symbol}`"
        lines.append(
            f"- `0x{entry.dll_id:04X}` {target_text}: status=`{entry.status}`"
            + (f", issue=`{entry.issue}`" if entry.issue is not None else "")
            + f", placements={entry.placements}, defs={len(entry.object_defs)}"
        )
        if entry.object_defs:
            lines.append(f"  objects: {format_sample_objects(entry, limit=6)}")
        if entry.status == "descriptor":
            lines.append(
                f"  descriptor: slots={entry.function_count}, mask=`{entry.slot_mask}`, "
                f"prev=`0x{entry.prev:08X}`, next=`0x{entry.next:08X}`, unk08=`0x{entry.unk08:08X}`, unk0e=`0x{entry.unk0e:04X}`"
            )
            for slot in entry.slots:
                if slot.address == 0:
                    continue
                label = slot.symbol or "unknown"
                stub_text = "" if slot.stub_kind is None else f", stub={slot.stub_kind}"
                lines.append(
                    f"  slot {slot.index:02d}: `0x{slot.address:08X}` `{label}`{stub_text}"
                )
        else:
            lines.append(
                "  sample words: "
                + " ".join(f"`0x{word:08X}`" for word in entry.sample_words)
            )
    if len(matches) > 25:
        lines.append(f"- ... {len(matches) - 25} more matches omitted")
    return "\n".join(lines)


def rows_to_csv(entries: list[DllEntry]) -> str:
    fieldnames = [
        "dll_id",
        "dll_id_hex",
        "status",
        "issue",
        "table_slot_address",
        "target_address",
        "target_symbol",
        "target_symbol_size_hex",
        "function_count",
        "slot_mask",
        "placements",
        "object_defs",
        "sample_objects",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for entry in entries:
        writer.writerow(
            {
                "dll_id": entry.dll_id,
                "dll_id_hex": f"0x{entry.dll_id:04X}",
                "status": entry.status,
                "issue": entry.issue or "",
                "table_slot_address": f"0x{entry.table_slot_address:08X}",
                "target_address": f"0x{entry.target_address:08X}",
                "target_symbol": entry.target_symbol or "",
                "target_symbol_size_hex": "" if entry.target_symbol_size is None else f"0x{entry.target_symbol_size:X}",
                "function_count": "" if entry.function_count is None else entry.function_count,
                "slot_mask": entry.slot_mask,
                "placements": entry.placements,
                "object_defs": len(entry.object_defs),
                "sample_objects": " | ".join(f"0x{obj.def_id:04X}:{obj.name}" for obj in entry.object_defs[:6]),
            }
        )
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Recover the EN retail DLL pointer table and tie it to object DLL IDs.")
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
        help="Path to the extracted EN files/ directory.",
    )
    parser.add_argument(
        "--symbols",
        type=Path,
        default=Path("config/GSAE01/symbols.txt"),
        help="Optional symbols.txt used for DLL-table discovery and function naming.",
    )
    parser.add_argument(
        "--dll-table",
        type=lambda value: int(value, 0),
        help="Override the DLL table address instead of inferring it from symbols.txt.",
    )
    parser.add_argument(
        "--max-dll-id",
        type=lambda value: int(value, 0),
        default=MAX_DLL_ID,
        help="Highest DLL ID in the table. Default: 0x2C1",
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
        help="Substring search across DLL IDs, symbols, object names, and slot addresses.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()

    symbols = load_symbols(args.symbols)
    table_address = args.dll_table if args.dll_table is not None else guess_dll_table_address(symbols, args.max_dll_id)
    dol = DolFile(args.dol)
    object_defs, objindex = load_object_defs(args.files_root)
    placements_by_dll = load_dll_placement_counts(args.files_root, object_defs, objindex)
    objects_by_dll: dict[int, list[ObjectDef]] = defaultdict(list)
    for obj in object_defs:
        if obj.dll_id != 0xFFFF:
            objects_by_dll[obj.dll_id].append(obj)

    entries = parse_dll_entries(
        dol=dol,
        table_address=table_address,
        max_dll_id=args.max_dll_id,
        symbols=symbols,
        objects_by_dll=objects_by_dll,
        placements_by_dll=placements_by_dll,
    )

    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(entries))
        elif args.search:
            sys.stdout.write(search_markdown(entries, args.search))
            sys.stdout.write("\n")
        else:
            sys.stdout.write(summary_markdown(entries, table_address, args.max_dll_id))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
