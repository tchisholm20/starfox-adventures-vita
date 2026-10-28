from __future__ import annotations

import argparse
import csv
import io
import sys
from dataclasses import dataclass
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.orig.dol_xrefs import FunctionSymbol, format_function_name, load_function_symbols
from tools.orig.source_boundaries import suggested_path as pick_suggested_path
from tools.orig.source_reference_hints import build_groups, collect_reference_hints, parse_source_inventory
from tools.orig.source_recovery import parse_debug_split_text_ranges


@dataclass(frozen=True)
class SourceAnchor:
    retail_source_name: str
    suggested_path: str
    xref_count: int
    retail_labels: tuple[str, ...]
    retail_messages: tuple[str, ...]
    en_xrefs: tuple[str, ...]
    en_seed_functions: tuple[FunctionSymbol, ...]
    function_start_index: int | None
    function_end_index: int | None
    en_span_start: int | None
    en_span_end: int | None
    debug_split_path: str | None
    debug_split_index: int | None
    debug_split_start: int | None
    debug_split_end: int | None
    debug_split_function_count: int
    srcfile_index: int | None
    srcfile_match_count: int
    split_prev_paths: tuple[str, ...]
    split_next_paths: tuple[str, ...]
    srcfile_prev: tuple[str, ...]
    srcfile_next: tuple[str, ...]

    @property
    def en_span_size(self) -> int | None:
        if self.en_span_start is None or self.en_span_end is None:
            return None
        return self.en_span_end - self.en_span_start

    @property
    def en_function_count(self) -> int:
        return len(self.en_seed_functions)

    @property
    def debug_split_size(self) -> int | None:
        if self.debug_split_start is None or self.debug_split_end is None:
            return None
        return self.debug_split_end - self.debug_split_start

    @property
    def fit_status(self) -> str:
        if self.en_span_size is None:
            return "no-en-xrefs"
        if self.debug_split_size is None or self.debug_split_size == 0:
            return "no-debug-split"
        ratio = self.en_span_size / self.debug_split_size
        if ratio >= 1.5:
            return "seed-too-wide"
        if ratio <= 0.6:
            return "seed-too-small"
        return "seed-near-fit"

    @property
    def size_delta(self) -> int | None:
        if self.en_span_size is None or self.debug_split_size is None:
            return None
        return self.en_span_size - self.debug_split_size

    @property
    def size_ratio(self) -> float | None:
        if self.en_span_size is None or self.debug_split_size in (None, 0):
            return None
        return self.en_span_size / self.debug_split_size

    @property
    def order_mode(self) -> str:
        if self.debug_split_index is not None:
            return "debug-split"
        if self.srcfile_index is not None:
            return "srcfiles"
        return "unordered"


@dataclass(frozen=True)
class SourceCorridor:
    left: SourceAnchor
    right: SourceAnchor
    srcfile_gap_paths: tuple[str, ...]
    gap_functions: tuple[FunctionSymbol, ...]

    @property
    def en_gap_start(self) -> int | None:
        if self.left.en_span_end is None or self.right.en_span_start is None:
            return None
        return self.left.en_span_end

    @property
    def en_gap_end(self) -> int | None:
        if self.left.en_span_end is None or self.right.en_span_start is None:
            return None
        return self.right.en_span_start

    @property
    def en_gap_size(self) -> int | None:
        if self.en_gap_start is None or self.en_gap_end is None:
            return None
        return self.en_gap_end - self.en_gap_start

    @property
    def gap_path_count(self) -> int:
        return len(self.srcfile_gap_paths)


def unique_strings(values: list[str]) -> tuple[str, ...]:
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if not value or value in seen:
            continue
        seen.add(value)
        result.append(value)
    return tuple(result)


def format_symbol_span(function: FunctionSymbol) -> str:
    return f"{function.name}@0x{function.address:08X}-0x{function.address + function.size:08X}"


def basename_index(entries: list[str]) -> dict[str, list[int]]:
    mapping: dict[str, list[int]] = {}
    for index, entry in enumerate(entries):
        mapping.setdefault(Path(entry).name.lower(), []).append(index)
    return mapping


def exact_split_neighbors(paths: list[str], index: int | None, radius: int = 2) -> tuple[tuple[str, ...], tuple[str, ...]]:
    if index is None:
        return (), ()
    before = tuple(paths[max(0, index - radius):index])
    after = tuple(paths[index + 1: min(len(paths), index + 1 + radius)])
    return before, after


