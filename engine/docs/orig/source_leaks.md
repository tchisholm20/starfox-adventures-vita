# Retail source-leak inventory

This pass answers a simpler question than `source_recovery.py`: where in the bundled EN `orig/` data do real source-file or header names still survive at all?

The point is not to overfit on every accidental `.c`-looking byte pattern. The point is to separate trustworthy source leftovers from binary noise so recovery time stays focused on the few files that actually carry developer-authored names.

## Tool

- `python tools/orig/source_leaks.py`
  - Inventories direct source/header artifacts already shipped on disc, such as `*.c.new` and `*.h.bak`.
  - Scans printable strings across `orig/GSAE01`, including optional ZLB payloads.
  - Scores each embedded source-like token so `main.dol` and `apploader.img` surface while media noise and accidental asset matches sink out of the summary.
  - Defaults to skipping bulky audio/video containers where `.c` and `.h` matches are almost entirely false positives.

## High-value findings

### 1. The only direct source artifacts on disc are the boot generator outputs and one MusyX header backup

The EN bundle contains `12` direct source/header artifacts:

- `files/Boot/*.c.new`
- `files/gametext/Boot/*.c.new`
- `files/audio/starfox.h.bak`

These are not inferred names. They are literal source/header leftovers preserved in the retail bundle.

The strongest direct clues inside those files are:

- `binary/Boot/English.c` and the other language variants
- `gametext/Boot/English.c` and the other language variants
- `GameTextData.h`

That means the boot text pipeline is already backed by real source filenames and a real header name.

### 2. `main.dol` is the only retail binary with a concentrated cluster of game-specific source names

The high-confidence EN `main.dol` source leaks are:

- `camcontrol.c`
- `objanim.c`
- `curves.c`
- `DIMBoss.c`
- `expgfx.c`
- `laser.c`
- `objHitReact.c`
- `textblock.c`

Medium-confidence `main.dol` leaks still worth keeping in mind are:

- `dvdfs.c`
- `n_attractmode.c`
- `SHthorntail.c`

The important implication is that retail source-name hunting is not spread evenly across the disc. If the goal is source-file or function recovery, `main.dol` is where the game-specific leftovers still live.

### 3. `apploader.img` preserves SDK family names, not game code

The EN apploader still contains:

- `BS2Mach.c`
- `CRCMain.c`
- `dvd.c`

These are not game-specific, but they are still useful because they pin the apploader to concrete SDK source names and help avoid guesswork during SDK matching.

### 4. Decompressed `*.zlb` payloads do not currently carry trustworthy source-name leftovers

The scanner found `0` medium/high-confidence source leaks inside decompressed ZLB payloads.

That negative result matters. It means compressed map/object payloads are probably the wrong place to spend time if the specific goal is source-file or function-name recovery from leaked names.

### 5. Most apparent `.c` / `.h` strings in raw asset binaries are accidental byte noise

Without scoring and filtering, the raw scan produces junk like short pseudo-names in model, texture, and audio data. Those are not actionable source leaks.

The practical rule is simple:

- trust direct source/header artifacts
- trust `main.dol`
- trust `apploader.img` for SDK names
- treat most other asset hits as noise unless they come with readable string context

## Practical use

- get the overall inventory:
  - `python tools/orig/source_leaks.py`
- look for one file or subsystem hint:
  - `python tools/orig/source_leaks.py --search camcontrol`
  - `python tools/orig/source_leaks.py --search GameTextData`
  - `python tools/orig/source_leaks.py --search dvd.c`
- dump the full inventory for spreadsheet or scripting work:
  - `python tools/orig/source_leaks.py --format csv`

## How to use this with decomp work

Use `source_leaks.py` first to answer "does retail `orig/` even preserve a real source/header name for this area?".

If the answer is yes and the hit is in `main.dol`, follow up immediately with [source_recovery.md](/C:/Projects/SFA-Decomp/docs/orig/source_recovery.md) to resolve the source tag back to current EN xrefs and retail-authored function/context labels.

If the answer is no, it is usually better to switch to other recovery angles such as romlists, object placement formats, loader tables, or DLL descriptor work instead of continuing to grep blindly through asset blobs.
