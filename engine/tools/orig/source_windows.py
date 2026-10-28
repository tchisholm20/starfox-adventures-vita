from __future__ import annotations

import argparse
import bisect
import csv
import io
import sys
from dataclasses import dataclass
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.orig.dol_xrefs import FunctionSymbol, load_function_symbols
from tools.orig.source_boundaries import unique_xref_functions
from tools.orig.source_corridors import SourceAnchor, SourceCorridor, build_anchors, build_corridors, format_symbol_span
from tools.orig.source_reference_hints import build_groups, collect_reference_hints, parse_source_inventory
from tools.orig.source_recovery import parse_debug_split_text_ranges


@dataclass(frozen=True)
class WindowCandidate:
    start_index: int
    end_index: int
    start_address: int
    end_address: int
    size: int
    size_delta: int
    size_ratio: float
    covered_xref_functions: tuple[FunctionSymbol, ...]
    score: int

    @property
    def covered_xref_count(self) -> int:
        return len(self.covered_xref_functions)

    @property
    def function_count(self) -> int:
        return self.end_index - self.start_index + 1


@dataclass(frozen=True)
class SourceWindowEstimate:
    anchor: SourceAnchor
    xref_functions: tuple[FunctionSymbol, ...]
    total_xref_count: int
    best_candidate: WindowCandidate
    candidate_windows: tuple[WindowCandidate, ...]
    search_start_index: int
    search_end_index: int
    search_start_address: int
    search_end_address: int
    search_size: int
    previous_anchor_name: str | None
    next_anchor_name: str | None
    previous_gap_paths: tuple[str, ...]
    next_gap_paths: tuple[str, ...]
    bounded_left: bool
    bounded_right: bool
    window_mode: str
    confidence: str


def interval_size(current_functions: list[FunctionSymbol], start_index: int, end_index: int) -> int:
    return current_functions[end_index].address + current_functions[end_index].size - current_functions[start_index].address


def interval_addresses(current_functions: list[FunctionSymbol], start_index: int, end_index: int) -> tuple[int, int]:
    return current_functions[start_index].address, current_functions[end_index].address + current_functions[end_index].size