def srcfile_neighbors(entries: list[str], index: int | None, radius: int = 2) -> tuple[tuple[str, ...], tuple[str, ...]]:
    if index is None:
        return (), ()
    before = tuple(entries[max(0, index - radius):index])
    after = tuple(entries[index + 1: min(len(entries), index + 1 + radius)])
    return before, after


def seed_functions_for_group(
    group,
    function_by_address: dict[int, FunctionSymbol],
    current_functions: list[FunctionSymbol],
    index_by_address: dict[int, int],
) -> tuple[tuple[FunctionSymbol, ...], int | None, int | None]:
    values: list[FunctionSymbol] = []
    seen: set[int] = set()
    for xref in group.xrefs:
        if xref.function_start is None:
            continue
        function = function_by_address.get(xref.function_start)
        if function is None or function.address in seen:
            continue
        seen.add(function.address)
        values.append(function)
    values.sort(key=lambda item: item.address)
    if not values:
        return (), None, None
    start_index = min(index_by_address[item.address] for item in values)
    end_index = max(index_by_address[item.address] for item in values)
    return tuple(current_functions[start_index:end_index + 1]), start_index, end_index


def build_anchors(
    groups,
    reference_hints,
    current_functions: list[FunctionSymbol],
    debug_split_paths: list[str],
    srcfiles_entries: list[str],
) -> list[SourceAnchor]:
    hint_by_name = {hint.retail_source_name.lower(): hint for hint in reference_hints}
    function_by_address = {function.address: function for function in current_functions}
    index_by_address = {function.address: index for index, function in enumerate(current_functions)}
    debug_split_index_by_path = {path: index for index, path in enumerate(debug_split_paths)}
    srcfile_indices = basename_index(srcfiles_entries)

    anchors: list[SourceAnchor] = []
    for group in groups:
        hint = hint_by_name[group.retail_source_name.lower()]
        seed_functions, start_index, end_index = seed_functions_for_group(
            group,
            function_by_address,
            current_functions,
            index_by_address,
        )

        en_span_start = None if not seed_functions else seed_functions[0].address
        en_span_end = None if not seed_functions else seed_functions[-1].address + seed_functions[-1].size

        debug_source = group.debug_sources[0] if group.debug_sources else None
        debug_split_path = None if debug_source is None else debug_source.path
        debug_split_index = None if debug_split_path is None else debug_split_index_by_path.get(debug_split_path)
        split_prev_paths, split_next_paths = exact_split_neighbors(debug_split_paths, debug_split_index)

        basename = Path(group.retail_source_name).name.lower()
        matches = srcfile_indices.get(basename, [])
        srcfile_index = matches[0] if len(matches) == 1 else None
        src_prev, src_next = srcfile_neighbors(srcfiles_entries, srcfile_index)

        anchors.append(
            SourceAnchor(
                retail_source_name=group.retail_source_name,
                suggested_path=pick_suggested_path(hint),
                xref_count=len(group.xrefs),
                retail_labels=group.retail_labels,
                retail_messages=group.retail_messages,
                en_xrefs=tuple(format_function_name(xref) for xref in group.xrefs),
                en_seed_functions=seed_functions,
                function_start_index=start_index,
                function_end_index=end_index,
                en_span_start=en_span_start,
                en_span_end=en_span_end,
                debug_split_path=debug_split_path,
                debug_split_index=debug_split_index,
                debug_split_start=None if debug_source is None else debug_source.text_start,
                debug_split_end=None if debug_source is None else debug_source.text_end,
                debug_split_function_count=0 if debug_source is None else len(debug_source.functions),
                srcfile_index=srcfile_index,
                srcfile_match_count=len(matches),
                split_prev_paths=split_prev_paths,
                split_next_paths=split_next_paths,
                srcfile_prev=src_prev,
                srcfile_next=src_next,
            )
        )

    anchors.sort(
        key=lambda item: (
            item.en_span_start is None,
            0xFFFFFFFF if item.en_span_start is None else item.en_span_start,
            item.retail_source_name.lower(),
        )
    )
    return anchors


