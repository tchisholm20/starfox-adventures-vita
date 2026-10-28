from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.orig.dol_xrefs import FunctionSymbol, load_function_symbols
from tools.orig.source_gap_windows import choose_segment_boundaries
from tools.orig.source_recovery import parse_debug_split_text_ranges


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Project a known debug-side file order onto a current EN address range."
    )
    parser.add_argument("--symbols", type=Path, default=Path("config/GSAE01/symbols.txt"))
    parser.add_argument(
        "--debug-splits",
        type=Path,
        default=Path("reference_projects/rena-tools/sfadebug/config/GSAP01-DEBUG/splits.txt"),
    )
    parser.add_argument("--current-start", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--current-end", type=lambda x: int(x, 0), required=True)
    parser.add_argument(
        "--path",
        action="append",
        dest="paths",
        required=True,
        help="Debug split path to include, in order. Repeat for each file.",
    )
    parser.add_argument(
        "--enumerate-subsets",
        action="store_true",
        help="Rank contiguous path subsets inside the provided ordered path list.",
    )
    parser.add_argument(
        "--min-paths",
        type=int,
        default=2,
        help="Minimum contiguous subset size when using --enumerate-subsets.",
    )
    parser.add_argument(
        "--max-results",
        type=int,
        default=12,
        help="Maximum ranked subset results to print when using --enumerate-subsets.",
    )
    return parser


def classify_ratio(ratio: float) -> str:
    if 0.75 <= ratio <= 1.5:
        return "near-fit"
    if 0.5 <= ratio <= 2.0:
        return "plausible"
    return "stretched"


def ratio_penalty(ratio: float) -> float:
    if ratio <= 0.0:
        return math.inf
    return abs(math.log2(ratio))


def project_interval(
    current_functions: list[FunctionSymbol],
    current_start: int,
    current_end: int,
    debug_split_ranges: dict[str, tuple[int, int]],
    paths: list[str],
) -> tuple[float, list[tuple[str, int, int, int]]] | None:
    functions = [function for function in current_functions if current_start <= function.address < current_end]
    if not functions:
        return None

    current_sizes = [functions[i + 1].address - functions[i].address for i in range(len(functions) - 1)]
    current_sizes.append(current_end - functions[-1].address)

    current_cumulative: list[int] = []
    total = 0
    for size in current_sizes:
        total += size
        current_cumulative.append(total)

    debug_sizes: list[int] = []
    debug_total = 0
    for path in paths:
        start, end = debug_split_ranges[path]
        size = end - start
        debug_sizes.append(size)
        debug_total += size

    scale_ratio = (current_end - current_start) / debug_total
    target_cumulative: list[float] = []
    scaled_total = 0.0
    for size in debug_sizes:
        scaled_total += size * scale_ratio
        target_cumulative.append(scaled_total)

    boundaries = choose_segment_boundaries(current_cumulative, target_cumulative)
    if boundaries is None:
        return None

    projection: list[tuple[str, int, int, int]] = []
    previous = 0
    for path, boundary, debug_size in zip(paths, boundaries, debug_sizes):
        start = functions[previous].address
        end = functions[boundary + 1].address if boundary + 1 < len(functions) else current_end
        projection.append((path, start, end, debug_size))
        previous = boundary + 1
    return scale_ratio, projection


def subset_projection_score(scale_ratio: float, projection: list[tuple[str, int, int, int]]) -> tuple[float, int, int]:
    penalties = [ratio_penalty(scale_ratio)]
    near_fit = 0
    plausible = 0
    for _, start, end, debug_size in projection:
        current_size = end - start
        file_ratio = current_size / debug_size if debug_size else 0.0
        verdict = classify_ratio(file_ratio)
        penalties.append(ratio_penalty(file_ratio))
        if verdict == "near-fit":
            near_fit += 1
        elif verdict == "plausible":
            plausible += 1
    return sum(penalties), near_fit, plausible


def enumerate_subset_projections(
    current_functions: list[FunctionSymbol],
    current_start: int,
    current_end: int,
    debug_split_ranges: dict[str, tuple[int, int]],
    paths: list[str],
    min_paths: int,
) -> list[tuple[float, int, int, float, list[tuple[str, int, int, int]]]]:
    results: list[tuple[float, int, int, float, list[tuple[str, int, int, int]]]] = []
    for start_index in range(len(paths)):
        for end_index in range(start_index + min_paths, len(paths) + 1):
            subset = paths[start_index:end_index]
            result = project_interval(
                current_functions=current_functions,
                current_start=current_start,
                current_end=current_end,
                debug_split_ranges=debug_split_ranges,
                paths=subset,
            )
            if result is None:
                continue
            scale_ratio, projection = result
            score, near_fit, plausible = subset_projection_score(scale_ratio, projection)
            results.append((score, near_fit, plausible, scale_ratio, projection))
    results.sort(
        key=lambda item: (
            item[0],
            -item[1],
            -item[2],
            -len(item[4]),
            item[4][0][1],
            item[4][-1][2],
        )
    )
    return results


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()

    current_functions = load_function_symbols(args.symbols)
    debug_split_ranges = parse_debug_split_text_ranges(args.debug_splits)
    normalized_paths = [path.replace("\\", "/") for path in args.paths]

    missing = [path for path in normalized_paths if path not in debug_split_ranges]
    if missing:
        parser.error("unknown debug split path(s): " + ", ".join(missing))

    if args.enumerate_subsets:
        results = enumerate_subset_projections(
            current_functions=current_functions,
            current_start=args.current_start,
            current_end=args.current_end,
            debug_split_ranges=debug_split_ranges,
            paths=normalized_paths,
            min_paths=max(1, args.min_paths),
        )
        if not results:
            parser.error("could not project any contiguous subset onto current EN functions")

        print("# Source interval subset projections")
        print(
            f"- current range: `0x{args.current_start:08X}-0x{args.current_end:08X}` "
            f"size=`0x{args.current_end - args.current_start:X}`"
        )
        print(f"- source path count: `{len(normalized_paths)}`")
        print(f"- subset minimum paths: `{max(1, args.min_paths)}`")
        print(f"- ranked results shown: `{min(args.max_results, len(results))}`")
        for rank, (score, near_fit, plausible, scale_ratio, projection) in enumerate(
            results[: max(1, args.max_results)],
            start=1,
        ):
            subset_paths = [path for path, _, _, _ in projection]
            print(
                f"- rank {rank}: score=`{score:.3f}` overall=`{scale_ratio:.3f}x` "
                f"verdict=`{classify_ratio(scale_ratio)}` near-fit=`{near_fit}` plausible=`{plausible}` "
                f"subset=`{subset_paths[0]}` -> `{subset_paths[-1]}` paths=`{len(subset_paths)}`"
            )
            for path, start, end, debug_size in projection:
                current_size = end - start
                file_ratio = current_size / debug_size if debug_size else 0.0
                print(
                    f"  - `{path}` `0x{start:08X}-0x{end:08X}` "
                    f"size=`0x{current_size:X}` debug=`0x{debug_size:X}` "
                    f"ratio=`{file_ratio:.3f}x` verdict=`{classify_ratio(file_ratio)}`"
                )
        return

    result = project_interval(
        current_functions=current_functions,
        current_start=args.current_start,
        current_end=args.current_end,
        debug_split_ranges=debug_split_ranges,
        paths=normalized_paths,
    )
    if result is None:
        parser.error("could not project interval onto current EN functions")

    scale_ratio, projection = result
    debug_total = sum(debug_size for _, _, _, debug_size in projection)

    print("# Source interval projection")
    print(
        f"- current range: `0x{args.current_start:08X}-0x{args.current_end:08X}` "
        f"size=`0x{args.current_end - args.current_start:X}`"
    )
    print(f"- debug total size: `0x{debug_total:X}`")
    print(
        f"- scale ratio: `{scale_ratio:.3f}x`"
        f" verdict=`{classify_ratio(scale_ratio)}`"
    )
    print("- projected windows:")
    for path, start, end, debug_size in projection:
        current_size = end - start
        file_ratio = current_size / debug_size if debug_size else 0.0
        print(
            f"  - `{path}` `0x{start:08X}-0x{end:08X}` "
            f"size=`0x{current_size:X}` debug=`0x{debug_size:X}` "
            f"ratio=`{file_ratio:.3f}x` verdict=`{classify_ratio(file_ratio)}`"
        )


if __name__ == "__main__":
    main()
