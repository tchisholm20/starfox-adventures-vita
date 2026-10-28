# Retail Source Materialization

This pass turns the `orig/` audit work into concrete stub targets under `src/` without touching the active build configuration, while keeping literal recovered source/header artifacts out of the committable source tree by default.

## Tool

- `python tools/orig/source_materialize.py`
  - Reports literal source/header artifacts preserved on disc in the manifest, including hashes and whether the file still carries Rare copyright / machine-generated notices.
  - Generates non-built `.c` stubs for EN `main.dol` source-recovery candidates directly into `src/`.
  - Can also promote weak EN candidates when the same source tag repeats across multiple bundled retail versions.
  - Writes a machine-readable manifest to [source_materialize.json](/C:/Projects/SFA-Decomp/docs/orig/source_materialize.json).
  - Treats previously generated stubs as managed outputs, so reruns can refresh them in place while still refusing to overwrite unrelated real sources.
  - Only exports exact disc artifacts when explicitly asked, and refuses to place those exports anywhere under `src/`.

## What It Produces

The current run produces two different classes of output.

### 1. Exact disc artifacts

These are reported straight from `orig/GSAE01` with recommended export paths, not copied into `src/`:

- `files/Boot/{English,French,German,Italian,Spanish}.c.new`
- `files/gametext/Boot/{English,French,German,Italian,Japanese,Spanish}.c.new`
- `files/audio/starfox.h.bak`

These are the strongest possible recovery wins because they are not inferred. They are also archival evidence, not decomp source. Several of the boot `.c.new` files still carry the original Rare copyright block verbatim, so the default workflow keeps them out of `src/`.

### 2. Retail-backed source stubs

These files are generated placeholders, not source-truth. Each one carries:

- the retail EN `main.dol` string address
- the exact retail string text
- any retail-authored function label extracted from that string
- current EN xrefs resolved through `config/GSAE01/symbols.txt`
- cross-version bundle evidence when PAL, JP, or EN rev1 preserve the same source tag
- debug-side path/function hints kept clearly separate from retail evidence

Current generated stubs:

- [camcontrol.c](/C:/Projects/SFA-Decomp/src/dll/CAM/camcontrol.c)
- [curves.c](/C:/Projects/SFA-Decomp/src/dll/curves.c)
- [laser.c](/C:/Projects/SFA-Decomp/src/dll/CF/laser.c)
- [DIMboss.c](/C:/Projects/SFA-Decomp/src/dll/DIM/DIMboss.c)
- [SHthorntail.c](/C:/Projects/SFA-Decomp/src/dll/SH/SHthorntail.c)
- [objanim.c](/C:/Projects/SFA-Decomp/src/main/objanim.c)
- [expgfx.c](/C:/Projects/SFA-Decomp/src/expgfx.c)
- [n_attractmode.c](/C:/Projects/SFA-Decomp/src/n_attractmode.c)
- [objHitReact.c](/C:/Projects/SFA-Decomp/src/objHitReact.c)
- [textblock.c](/C:/Projects/SFA-Decomp/src/textblock.c)

The root-level outputs are deliberate. Retail evidence was strong enough to justify a file, but not strong enough to justify a directory assignment yet, so the materializer keeps those stubs at `src/<basename>` instead of inventing a synthetic folder.

Two immediate examples of why this matters:

- [objanim.c](/C:/Projects/SFA-Decomp/src/main/objanim.c) now carries both the retail label `setBlendMove` and the debug-side bridge `Object_ObjAnimSetMove`.
- [textblock.c](/C:/Projects/SFA-Decomp/src/textblock.c) now materializes a concrete `Init` placeholder from the retail string even without any usable debug-side names.
- [curves.c](/C:/Projects/SFA-Decomp/src/dll/curves.c) now records the JP-only alias `hcurves.c` next to the shared `MAX_ROMCURVES exceeded!!` warning.
- [n_attractmode.c](/C:/Projects/SFA-Decomp/src/n_attractmode.c) is now present because the same weak source tag repeats in EN v1.0, EN rev1, PAL, and JP.

## Skips That Matter

- `dvdfs.c` was not materialized as a stub because the active tree already has [dvdfs.c](/C:/Projects/SFA-Decomp/src/dvd/dvdfs.c).

## Practical Use

- Regenerate the stub set and manifest:
  - `python tools/orig/source_materialize.py`
- Also export the direct artifacts for local inspection outside `src/`:
  - `python tools/orig/source_materialize.py --export-direct-artifacts`
- Export the direct artifacts to a custom non-source folder:
  - `python tools/orig/source_materialize.py --export-direct-artifacts --artifact-output-root temp/orig/recovered_source`
- Also include debug-path-only files that do not yet have an EN xref:
  - `python tools/orig/source_materialize.py --include-debug-path-only`
- Also include weak EN candidates that are repeated across multiple bundled retail versions:
  - `python tools/orig/source_materialize.py --include-cross-version-weak`

The intent is not to declare these files solved. The intent is to reduce the activation energy between "retail evidence exists" and "there is a concrete file in-tree someone can start recovering right now," without turning literal recovered Rare source files into normal `src/` content.