def build_corridors(anchors: list[SourceAnchor], srcfiles_entries: list[str], current_functions: list[FunctionSymbol]) -> list[SourceCorridor]:
    ordered = [
        anchor
        for anchor in anchors
        if anchor.srcfile_index is not None and anchor.en_span_start is not None and anchor.en_span_end is not None
    ]
    ordered.sort(key=lambda item: (item.srcfile_index, item.en_span_start, item.retail_source_name.lower()))

    corridors: list[SourceCorridor] = []
    for left, right in zip(ordered, ordered[1:]):
        assert left.srcfile_index is not None
        assert right.srcfile_index is not None
        if left.srcfile_index >= right.srcfile_index:
            continue

        if (
            left.function_end_index is not None
            and right.function_start_index is not None
            and left.function_end_index + 1 <= right.function_start_index - 1
        ):
            gap_functions = tuple(current_functions[left.function_end_index + 1: right.function_start_index])
        else:
            gap_functions = ()

        corridors.append(
            SourceCorridor(
                left=left,
                right=right,
                srcfile_gap_paths=tuple(srcfiles_entries[left.srcfile_index + 1: right.srcfile_index]),
                gap_functions=gap_functions,
            )
        )
    return corridors


def corridor_preview(corridor: SourceCorridor, limit: int = 6) -> str:
    if not corridor.srcfile_gap_paths:
        return "none"
    preview = ", ".join(f"`{item}`" for item in corridor.srcfile_gap_paths[:limit])
    if corridor.gap_path_count > limit:
        preview += f", ... (+{corridor.gap_path_count - limit} more)"
    return preview


def function_preview(functions: tuple[FunctionSymbol, ...], limit: int = 6) -> str:
    if not functions:
        return "none"
    preview = ", ".join(f"`{format_symbol_span(function)}`" for function in functions[:limit])
    if len(functions) > limit:
        preview += f", ... (+{len(functions) - limit} more)"
    return preview


def fit_summary(anchor: SourceAnchor) -> str:
    if anchor.fit_status == "no-en-xrefs":
        return "no EN xrefs"
    if anchor.fit_status == "no-debug-split":
        return "no exact debug split"
    assert anchor.debug_split_size is not None
    assert anchor.en_span_size is not None
    assert anchor.size_delta is not None
    assert anchor.size_ratio is not None
    sign = "+" if anchor.size_delta >= 0 else "-"
    return (
        f"{anchor.fit_status} "
        f"(EN=`0x{anchor.en_span_size:X}` debug=`0x{anchor.debug_split_size:X}` "
        f"delta=`{sign}0x{abs(anchor.size_delta):X}` ratio=`{anchor.size_ratio:.2f}x`)"
    )


def local_corridors(corridors: list[SourceCorridor], anchor: SourceAnchor) -> tuple[SourceCorridor | None, SourceCorridor | None]:
    previous = next((corridor for corridor in reversed(corridors) if corridor.right.retail_source_name == anchor.retail_source_name), None)
    following = next((corridor for corridor in corridors if corridor.left.retail_source_name == anchor.retail_source_name), None)
    return previous, following


def exact_anchor_score(anchor: SourceAnchor) -> tuple[int, int, int]:
    if anchor.debug_split_size is None or anchor.en_span_size is None or anchor.size_delta is None:
        return (0, 0, 0)
    return (
        abs(anchor.size_delta),
        anchor.debug_split_size,
        anchor.en_span_size,
    )


