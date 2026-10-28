#!/usr/bin/env python3
"""Regenerate and audit DLL source folders using retail-only names."""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
from collections import defaultdict
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
DLLS_ROOT = REPO / "src/dlls"
DLLS_TXT = DLLS_ROOT / "dlls.txt"
DEFAULT_DOL = REPO / "orig/GSAE01/sys/main.dol"
DEFAULT_FILES_ROOT = REPO.parent / "sfa-extracted"

BANKS = (
    ("engine", 0, 88),
    ("modgfx", 89, 170),
    ("projgfx", 171, 194),
    ("objects", 195, 704),
)

MAPPING_RE = re.compile(r"^(\d+)\s*=\s*([^/]+)/(\S+)$")
LEAF_RE = re.compile(r"^(\d+)(?:_(.+))?$")
SLOT_RANGE_RE = re.compile(r"^(\d+)(?:-(\d+))?$")
SAFE_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
PRINTABLE_RE = re.compile(rb"[ -~]{4,}")
SOURCE_BASENAME_RE = re.compile(
    r"(?<![A-Za-z0-9_])([A-Za-z_][A-Za-z0-9_]*)\.c(?![A-Za-z0-9_])"
)


def bank_for_slot(slot: int) -> str:
    for bank, first, last in BANKS:
        if first <= slot <= last:
            return bank
    raise ValueError(f"slot {slot} is outside the scaffold range")


def parse_mappings(text: str, source: str) -> dict[int, tuple[str, str]]:
    mappings: dict[int, tuple[str, str]] = {}
    for line in text.splitlines():
        match = MAPPING_RE.match(line)
        if match is None:
            continue
        slot = int(match.group(1))
        if slot in mappings:
            raise SystemExit(f"{source} has duplicate slot {slot}")
        mappings[slot] = (match.group(2), match.group(3))
    expected = set(range(705))
    if set(mappings) != expected:
        missing = sorted(expected - set(mappings))
        extra = sorted(set(mappings) - expected)
        raise SystemExit(
            f"{source} slot mismatch: missing={missing}, extra={extra}"
        )
    return mappings


def load_existing_mappings() -> dict[int, tuple[str, str]]:
    return parse_mappings(
        DLLS_TXT.read_text(encoding="utf-8"),
        str(DLLS_TXT.relative_to(REPO)),
    )


