from __future__ import annotations

import argparse
import csv
import io
import re
import sys
from dataclasses import dataclass
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.orig.source_recovery import (
    RecoveryGroup,
    collect_candidates,
    format_function_name,
    group_candidates,
    normalize_token,
    top_named_functions,
)


WORD_SPLIT_RE = re.compile(r"[A-Z]?[a-z]+|[A-Z]+(?=[A-Z]|$)|[0-9]+")


@dataclass(frozen=True)
class RetailFunctionHint:
    retail_source_name: str
    retail_label: str
    retail_messages: tuple[str, ...]
    retail_addresses: tuple[int, ...]
    xrefs: tuple[str, ...]
    debug_paths: tuple[str, ...]
    debug_matches: tuple[str, ...]
    debug_candidates: tuple[str, ...]


def split_words(value: str) -> tuple[str, ...]:
    parts: list[str] = []
    for token in re.split(r"[^A-Za-z0-9]+", value):
        if not token:
            continue
        parts.extend(part.lower() for part in WORD_SPLIT_RE.findall(token) if part)
    return tuple(parts)


def label_match_score(label_words: tuple[str, ...], debug_name: str) -> tuple[int, int]:
    if not label_words:
        return (0, 0)
    debug_words = set(split_words(debug_name))
    overlap = sum(1 for word in label_words if word in debug_words)
    normalized_match = 1 if normalize_token(debug_name).endswith(normalize_token("".join(label_words))) else 0
    return overlap, normalized_match


def unique_names(values: list[str], limit: int) -> tuple[str, ...]:
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
        if len(result) >= limit:
            break
    return tuple(result)


def collect_function_hints(groups: list[RecoveryGroup]) -> list[RetailFunctionHint]:
    hints: list[RetailFunctionHint] = []
    for group in groups:
        if not group.retail_labels:
            continue
        debug_candidates = top_named_functions(group.debug_sources, group.debug_symbol_hits, limit=12)
        for label in group.retail_labels:
            label_words = split_words(label)
            scored: list[tuple[int, int, str]] = []
            for name in debug_candidates:
                overlap, normalized_match = label_match_score(label_words, name)
                if overlap == 0 and normalized_match == 0:
                    continue
                scored.append((overlap, normalized_match, name))
            scored.sort(key=lambda item: (-item[1], -item[0], item[2].lower()))
            hints.append(
                RetailFunctionHint(
                    retail_source_name=group.retail_source_name,
                    retail_label=label,
                    retail_messages=group.retail_messages,
                    retail_addresses=group.retail_addresses,
                    xrefs=tuple(format_function_name(xref) for xref in group.xrefs),
                    debug_paths=tuple(source.path for source in group.debug_sources),
                    debug_matches=unique_names([name for _overlap, _normalized, name in scored], 6),
                    debug_candidates=tuple(debug_candidates),
                )
            )
    hints.sort(
        key=lambda item: (
            not item.xrefs,
            not item.debug_matches,
            item.retail_source_name.lower(),
            item.retail_label.lower(),
        )
    )
    return hints


