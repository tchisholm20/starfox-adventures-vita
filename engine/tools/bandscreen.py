#!/usr/bin/env python3
"""Rank the sub-100 frontier by SAVED-REGISTER BAND WIDTH.

Why this exists: the register-band assignment model is exact for narrow bands
and then falls off a cliff.  Scoring the CLAUDE.md load-class rule (non-copy
saved regs = one declaration-keyed population filling r31/f31 downward) over
6,288 SFA retail functions, by total saved regs in the band:

    saved regs in band :   2      3      4      5     6+
    GPR                : 97.7%  99.3%  98.8%   1.4%   0.1%
    FP                 : 45.5%  36.8%  19.3%  14.9%   5.1%

It is a cliff, not a decay: the rule holds through width 4 and dies at 5.  So
in a <=4-register function the band is effectively deterministic and STRUCTURE
is the only free variable -- a structural fix lands cleanly instead of being
swamped by allocation noise.  At >=5 there is no total order to fit and
declaration sweeps are provably flat (four exhaustive sweeps of 720/225/144/121
candidates all returned zero movement, every one a wide-band function).

Narrow band means the assignment is PREDICTABLE, not STEERABLE -- a
120-permutation sweep of playerUpdate (3 saved GPRs) was also flat.  Use this to
find *shape* defects (rank by structB), not to plan register moves.

    python3 tools/bandscreen.py --struct-only    # shape defects only
    python3 tools/bandscreen.py --max-band 2     # the tightest regime
    python3 tools/bandscreen.py --all            # every differing function
"""
from __future__ import annotations
import argparse, json, os, re, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump"
INS = re.compile(r'^\s*([0-9a-f]+):\t(?:[0-9a-f]{2} ){4}\t(.*)$', re.M)
FUNC = re.compile(r'^([0-9a-f]{8}) <([^>]+)>:\n(.*?)(?=^\S|\Z)', re.S | re.M)
GDST = re.compile(r'^[a-z][a-z0-9_.]*\s+r(\d+),')
FDST = re.compile(r'^(?:lfs|lfd|fmr|psq_l|f[a-z]+)\s+f(\d+),')


def dis(path: Path) -> str | None:
    if not path.is_file():
        return None
    return subprocess.run([str(OBJDUMP), "-M", "gekko", "-drz", str(path)],
                          capture_output=True, text=True).stdout


def parse(text: str) -> dict:
    """Symbol -> instruction list.

    Retail objects carry interior `lbl_*` symbols (loop heads inside a
    function); objdump starts a fresh block at each one, which would otherwise
    compare a 3-instruction fragment of retail against a whole function of ours
    and report a spurious 100% structural diff.  Fold them into the preceding
    real symbol.
    """
    out, last = {}, None
    for m in FUNC.finditer(text):
        name = m.group(2)
        ins = [t.strip() for _, t in INS.findall(m.group(3))]
        if name.startswith("lbl_") and last is not None:
            out[last].extend(ins)
        else:
            out[name] = ins
            last = name
    return out


def band_widths(ins: list[str]) -> tuple[int, int]:
    g, f = set(), set()
    for t in ins:
        a = GDST.match(t)
        if a and 14 <= int(a.group(1)) <= 31:
            g.add(int(a.group(1)))
        b = FDST.match(t)
        if b and 14 <= int(b.group(1)) <= 31:
            f.add(int(b.group(1)))
    return len(g), len(f)


def differing(t: list[str], c: list[str]) -> tuple[int, int]:
    """(structB_instructions, regB_instructions).

    Align the two streams on their MNEMONIC sequence.  Inside a block whose
    mnemonics agree, an instruction that still differs is an operand-only
    difference -- register allocation (regB).  Anything the mnemonic alignment
    could not match is a real shape difference (structB).

    Aligning on mnemonics rather than on full text is what keeps a whole-function
    register rename (e.g. an r5<->r7 swap where a mr/addi pair trades roles) out
    of the structural bucket, and it behaves identically whether or not the two
    functions are the same length.
    """
    strip = lambda x: re.sub(r'\b[0-9a-f]{1,8} <[^>]+>', '<T>', x)
    T, C = [strip(x) for x in t], [strip(x) for x in c]
    mt, mc = [x.split()[0] for x in T], [x.split()[0] for x in C]
    import difflib
    sm = difflib.SequenceMatcher(None, mt, mc, autojunk=False)
    struct = reg = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            reg += sum(1 for a, b in zip(T[i1:i2], C[j1:j2]) if a != b)
        else:
            struct += max(i2 - i1, j2 - j1)
    return struct, reg


