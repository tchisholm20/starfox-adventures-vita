from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


REGIONS = ("GSAE01", "GSAP01", "GSAJ01")
SPECIAL_RELOCS = {
    0xCA: "R_DOLPHIN_SECTION",
    0xCB: "R_DOLPHIN_END",
}
RELOC_NAMES = {
    0x01: "R_PPC_ADDR32",
    0x04: "R_PPC_ADDR16_LO",
    0x06: "R_PPC_ADDR16_HA",
    0x0A: "R_PPC_REL24",
    **SPECIAL_RELOCS,
}


@dataclass(frozen=True)
class SectionGuess:
    index: int
    table_offset: int
    file_offset: int
    size: int
    is_executable: bool
    kind: str


@dataclass(frozen=True)
class Relocation:
    offset_delta: int
    reloc_type: int
    section: int
    addend: int

    @property
    def type_name(self) -> str:
        return RELOC_NAMES.get(self.reloc_type, f"0x{self.reloc_type:02X}")


@dataclass(frozen=True)
class ImportEntry:
    module_id: int
    relocation_offset: int
    relocations: tuple[Relocation, ...]


@dataclass(frozen=True)
class RegionDigest:
    region: str
    rel_size: int
    rel_sha1: str
    str_size: int
    str_sha1: str


@dataclass(frozen=True)
class ModuleAudit:
    rel_path: Path
    str_path: Path
    raw_version: int
    rel_offset: int
    imp_offset: int
    imp_size: int
    guessed_sections: tuple[SectionGuess, ...]
    imports: tuple[ImportEntry, ...]
    data_strings: tuple[tuple[int, str], ...]
    external_path: str | None
    region_digests: tuple[RegionDigest, ...]


def read_c_strings(blob: bytes, base_offset: int) -> list[tuple[int, str]]:
    results: list[tuple[int, str]] = []
    start: int | None = None
    for index, value in enumerate(blob):
        if value in (0x09, 0x0A) or 0x20 <= value <= 0x7E:
            if start is None:
                start = index
            continue
        if value == 0 and start is not None and index - start >= 4:
            text = blob[start:index].decode("ascii", "replace").replace("\n", "\\n").replace("\t", "\\t")
            results.append((base_offset + start, text))
        start = None
    if start is not None and len(blob) - start >= 4:
        text = blob[start:].decode("ascii", "replace").replace("\n", "\\n").replace("\t", "\\t")
        results.append((base_offset + start, text))
    return results


def parse_imports(data: bytes, imp_offset: int, imp_size: int) -> tuple[ImportEntry, ...]:
    entries: list[ImportEntry] = []
    for cursor in range(imp_offset, imp_offset + imp_size, 8):
        module_id, reloc_offset = struct.unpack_from(">II", data, cursor)
        relocations: list[Relocation] = []
        rel_cursor = reloc_offset
        while rel_cursor + 8 <= len(data):
            offset_delta, reloc_type, section, addend = struct.unpack_from(">HBBI", data, rel_cursor)
            relocation = Relocation(
                offset_delta=offset_delta,
                reloc_type=reloc_type,
                section=section,
                addend=addend,
            )
            relocations.append(relocation)
            rel_cursor += 8
            if reloc_type == 0xCB:
                break
        entries.append(
            ImportEntry(
                module_id=module_id,
                relocation_offset=reloc_offset,
                relocations=tuple(relocations),
            )
        )
    return tuple(entries)


def guess_sections(data: bytes) -> tuple[SectionGuess, ...]:
    best: list[SectionGuess] = []
    for base in range(0x20, min(0x80, len(data) - 16), 4):
        current: list[SectionGuess] = []
        index = 1
        for cursor in range(base, min(base + 0x30, len(data) - 8), 8):
            raw_offset, size = struct.unpack_from(">II", data, cursor)
            if raw_offset == 0 and size == 0:
                if current:
                    break
                continue

            file_offset = raw_offset & ~1
            is_executable = bool(raw_offset & 1)
            if size == 0:
                break

            if file_offset == 0 and not is_executable:
                kind = "bss"
            elif 0 < file_offset < len(data) and file_offset + size <= len(data):
                kind = "text" if is_executable else "data"
            else:
                break

            current.append(
                SectionGuess(
                    index=index,
                    table_offset=cursor,
                    file_offset=file_offset,
                    size=size,
                    is_executable=is_executable,
                    kind=kind,
                )
            )
            index += 1
        if len(current) > len(best) and any(item.is_executable for item in current):
            best = current
    return tuple(best)


def load_region_digests(repo_root: Path) -> tuple[RegionDigest, ...]:
    digests: list[RegionDigest] = []
    for region in REGIONS:
        rel_path = repo_root / "orig" / region / "files" / "modules" / "testmod.rel"
        str_path = repo_root / "orig" / region / "files" / "modules" / "dino.str"
        rel_bytes = rel_path.read_bytes()
        str_bytes = str_path.read_bytes()
        digests.append(
            RegionDigest(
                region=region,
                rel_size=len(rel_bytes),
                rel_sha1=hashlib.sha1(rel_bytes).hexdigest(),
                str_size=len(str_bytes),
                str_sha1=hashlib.sha1(str_bytes).hexdigest(),
            )
        )
    return tuple(digests)


