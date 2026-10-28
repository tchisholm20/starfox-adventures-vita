#!/usr/bin/env python3
"""jtoracle.py -- jump-table addend oracle (instruction-count oracle).

A unit that contains switch jump tables stores them in a data section as
R_PPC_ADDR32 relocations whose addends are *offsets of jump targets inside
functions*.  If our source emits a surplus (or misses) an instruction ahead of
a function's jump targets, every addend past that point shifts by a uniform
+/-4*N while the addends before it stay put.  That makes the addend delta an
exact instruction-count oracle:

  * WHICH function diverges            -- the relocation's target function
  * DIRECTION and MAGNITUDE            -- delta/4 instructions
                                          (delta > 0 => we emit N too many)
  * WHERE                              -- strictly between the last unshifted
                                          retail target offset and the first
                                          shifted one

Unlike objdiff's fuzzy metric this gives no partial credit: it is a byte-exact
statement about the instruction stream, and it fires even inside functions the
fuzzy metric already rewards at 98%+.

Comparison is layout-independent.  Entries are grouped by the function they
point into and the *sorted distinct* target-offset lists are paired, so a
shifted jump table in .data does not blind the oracle.  Equal-length lists give
per-target deltas; unequal lists mean a genuinely different number of jump
targets (a real control-flow difference) and are reported as RAGGED.

GENERALISATION (--text).  The jump-table addend is only one kind of *anchor*
whose offset must agree with retail.  Every relocation inside .text is another:
a call, an SDA21 pool reference, a jump-table base.  If our function carries a
surplus instruction at P, every .text relocation after P shifts by +4 while
those before P stay put -- exactly the same staircase.  Anchoring on the
relocation's (type, symbol) sequence rather than on its offset makes the
comparison layout-independent, and it covers essentially every function in the
image instead of only the 115 units that have jump tables.  The function's own
st_size is used as a final anchor so a divergence after the last relocation is
still caught.

Usage:
    jtoracle.py                     # image-wide jump-table scan, ranked
    jtoracle.py <unit-substr>       # restrict to matching units
    jtoracle.py --pair T.o B.o      # compare two objects directly
    jtoracle.py --text [substr]     # generalised .text relocation staircase
    jtoracle.py --json out.json     # machine-readable dump
    jtoracle.py --quiet             # only units that produced a hit

CAVEAT (measured 2026-08-05): a UNIFORM hit means the ANCHOR OFFSETS moved, which
is usually a surplus/missing instruction but is NOT always.  Constant-materialisation
placement produces the same signature at an IDENTICAL instruction count: MWCC can
hoist an `li rX,0` from its use up to an earlier point, and if one to three
relocations sit inside that hoist window their offsets shift while the streams
realign a few instructions later.  Measured on player.c, where retail and ours are
349/349, 409/409 and 677/677 instructions yet four functions report UNIFORM.  So
confirm the count directly (objdump both sides) before concluding "surplus
instruction" -- the oracle localizes divergence, it does not classify it.
"""
import json
import os
import re
import sys
from bisect import bisect_right
from collections import defaultdict

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
from elftools.elf.relocation import RelocationSection

ROOT = os.environ.get("SFA_ROOT", "/Users/zcanann/Documents/Projects/SFA-Decomp")
R_PPC_ADDR32 = 1
DATA_SECTIONS = (".data", ".rodata", ".sdata", ".sdata2", ".data2", ".ctors", ".dtors")


def read_object(path):
    """-> {(section, offset): (funcname, target_offset_in_func)}"""
    ents = {}
    with open(path, "rb") as fh:
        elf = ELFFile(fh)
        secs = list(elf.iter_sections())
        # function map per section index, for resolving section-symbol relocs
        funcs = defaultdict(list)  # sec_idx -> [(start, end, name)]
        for st in secs:
            if not isinstance(st, SymbolTableSection):
                continue
            for sym in st.iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym.name:
                    funcs[sym["st_shndx"]].append(
                        (sym["st_value"], sym["st_value"] + sym["st_size"], sym.name))
        for k in funcs:
            funcs[k].sort()
        starts = {k: [f[0] for f in v] for k, v in funcs.items()}

        def resolve(shndx, addr):
            v = funcs.get(shndx)
            if not v:
                return None
            i = bisect_right(starts[shndx], addr) - 1
            if i < 0:
                return None
            s, e, n = v[i]
            if addr > e:
                return None
            return n, addr - s

        for sec in secs:
            if not isinstance(sec, RelocationSection):
                continue
            owner = secs[sec["sh_info"]].name
            if owner not in DATA_SECTIONS:
                continue
            symtab = secs[sec["sh_link"]]
            for r in sec.iter_relocations():
                if r["r_info_type"] != R_PPC_ADDR32:
                    continue
                sym = symtab.get_symbol(r["r_info_sym"])
                addend = r["r_addend"] if r.is_RELA() else 0
                if sym.name and sym["st_info"]["type"] == "STT_FUNC":
                    ents[(owner, r["r_offset"])] = (sym.name, addend)
                elif sym["st_info"]["type"] == "STT_SECTION":
                    got = resolve(sym["st_shndx"], addend)
                    if got:
                        ents[(owner, r["r_offset"])] = got
    return ents