def summary_markdown(hints: list[RetailFunctionHint]) -> str:
    with_debug_matches = [hint for hint in hints if hint.debug_matches]
    retail_only = [hint for hint in hints if not hint.debug_matches]
    lines: list[str] = []
    lines.append("# Retail function-label recovery")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- Retail-labeled function candidates: `{len(hints)}`")
    lines.append(f"- Candidates with direct EN xrefs: `{sum(1 for hint in hints if hint.xrefs)}`")
    lines.append(f"- Candidates with debug-side name bridges: `{len(with_debug_matches)}`")
    lines.append("")
    lines.append("## Strongest function-name bridges")
    if with_debug_matches:
        for hint in with_debug_matches:
            lines.append(f"- `{hint.retail_source_name}` retail label `{hint.retail_label}`")
            lines.append(
                "  retail addresses: " + ", ".join(f"`0x{address:08X}`" for address in hint.retail_addresses[:6])
            )
            if hint.retail_messages:
                lines.append(
                    "  retail messages: " + ", ".join(f"`{message}`" for message in hint.retail_messages[:3])
                )
            if hint.xrefs:
                lines.append("  EN xrefs: " + ", ".join(f"`{xref}`" for xref in hint.xrefs[:6]))
            if hint.debug_paths:
                lines.append("  debug paths: " + ", ".join(f"`{path}`" for path in hint.debug_paths[:3]))
            lines.append("  debug name bridges: " + ", ".join(f"`{name}`" for name in hint.debug_matches))
    else:
        lines.append("- None")
    lines.append("")
    lines.append("## Retail labels without a stronger bridge yet")
    if retail_only:
        for hint in retail_only:
            lines.append(f"- `{hint.retail_source_name}` retail label `{hint.retail_label}`")
            lines.append(
                "  retail addresses: " + ", ".join(f"`0x{address:08X}`" for address in hint.retail_addresses[:6])
            )
            if hint.retail_messages:
                lines.append(
                    "  retail messages: " + ", ".join(f"`{message}`" for message in hint.retail_messages[:3])
                )
            if hint.xrefs:
                lines.append("  EN xrefs: " + ", ".join(f"`{xref}`" for xref in hint.xrefs[:6]))
            else:
                lines.append("  EN xrefs: none")
            if hint.debug_candidates:
                lines.append(
                    "  debug-side candidates: " + ", ".join(f"`{name}`" for name in hint.debug_candidates[:6])
                )
            else:
                lines.append("  debug-side candidates: none")
    else:
        lines.append("- None")
    lines.append("")
    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/source_functions.py`")
    lines.append("- Search one file or label: `python tools/orig/source_functions.py --search objanim setBlendMove Init`")
    lines.append("- CSV dump: `python tools/orig/source_functions.py --format csv`")
    return "\n".join(lines)


def search_markdown(hints: list[RetailFunctionHint], patterns: list[str]) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    matches: list[RetailFunctionHint] = []
    for hint in hints:
        fields = [
            hint.retail_source_name.lower(),
            hint.retail_label.lower(),
        ]
        fields.extend(message.lower() for message in hint.retail_messages)
        fields.extend(xref.lower() for xref in hint.xrefs)
        fields.extend(path.lower() for path in hint.debug_paths)
        fields.extend(name.lower() for name in hint.debug_matches)
        fields.extend(name.lower() for name in hint.debug_candidates)
        if any(any(pattern in field for field in fields) for pattern in lowered):
            matches.append(hint)

    lines = ["# Retail function-label search", ""]
    if not matches:
        lines.append("- No matching retail function-label candidates.")
        return "\n".join(lines)

    for hint in matches:
        lines.append(f"- `{hint.retail_source_name}` retail label `{hint.retail_label}`")
        lines.append("  retail addresses: " + ", ".join(f"`0x{address:08X}`" for address in hint.retail_addresses[:6]))
        if hint.retail_messages:
            lines.append("  retail messages: " + ", ".join(f"`{message}`" for message in hint.retail_messages[:4]))
        if hint.xrefs:
            lines.append("  EN xrefs: " + ", ".join(f"`{xref}`" for xref in hint.xrefs[:6]))
        else:
            lines.append("  EN xrefs: none")
        if hint.debug_paths:
            lines.append("  debug paths: " + ", ".join(f"`{path}`" for path in hint.debug_paths[:3]))
        if hint.debug_matches:
            lines.append("  debug name bridges: " + ", ".join(f"`{name}`" for name in hint.debug_matches))
        elif hint.debug_candidates:
            lines.append("  debug-side candidates: " + ", ".join(f"`{name}`" for name in hint.debug_candidates[:6]))
        else:
            lines.append("  debug-side candidates: none")
    return "\n".join(lines)


def rows_to_csv(hints: list[RetailFunctionHint]) -> str:
    fieldnames = [
        "retail_source_name",
        "retail_label",
        "retail_addresses",
        "retail_messages",
        "en_xrefs",
        "debug_paths",
        "debug_name_bridges",
        "debug_candidates",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for hint in hints:
        writer.writerow(
            {
                "retail_source_name": hint.retail_source_name,
                "retail_label": hint.retail_label,
                "retail_addresses": ",".join(f"0x{address:08X}" for address in hint.retail_addresses),
                "retail_messages": ",".join(hint.retail_messages),
                "en_xrefs": ",".join(hint.xrefs),
                "debug_paths": ",".join(hint.debug_paths),
                "debug_name_bridges": ",".join(hint.debug_matches),
                "debug_candidates": ",".join(hint.debug_candidates),
            }
        )
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Recover retail function-label hints from EN main.dol source-tagged strings."
    )
    parser.add_argument(
        "--dol",
        type=Path,
        default=Path("orig/GSAE01/sys/main.dol"),
        help="Path to the retail EN main.dol.",
    )
    parser.add_argument(
        "--symbols",
        type=Path,
        default=Path("config/GSAE01/symbols.txt"),
        help="Current EN symbols.txt for naming retail xref callsites.",
    )
    parser.add_argument(
        "--debug-symbols",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/symbols.txt"),
        help="Debug-side symbols used only as side evidence.",
    )
    parser.add_argument(
        "--debug-splits",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/splits.txt"),
        help="Debug-side splits used only as side evidence.",
    )
    parser.add_argument(
        "--debug-srcfiles",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/notes/srcfiles.txt"),
        help="Debug-side source filename inventory used only as side evidence.",
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
        help="Case-insensitive substring search across retail labels, xrefs, debug paths, and debug candidates.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()
    candidates = collect_candidates(
        retail_strings_path=args.dol,
        retail_symbols_path=args.symbols,
        debug_symbols_path=args.debug_symbols,
        debug_splits_path=args.debug_splits,
        debug_srcfiles_path=args.debug_srcfiles,
    )
    groups = group_candidates(candidates)
    hints = collect_function_hints(groups)

    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(hints))
        elif args.search:
            sys.stdout.write(search_markdown(hints, args.search))
            sys.stdout.write("\n")
        else:
            sys.stdout.write(summary_markdown(hints))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