def summary_markdown(anchors: list[SourceAnchor], corridors: list[SourceCorridor], limit: int) -> str:
    exact = [anchor for anchor in anchors if anchor.debug_split_path is not None and anchor.en_span_start is not None]
    srcfile_only = [
        anchor
        for anchor in anchors
        if anchor.debug_split_path is None and anchor.srcfile_index is not None and anchor.en_span_start is not None
    ]
    unordered = [
        anchor
        for anchor in anchors
        if anchor.srcfile_index is None and anchor.en_span_start is not None
    ]
    order_mismatches = [
        corridor
        for corridor in corridors
        if corridor.en_gap_size is not None and corridor.en_gap_size < 0
    ]
    short_corridors = sorted(
        [corridor for corridor in corridors if corridor.gap_path_count <= 8],
        key=lambda item: (item.gap_path_count, 0x7FFFFFFF if item.en_gap_size is None else abs(item.en_gap_size), item.left.srcfile_index or 0),
    )

    lines: list[str] = []
    lines.append("# Retail source corridors")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- Retail EN source anchors with xrefs: `{len([anchor for anchor in anchors if anchor.en_span_start is not None])}`")
    lines.append(f"- Exact debug-split-backed anchors: `{len(exact)}`")
    lines.append(f"- `srcfiles.txt`-ordered anchors without exact splits: `{len(srcfile_only)}`")
    lines.append(f"- Anchors still unordered against debug-side inventory: `{len(unordered)}`")
    lines.append(f"- Source-order corridors with EN spans on both ends: `{len(corridors)}`")
    lines.append(f"- EN/source-order mismatches detected: `{len(order_mismatches)}`")
    lines.append("")

    lines.append("## Exact Split Fit Audit")
    for anchor in sorted(exact, key=exact_anchor_score, reverse=True)[:limit]:
        lines.append(
            f"- `{anchor.retail_source_name}` -> `{anchor.suggested_path}` "
            f"EN=`0x{anchor.en_span_start:08X}-0x{anchor.en_span_end:08X}` "
            f"debug=`0x{anchor.debug_split_start:08X}-0x{anchor.debug_split_end:08X}` "
            f"{fit_summary(anchor)}"
        )
        if anchor.split_prev_paths or anchor.split_next_paths:
            parts: list[str] = []
            if anchor.split_prev_paths:
                parts.append("prev " + ", ".join(f"`{item}`" for item in anchor.split_prev_paths))
            if anchor.split_next_paths:
                parts.append("next " + ", ".join(f"`{item}`" for item in anchor.split_next_paths))
            lines.append("  debug split neighbors: " + "; ".join(parts))
    lines.append("")

    lines.append("## Short Source-Order Corridors")
    if short_corridors:
        for corridor in short_corridors[:limit]:
            gap_text = "unknown"
            if corridor.en_gap_size is not None:
                gap_text = f"`0x{corridor.en_gap_size:X}`"
            lines.append(
                f"- `{corridor.left.retail_source_name}` -> `{corridor.right.retail_source_name}` "
                f"intervening_srcfiles=`{corridor.gap_path_count}` en_gap={gap_text} "
                f"gap_functions=`{len(corridor.gap_functions)}`"
            )
            lines.append("  between: " + corridor_preview(corridor))
    else:
        lines.append("- None")
    lines.append("")

    if unordered:
        lines.append("## Unordered EN Anchors")
        for anchor in unordered[:limit]:
            lines.append(
                f"- `{anchor.retail_source_name}` -> `{anchor.suggested_path}` "
                f"EN=`0x{anchor.en_span_start:08X}-0x{anchor.en_span_end:08X}` "
                f"xrefs=`{anchor.xref_count}`"
            )
    else:
        lines.append("## Unordered EN Anchors")
        lines.append("- None")
    lines.append("")

    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/source_corridors.py`")
    lines.append("- Focus one anchor or local corridor: `python tools/orig/source_corridors.py --search objanim objHitReact`")
    lines.append("- CSV dump: `python tools/orig/source_corridors.py --format csv`")
    return "\n".join(lines)


def detailed_markdown(anchors: list[SourceAnchor], corridors: list[SourceCorridor], patterns: list[str]) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    visible: list[SourceAnchor] = []
    for anchor in anchors:
        fields = [
            anchor.retail_source_name.lower(),
            anchor.suggested_path.lower(),
            anchor.fit_status.lower(),
            *(label.lower() for label in anchor.retail_labels),
            *(message.lower() for message in anchor.retail_messages),
            *(xref.lower() for xref in anchor.en_xrefs),
            *(item.lower() for item in anchor.split_prev_paths),
            *(item.lower() for item in anchor.split_next_paths),
            *(item.lower() for item in anchor.srcfile_prev),
            *(item.lower() for item in anchor.srcfile_next),
        ]
        if anchor.debug_split_path is not None:
            fields.append(anchor.debug_split_path.lower())
        if any(any(pattern in field for field in fields) for pattern in lowered):
            visible.append(anchor)

    lines = ["# Retail source corridor search", ""]
    if not visible:
        lines.append("- No matching anchors.")
        return "\n".join(lines)

    for anchor in visible:
        prev_corridor, next_corridor = local_corridors(corridors, anchor)
        lines.append(f"## `{anchor.retail_source_name}`")
        lines.append(f"- suggested path: `{anchor.suggested_path}`")
        lines.append(f"- order mode: `{anchor.order_mode}`")
        if anchor.en_span_start is not None and anchor.en_span_end is not None:
            lines.append(
                f"- EN seed span: `0x{anchor.en_span_start:08X}-0x{anchor.en_span_end:08X}` "
                f"size=`0x{anchor.en_span_size:X}` functions=`{anchor.en_function_count}`"
            )
            lines.append("- EN seed functions: " + function_preview(anchor.en_seed_functions))
        else:
            lines.append("- EN seed span: none")
        if anchor.en_xrefs:
            lines.append("- EN xrefs: " + ", ".join(f"`{item}`" for item in anchor.en_xrefs[:8]))
        if anchor.debug_split_path is not None:
            lines.append(
                f"- debug split: `{anchor.debug_split_path}` "
                f"`0x{anchor.debug_split_start:08X}-0x{anchor.debug_split_end:08X}` "
                f"size=`0x{anchor.debug_split_size:X}` functions=`{anchor.debug_split_function_count}`"
            )
            lines.append("- fit verdict: " + fit_summary(anchor))
            if anchor.split_prev_paths:
                lines.append("- debug split before: " + ", ".join(f"`{item}`" for item in anchor.split_prev_paths))
            if anchor.split_next_paths:
                lines.append("- debug split after: " + ", ".join(f"`{item}`" for item in anchor.split_next_paths))
        if anchor.srcfile_index is not None:
            lines.append(f"- srcfiles index: `{anchor.srcfile_index}`")
            if anchor.srcfile_prev:
                lines.append("- srcfiles before: " + ", ".join(f"`{item}`" for item in anchor.srcfile_prev))
            if anchor.srcfile_next:
                lines.append("- srcfiles after: " + ", ".join(f"`{item}`" for item in anchor.srcfile_next))
        elif anchor.srcfile_match_count > 1:
            lines.append(f"- srcfiles match: ambiguous (`{anchor.srcfile_match_count}` matches)")
        else:
            lines.append("- srcfiles match: none")
        if anchor.retail_labels:
            lines.append("- retail labels: " + ", ".join(f"`{item}`" for item in anchor.retail_labels))
        if anchor.retail_messages:
            lines.append("- retail messages: " + ", ".join(f"`{item}`" for item in anchor.retail_messages[:4]))
        if prev_corridor is not None:
            gap_text = "unknown"
            if prev_corridor.en_gap_size is not None:
                gap_text = f"`0x{prev_corridor.en_gap_size:X}`"
            lines.append(
                f"- previous corridor: `{prev_corridor.left.retail_source_name}` -> `{anchor.retail_source_name}` "
                f"intervening_srcfiles=`{prev_corridor.gap_path_count}` en_gap={gap_text}"
            )
            lines.append("  between: " + corridor_preview(prev_corridor))
            lines.append("  EN gap functions: " + function_preview(prev_corridor.gap_functions))
        if next_corridor is not None:
            gap_text = "unknown"
            if next_corridor.en_gap_size is not None:
                gap_text = f"`0x{next_corridor.en_gap_size:X}`"
            lines.append(
                f"- next corridor: `{anchor.retail_source_name}` -> `{next_corridor.right.retail_source_name}` "
                f"intervening_srcfiles=`{next_corridor.gap_path_count}` en_gap={gap_text}"
            )
            lines.append("  between: " + corridor_preview(next_corridor))
            lines.append("  EN gap functions: " + function_preview(next_corridor.gap_functions))
        lines.append("")
    return "\n".join(lines).rstrip()


def rows_to_csv(anchors: list[SourceAnchor]) -> str:
    fieldnames = [
        "retail_source_name",
        "suggested_path",
        "order_mode",
        "xref_count",
        "en_span_start",
        "en_span_end",
        "en_span_size",
        "en_function_count",
        "debug_split_path",
        "debug_split_start",
        "debug_split_end",
        "debug_split_size",
        "debug_split_function_count",
        "fit_status",
        "size_delta",
        "size_ratio",
        "srcfile_index",
        "srcfile_match_count",
        "split_prev_paths",
        "split_next_paths",
        "srcfile_prev",
        "srcfile_next",
        "retail_labels",
        "retail_messages",
        "en_xrefs",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for anchor in anchors:
        writer.writerow(
            {
                "retail_source_name": anchor.retail_source_name,
                "suggested_path": anchor.suggested_path,
                "order_mode": anchor.order_mode,
                "xref_count": anchor.xref_count,
                "en_span_start": "" if anchor.en_span_start is None else f"0x{anchor.en_span_start:08X}",
                "en_span_end": "" if anchor.en_span_end is None else f"0x{anchor.en_span_end:08X}",
                "en_span_size": "" if anchor.en_span_size is None else f"0x{anchor.en_span_size:X}",
                "en_function_count": anchor.en_function_count,
                "debug_split_path": anchor.debug_split_path or "",
                "debug_split_start": "" if anchor.debug_split_start is None else f"0x{anchor.debug_split_start:08X}",
                "debug_split_end": "" if anchor.debug_split_end is None else f"0x{anchor.debug_split_end:08X}",
                "debug_split_size": "" if anchor.debug_split_size is None else f"0x{anchor.debug_split_size:X}",
                "debug_split_function_count": anchor.debug_split_function_count,
                "fit_status": anchor.fit_status,
                "size_delta": "" if anchor.size_delta is None else f"{anchor.size_delta}",
                "size_ratio": "" if anchor.size_ratio is None else f"{anchor.size_ratio:.4f}",
                "srcfile_index": "" if anchor.srcfile_index is None else str(anchor.srcfile_index),
                "srcfile_match_count": anchor.srcfile_match_count,
                "split_prev_paths": ",".join(anchor.split_prev_paths),
                "split_next_paths": ",".join(anchor.split_next_paths),
                "srcfile_prev": ",".join(anchor.srcfile_prev),
                "srcfile_next": ",".join(anchor.srcfile_next),
                "retail_labels": ",".join(anchor.retail_labels),
                "retail_messages": ",".join(anchor.retail_messages),
                "en_xrefs": ",".join(anchor.en_xrefs),
            }
        )
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Bridge retail EN source anchors to debug-side file order and split-fit context."
    )
    parser.add_argument("--dol", type=Path, default=Path("orig/GSAE01/sys/main.dol"), help="Path to the retail EN main.dol.")
    parser.add_argument("--symbols", type=Path, default=Path("config/GSAE01/symbols.txt"), help="Current EN symbols.txt.")
    parser.add_argument("--debug-symbols", type=Path, default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/symbols.txt"), help="Debug-side symbols used for the retail source crosswalk.")
    parser.add_argument("--debug-splits", type=Path, default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/splits.txt"), help="Debug-side splits used for file-order and size context.")
    parser.add_argument("--debug-srcfiles", type=Path, default=Path("reference_projects/rena-tools/sfadebug/notes/srcfiles.txt"), help="Debug-side source inventory used for approximate source order.")
    parser.add_argument("--reference-configure", type=Path, default=Path("reference_projects/rena-tools/sfadebug/configure.py"), help="Reference configure.py mined only for side-path hints.")
    parser.add_argument("--reference-symbols", type=Path, default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/symbols.txt"), help="Reference symbols mined only for side-function hints.")
    parser.add_argument("--reference-inventory", type=Path, default=Path("reference_projects/rena-tools/sfadebug/notes/srcfiles.txt"), help="Reference inventory mined only for side-path hints.")
    parser.add_argument("--reference-dll-registry", type=Path, default=Path("reference_projects/rena-tools/StarFoxAdventures/data/KD/dlls.xml"), help="Reference DLL registry mined only for side-path hints.")
    parser.add_argument("--reference-object-xml", type=Path, nargs="*", default=(Path("reference_projects/rena-tools/StarFoxAdventures/data/U0/objects.xml"), Path("reference_projects/rena-tools/StarFoxAdventures/data/U0/objects2.xml")), help="Reference object XML files mined only for side-path hints.")
    parser.add_argument("--format", choices=("markdown", "csv"), default="markdown", help="Output format.")
    parser.add_argument("--search", nargs="+", help="Case-insensitive substring search across anchors, fit verdicts, and neighboring debug paths.")
    parser.add_argument("--limit", type=int, default=8, help="Maximum rows to show in summary sections.")
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
    reference_hints = collect_reference_hints(
        groups=groups,
        reference_configure=args.reference_configure,
        reference_symbols=args.reference_symbols,
        reference_inventory=args.reference_inventory,
        reference_dll_registry=args.reference_dll_registry,
        reference_object_xmls=tuple(args.reference_object_xml),
    )
    current_functions = load_function_symbols(args.symbols)
    debug_split_paths = list(parse_debug_split_text_ranges(args.debug_splits))
    srcfiles_entries = parse_source_inventory(args.debug_srcfiles)

    anchors = build_anchors(
        groups=groups,
        reference_hints=reference_hints,
        current_functions=current_functions,
        debug_split_paths=debug_split_paths,
        srcfiles_entries=srcfiles_entries,
    )
    corridors = build_corridors(anchors, srcfiles_entries, current_functions)

    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(anchors))
        elif args.search:
            sys.stdout.write(detailed_markdown(anchors, corridors, args.search))
            sys.stdout.write("\n")
        else:
            sys.stdout.write(summary_markdown(anchors, corridors, args.limit))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
