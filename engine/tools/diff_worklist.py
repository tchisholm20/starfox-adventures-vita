#!/usr/bin/env python3
"""Rank near-matching functions by the kinds of remaining instruction diffs.

The normal report gives one percentage, but a 99.9% function with a real
branch/opcode mismatch is usually more actionable than one containing only a
register permutation or anonymous-constant relocation identity.  This tool
disassembles the highest-matching incomplete units and summarizes normalized
diff regions into a compact worklist.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
from collections import Counter
from pathlib import Path

from function_objdump import objdump_symbol, strip_preamble
from ndiff import classify, normalize, regions


ROOT = Path(__file__).resolve().parents[1]
REGISTER_RE = re.compile(r"\b[rf]\d+\b")
POOL_RELOC_RE = re.compile(
    r"^RELOC (@\d+|lbl_[0-9A-Fa-f]{6,}|jumptable_[0-9A-Fa-f]{6,})$"
)


def objdump() -> Path:
    executable = ROOT / "build" / "binutils" / "powerpc-eabi-objdump.exe"
    if not executable.is_file():
        executable = executable.with_suffix("")
    return executable


def object_paths(version: str, source_path: str) -> tuple[Path, Path] | None:
    source = Path(source_path)
    try:
        relative = source.relative_to("src").with_suffix(".o")
    except ValueError:
        return None
    target = ROOT / "build" / version / "obj" / relative
    base = ROOT / "build" / version / "src" / relative
    if not target.exists() or not base.exists():
        return None
    return target, base


def blind(instructions: list[str], fold_pools: bool) -> list[str]:
    result = []
    for instruction in instructions:
        instruction = REGISTER_RE.sub("R", instruction)
        if fold_pools and POOL_RELOC_RE.match(instruction):
            instruction = "RELOC POOL"
        result.append(instruction)
    return result


def difference_size(target: list[str], base: list[str]) -> int:
    return sum(
        max(target_end - target_start, base_end - base_start)
        for tag, target_start, target_end, base_start, base_end in difflib.SequenceMatcher(
            None, target, base, autojunk=False
        ).get_opcodes()
        if tag != "equal"
    )


def classify_symbol(
    objdump_path: Path, target: Path, base: Path, symbol: str
) -> Counter[str]:
    target_instructions = normalize(
        strip_preamble(objdump_symbol(objdump_path, target, symbol)), symbol
    )
    base_instructions = normalize(
        strip_preamble(objdump_symbol(objdump_path, base, symbol)), symbol
    )
    blind_target = blind(target_instructions, fold_pools=False)
    blind_base = blind(base_instructions, fold_pools=False)
    folded_target = blind(target_instructions, fold_pools=True)
    folded_base = blind(base_instructions, fold_pools=True)

    counts: Counter[str] = Counter()
    raw_size = difference_size(target_instructions, base_instructions)
    blind_size = difference_size(blind_target, blind_base)
    folded_size = difference_size(folded_target, folded_base)
    if raw_size > blind_size:
        counts["reg-perm"] = raw_size - blind_size
    if blind_size > folded_size:
        counts["pool-reloc"] = blind_size - folded_size

    for _, target_start, target_end, base_start, base_end in regions(
        folded_target, folded_base
    ):
        target_region = folded_target[target_start:target_end]
        base_region = folded_base[base_start:base_end]
        category = classify(target_region, base_region) or "structural"
        counts[category] += max(len(target_region), len(base_region))
    return counts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-v", "--version", default="GSAE01")
    parser.add_argument("--min-match", type=float, default=95.0)
    parser.add_argument(
        "--unit-limit",
        type=int,
        default=60,
        help="number of highest-matching incomplete units to inspect",
    )
    parser.add_argument("--top", type=int, default=80)
    parser.add_argument(
        "--include-reloc-only",
        action="store_true",
        help="include functions whose differences are only relocation arguments",
    )
    args = parser.parse_args()

    report_path = ROOT / "build" / args.version / "report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    candidates: list[tuple[float, dict]] = []
    for unit in report["units"]:
        functions = unit.get("functions", [])
        incomplete = [
            function
            for function in functions
            if args.min_match <= function.get("fuzzy_match_percent", 0) < 100
        ]
        if not incomplete:
            continue
        best = max(function.get("fuzzy_match_percent", 0.0) for function in incomplete)
        candidates.append((best, unit))
    candidates.sort(reverse=True, key=lambda item: item[0])

    objdump_path = objdump()
    rows: list[tuple[int, float, str, str, Counter[str]]] = []
    for _, unit in candidates[: args.unit_limit]:
        source_path = unit.get("metadata", {}).get("source_path", "")
        paths = object_paths(args.version, source_path)
        if paths is None:
            continue
        for symbol in unit.get("functions", []):
            match = symbol.get("fuzzy_match_percent")
            if match is None or not args.min_match <= match < 100:
                continue
            counts = classify_symbol(objdump_path, *paths, symbol["name"])
            substantive = sum(
                count
                for category, count in counts.items()
                if category not in {"pool-reloc", "reg-perm"}
            )
            if not args.include_reloc_only and counts and set(counts) == {"pool-reloc"}:
                continue
            rows.append(
                (substantive, float(match), source_path, symbol["name"], counts)
            )

    rows.sort(key=lambda row: (-row[0], -row[1], row[2], row[3]))
    for substantive, match, source, symbol, counts in rows[: args.top]:
        summary = ", ".join(
            f"{category}={count}" for category, count in sorted(counts.items())
        )
        print(
            f"{match:9.5f}% structural={substantive:3d}  "
            f"{source}::{symbol}  [{summary}]"
        )


if __name__ == "__main__":
    main()
