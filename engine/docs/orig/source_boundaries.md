# Retail source-boundary hints

This pass closes a gap in the existing `orig/` tooling.

`source_recovery.py` tells us which retail source-tag strings still survive and where their EN xrefs land. What it did not answer directly was the split-planning question: are those EN functions already covered by `config/GSAE01/splits.txt`, what text span do they imply, and what is the best current path hint for turning them into a real source skeleton?

## Tool

- `python tools/orig/source_boundaries.py`
  - Starts from the existing retail EN source-tag crosswalk.
  - Resolves each source group to current EN function spans using `config/GSAE01/symbols.txt`.
  - Checks whether those spans are already owned by `config/GSAE01/splits.txt` or still live in an unsplit gap.
  - Pulls in the best current path hint from `sfadebug` / `reference_projects/rena-tools`.
  - Adds neighboring EN function names so the next decomp pass can open the right local window immediately.
  - For retail tags with no direct EN xref, now also mines nearby-string consensus across EN rev1, PAL, and JP before ranking the indirect EN neighborhood.

## High-value findings

### 1. Every retail source-tag group with live EN xrefs is still outside current `splits.txt`

The current report finds `9` retail source groups with live EN xrefs, and all `9` sit before the first current split:

- first current split: `dolphin/base/PPCArch.c`
- current split start: `0x80240A6C`

That matters because these retail leftovers are not just renaming already-split code. They are direct anchors into the still-unknown main EN text block.

### 2. `objanim.c` and `expgfx.c` are the strongest current file-boundary seeds

- `objanim.c`
  - suggested path: `main/objanim.c`
  - retail label: `setBlendMove`
  - EN span: `0x8002EC4C-0x80030780`
  - three live xref functions, all in one early unsplit island

- `expgfx.c`
  - suggested path: `dll/expgfx.c`
  - EN span: `0x8009B36C-0x8009FF68`
  - six live retail xrefs spread across four EN functions
  - strongest current xref density of any retail-backed source file

If the goal is to materialize a first-pass source skeleton with real retail backing, these two are the cleanest current starts.

### 3. `textblock.c` and `laser.c` share one tiny constructor-like neighborhood

The new report makes this much easier to see:

- `textblock.c` spans `0x80209624-0x802096AC`
- `laser.c` starts immediately after at `0x802096AC-0x802096D8`

That puts both source names in one tight cluster of tiny EN functions. This looks more like a shared registration / stub neighborhood than two unrelated distant systems, and it gives a very small address window to inspect first.

### 4. Several retail-backed DLL names now come with concrete EN windows

The report gives immediate split windows for:

- `dll/CAM/camcontrol.c`
- `dll/curves.c`
- `dll/DIM/DIMboss.c`
- `dll/SH/SHthorntail.c`
- `dll/CF/laser.c`

Even when the final file boundaries still need adjustment, that is already enough to stop guessing at buckets and start from retail-backed EN islands instead.

### 5. `n_attractmode.c` now carries both an indirect EN window and stable cross-bundle context

One weak spot in the earlier boundary pass was that a retail source tag without a direct EN string xref simply disappeared into the residual list.

The tool now keeps a separate low-confidence path for those cases:

- it walks nearby retail strings around the no-xref source tag
- it keeps the nearby strings that survive across EN v1.0, EN rev1, PAL, and JP as context
- keeps only nearby strings that do resolve to current EN functions
- reports the resulting EN neighborhood explicitly as indirect evidence, not as a final file boundary

That currently helps one real case:

- `n_attractmode.c`
  - no direct EN source-tag xref
  - stable nearby strings: `starfox.thp`, `malloc for movie failed`, `/savegame/save%d.bin`, `PICMENU: tex overflow`
  - indirect EN neighborhood: `0x8010ACF0-0x80130618`
  - EN-xref support strings: `n_rareware`, `PATHCAM error: need at least two control points`, `/savegame/save%d.bin`, `PICMENU: tex overflow`

This is still a broad window, but it is materially better than having no bounded place to open at all when recovering the title-screen / movie path.

### 6. `dvdfs.c` still refuses a fake EN window, but the surrounding loader context is now explicit

The tool still does the conservative thing here:

- `dvdfs.c` has no direct EN string xref
- there is still no trustworthy indirect EN neighborhood to promote as a boundary

What changed is the context around that dead-end:

- stable nearby strings: `SFX.bin`, `SFX.tab`
- exact debug-side path hint: `dolphin/dvdfs.c`

That is enough to keep the name grounded in the loader/DVD area without pretending the current EN boundary is known.

### 7. Region variants can tighten naming decisions

The report keeps region alias data attached to the same boundary hint. The clearest current case is:

- `curves.c`
- JP alias: `hcurves.c`

That does not override EN, but it is a useful warning that a file stem may have changed between regions and should be checked before materializing names too aggressively.

## Practical use

- Summary:
  - `python tools/orig/source_boundaries.py`
- Search one source or neighborhood:
  - `python tools/orig/source_boundaries.py --search objanim`
  - `python tools/orig/source_boundaries.py --search textblock laser`
  - `python tools/orig/source_boundaries.py --search camcontrol curves`
  - `python tools/orig/source_boundaries.py --search n_attractmode`
- Spreadsheet-friendly dump:
  - `python tools/orig/source_boundaries.py --format csv`

## How to use this with the other orig tools

- Start with `source_boundaries.py` when choosing the next file or split boundary to recover.
- Use `source_recovery.py` when you want the retail string text and exact EN callsites in more detail.
- Use `source_object_packets.py` when the same source name also needs to be tied to object / DLL / class evidence.

The main value of `source_boundaries.py` is that it turns retail file-name evidence into a current EN work window instead of leaving it as a detached string clue.

Even when the source tag has no direct xref, it now tries to surface one bounded indirect neighborhood before giving up.