class Hit(object):
    __slots__ = ("fn", "n_targets", "n_shifted", "deltas", "last_ok",
                 "first_bad", "uniform", "kind")

    def line(self):
        if self.n_shifted == -1:
            return ("%-7s %-46s anchor sequence differs (retail %s vs ours %s)"
                    % (self.kind, self.fn[:46], self.n_targets[0],
                       self.n_targets[1]))
        loc = ("between +0x%x and +0x%x" % (self.last_ok, self.first_bad)
               if self.last_ok is not None else "before +0x%x" % self.first_bad)
        nz = [d for d in self.deltas if d]
        ins = ",".join("%+d" % (d // 4) for d in nz if d % 4 == 0)
        return ("%-7s %-46s %3d/%-3d targets shifted  delta=%s (%s insn)  %s"
                % (self.kind, self.fn[:46], self.n_shifted, self.n_targets,
                   ",".join("%+d" % d for d in nz), ins, loc))


def analyse(target_path, base_path):
    """-> (hits, notes)"""
    t = read_object(target_path)
    b = read_object(base_path)
    hits, notes = [], []
    tf, bf = defaultdict(set), defaultdict(set)
    for (_s, _o), (fn, a) in t.items():
        tf[fn].add(a)
    for (_s, _o), (fn, a) in b.items():
        bf[fn].add(a)
    only_t = sorted(set(tf) - set(bf))
    only_b = sorted(set(bf) - set(tf))
    if only_t:
        notes.append("functions targeted only in retail: " + ",".join(only_t[:6]))
    if only_b:
        notes.append("functions targeted only in ours: " + ",".join(only_b[:6]))
    for fn in sorted(set(tf) & set(bf)):
        ta, ba = sorted(tf[fn]), sorted(bf[fn])
        h = Hit()
        h.fn = fn
        h.n_targets = len(ta)
        if len(ta) != len(ba):
            h.kind = "RAGGED"
            h.deltas = []
            h.n_shifted = -1
            h.uniform = False
            h.first_bad = -1
            h.last_ok = None
            h.n_targets = (len(ta), len(ba))
            hits.append(h)
            continue
        deltas = [x - y for y, x in zip(ta, ba)]
        nz = sorted({d for d in deltas if d})
        if not nz:
            continue
        h.deltas = sorted(set(deltas))
        h.n_shifted = sum(1 for d in deltas if d)
        h.first_bad = min(o for o, d in zip(ta, deltas) if d)
        prev = [o for o, d in zip(ta, deltas) if not d and o < h.first_bad]
        h.last_ok = max(prev) if prev else None
        h.uniform = len(nz) == 1 and nz[0] % 4 == 0
        # a monotone staircase (deltas non-decreasing) is still actionable:
        # several independent surplus/missing instructions in one function
        mono = all(y >= x for x, y in zip(deltas, deltas[1:])) or \
            all(y <= x for x, y in zip(deltas, deltas[1:]))
        if h.uniform:
            h.kind = "UNIFORM"
        elif mono and all(d % 4 == 0 for d in deltas):
            h.kind = "STAIRS"
        else:
            h.kind = "RAGGED"
        hits.append(h)
    return hits, notes


# ---------------------------------------------------------------- .text mode

ANON = re.compile(
    r"^(@\d+|lbl_[0-9A-Fa-f]{8}|jumptable_[0-9A-Fa-f]+|@stringBase\d*|\.\w.*)$")


def norm(name):
    return "@ANON" if (not name or ANON.match(name)) else name


def read_text_relocs(path):
    """-> {func: (size, [(off_in_fn, (type, normsym)), ...])}"""
    out = {}
    with open(path, "rb") as fh:
        elf = ELFFile(fh)
        secs = list(elf.iter_sections())
        funcs = defaultdict(list)
        for st in secs:
            if not isinstance(st, SymbolTableSection):
                continue
            for sym in st.iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym.name and sym["st_size"]:
                    funcs[sym["st_shndx"]].append(
                        (sym["st_value"], sym["st_size"], sym.name))
        for k in funcs:
            funcs[k].sort()
        starts = {k: [f[0] for f in v] for k, v in funcs.items()}
        rel = defaultdict(list)  # shndx -> [(addr, key)]
        for sec in secs:
            if not isinstance(sec, RelocationSection):
                continue
            shndx = sec["sh_info"]
            if not secs[shndx].name.startswith(".text"):
                continue
            symtab = secs[sec["sh_link"]]
            for r in sec.iter_relocations():
                sym = symtab.get_symbol(r["r_info_sym"])
                # mask to the instruction boundary: the splitter and MWCC
                # disagree by 2 on some half-word relocation placements, and
                # we only care about instruction granularity anyway
                rel[shndx].append((r["r_offset"] & ~3,
                                   (r["r_info_type"], norm(sym.name))))
        for shndx, flist in funcs.items():
            rs = sorted(rel.get(shndx, []))
            offs = [x[0] for x in rs]
            for start, size, name in flist:
                i = bisect_right(offs, start - 1)
                j = bisect_right(offs, start + size - 1)
                out[name] = (size, [(o - start, k) for o, k in rs[i:j]])
    return out


def analyse_text(target_path, base_path, fnfilter=None):
    t = read_text_relocs(target_path)
    b = read_text_relocs(base_path)
    hits = []
    for fn in sorted(set(t) & set(b)):
        if fnfilter and fn not in fnfilter:
            continue
        (tsz, ta), (bsz, ba) = t[fn], b[fn]
        h = Hit()
        h.fn = fn
        if [k for _o, k in ta] != [k for _o, k in ba]:
            if tsz == bsz:
                continue  # same length, different reloc identity: not our oracle
            h.kind = "RAGGED"
            h.deltas = [bsz - tsz]
            h.n_targets = (len(ta), len(ba))
            h.n_shifted = -1
            h.uniform = False
            h.first_bad = -1
            h.last_ok = None
            hits.append(h)
            continue
        # anchors: every reloc offset, then the function end
        toff = [o for o, _k in ta] + [tsz]
        boff = [o for o, _k in ba] + [bsz]
        deltas = [x - y for y, x in zip(toff, boff)]
        nz = sorted({d for d in deltas if d})
        if not nz:
            continue
        h.n_targets = len(toff)
        h.n_shifted = sum(1 for d in deltas if d)
        h.deltas = sorted(set(deltas))
        h.first_bad = min(o for o, d in zip(toff, deltas) if d)
        prev = [o for o, d in zip(toff, deltas) if not d and o < h.first_bad]
        h.last_ok = max(prev) if prev else None
        h.uniform = len(nz) == 1 and nz[0] % 4 == 0
        mono = (all(y >= x for x, y in zip(deltas, deltas[1:]))
                or all(y <= x for x, y in zip(deltas, deltas[1:])))
        h.kind = ("UNIFORM" if h.uniform
                  else "STAIRS" if mono and all(d % 4 == 0 for d in deltas)
                  else "RAGGED")
        hits.append(h)
    return hits


def load_units():
    cfg = json.load(open(os.path.join(ROOT, "objdiff.json")))
    rep_path = os.path.join(ROOT, "build/GSAE01/report.json")
    rep = {}
    if os.path.exists(rep_path):
        for u in json.load(open(rep_path))["units"]:
            rep[u["name"]] = u
    units = []
    for u in cfg["units"]:
        tp = os.path.join(ROOT, u["target_path"])
        bp = os.path.join(ROOT, u.get("base_path")
                          or u["target_path"].replace("/obj/", "/src/"))
        if not os.path.exists(tp) or not os.path.exists(bp):
            continue
        m = rep.get(u["name"], {}).get("measures", {})
        units.append({
            "name": u["name"], "target": tp, "base": bp,
            "src": u.get("metadata", {}).get("source_path"),
            "stake": int(m.get("total_data", 0)) - int(m.get("matched_data", 0)),
        })
    return units


def main():
    argv = sys.argv[1:]
    if "--pair" in argv:
        i = argv.index("--pair")
        hits, notes = analyse(argv[i + 1], argv[i + 2])
        for h in sorted(hits, key=lambda h: h.kind):
            print("    " + h.line())
        for n in notes:
            print("    NOTE    " + n)
        if not hits and not notes:
            print("    (no addend divergence)")
        return

    if "--text" in argv:
        argv = [a for a in argv if a != "--text"]
        jsonout = None
        if "--json" in argv:
            i = argv.index("--json")
            jsonout = argv[i + 1]
            argv = argv[:i] + argv[i + 2:]
        filt = ([a for a in argv if not a.startswith("--")] or [None])[0]
        rep = {}
        rp = os.path.join(ROOT, "build/GSAE01/report.json")
        if os.path.exists(rp):
            for u in json.load(open(rp))["units"]:
                rep[u["name"]] = {f["name"]: f for f in u.get("functions", [])}
        rows = []
        for u in load_units():
            if filt and filt not in u["name"]:
                continue
            try:
                hits = analyse_text(u["target"], u["base"])
            except Exception:
                continue
            fr = rep.get(u["name"], {})
            for h in hits:
                g = fr.get(h.fn, {})
                rows.append((u["name"], h, int(g.get("size", 0) or 0),
                             g.get("fuzzy_match_percent", -1.0)))
        rows.sort(key=lambda r: (r[1].kind != "UNIFORM", r[1].kind != "STAIRS",
                                 -r[3], -r[2]))
        print("%-7s %6s %8s  %-40s %s" % ("KIND", "SIZE", "FUZZY", "FUNCTION",
                                          "UNIT / DETAIL"))
        for un, h, sz, fz in rows:
            print("%-7s %6d %8.3f  %-40s %s" % (h.kind, sz, fz, h.fn[:40], un))
            print("        " + h.line())
        print("\n%d functions with a .text anchor divergence "
              "(UNIFORM %d, STAIRS %d, RAGGED %d)"
              % (len(rows), sum(1 for r in rows if r[1].kind == "UNIFORM"),
                 sum(1 for r in rows if r[1].kind == "STAIRS"),
                 sum(1 for r in rows if r[1].kind == "RAGGED")))
        if jsonout:
            json.dump([{"unit": un, "fn": h.fn, "kind": h.kind, "size": sz,
                        "fuzzy": fz, "deltas": h.deltas, "n_targets": h.n_targets,
                        "n_shifted": h.n_shifted, "last_ok": h.last_ok,
                        "first_bad": h.first_bad}
                       for un, h, sz, fz in rows], open(jsonout, "w"), indent=1)
            print("wrote %s" % jsonout)
        return

    jsonout = None
    if "--json" in argv:
        i = argv.index("--json")
        jsonout = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    quiet = "--quiet" in argv
    args = [a for a in argv if not a.startswith("--")]
    filt = args[0] if args else None

    results = []
    for u in load_units():
        if filt and filt not in u["name"]:
            continue
        try:
            hits, notes = analyse(u["target"], u["base"])
        except Exception as exc:  # pragma: no cover
            hits, notes = [], ["EXC %s" % exc]
        if not hits and not notes:
            continue
        u["hits"], u["notes"] = hits, notes
        results.append(u)

    def nk(u, k):
        return sum(1 for h in u["hits"] if h.kind == k)

    results.sort(key=lambda u: (-u["stake"], -nk(u, "UNIFORM"), -nk(u, "STAIRS"),
                                u["name"]))
    print("%-56s %7s %5s %6s %6s" % ("UNIT", "STAKE", "UNIF", "STAIRS", "RAGGED"))
    for u in results:
        if quiet and not u["hits"]:
            continue
        print("%-56s %7d %5d %6d %6d"
              % (u["name"][:56], u["stake"], nk(u, "UNIFORM"), nk(u, "STAIRS"),
                 nk(u, "RAGGED")))
        for h in sorted(u["hits"], key=lambda h: (h.kind != "UNIFORM",
                                                  h.kind != "STAIRS", h.fn)):
            print("    " + h.line())
        for n in u["notes"]:
            print("    NOTE    " + n)

    if jsonout:
        json.dump([{
            "unit": u["name"], "src": u["src"], "stake": u["stake"],
            "hits": [{"fn": h.fn, "kind": h.kind, "n_targets": h.n_targets,
                      "n_shifted": h.n_shifted, "deltas": h.deltas,
                      "last_ok": h.last_ok, "first_bad": h.first_bad}
                     for h in u["hits"]],
            "notes": u["notes"]} for u in results],
            open(jsonout, "w"), indent=1)
        print("\nwrote %s" % jsonout)


if __name__ == "__main__":
    main()
