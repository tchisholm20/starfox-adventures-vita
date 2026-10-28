from __future__ import annotations

import argparse
import csv
import io
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.orig.dol_xrefs import load_function_symbols
from tools.orig.source_recovery import (
    RecoveryGroup,
    collect_candidates,
    format_function_name,
    group_candidates,
    normalize_token,
)


CONFIG_OBJECT_RE = re.compile(r'Object\([^,]+,\s*"([^"]+\.(?:c|cpp|h|hpp))"\)')
WORD_SPLIT_RE = re.compile(r"[A-Z]?[a-z]+|[A-Z]+(?=[A-Z]|$)|[0-9]+")
GENERIC_SYMBOL_RE = re.compile(
    r"(?:^fn_|_func[0-9a-f]+$|func[0-9a-f]{2,}$|.*Fn_[0-9a-f]+$)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class DllRegistryHint:
    dll_id: str
    dll_name: str
    srcfile: str


@dataclass(frozen=True)
class ObjectDescriptionHint:
    object_id: str
    object_name: str
    dll_id: str | None
    description: str
    source_path: str


@dataclass(frozen=True)
class BucketInference:
    bucket: str
    support: tuple[str, ...]


@dataclass(frozen=True)
class ReferenceHint:
    retail_source_name: str
    retail_labels: tuple[str, ...]
    retail_messages: tuple[str, ...]
    en_xrefs: tuple[str, ...]
    current_debug_paths: tuple[str, ...]
    current_debug_names: tuple[str, ...]
    reference_configure_paths: tuple[str, ...]
    reference_dlls: tuple[DllRegistryHint, ...]
    reference_symbol_hints: tuple[str, ...]
    reference_inventory_neighbors: tuple[str, ...]
    inferred_bucket: BucketInference | None
    object_description_hints: tuple[ObjectDescriptionHint, ...]

    @property
    def is_unplaced(self) -> bool:
        return not self.current_debug_paths

    @property
    def has_reference_path_hint(self) -> bool:
        return bool(self.reference_configure_paths)

    @property
    def has_reference_function_hint(self) -> bool:
        return bool(self.reference_symbol_hints)


def split_words(value: str) -> tuple[str, ...]:
    parts: list[str] = []
    for token in re.split(r"[^A-Za-z0-9]+", value):
        if not token:
            continue
        parts.extend(part.lower() for part in WORD_SPLIT_RE.findall(token) if part)
    return tuple(parts)


def unique_strings(values: list[str], limit: int | None = None) -> tuple[str, ...]:
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
        if limit is not None and len(result) >= limit:
            break
    return tuple(result)


def top_bucket(path: str) -> str | None:
    normalized = path.replace("\\", "/").strip("/")
    if not normalized or "/" not in normalized:
        return None
    return normalized.split("/", 1)[0]


def parse_reference_configure_paths(path: Path) -> dict[str, list[str]]:
    by_basename: dict[str, list[str]] = {}
    if not path.is_file():
        return by_basename
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = CONFIG_OBJECT_RE.search(line)
        if match is None:
            continue
        relpath = match.group(1).replace("\\", "/")
        by_basename.setdefault(Path(relpath).name.lower(), []).append(relpath)
    for relpaths in by_basename.values():
        relpaths.sort(key=str.lower)
    return by_basename


def parse_dll_registry(path: Path) -> dict[str, list[DllRegistryHint]]:
    by_basename: dict[str, list[DllRegistryHint]] = {}
    if not path.is_file():
        return by_basename
    for _event, element in ET.iterparse(path, events=("end",)):
        if element.tag != "dll":
            continue
        srcfile = element.attrib.get("srcfile")
        if not srcfile:
            element.clear()
            continue
        hint = DllRegistryHint(
            dll_id=element.attrib.get("id", ""),
            dll_name=element.attrib.get("name", ""),
            srcfile=srcfile,
        )
        by_basename.setdefault(Path(srcfile).name.lower(), []).append(hint)
        element.clear()
    for hints in by_basename.values():
        hints.sort(key=lambda item: (item.dll_id, item.dll_name.lower(), item.srcfile.lower()))
    return by_basename


def parse_source_inventory(path: Path) -> list[str]:
    entries: list[str] = []
    if not path.is_file():
        return entries
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.split(";", 1)[0].strip()
        if not line or not line.endswith((".c", ".cpp", ".h", ".hpp")):
            continue
        entries.append(line.replace("\\", "/"))
    return entries


def inventory_neighbors(entries: list[str], source_name: str, radius: int = 2) -> tuple[str, ...]:
    basename = Path(source_name).name.lower()
    values: list[str] = []
    for index, entry in enumerate(entries):
        if Path(entry).name.lower() != basename:
            continue
        start = max(0, index - radius)
        end = min(len(entries), index + radius + 1)
        for offset in range(start, end):
            if offset == index:
                continue
            neighbor = entries[offset]
            values.append(neighbor)
    return unique_strings(values, limit=8)


def infer_bucket_from_neighbors(
    neighbors: tuple[str, ...],
    known_bucket_by_basename: dict[str, str],
) -> BucketInference | None:
    votes: dict[str, list[str]] = {}
    for neighbor in neighbors:
        bucket = known_bucket_by_basename.get(Path(neighbor).name.lower())
        if bucket is None:
            continue
        votes.setdefault(bucket, []).append(neighbor)
    if not votes:
        return None
    ordered = sorted(
        ((bucket, len(items), tuple(items)) for bucket, items in votes.items()),
        key=lambda item: (-item[1], item[0]),
    )
    bucket, count, support = ordered[0]
    if count < 2:
        return None
    if len(ordered) > 1 and ordered[1][1] == count:
        return None
    return BucketInference(bucket=bucket, support=support)


def parse_object_descriptions(path: Path) -> list[ObjectDescriptionHint]:
    hints: list[ObjectDescriptionHint] = []
    if not path.is_file():
        return hints
    for _event, element in ET.iterparse(path, events=("end",)):
        if element.tag != "object":
            continue
        description = (element.findtext("description") or "").strip()
        if not description:
            element.clear()
            continue
        hints.append(
            ObjectDescriptionHint(
                object_id=element.attrib.get("id", ""),
                object_name=element.attrib.get("name", ""),
                dll_id=element.attrib.get("dll"),
                description=description,
                source_path=path.as_posix(),
            )
        )
        element.clear()
    return hints


def interesting_reference_symbols(group: RecoveryGroup, symbol_names: list[str], limit: int = 8) -> tuple[str, ...]:
    stem = normalize_token(Path(group.retail_source_name).stem)
    if not stem:
        return ()

    interesting_words: set[str] = set()
    for value in (group.retail_source_name, *group.retail_labels, *group.retail_messages):
        interesting_words.update(split_words(value))

    scored: list[tuple[int, int, int, str]] = []
    generic_fallback: list[str] = []
    for name in symbol_names:
        normalized_name = normalize_token(name)
        if stem not in normalized_name:
            continue
        if GENERIC_SYMBOL_RE.search(name):
            generic_fallback.append(name)
            continue
        name_words = set(split_words(name))
        overlap = sum(1 for word in interesting_words if word and word in name_words)
        label_match = sum(1 for label in group.retail_labels if normalize_token(label) in normalized_name)
        message_match = sum(1 for message in group.retail_messages if normalize_token(message) in normalized_name)
        score = overlap * 3 + label_match * 4 + message_match * 2
        scored.append((score, overlap, label_match + message_match, name))

    scored.sort(key=lambda item: (-item[0], -item[1], -item[2], item[3].lower()))
    chosen = [name for score, _overlap, _matches, name in scored if score > 0]
    if not chosen:
        chosen = [name for _score, _overlap, _matches, name in scored]
    if not chosen and generic_fallback:
        chosen = sorted(set(generic_fallback), key=str.lower)
    return unique_strings(chosen, limit=limit)


def object_matches(group: RecoveryGroup, objects: list[ObjectDescriptionHint]) -> tuple[ObjectDescriptionHint, ...]:
    patterns = [group.retail_source_name.lower()]
    matches: list[ObjectDescriptionHint] = []
    seen: set[tuple[str, str, str | None, str]] = set()
    for hint in objects:
        haystack = " ".join(
            [
                hint.object_name.lower(),
                hint.description.lower(),
                (hint.dll_id or "").lower(),
            ]
        )
        if any(pattern and pattern in haystack for pattern in patterns):
            key = (hint.object_id, hint.object_name, hint.dll_id, hint.description)
            if key in seen:
                continue
            seen.add(key)
            matches.append(hint)
    matches.sort(key=lambda item: (item.object_id, item.object_name.lower(), item.description.lower()))
    return tuple(matches[:6])


def build_groups(
    dol: Path,
    symbols: Path,
    debug_symbols: Path,
    debug_splits: Path,
    debug_srcfiles: Path,
) -> list[RecoveryGroup]:
    candidates = collect_candidates(
        retail_strings_path=dol,
        retail_symbols_path=symbols,
        debug_symbols_path=debug_symbols,
        debug_splits_path=debug_splits,
        debug_srcfiles_path=debug_srcfiles,
    )
    return group_candidates(candidates)


def collect_reference_hints(
    groups: list[RecoveryGroup],
    reference_configure: Path,
    reference_symbols: Path,
    reference_inventory: Path,
    reference_dll_registry: Path,
    reference_object_xmls: tuple[Path, ...],
) -> list[ReferenceHint]:
    configure_paths = parse_reference_configure_paths(reference_configure)
    reference_symbol_names = [symbol.name for symbol in load_function_symbols(reference_symbols)]
    inventory_entries = parse_source_inventory(reference_inventory)
    dll_registry = parse_dll_registry(reference_dll_registry)
    object_hints: list[ObjectDescriptionHint] = []
    for path in reference_object_xmls:
        object_hints.extend(parse_object_descriptions(path))

    known_bucket_by_basename: dict[str, str] = {}
    for group in groups:
        basename = Path(group.retail_source_name).name.lower()
        for source in group.debug_sources:
            bucket = top_bucket(source.path)
            if bucket is not None:
                known_bucket_by_basename.setdefault(basename, bucket)
    for basename, paths in configure_paths.items():
        bucket = top_bucket(paths[0])
        if bucket is not None:
            known_bucket_by_basename.setdefault(basename, bucket)

    hints: list[ReferenceHint] = []
    for group in groups:
        basename = Path(group.retail_source_name).name.lower()
        neighbors = inventory_neighbors(inventory_entries, group.retail_source_name)
        hints.append(
            ReferenceHint(
                retail_source_name=group.retail_source_name,
                retail_labels=group.retail_labels,
                retail_messages=group.retail_messages,
                en_xrefs=tuple(format_function_name(xref) for xref in group.xrefs),
                current_debug_paths=tuple(source.path for source in group.debug_sources),
                current_debug_names=tuple(group.debug_symbol_hits[:8]),
                reference_configure_paths=tuple(configure_paths.get(basename, [])),
                reference_dlls=tuple(dll_registry.get(basename, [])),
                reference_symbol_hints=interesting_reference_symbols(group, reference_symbol_names),
                reference_inventory_neighbors=neighbors,
                inferred_bucket=infer_bucket_from_neighbors(neighbors, known_bucket_by_basename),
                object_description_hints=object_matches(group, object_hints),
            )
        )

    hints.sort(
        key=lambda item: (
            not item.is_unplaced,
            not item.has_reference_path_hint,
            not item.has_reference_function_hint,
            item.retail_source_name.lower(),
        )
    )
    return hints


def summary_markdown(hints: list[ReferenceHint]) -> str:
    with_paths = [hint for hint in hints if hint.reference_configure_paths]
    with_functions = [hint for hint in hints if hint.reference_symbol_hints]
    with_objects = [hint for hint in hints if hint.object_description_hints]
    unplaced_with_extra = [
        hint
        for hint in hints
        if hint.is_unplaced and (hint.reference_configure_paths or hint.inferred_bucket or hint.object_description_hints)
    ]
    strong = [
        hint
        for hint in hints
        if hint.is_unplaced and (hint.reference_configure_paths or hint.reference_symbol_hints or hint.object_description_hints)
    ]

    lines: list[str] = []
    lines.append("# Reference-side source recovery hints")
    lines.append("")
    lines.append("These hints come from local reference projects bundled in this checkout.")
    lines.append("They are useful leads, not source-truth, and should stay secondary to retail EN evidence.")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- Retail source candidates scanned: `{len(hints)}`")
    lines.append(f"- Candidates with reference configure-path hints: `{len(with_paths)}`")
    lines.append(f"- Candidates with reference function-name hints: `{len(with_functions)}`")
    lines.append(f"- Candidates with reference object-description matches: `{len(with_objects)}`")
    lines.append(f"- Unplaced retail candidates with extra reference clues: `{len(unplaced_with_extra)}`")
    lines.append("")
    lines.append("## Highest-value reference-only bridges")
    if strong:
        for hint in strong:
            lines.extend(markdown_lines_for_hint(hint))
    else:
        lines.append("- None")
    lines.append("")
    lines.append("## Unplaced candidates still lacking useful side evidence")
    residual = [hint for hint in hints if hint.is_unplaced and hint not in strong]
    if residual:
        for hint in residual:
            lines.append(f"- `{hint.retail_source_name}`")
            if hint.en_xrefs:
                lines.append("  EN xrefs: " + ", ".join(f"`{xref}`" for xref in hint.en_xrefs[:6]))
            else:
                lines.append("  EN xrefs: none")
    else:
        lines.append("- None")
    lines.append("")
    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/source_reference_hints.py`")
    lines.append("- Search one source or hint: `python tools/orig/source_reference_hints.py --search expgfx textblock objHitReact`")
    lines.append("- CSV dump: `python tools/orig/source_reference_hints.py --format csv`")
    return "\n".join(lines)


def markdown_lines_for_hint(hint: ReferenceHint) -> list[str]:
    lines: list[str] = []
    lines.append(f"- `{hint.retail_source_name}`")
    if hint.retail_labels:
        lines.append("  retail labels: " + ", ".join(f"`{label}`" for label in hint.retail_labels))
    if hint.retail_messages:
        lines.append("  retail messages: " + ", ".join(f"`{message}`" for message in hint.retail_messages[:4]))
    if hint.en_xrefs:
        lines.append("  EN xrefs: " + ", ".join(f"`{xref}`" for xref in hint.en_xrefs[:6]))
    else:
        lines.append("  EN xrefs: none")
    if hint.current_debug_paths:
        lines.append("  current debug paths: " + ", ".join(f"`{path}`" for path in hint.current_debug_paths[:3]))
    if hint.reference_configure_paths:
        lines.append("  reference configure paths: " + ", ".join(f"`{path}`" for path in hint.reference_configure_paths))
    if hint.inferred_bucket is not None:
        lines.append(f"  inferred bucket: `{hint.inferred_bucket.bucket}`")
        lines.append("  bucket support: " + ", ".join(f"`{value}`" for value in hint.inferred_bucket.support))
    if hint.reference_dlls:
        lines.append(
            "  reference DLL registry: "
            + ", ".join(
                f"`{dll.dll_id}` `{dll.dll_name}` `{dll.srcfile}`"
                for dll in hint.reference_dlls[:4]
            )
        )
    if hint.reference_symbol_hints:
        lines.append(
            "  reference function hints: "
            + ", ".join(f"`{name}`" for name in hint.reference_symbol_hints[:8])
        )
    if hint.object_description_hints:
        lines.append(
            "  reference object hints: "
            + ", ".join(
                f"`{obj.object_name}` (id `{obj.object_id}`, dll `{obj.dll_id or '?'}`)"
                for obj in hint.object_description_hints[:4]
            )
        )
    return lines


def search_markdown(hints: list[ReferenceHint], patterns: list[str]) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    matches: list[ReferenceHint] = []
    for hint in hints:
        fields = [
            hint.retail_source_name.lower(),
            *(label.lower() for label in hint.retail_labels),
            *(message.lower() for message in hint.retail_messages),
            *(xref.lower() for xref in hint.en_xrefs),
            *(path.lower() for path in hint.current_debug_paths),
            *(path.lower() for path in hint.reference_configure_paths),
            *(dll.dll_id.lower() for dll in hint.reference_dlls),
            *(dll.dll_name.lower() for dll in hint.reference_dlls),
            *(dll.srcfile.lower() for dll in hint.reference_dlls),
            *(name.lower() for name in hint.reference_symbol_hints),
            *(value.lower() for value in hint.reference_inventory_neighbors),
            *((hint.inferred_bucket.bucket.lower(),) if hint.inferred_bucket is not None else ()),
            *(obj.object_name.lower() for obj in hint.object_description_hints),
            *(obj.description.lower() for obj in hint.object_description_hints),
        ]
        if any(any(pattern in field for field in fields) for pattern in lowered):
            matches.append(hint)

    lines = ["# Reference-side source-hint search", ""]
    if not matches:
        lines.append("- No matching hints.")
        return "\n".join(lines)

    for hint in matches:
        lines.extend(markdown_lines_for_hint(hint))
    return "\n".join(lines)


def rows_to_csv(hints: list[ReferenceHint]) -> str:
    fieldnames = [
        "retail_source_name",
        "retail_labels",
        "retail_messages",
        "en_xrefs",
        "current_debug_paths",
        "reference_configure_paths",
        "reference_dlls",
        "reference_symbol_hints",
        "reference_inventory_neighbors",
        "inferred_bucket",
        "bucket_support",
        "reference_object_hints",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for hint in hints:
        writer.writerow(
            {
                "retail_source_name": hint.retail_source_name,
                "retail_labels": ",".join(hint.retail_labels),
                "retail_messages": ",".join(hint.retail_messages),
                "en_xrefs": ",".join(hint.en_xrefs),
                "current_debug_paths": ",".join(hint.current_debug_paths),
                "reference_configure_paths": ",".join(hint.reference_configure_paths),
                "reference_dlls": ",".join(
                    f"{dll.dll_id}:{dll.dll_name}:{dll.srcfile}" for dll in hint.reference_dlls
                ),
                "reference_symbol_hints": ",".join(hint.reference_symbol_hints),
                "reference_inventory_neighbors": ",".join(hint.reference_inventory_neighbors),
                "inferred_bucket": "" if hint.inferred_bucket is None else hint.inferred_bucket.bucket,
                "bucket_support": ""
                if hint.inferred_bucket is None
                else ",".join(hint.inferred_bucket.support),
                "reference_object_hints": ",".join(
                    f"{obj.object_id}:{obj.object_name}:{obj.dll_id or ''}:{obj.description}"
                    for obj in hint.object_description_hints
                ),
            }
        )
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Mine local reference projects for source/file/function hints while keeping retail EN evidence primary."
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
        help="Debug-side symbols used for the existing retail crosswalk.",
    )
    parser.add_argument(
        "--debug-splits",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/splits.txt"),
        help="Debug-side splits used for the existing retail crosswalk.",
    )
    parser.add_argument(
        "--debug-srcfiles",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/notes/srcfiles.txt"),
        help="Debug-side source inventory used for the existing retail crosswalk.",
    )
    parser.add_argument(
        "--reference-configure",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/configure.py"),
        help="Reference configure.py mined only for side-path hints.",
    )
    parser.add_argument(
        "--reference-symbols",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/symbols.txt"),
        help="Reference symbols.txt mined only for side-function hints.",
    )
    parser.add_argument(
        "--reference-inventory",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/notes/srcfiles.txt"),
        help="Reference source-order inventory mined only for neighbor context.",
    )
    parser.add_argument(
        "--reference-dll-registry",
        type=Path,
        default=Path("reference_projects/rena-tools/StarFoxAdventures/data/KD/dlls.xml"),
        help="Reference DLL registry mined only for side DLL/srcfile hints.",
    )
    parser.add_argument(
        "--reference-object-xml",
        type=Path,
        nargs="*",
        default=(
            Path("reference_projects/rena-tools/StarFoxAdventures/data/U0/objects.xml"),
            Path("reference_projects/rena-tools/StarFoxAdventures/data/U0/objects2.xml"),
        ),
        help="Reference object XML files mined only for descriptive hits.",
    )
    parser.add_argument(
        "--search",
        nargs="+",
        help="Case-insensitive substring search across source names, hints, and descriptions.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "csv"),
        default="markdown",
        help="Output format.",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()

    groups = build_groups(
        dol=args.dol,
        symbols=args.symbols,
        debug_symbols=args.debug_symbols,
        debug_splits=args.debug_splits,
        debug_srcfiles=args.debug_srcfiles,
    )
    hints = collect_reference_hints(
        groups=groups,
        reference_configure=args.reference_configure,
        reference_symbols=args.reference_symbols,
        reference_inventory=args.reference_inventory,
        reference_dll_registry=args.reference_dll_registry,
        reference_object_xmls=tuple(args.reference_object_xml),
    )

    if args.format == "csv":
        print(rows_to_csv(hints), end="")
        return
    if args.search:
        print(search_markdown(hints, args.search))
        return
    print(summary_markdown(hints))


if __name__ == "__main__":
    main()