def git_output(*args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=REPO,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise SystemExit(f"git {' '.join(args)} failed: {detail}")
    return result.stdout


def load_reference_mappings(ref: str) -> dict[int, tuple[str, str]]:
    path = DLLS_TXT.relative_to(REPO).as_posix()
    return parse_mappings(git_output("show", f"{ref}:{path}"), f"{ref}:{path}")


def parse_slots(spec: str | None) -> list[int]:
    if spec is None:
        return list(range(705))

    slots: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            raise SystemExit("empty entry in --slots")
        match = SLOT_RANGE_RE.fullmatch(part)
        if match is None:
            raise SystemExit(f"invalid --slots entry: {part}")
        first = int(match.group(1), 10)
        last = int(match.group(2), 10) if match.group(2) else first
        if first > last:
            raise SystemExit(f"descending --slots range: {part}")
        slots.update(range(first, last + 1))

    invalid = sorted(slot for slot in slots if slot < 0 or slot > 704)
    if invalid:
        raise SystemExit(f"--slots values outside 0..704: {invalid}")
    return sorted(slots)


def current_payloads(directory: str) -> tuple[str, ...]:
    path = DLLS_ROOT / directory
    if not path.is_dir():
        return ("<missing directory>",)
    return tuple(
        sorted(
            entry.relative_to(path).as_posix()
            for entry in path.rglob("*")
            if entry.is_file() and entry.name != ".gitkeep"
        )
    )


def current_slot_dirs(slot: int) -> tuple[str, ...]:
    directories: list[str] = []
    for bank, _, _ in BANKS:
        bank_path = DLLS_ROOT / bank
        if not bank_path.is_dir():
            continue
        for path in bank_path.iterdir():
            match = LEAF_RE.fullmatch(path.name)
            if path.is_dir() and match is not None and int(match.group(1)) == slot:
                directories.append(f"{bank}/{path.name}")
    return tuple(sorted(directories))


def load_reference_layout(
    ref: str,
) -> tuple[
    dict[int, tuple[str, ...]],
    dict[str, tuple[str, ...]],
]:
    paths = git_output(
        "ls-tree", "-r", "--name-only", ref, "--", "src/dlls"
    )
    directories: dict[int, set[str]] = defaultdict(set)
    payloads: dict[str, list[str]] = defaultdict(list)
    for path in paths.splitlines():
        parts = path.split("/")
        if len(parts) < 5:
            continue
        bank, leaf = parts[2], parts[3]
        directory = f"{bank}/{leaf}"
        match = LEAF_RE.fullmatch(leaf)
        if match is not None:
            directories[int(match.group(1))].add(directory)
        if parts[-1] != ".gitkeep":
            payloads[directory].append("/".join(parts[4:]))
    sorted_directories = {
        key: tuple(sorted(leaves))
        for key, leaves in directories.items()
    }
    sorted_payloads = {
        key: tuple(sorted(names))
        for key, names in payloads.items()
    }
    return sorted_directories, sorted_payloads


def reference_slot_payloads(
    directories: tuple[str, ...],
    payloads: dict[str, tuple[str, ...]],
) -> tuple[str, ...]:
    if len(directories) == 1:
        return payloads.get(directories[0], ())
    return tuple(
        sorted(
            f"{directory}/{name}"
            for directory in directories
            for name in payloads.get(directory, ())
        )
    )


def current_slot_payloads(
    directories: tuple[str, ...],
) -> tuple[str, ...]:
    if len(directories) == 1:
        return current_payloads(directories[0])
    return tuple(
        sorted(
            f"{directory}/{name}"
            for directory in directories
            for name in current_payloads(directory)
        )
    )


def audit_reference_layout(
    ref: str,
    slots: list[int],
    mappings: dict[int, tuple[str, str]],
) -> None:
    reference = load_reference_mappings(ref)
    reference_dirs, reference_payloads = load_reference_layout(ref)
    differences = 0

    for slot in slots:
        expected_bank, expected_leaf = reference[slot]
        current_bank, current_leaf = mappings[slot]
        expected_dirs = reference_dirs.get(slot, ())
        actual_dirs = current_slot_dirs(slot)
        expected_payloads = reference_slot_payloads(
            expected_dirs, reference_payloads
        )
        actual_payloads = current_slot_payloads(actual_dirs)

        mapping_changed = (current_bank, current_leaf) != (
            expected_bank,
            expected_leaf,
        )
        payloads_changed = actual_payloads != expected_payloads
        dirs_changed = actual_dirs != expected_dirs
        if not mapping_changed and not payloads_changed and not dirs_changed:
            continue

        differences += 1
        print(f"slot {slot}:")
        if mapping_changed:
            print(f"  expected mapping: {expected_bank}/{expected_leaf}")
            print(f"  current mapping:  {current_bank}/{current_leaf}")
        if dirs_changed:
            expected = ", ".join(expected_dirs) or "(none)"
            actual = ", ".join(actual_dirs) or "(none)"
            print(f"  expected directories: {expected}")
            print(f"  current directories:  {actual}")
        if payloads_changed:
            expected = ", ".join(expected_payloads) or "(empty)"
            actual = ", ".join(actual_payloads) or "(empty)"
            print(f"  expected payloads: {expected}")
            print(f"  current payloads:  {actual}")

    if differences:
        print(
            f"{differences} slot(s) differ from {ref}; "
            "no files were changed"
        )
        raise SystemExit(1)
    print(f"selected DLL source paths match {ref}")


def load_retail_source_basenames(dol_path: Path) -> set[str]:
    data = dol_path.read_bytes()
    names: set[str] = set()
    for match in PRINTABLE_RE.finditer(data):
        text = match.group().decode("ascii", errors="ignore")
        names.update(source.group(1) for source in SOURCE_BASENAME_RE.finditer(text))
    return names


def load_retail_object_names(files_root: Path) -> dict[int, list[str]]:
    objects = (files_root / "OBJECTS.bin").read_bytes()
    table = (files_root / "OBJECTS.tab").read_bytes()
    offsets: list[int] = []
    for offset in range(0, len(table), 4):
        value = struct.unpack_from(">I", table, offset)[0]
        if value == 0xFFFFFFFF:
            break
        offsets.append(value)
    if len(offsets) < 2:
        raise SystemExit("OBJECTS.tab does not contain a valid offset table")

    names: dict[int, list[str]] = defaultdict(list)
    for offset in offsets[:-1]:
        dll_id = struct.unpack_from(">H", objects, offset + 0x50)[0]
        if dll_id == 0xFFFF or dll_id > 704:
            continue
        name = (
            objects[offset + 0x91 : offset + 0x9C]
            .split(b"\0", 1)[0]
            .decode("ascii", errors="strict")
        )
        if name and name not in names[dll_id]:
            names[dll_id].append(name)
    return dict(names)


def inspect_leaf(path: Path) -> tuple[int, bool]:
    match = LEAF_RE.fullmatch(path.name)
    if match is None:
        raise SystemExit(f"unexpected DLL scaffold directory name: {path}")
    slot = int(match.group(1))
    if bank_for_slot(slot) != path.parent.name:
        raise SystemExit(f"DLL slot {slot} is in the wrong bank: {path}")

    entries = list(path.iterdir())
    payload = [entry for entry in entries if entry.name != ".gitkeep"]
    if payload:
        return slot, True
    if len(entries) > 1 or (entries and not entries[0].is_file()):
        raise SystemExit(f"empty scaffold directory has unexpected contents: {path}")
    return slot, False


def inspect_scaffold() -> tuple[dict[int, Path], list[Path]]:
    populated: dict[int, Path] = {}
    empty: list[Path] = []
    for bank, _, _ in BANKS:
        bank_path = DLLS_ROOT / bank
        for path in sorted(bank_path.iterdir()):
            if not path.is_dir():
                raise SystemExit(f"unexpected non-directory in scaffold bank: {path}")
            slot, has_payload = inspect_leaf(path)
            if has_payload:
                if slot in populated:
                    raise SystemExit(
                        f"multiple populated folders claim DLL slot {slot}: "
                        f"{populated[slot]} and {path}"
                    )
                populated[slot] = path
            else:
                empty.append(path)
    return populated, empty


def existing_candidate(slot: int, mappings: dict[int, tuple[str, str]]) -> str | None:
    bank, leaf = mappings[slot]
    if bank != bank_for_slot(slot):
        raise SystemExit(f"dlls.txt maps slot {slot} to the wrong bank: {bank}")
    match = LEAF_RE.fullmatch(leaf)
    if match is None or int(match.group(1)) != slot:
        raise SystemExit(f"dlls.txt has an invalid leaf for slot {slot}: {leaf}")
    return match.group(2)


def retail_name_for_slot(
    slot: int,
    candidate: str | None,
    dol_source_names: set[str],
    object_names: dict[int, list[str]],
) -> tuple[str | None, str]:
    names = object_names.get(slot, [])
    if candidate in dol_source_names:
        return candidate, "DOL source basename"
    if candidate in names:
        return candidate, "OBJECTS.bin exact name"
    if len(names) == 1 and SAFE_NAME_RE.fullmatch(names[0]):
        return names[0], "OBJECTS.bin sole exact name"
    return None, "number-only"


def render_mappings(leaves: dict[int, str]) -> str:
    lines: list[str] = []
    for bank, first, last in BANKS:
        lines.append(f"# {bank}")
        for slot in range(first, last + 1):
            lines.append(f"{slot} = {bank}/{leaves[slot]}")
        if last != BANKS[-1][2]:
            lines.append("")
    return "\n".join(lines) + "\n"


def remove_empty_leaf(path: Path) -> None:
    keep = path / ".gitkeep"
    if keep.exists():
        keep.unlink()
    path.rmdir()


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Regenerate empty DLL folders from exact retail evidence or audit "
            "source-path drift."
        )
    )
    parser.add_argument("--dol", type=Path, default=DEFAULT_DOL)
    parser.add_argument("--files-root", type=Path, default=DEFAULT_FILES_ROOT)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument(
        "--check",
        action="store_true",
        help="Report whether regeneration is needed without changing files.",
    )
    modes.add_argument(
        "--audit-ref",
        metavar="REF",
        help=(
            "Compare live slot mappings and payload filenames with a known-good "
            "Git tree without changing files."
        ),
    )
    parser.add_argument(
        "--slots",
        metavar="LIST",
        help=(
            "Limit --audit-ref to comma-separated slots or ranges "
            "(for example 227-229,231)."
        ),
    )
    args = parser.parse_args()

    if args.slots is not None and args.audit_ref is None:
        parser.error("--slots requires --audit-ref")

    mappings = load_existing_mappings()
    if args.audit_ref is not None:
        audit_reference_layout(
            args.audit_ref,
            parse_slots(args.slots),
            mappings,
        )
        return

    populated, empty = inspect_scaffold()
    dol_source_names = load_retail_source_basenames(args.dol)
    object_names = load_retail_object_names(args.files_root)

    leaves: dict[int, str] = {}
    reasons: dict[str, int] = defaultdict(int)
    for slot in range(705):
        if slot in populated:
            leaves[slot] = populated[slot].name
            reasons["populated"] += 1
            continue
        candidate = existing_candidate(slot, mappings)
        name, reason = retail_name_for_slot(
            slot, candidate, dol_source_names, object_names
        )
        leaves[slot] = f"{slot}_{name}" if name is not None else str(slot)
        reasons[reason] += 1

    desired_text = render_mappings(leaves)
    current_dirs = {
        path.relative_to(DLLS_ROOT).as_posix()
        for bank, _, _ in BANKS
        for path in (DLLS_ROOT / bank).iterdir()
        if path.is_dir()
    }
    desired_dirs = {
        f"{bank_for_slot(slot)}/{leaf}" for slot, leaf in leaves.items()
    }
    needs_update = current_dirs != desired_dirs or DLLS_TXT.read_text(
        encoding="utf-8"
    ) != desired_text

    if args.check:
        if needs_update:
            raise SystemExit("DLL scaffold differs from the retail-only mapping")
        print("DLL scaffold matches the retail-only mapping")
        return

    for path in empty:
        remove_empty_leaf(path)
    for slot, leaf in leaves.items():
        if slot in populated:
            continue
        path = DLLS_ROOT / bank_for_slot(slot) / leaf
        path.mkdir()
        (path / ".gitkeep").write_bytes(b"")
    DLLS_TXT.write_text(desired_text, encoding="utf-8")

    print(f"preserved populated folders: {reasons['populated']}")
    print(f"named from DOL source basenames: {reasons['DOL source basename']}")
    print(f"named from exact OBJECTS.bin candidates: {reasons['OBJECTS.bin exact name']}")
    print(f"named from sole exact OBJECTS.bin names: {reasons['OBJECTS.bin sole exact name']}")
    print(f"left number-only: {reasons['number-only']}")


if __name__ == "__main__":
    main()