def known_counts(fns: list[str]) -> dict[str, int]:
    roots = [REPO / "docs"]
    roots += sorted(Path.home().glob(".claude/projects/*/memory"))
    files = [p for r in roots if r.is_dir()
             for p in r.rglob("*.md") if p.is_file()]
    texts = []
    for p in files:
        try:
            texts.append(p.read_text(errors="ignore"))
        except OSError:
            pass
    return {fn: sum(1 for t in texts if fn in t) for fn in fns}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--max-band", type=int, default=4,
                    help="max saved regs in EACH band (default 4 -- the cliff is at 4->5)")
    ap.add_argument("--all", action="store_true", help="ignore the band filter")
    ap.add_argument("--struct-only", action="store_true",
                    help="only functions whose mnemonic stream differs")
    ap.add_argument("--limit", type=int, default=60)
    ap.add_argument("--annotate-known", action="store_true",
                    help="mark rows whose function is already discussed in docs/ or "
                         "the agent memory dir -- a high count usually means a "
                         "documented cap, not an unworked target")
    args = ap.parse_args()

    cfg = json.loads((REPO / "build" / args.version / "config.json").read_text())
    rows = []
    for u in cfg["units"]:
        if u.get("autogenerated") or not u.get("code_size"):
            continue
        tgt = REPO / u["object"]
        src = REPO / u["object"].replace(f"build/{args.version}/obj/",
                                         f"build/{args.version}/src/")
        tt, cc = dis(tgt), dis(src)
        if not tt or not cc:
            continue
        T, C = parse(tt), parse(cc)
        for fn, ti in T.items():
            ci = C.get(fn)
            if ci is None or not ti:
                continue
            nm, nr = differing(ti, ci)
            if nm + nr == 0:
                continue
            g, f = band_widths(ti)
            rows.append((nm * 4, nr * 4, len(ti) * 4, g, f, u["name"], fn))

    narrow = [r for r in rows if args.all or (r[3] <= args.max_band and r[4] <= args.max_band)]
    if args.struct_only:
        narrow = [r for r in narrow if r[0] > 0]
    narrow.sort(key=lambda r: (-r[0], -r[1]) if args.struct_only else -(r[0] + r[1]))
    tag = "all" if args.all else f"<={args.max_band} saved regs in BOTH bands"
    print(f"# frontier ranked by band width -- {tag}")
    print(f"# {len(narrow)} of {len(rows)} differing functions qualify; "
          f"structB {sum(r[0] for r in narrow)}/{sum(r[0] for r in rows)}, "
          f"regB {sum(r[1] for r in narrow)}/{sum(r[1] for r in rows)}\n")
    print("# structB = instructions whose MNEMONIC differs (source-addressable shape);")
    print("# regB    = same mnemonic, different operands (band/allocation)\n")
    shown = narrow[:args.limit]
    if not args.annotate_known:
        print("%-8s %-8s %-8s %-4s %-4s  %-34s %s"
              % ("structB", "regB", "sizeB", "G", "F", "unit", "function"))
        for r in shown:
            print("%-8d %-8d %-8d %-4d %-4d  %-34s %s" % r)
        return 0

    known = known_counts([r[6] for r in shown])
    print("# prior = files in docs/ + agent memory that mention the function. It measures how much\n"
          "#         has been WRITTEN about it, not that it is capped -- a big win gets written up\n"
          "#         too. Treat >=5 as 'read those first', never as 'skip'.\n")
    print("%-8s %-8s %-8s %-4s %-4s  %-34s %-44s %s"
          % ("structB", "regB", "sizeB", "G", "F", "unit", "function", "prior"))
    for r in shown:
        n = known.get(r[6], 0)
        mark = "none" if n == 0 else f"{n} file(s)" + ("  <-- read first" if n >= 5 else "")
        print("%-8d %-8d %-8d %-4d %-4d  %-34s %-44s %s" % (*r, mark))
    return 0


if __name__ == "__main__":
    sys.exit(main())
