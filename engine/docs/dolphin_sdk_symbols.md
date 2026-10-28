# Dolphin SDK Symbols

`resources/DolphinSymbolExport_*.txt` contains a mix of useful SDK matches and a lot of low-value collisions. The main failure modes are:

- repeated tiny debugger/TRK signatures such as `DBClose` and `gdev_cc_shutdown`
- anonymous `zz_*` placeholders
- raw Dolphin addresses that do not line up 1:1 with the repo's current `symbols.txt`
- symbol hits that land inside larger placeholder functions already present in `config/[GAMEID]/symbols.txt`

Because of that, the import loop should stay reviewable. The tool below does **not** edit the repo directly. It filters the Dolphin export against the current `symbols.txt` and `splits.txt`, then prints candidates and split seeds.

## Address Translation

The current `GSAE01` repo does not share raw `.text` addresses with the Dolphin export. Before using any Dolphin hit, translate it against anchors that already match in `config/GSAE01/symbols.txt`.

The tool now does this automatically:

```sh
python tools/dolphin_sdk_symbols.py summary
```

Typical output today shows dominant deltas such as `+0x6F8` and `+0x764`. Do not paste raw Dolphin addresses into `symbols.txt` or `splits.txt`.

## Commands

Summary:

```sh
python tools/dolphin_sdk_symbols.py summary
```

High-confidence candidates for one SDK object:

```sh
python tools/dolphin_sdk_symbols.py candidates --lib mtx.a --obj mtxvec.o
```

Reviewable split seeds:

```sh
python tools/dolphin_sdk_symbols.py split-seeds
```

Broader seed clusters when an object has named functions separated by anonymous code:

```sh
python tools/dolphin_sdk_symbols.py split-seeds --gap 0x200
```

## Suggested Loop

1. Run `summary` and verify the anchor deltas make sense for the region you want.
2. Run `split-seeds` and pick a cluster with real leverage.
3. Run `candidates --lib ... --obj ...` for that cluster.
4. Rename exact translated `fn_*` placeholders in `config/[GAMEID]/symbols.txt`.
5. Expand the split seed outward by inspecting neighboring translated Dolphin lines for the same object cluster.
6. Add the corresponding object path to `configure.py` and the reviewed range to `config/[GAMEID]/splits.txt`.
7. Build and check `objdiff`.

## Notes

- `split-seeds` prints minimal translated ranges from high-confidence candidates. Treat them as anchors, not final object boundaries.
- `candidates` prints both the raw Dolphin address and the translated repo address.
- When reference projects also have exact split bounds and `orig/*/sys/main.dol`, use [sdk_dol_match.md](/c:/Projects/SFA-Decomp/docs/sdk_dol_match.md) for cross-game normalized PPC signature matching.