def audit_module(repo_root: Path, region: str) -> ModuleAudit:
    rel_path = repo_root / "orig" / region / "files" / "modules" / "testmod.rel"
    str_path = repo_root / "orig" / region / "files" / "modules" / "dino.str"
    data = rel_path.read_bytes()
    str_bytes = str_path.read_bytes()

    guessed_sections = guess_sections(data)
    rel_offset = struct.unpack_from(">I", data, 0x1C)[0]
    imp_offset = struct.unpack_from(">I", data, 0x20)[0]
    imp_size = struct.unpack_from(">I", data, 0x24)[0]
    imports = parse_imports(data, imp_offset, imp_size)

    data_section = next((section for section in guessed_sections if section.kind == "data"), None)
    data_strings: tuple[tuple[int, str], ...] = ()
    if data_section is not None:
        blob = data[data_section.file_offset : data_section.file_offset + data_section.size]
        data_strings = tuple(read_c_strings(blob, 0))

    external_path = None
    if str_bytes:
        external_path = str_bytes.split(b"\0", 1)[0].decode("ascii", "replace")

    return ModuleAudit(
        rel_path=rel_path,
        str_path=str_path,
        raw_version=struct.unpack_from(">I", data, 0x0C)[0],
        rel_offset=rel_offset,
        imp_offset=imp_offset,
        imp_size=imp_size,
        guessed_sections=guessed_sections,
        imports=imports,
        data_strings=data_strings,
        external_path=external_path,
        region_digests=load_region_digests(repo_root),
    )


def referenced_strings(audit: ModuleAudit) -> list[str]:
    data_offsets = {offset: text for offset, text in audit.data_strings}
    matches: list[str] = []
    seen: set[str] = set()
    for entry in audit.imports:
        if entry.module_id != 1:
            continue
        for relocation in entry.relocations:
            if relocation.section != 2 or relocation.addend not in data_offsets:
                continue
            text = data_offsets[relocation.addend]
            if text in seen:
                continue
            seen.add(text)
            matches.append(text)
    return matches


def format_sections(audit: ModuleAudit) -> list[str]:
    lines: list[str] = []
    for section in audit.guessed_sections:
        exec_text = ", exec" if section.is_executable else ""
        if section.kind == "bss":
            lines.append(
                f"- Section `{section.index}` ({section.kind}) from descriptor `0x{section.table_offset:02X}`: size=`0x{section.size:X}`"
            )
            continue
        lines.append(
            f"- Section `{section.index}` ({section.kind}{exec_text}) from descriptor `0x{section.table_offset:02X}`:"
            f" file=`0x{section.file_offset:X}` size=`0x{section.size:X}`"
        )
    return lines


def format_import(entry: ImportEntry) -> str:
    type_counter = Counter(reloc.type_name for reloc in entry.relocations)
    parts = ", ".join(f"{name}={count}" for name, count in sorted(type_counter.items()))
    return (
        f"- Module `{entry.module_id}` relocation stream at `0x{entry.relocation_offset:X}`:"
        f" {len(entry.relocations)} records ({parts})"
    )


def summary_markdown(audit: ModuleAudit) -> str:
    same_rel = len({digest.rel_sha1 for digest in audit.region_digests}) == 1
    same_str = len({digest.str_sha1 for digest in audit.region_digests}) == 1
    self_strings = referenced_strings(audit)

    lines: list[str] = []
    lines.append("# `orig/*/files/modules` audit")
    lines.append("")
    lines.append("## Summary")
    lines.append(
        f"- `testmod.rel` is byte-identical across EN/PAL/JP: `{same_rel}`"
    )
    lines.append(
        f"- `dino.str` is byte-identical across EN/PAL/JP: `{same_str}`"
    )
    if audit.external_path is not None:
        lines.append(f"- `dino.str` preserves the original asset path: `{audit.external_path}`")
    lines.append(f"- REL version word: `0x{audit.raw_version:08X}`")
    lines.append(f"- Relocation table starts at `0x{audit.rel_offset:X}`")
    lines.append(f"- Import table starts at `0x{audit.imp_offset:X}` with size `0x{audit.imp_size:X}`")
    lines.append("")

    lines.append("## Inferred Sections")
    lines.extend(format_sections(audit))
    lines.append("")

    lines.append("## Import Streams")
    for entry in audit.imports:
        lines.append(format_import(entry))
    lines.append("")

    lines.append("## Self-Relocation Strings")
    if self_strings:
        for text in self_strings:
            lines.append(f"- `{text}`")
    else:
        lines.append("- No section-2 string relocs found")
    lines.append("")

    lines.append("## Actionable Takeaways")
    lines.append("- This is a minimal, known-good REL sample for the game’s loader path: one executable section, one string/data section, one tiny BSS, and only two import streams.")
    lines.append("- The self-relocation stream is especially useful because it shows clean `ADDR16_HA` / `ADDR16_LO` pairs against local string data and BSS, which makes it a compact testcase for any future REL loader or relocation parser.")
    lines.append("- The main-module import stream contains exactly three `R_PPC_REL24` calls to one external target, consistent with a tiny debug-print harness rather than gameplay code.")
    lines.append("- The preserved `baddies/testmod.plf` path is a direct naming clue for the content pipeline around modules, even if `files/modules` is unused in retail gameplay.")
    lines.append("")

    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/module_audit.py`")
    lines.append(f"- Inspect the raw retail files: `{audit.rel_path.as_posix()}` and `{audit.str_path.as_posix()}`")
    return "\n".join(lines)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Audit the retail module leftovers under orig/*/files/modules.")
    parser.add_argument(
        "--region",
        default="GSAE01",
        choices=REGIONS,
        help="Region to use for the detailed summary. Cross-region hashes are still reported.",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path("."),
        help="Repository root.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    audit = audit_module(args.repo_root.resolve(), args.region)
    try:
        sys.stdout.write(summary_markdown(audit))
        sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