def candidate_score(
    covered_count: int,
    total_xref_count: int,
    size_delta: int,
    left_extra_bytes: int,
    right_extra_bytes: int,
    function_count: int,
    target_size: int,
) -> int:
    coverage_bonus = covered_count * min(max(target_size // 4, 0x40), 0x200)
    complete_bonus = 0x100 if covered_count == total_xref_count else 0
    balance_penalty = abs(left_extra_bytes - right_extra_bytes) // 8
    function_penalty = function_count * 4
    return abs(size_delta) * 4 - coverage_bonus - complete_bonus + balance_penalty + function_penalty


def search_lower_bound(
    current_functions: list[FunctionSymbol],
    xref_end_index: int,
    max_span: int,
) -> int:
    start_index = xref_end_index
    while start_index > 0 and interval_size(current_functions, start_index - 1, xref_end_index) <= max_span:
        start_index -= 1
    return start_index


def search_upper_bound(
    current_functions: list[FunctionSymbol],
    xref_start_index: int,
    max_span: int,
) -> int:
    end_index = xref_start_index
    last_index = len(current_functions) - 1
    while end_index < last_index and interval_size(current_functions, xref_start_index, end_index + 1) <= max_span:
        end_index += 1
    return end_index


def source_order_neighbors(anchors: list[SourceAnchor]) -> dict[str, tuple[SourceAnchor | None, SourceAnchor | None]]:
    ordered = [
        anchor
        for anchor in anchors
        if anchor.srcfile_index is not None
        and anchor.function_start_index is not None
        and anchor.function_end_index is not None
    ]
    ordered.sort(
        key=lambda item: (
            item.srcfile_index if item.srcfile_index is not None else 0,
            item.en_span_start if item.en_span_start is not None else 0,
            item.retail_source_name.lower(),
        )
    )

    neighbors: dict[str, tuple[SourceAnchor | None, SourceAnchor | None]] = {}
    for index, anchor in enumerate(ordered):
        previous = ordered[index - 1] if index > 0 else None
        following = ordered[index + 1] if index + 1 < len(ordered) else None
        neighbors[anchor.retail_source_name.lower()] = (previous, following)
    return neighbors


def corridor_neighbors(corridors: list[SourceCorridor]) -> tuple[dict[str, SourceCorridor], dict[str, SourceCorridor]]:
    previous_by_name: dict[str, SourceCorridor] = {}
    next_by_name: dict[str, SourceCorridor] = {}
    for corridor in corridors:
        previous_by_name[corridor.right.retail_source_name.lower()] = corridor
        next_by_name[corridor.left.retail_source_name.lower()] = corridor
    return previous_by_name, next_by_name


def covered_xref_functions(
    xref_functions: tuple[FunctionSymbol, ...],
    start_index: int,
    end_index: int,
    index_by_address: dict[int, int],
) -> tuple[FunctionSymbol, ...]:
    values = [
        function
        for function in xref_functions
        if start_index <= index_by_address[function.address] <= end_index
    ]
    return tuple(values)


def build_candidate_windows(
    current_functions: list[FunctionSymbol],
    xref_functions: tuple[FunctionSymbol, ...],
    search_start_index: int,
    search_end_index: int,
    target_size: int,
    candidate_limit: int,
) -> tuple[WindowCandidate, ...]:
    index_by_address = {function.address: index for index, function in enumerate(current_functions)}
    xref_indices = sorted(index_by_address[function.address] for function in xref_functions)
    xref_end_addresses = [current_functions[index].address + current_functions[index].size for index in range(len(current_functions))]
    search_start_min = min(xref_indices)
    search_end_max = max(xref_indices)

    candidates_by_interval: dict[tuple[int, int], WindowCandidate] = {}
    for start_index in range(search_start_index, search_end_max + 1):
        base_address = current_functions[start_index].address
        target_end_address = base_address + target_size

        end_index = bisect.bisect_left(xref_end_addresses, target_end_address, lo=max(search_end_max, start_index), hi=search_end_index + 1)
        for candidate_end in (end_index - 1, end_index):
            if candidate_end < start_index or candidate_end < search_start_min or candidate_end > search_end_index:
                continue

            covered = covered_xref_functions(xref_functions, start_index, candidate_end, index_by_address)
            if not covered:
                continue

            start_address, end_address = interval_addresses(current_functions, start_index, candidate_end)
            size = end_address - start_address
            size_delta = size - target_size
            left_extra = current_functions[min(index_by_address[item.address] for item in covered)].address - start_address
            rightmost = max(index_by_address[item.address] for item in covered)
            right_extra = end_address - (current_functions[rightmost].address + current_functions[rightmost].size)
            score = candidate_score(
                covered_count=len(covered),
                total_xref_count=len(xref_functions),
                size_delta=size_delta,
                left_extra_bytes=left_extra,
                right_extra_bytes=right_extra,
                function_count=candidate_end - start_index + 1,
                target_size=target_size,
            )
            key = (start_index, candidate_end)
            candidates_by_interval[key] = WindowCandidate(
                start_index=start_index,
                end_index=candidate_end,
                start_address=start_address,
                end_address=end_address,
                size=size,
                size_delta=size_delta,
                size_ratio=size / target_size,
                covered_xref_functions=covered,
                score=score,
            )

    candidates = sorted(
        candidates_by_interval.values(),
        key=lambda item: (
            item.score,
            -item.covered_xref_count,
            abs(item.size_delta),
            item.function_count,
            item.start_address,
        )
    )
    return tuple(candidates[:candidate_limit])


def estimate_confidence(
    best_candidate: WindowCandidate,
    total_xref_count: int,
    target_size: int,
    search_size: int,
    window_mode: str,
) -> str:
    size_error_ratio = abs(best_candidate.size_delta) / max(target_size, 1)
    coverage_ratio = best_candidate.covered_xref_count / max(total_xref_count, 1)
    search_ratio = search_size / max(target_size, 1)

    if coverage_ratio >= 1.0 and size_error_ratio <= 0.10:
        if window_mode == "near-seed":
            return "high"
        if search_ratio <= 7.0 and best_candidate.function_count <= 12:
            return "high"
        return "medium"
    if size_error_ratio <= 0.25 and coverage_ratio >= 1.0 and search_ratio <= 12.0:
        return "medium"
    if size_error_ratio <= 0.35 and coverage_ratio >= 0.5:
        return "medium"
    return "low"


def estimate_window_mode(anchor: SourceAnchor, best_candidate: WindowCandidate) -> str:
    assert anchor.en_span_size is not None
    if best_candidate.size + 0x40 < anchor.en_span_size:
        return "shrink"
    if best_candidate.size > anchor.en_span_size + 0x40:
        return "expand"
    return "near-seed"


def build_window_estimates(
    groups,
    anchors: list[SourceAnchor],
    corridors: list[SourceCorridor],
    current_functions: list[FunctionSymbol],
    candidate_limit: int,
) -> list[SourceWindowEstimate]:
    group_by_name = {group.retail_source_name.lower(): group for group in groups}
    function_by_address = {function.address: function for function in current_functions}
    index_by_address = {function.address: index for index, function in enumerate(current_functions)}
    neighbor_map = source_order_neighbors(anchors)
    previous_corridor_by_name, next_corridor_by_name = corridor_neighbors(corridors)

    estimates: list[SourceWindowEstimate] = []
    for anchor in anchors:
        if anchor.debug_split_size is None:
            continue
        group = group_by_name.get(anchor.retail_source_name.lower())
        if group is None:
            continue
        xrefs = unique_xref_functions(group, function_by_address)
        if not xrefs:
            continue

        xref_indices = [index_by_address[function.address] for function in xrefs]
        xref_start_index = min(xref_indices)
        xref_end_index = max(xref_indices)
        previous_anchor, next_anchor = neighbor_map.get(anchor.retail_source_name.lower(), (None, None))
        previous_corridor = previous_corridor_by_name.get(anchor.retail_source_name.lower())
        next_corridor = next_corridor_by_name.get(anchor.retail_source_name.lower())

        if anchor.en_span_size is not None and anchor.debug_split_size <= anchor.en_span_size:
            search_start_index = xref_start_index
            search_end_index = xref_end_index
        else:
            max_search_span = max(anchor.debug_split_size * 3, (anchor.en_span_size or 0) * 2, 0x2000)
            search_start_index = search_lower_bound(current_functions, xref_end_index, max_search_span)
            search_end_index = search_upper_bound(current_functions, xref_start_index, max_search_span)

        bounded_left = previous_anchor is not None and previous_anchor.function_end_index is not None
        bounded_right = next_anchor is not None and next_anchor.function_start_index is not None
        if bounded_left:
            assert previous_anchor is not None
            assert previous_anchor.function_end_index is not None
            search_start_index = max(search_start_index, previous_anchor.function_end_index + 1)
        if bounded_right:
            assert next_anchor is not None
            assert next_anchor.function_start_index is not None
            search_end_index = min(search_end_index, next_anchor.function_start_index - 1)

        if search_start_index > search_end_index:
            search_start_index = xref_start_index
            search_end_index = xref_end_index
            bounded_left = False
            bounded_right = False

        search_start_address, search_end_address = interval_addresses(current_functions, search_start_index, search_end_index)
        candidates = build_candidate_windows(
            current_functions=current_functions,
            xref_functions=xrefs,
            search_start_index=search_start_index,
            search_end_index=search_end_index,
            target_size=anchor.debug_split_size,
            candidate_limit=candidate_limit,
        )
        if not candidates:
            continue
        best = candidates[0]
        search_size = search_end_address - search_start_address

        window_mode = estimate_window_mode(anchor, best)

        estimates.append(
            SourceWindowEstimate(
                anchor=anchor,
                xref_functions=xrefs,
                total_xref_count=len(xrefs),
                best_candidate=best,
                candidate_windows=candidates,
                search_start_index=search_start_index,
                search_end_index=search_end_index,
                search_start_address=search_start_address,
                search_end_address=search_end_address,
                search_size=search_size,
                previous_anchor_name=None if previous_anchor is None else previous_anchor.retail_source_name,
                next_anchor_name=None if next_anchor is None else next_anchor.retail_source_name,
                previous_gap_paths=() if previous_corridor is None else previous_corridor.srcfile_gap_paths,
                next_gap_paths=() if next_corridor is None else next_corridor.srcfile_gap_paths,
                bounded_left=bounded_left,
                bounded_right=bounded_right,
                window_mode=window_mode,
                confidence=estimate_confidence(best, len(xrefs), anchor.debug_split_size, search_size, window_mode),
            )
        )

    estimates.sort(
        key=lambda item: (
            {"high": 0, "medium": 1, "low": 2}[item.confidence],
            item.best_candidate.score,
            item.anchor.retail_source_name.lower(),
        )
    )
    return estimates


def candidate_summary(candidate: WindowCandidate, total_xref_count: int) -> str:
    sign = "+" if candidate.size_delta >= 0 else "-"
    return (
        f"`0x{candidate.start_address:08X}-0x{candidate.end_address:08X}` "
        f"size=`0x{candidate.size:X}` "
        f"delta=`{sign}0x{abs(candidate.size_delta):X}` "
        f"xrefs=`{candidate.covered_xref_count}/{total_xref_count}` "
        f"functions=`{candidate.function_count}`"
    )


def gap_preview(paths: tuple[str, ...], limit: int = 6) -> str:
    if not paths:
        return "none"
    preview = ", ".join(f"`{path}`" for path in paths[:limit])
    if len(paths) > limit:
        preview += f", ... (+{len(paths) - limit} more)"
    return preview


def xref_preview(functions: tuple[FunctionSymbol, ...], limit: int = 6) -> str:
    preview = ", ".join(f"`{format_symbol_span(function)}`" for function in functions[:limit])
    if len(functions) > limit:
        preview += f", ... (+{len(functions) - limit} more)"
    return preview


def summary_markdown(estimates: list[SourceWindowEstimate], limit: int) -> str:
    high = [item for item in estimates if item.confidence == "high"]
    shrink = [item for item in estimates if item.window_mode == "shrink"]
    expand = [item for item in estimates if item.window_mode == "expand"]

    lines: list[str] = []
    lines.append("# Retail source window candidates")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- Exact debug-split-backed anchors with EN xrefs: `{len(estimates)}`")
    lines.append(f"- High-confidence best-fit windows: `{len(high)}`")
    lines.append(f"- Best-fit shrink cases: `{len(shrink)}`")
    lines.append(f"- Best-fit expansion cases: `{len(expand)}`")
    lines.append("")

    lines.append("## Highest-confidence windows")
    if high:
        for estimate in high[:limit]:
            lines.append(
                f"- `{estimate.anchor.retail_source_name}` -> `{estimate.anchor.suggested_path}` "
                f"target=`0x{estimate.anchor.debug_split_size:X}` "
                f"best={candidate_summary(estimate.best_candidate, estimate.total_xref_count)}"
            )
            lines.append(
                f"  search envelope: `0x{estimate.search_start_address:08X}-0x{estimate.search_end_address:08X}` "
                f"size=`0x{estimate.search_size:X}` bounded_left=`{estimate.bounded_left}` bounded_right=`{estimate.bounded_right}`"
            )
    else:
        lines.append("- None")
    lines.append("")

    lines.append("## Strong Shrink Signals")
    if shrink:
        for estimate in shrink[:limit]:
            lines.append(
                f"- `{estimate.anchor.retail_source_name}` current_seed=`0x{estimate.anchor.en_span_size:X}` "
                f"target=`0x{estimate.anchor.debug_split_size:X}` best={candidate_summary(estimate.best_candidate, estimate.total_xref_count)}"
            )
            if estimate.next_gap_paths:
                lines.append("  next corridor: " + gap_preview(estimate.next_gap_paths))
    else:
        lines.append("- None")
    lines.append("")

    lines.append("## Strong Expansion Signals")
    if expand:
        for estimate in expand[:limit]:
            lines.append(
                f"- `{estimate.anchor.retail_source_name}` current_seed=`0x{estimate.anchor.en_span_size:X}` "
                f"target=`0x{estimate.anchor.debug_split_size:X}` best={candidate_summary(estimate.best_candidate, estimate.total_xref_count)}"
            )
            if estimate.previous_gap_paths:
                lines.append("  previous corridor: " + gap_preview(estimate.previous_gap_paths))
            if estimate.next_gap_paths:
                lines.append("  next corridor: " + gap_preview(estimate.next_gap_paths))
    else:
        lines.append("- None")
    lines.append("")

    lines.append("## Usage")
    lines.append("- Summary: `python tools/orig/source_windows.py`")
    lines.append("- Inspect one source: `python tools/orig/source_windows.py --search objanim`")
    lines.append("- CSV dump: `python tools/orig/source_windows.py --format csv`")
    return "\n".join(lines)


def detailed_markdown(estimates: list[SourceWindowEstimate], patterns: list[str]) -> str:
    lowered = [pattern.lower() for pattern in patterns]
    visible: list[SourceWindowEstimate] = []
    for estimate in estimates:
        fields = [
            estimate.anchor.retail_source_name.lower(),
            estimate.anchor.suggested_path.lower(),
            estimate.confidence.lower(),
            estimate.window_mode.lower(),
        ]
        fields.extend(function.name.lower() for function in estimate.xref_functions)
        fields.extend(path.lower() for path in estimate.previous_gap_paths)
        fields.extend(path.lower() for path in estimate.next_gap_paths)
        if any(any(pattern in field for field in fields) for pattern in lowered):
            visible.append(estimate)

    lines = ["# Retail source window search", ""]
    if not visible:
        lines.append("- No matching estimates.")
        return "\n".join(lines)

    for estimate in visible:
        lines.append(f"## `{estimate.anchor.retail_source_name}`")
        lines.append(f"- suggested path: `{estimate.anchor.suggested_path}`")
        lines.append(f"- confidence: `{estimate.confidence}`")
        lines.append(f"- window mode: `{estimate.window_mode}`")
        lines.append(
            f"- target debug size: `0x{estimate.anchor.debug_split_size:X}` "
            f"current seed: `0x{estimate.anchor.en_span_size:X}`"
        )
        lines.append(
            f"- search envelope: `0x{estimate.search_start_address:08X}-0x{estimate.search_end_address:08X}` "
            f"size=`0x{estimate.search_size:X}` bounded_left=`{estimate.bounded_left}` bounded_right=`{estimate.bounded_right}`"
        )
        lines.append(f"- xref functions: {xref_preview(estimate.xref_functions)}")
        if estimate.previous_anchor_name is not None:
            lines.append(f"- previous ordered anchor: `{estimate.previous_anchor_name}`")
        if estimate.previous_gap_paths:
            lines.append("- previous corridor files: " + gap_preview(estimate.previous_gap_paths))
        if estimate.next_anchor_name is not None:
            lines.append(f"- next ordered anchor: `{estimate.next_anchor_name}`")
        if estimate.next_gap_paths:
            lines.append("- next corridor files: " + gap_preview(estimate.next_gap_paths))
        lines.append("- top candidates:")
        for candidate in estimate.candidate_windows:
            lines.append(f"  - {candidate_summary(candidate, estimate.total_xref_count)}")
            lines.append("    xref functions: " + xref_preview(candidate.covered_xref_functions))
        lines.append("")
    return "\n".join(lines).rstrip()


def rows_to_csv(estimates: list[SourceWindowEstimate]) -> str:
    fieldnames = [
        "retail_source_name",
        "suggested_path",
        "confidence",
        "window_mode",
        "target_debug_size",
        "current_seed_size",
        "search_start",
        "search_end",
        "search_size",
        "best_start",
        "best_end",
        "best_size",
        "best_size_delta",
        "best_size_ratio",
        "best_xref_coverage",
        "best_function_count",
        "previous_anchor",
        "next_anchor",
        "previous_gap_paths",
        "next_gap_paths",
    ]
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    for estimate in estimates:
        writer.writerow(
            {
                "retail_source_name": estimate.anchor.retail_source_name,
                "suggested_path": estimate.anchor.suggested_path,
                "confidence": estimate.confidence,
                "window_mode": estimate.window_mode,
                "target_debug_size": f"0x{estimate.anchor.debug_split_size:X}",
                "current_seed_size": "" if estimate.anchor.en_span_size is None else f"0x{estimate.anchor.en_span_size:X}",
                "search_start": f"0x{estimate.search_start_address:08X}",
                "search_end": f"0x{estimate.search_end_address:08X}",
                "search_size": f"0x{estimate.search_size:X}",
                "best_start": f"0x{estimate.best_candidate.start_address:08X}",
                "best_end": f"0x{estimate.best_candidate.end_address:08X}",
                "best_size": f"0x{estimate.best_candidate.size:X}",
                "best_size_delta": str(estimate.best_candidate.size_delta),
                "best_size_ratio": f"{estimate.best_candidate.size_ratio:.4f}",
                "best_xref_coverage": f"{estimate.best_candidate.covered_xref_count}/{estimate.total_xref_count}",
                "best_function_count": estimate.best_candidate.function_count,
                "previous_anchor": estimate.previous_anchor_name or "",
                "next_anchor": estimate.next_anchor_name or "",
                "previous_gap_paths": ",".join(estimate.previous_gap_paths),
                "next_gap_paths": ",".join(estimate.next_gap_paths),
            }
        )
    return buffer.getvalue()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Estimate plausible EN source windows from retail source anchors and debug split sizes."
    )
    parser.add_argument("--dol", type=Path, default=Path("orig/GSAE01/sys/main.dol"), help="Path to the retail EN main.dol.")
    parser.add_argument("--symbols", type=Path, default=Path("config/GSAE01/symbols.txt"), help="Current EN symbols.txt.")
    parser.add_argument("--debug-symbols", type=Path, default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/symbols.txt"), help="Debug-side symbols used for the retail source crosswalk.")
    parser.add_argument("--debug-splits", type=Path, default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/splits.txt"), help="Debug-side splits used for exact split-size context.")
    parser.add_argument("--debug-srcfiles", type=Path, default=Path("reference_projects/rena-tools/sfadebug/notes/srcfiles.txt"), help="Debug-side source inventory used for source-order context.")
    parser.add_argument("--reference-configure", type=Path, default=Path("reference_projects/rena-tools/sfadebug/configure.py"), help="Reference configure.py mined only for side-path hints.")
    parser.add_argument("--reference-symbols", type=Path, default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/symbols.txt"), help="Reference symbols mined only for side-function hints.")
    parser.add_argument("--reference-inventory", type=Path, default=Path("reference_projects/rena-tools/sfadebug/notes/srcfiles.txt"), help="Reference inventory mined only for side-path hints.")
    parser.add_argument("--reference-dll-registry", type=Path, default=Path("reference_projects/rena-tools/StarFoxAdventures/data/KD/dlls.xml"), help="Reference DLL registry mined only for side-path hints.")
    parser.add_argument("--reference-object-xml", type=Path, nargs="*", default=(Path("reference_projects/rena-tools/StarFoxAdventures/data/U0/objects.xml"), Path("reference_projects/rena-tools/StarFoxAdventures/data/U0/objects2.xml")), help="Reference object XML files mined only for side-path hints.")
    parser.add_argument("--format", choices=("markdown", "csv"), default="markdown", help="Output format.")
    parser.add_argument("--search", nargs="+", help="Case-insensitive substring search across sources, confidence, and corridor neighbors.")
    parser.add_argument("--limit", type=int, default=6, help="Maximum rows to show in summary sections.")
    parser.add_argument("--candidate-limit", type=int, default=5, help="Maximum ranked candidate windows to keep per source.")
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
    estimates = build_window_estimates(
        groups=groups,
        anchors=anchors,
        corridors=corridors,
        current_functions=current_functions,
        candidate_limit=args.candidate_limit,
    )

    try:
        if args.format == "csv":
            sys.stdout.write(rows_to_csv(estimates))
        elif args.search:
            sys.stdout.write(detailed_markdown(estimates, args.search))
            sys.stdout.write("\n")
        else:
            sys.stdout.write(summary_markdown(estimates, args.limit))
            sys.stdout.write("\n")
    except BrokenPipeError:
        pass


if __name__ == "__main__":
    main()
