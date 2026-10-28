#!/usr/bin/env python3
"""Compare two build endpoints on EVERY axis a match can be lost on, with a
positive control that runs first.

Why this exists
---------------
Commits 21b90aff9f and 5b120c0545 each recorded "dfuzzy +0.000000 / ddata
+0.000000 / 0 per-function regressions". Full rebuilds plus report.json at
those two shas measure tree 99.81533 -> 99.81422, matched_data 1198129 ->
1197137 (-992 across 11 DLL units), complete_units 877 -> 862, and five
functions knocked off 100.0. The purge itself was policy-correct; the
MEASUREMENT was wrong, and nothing downstream caught it:

  * the units were DEMOTED, so the forced-link / DOL gate stayed green -- a
    NonMatching unit is allowed to differ, so the DOL is rebuilt from the
    retail object and the loss is invisible there;
  * matched_code alone does not move when a pool shrinks, so a matched_code
    gate reads zero;
  * and a loader that reaches for the wrong JSON path scores every function
    -1 on BOTH sides, so its regression list is empty by construction.

There is a fourth way to lose ground with every score axis flat, and no score
can see it. matched_data is all-or-nothing per section, so a data section that
is already short of 100 can be rotated arbitrarily far from its retail carve
for free: ddata +0, dfuzzy +0.000000, no regression, no demotion, not one
instruction changed. That is what 5d467157cb, f5fe00213f and 620b69dc2d each
did (-144, -60, -16 bytes). The answer is not another score: it is to compare
the bytes. The POOL WORD-DIFF below reads our built objects and the retail
carve straight out of the ELF and diffs their data sections word for word.

That last one is not hypothetical. objdiff's report puts a function's score at
`function["fuzzy_match_percent"]`; a loader written against
`function["measures"]["fuzzy_match_percent"]` gets an empty dict for every
function, defaults them all to the same sentinel, and prints "REGRESSED 0" for
any input whatsoever. A harness that cannot fail is indistinguishable from a
harness that passed.

So this tool refuses to report a zero it has not earned. Every mode runs the
schema guard, and the build modes run an end-to-end positive control -- a
synthetic regression injected into real source, rebuilt through the real
pipeline, which the differ must catch -- before the real comparison is
allowed to print a verdict.

What it compares
----------------
  per function   fuzzy_match_percent: REGRESSED / IMPROVED / LOST / NEW, plus
                 UNSCORED (objdiff OMITS the key at 0.0, so a 0% function
                 looks like a missing one) and NEW-UNSCORED. LOST and NEW are
                 paired by virtual address + size so a two-sided rename does
                 not read as a loss.
  per unit       matched_data -- the axis the purge regressions actually moved.
  per unit       metadata.complete -- a DEMOTION is reported even when it costs
                 nothing, because demotion is what blinds the DOL gate.
  per section    fuzzy_match_percent, so a pool that scores 0/84 is visible.
  per section    the POOL WORD-DIFF: our built object's data sections compared
                 word for word against the retail carve, positionally and as a
                 multiset. This is the only layer here that is not derived from
                 a score, and it is the only one that sees a pool ROTATION
                 inside an already-NonMatching unit -- matched_data is
                 all-or-nothing per section, so a section already short of 100
                 can be rotated further from its carve at ddata +0, dfuzzy
                 +0.000000, zero regressions and zero demotions. 5d467157cb,
                 f5fe00213f and 620b69dc2d each did exactly that.
  tree           the top-level measures block.
  the DOL        endpoint B is force-linked (main.dol and main.elf deleted
                 first, or ninja no-ops) and its md5 compared to retail, because
                 a MatchingFor DLL unit's pool links into main.dol .sdata2 and a
                 purge there can grow the DOL while all_source stays green.

Usage
-----
  python3 tools/score_delta_gate.py --self-test
  python3 tools/score_delta_gate.py --commits <A> <B>
  python3 tools/score_delta_gate.py --head
  python3 tools/score_delta_gate.py --reports A.json B.json
  python3 tools/score_delta_gate.py --reports A.json B.json --no-positive-control

--commits and --head build BOTH endpoints from scratch in throwaway worktrees
(git worktree add --detach; nothing is ever checked out in your tree) and run
the build-layer positive control. --reports diffs two report.json files you
already have; it still runs the schema guard and the differ-layer control, and
it warns that the build layer went unproven.

Exit status: 0 clean, 1 regressions found, 2 usage/build error, 3 the positive
control failed -- meaning the tool could not prove it can see a loss, so its
verdict is worthless and must not be quoted.
"""

import argparse
import collections
import copy
import difflib
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = "GSAE01"

# The three paired-single islands in main/main/model are permanently 0% and are
# the only legitimate unscored rows in this tree.
STUCK_UNSCORED = {
    "main/main/model/ObjModel_TransformVerticesWithTranslation",
    "main/main/model/ObjModel_TransformVerticesLinear",
    "main/main/model/ObjModel_TransformQuadVerticesLinear",
}

# The mutation the build-layer positive control injects. Plain C89: a new block,
# so the declaration is legal anywhere a statement is, and a file-scope-lifetime
# counter that MWCC cannot fold away. It moves code (lwz/addi/stw) AND data (a
# fresh .bss atom), so it exercises the per-function axis and the data axis at
# once. It is written, measured, and removed inside one call, and the file is
# md5-verified back to its original bytes.
CONTROL_MUTATION = "    { static int zzScoreDeltaGateControl; zzScoreDeltaGateControl++; }\n"

# Blind spot 3: a pool rotation inside an already-NonMatching unit. matched_data
# is all-or-nothing per section, so a section that is already short of 100 can be
# rotated further from its retail carve at ddata +0, dfuzzy +0.000000, zero
# per-function regressions and zero demotions. Nothing in the score model sees
# it. These are the PROGBITS data sections compared word for word instead.
POOL_SECTION_PREFIXES = (".sdata2", ".sdata", ".rodata", ".data")
SHT_PROGBITS = 1


class SchemaError(Exception):
    """The report does not look like what this tool knows how to read."""


class BuildError(Exception):
    pass


class ControlError(Exception):
    """The positive control did not observe the loss it injected."""


# ---------------------------------------------------------------------------
# report model
# ---------------------------------------------------------------------------

class Fn:
    __slots__ = ("unit", "name", "score", "size", "va", "scored")

    def __init__(self, unit, name, score, size, va, scored):
        self.unit = unit
        self.name = name
        self.score = score
        self.size = size
        self.va = va
        self.scored = scored

    @property
    def key(self):
        return "%s/%s" % (self.unit, self.name)


class Unit:
    __slots__ = ("name", "matched_data", "total_data", "complete", "source_path",
                 "sections")

    def __init__(self, name, matched_data, total_data, complete, source_path, sections):
        self.name = name
        self.matched_data = matched_data
        self.total_data = total_data
        self.complete = complete
        self.source_path = source_path
        self.sections = sections


class Report:
    def __init__(self, path, measures, fns, units, raw_version, label=None):
        self.path = path
        self.measures = measures
        self.fns = fns
        self.units = units
        self.raw_version = raw_version
        self.label = label or path
        # {(unit, section): PoolStat} once a build tree has been read, or None
        # while this endpoint is report-only. Never {} -- see pool_fingerprint.
        self.pools = None

    @property
    def unscored(self):
        return {k for k, f in self.fns.items() if not f.scored}


def _int(v, default=0):
    if v is None:
        return default
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def parse_report(doc, path="<memory>", label=None, min_functions=1000):
    """Turn an objdiff report document into a Report, or refuse to.

    The refusals are the point. Every one of them is a shape that would
    otherwise produce an empty regression list from a broken read.
    """
    if not isinstance(doc, dict):
        raise SchemaError("%s: report is not a JSON object" % path)
    measures = doc.get("measures")
    if not isinstance(measures, dict) or "fuzzy_match_percent" not in measures:
        raise SchemaError("%s: no top-level measures.fuzzy_match_percent" % path)
    units_in = doc.get("units")
    if not isinstance(units_in, list) or not units_in:
        raise SchemaError("%s: report has no units" % path)

    fns = {}
    units = {}
    with_key = 0
    for u in units_in:
        name = u.get("name")
        if not name:
            raise SchemaError("%s: a unit has no name" % path)
        um = u.get("measures") or {}
        meta = u.get("metadata") or {}
        sections = {}
        for s in u.get("sections") or []:
            # objdiff omits fuzzy_match_percent at 0.0 here too.
            sections[s.get("name", "?")] = (
                float(s.get("fuzzy_match_percent", 0.0)), _int(s.get("size")))
        units[name] = Unit(
            name=name,
            matched_data=_int(um.get("matched_data")),
            total_data=_int(um.get("total_data")),
            complete=bool(meta.get("complete", False)),
            source_path=meta.get("source_path"),
            sections=sections,
        )
        for f in u.get("functions") or []:
            fname = f.get("name")
            if fname is None:
                raise SchemaError("%s: unit %s has a function with no name" % (path, name))
            # THE TRAP: objdiff omits the key when the score is 0.0, so a 0%
            # function looks like an absent one. Score it -1 so it sorts below
            # every real score and lands in UNSCORED rather than vanishing.
            if "fuzzy_match_percent" in f:
                scored = True
                score = float(f["fuzzy_match_percent"])
                with_key += 1
            else:
                scored = False
                score = -1.0
            fmeta = f.get("metadata") or {}
            fns["%s/%s" % (name, fname)] = Fn(
                unit=name, name=fname, score=score, size=_int(f.get("size")),
                va=_int(fmeta.get("virtual_address"), -1), scored=scored)

    if not fns:
        raise SchemaError("%s: report lists no functions at all" % path)
    if len(fns) < min_functions:
        raise SchemaError(
            "%s: only %d functions -- expected at least %d; a truncated report "
            "cannot be trusted to show a regression" % (path, len(fns), min_functions))
    # A loader reading the wrong JSON path scores every function the same
    # sentinel and reports zero regressions against any input. If essentially
    # nothing carries a real score, the schema moved and this tool is blind.
    if with_key < len(fns) // 2:
        raise SchemaError(
            "%s: only %d of %d functions carry fuzzy_match_percent -- the report "
            "schema is not what this tool reads, so a zero here would be a false "
            "zero" % (path, with_key, len(fns)))
    return Report(path, measures, fns, units, doc.get("version"), label=label)


def load_report(path, label=None, min_functions=1000):
    with open(path) as fh:
        doc = json.load(fh)
    return parse_report(doc, path=path, label=label, min_functions=min_functions)


# ---------------------------------------------------------------------------
# diff
# ---------------------------------------------------------------------------

EPS = 1e-6


# ---------------------------------------------------------------------------
# pool word-diff -- the sensor for blind spot 3
# ---------------------------------------------------------------------------

class PoolStat:
    """One data section of one unit, ours against the retail carve.

    Three axes, and only two of them are allowed to turn a row red.

    VALUE axis -- `missing` and `extra`, the word MULTISET shortfall and
    surplus against the carve. This is the axis that says whether our pool
    holds the right constants. It is immune to where in the section they sit.

    ORDER axis -- `aligned`, the number of words covered by SequenceMatcher's
    matching blocks. A rotation preserves the multiset exactly but shreds the
    alignment, so this is the field that catches blind spot 3. Unlike raw
    positional agreement it is SHIFT-INVARIANT: deleting one word from the
    front does not cost the whole tail.

    `hits`, the raw positional agreement, is kept for continuity of the printed
    row but is NO LONGER a gate. It was: on an already-short section any size
    change slid the tail and read as a large regression, and lane A83's
    correct engine/0 literal-width fix went red on it (hits 94 -> 64) while the
    multiset moved strictly TOWARD the carve (extra {0x40840000:1} -> {}) and
    the three newly-"missing" words were 0x00000000 padding that existed only
    because our literal had the wrong width. A red row that is really an
    improvement eventually costs a lane a correct fix.
    """

    __slots__ = ("unit", "section", "hits", "aligned", "ours", "retail",
                 "missing", "extra", "mdelta")

    def __init__(self, unit, section, hits, ours, retail, missing, extra,
                 aligned=None, mdelta=None):
        self.unit = unit
        self.section = section
        self.hits = hits
        self.aligned = hits if aligned is None else aligned
        self.ours = ours
        self.retail = retail
        self.missing = missing
        self.extra = extra
        # {word: +n surplus / -n shortfall}, the value axis in full.
        self.mdelta = mdelta or {}

    # A 0x00000000 word is alignment padding or a reservation, not a constant
    # any source text names, and its count moves whenever a literal changes
    # width. The gate reads the NONZERO shortfall/surplus; the zero counts are
    # printed but never turn a row red. A genuinely deleted 0.0f still shows on
    # the alignment axis and in the section's own fuzzy score.
    @property
    def missing_nz(self):
        return sum(-n for w, n in self.mdelta.items() if n < 0 and w)

    @property
    def extra_nz(self):
        return sum(n for w, n in self.mdelta.items() if n > 0 and w)

    @property
    def zero_delta(self):
        return self.mdelta.get(0, 0)

    @property
    def unmatched(self):
        """OUR words that no aligned run accounts for -- the ones sitting in
        the wrong place or holding the wrong value. `aligned` alone cannot gate
        because it is capped by len(ours), so deleting a surplus word lowers it
        on a correct fix; this difference is not."""
        return self.ours - self.aligned

    @property
    def key(self):
        return (self.unit, self.section)

    def mdelta_str(self, limit=4):
        """The multiset delta as source text, worst first. Printing this is the
        whole point: a pool row is only believable next to its value delta."""
        if not self.mdelta:
            return "{}"
        items = sorted(self.mdelta.items(), key=lambda kv: (-abs(kv[1]), kv[0]))
        head = ", ".join("0x%08x:%+d" % (w, n) for w, n in items[:limit])
        if len(items) > limit:
            head += ", +%d more" % (len(items) - limit)
        return "{%s}" % head

    def __repr__(self):
        return ("PoolStat(%s %s aligned=%d/%d hits=%d missing=%d extra=%d)"
                % (self.unit, self.section, self.aligned, self.retail,
                   self.hits, self.missing, self.extra))


def elf_progbits(path):
    """Minimal ELF32 big-endian section reader.

    objcopy would do this too, but at two subprocesses per section per object
    it costs minutes across ~1000 units at two endpoints; reading the headers
    directly costs under a second.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    if blob[:4] != b"\x7fELF" or len(blob) < 0x34:
        raise SchemaError("%s is not an ELF object" % path)
    if blob[4] != 1 or blob[5] != 2:
        raise SchemaError("%s is not ELF32 big-endian" % path)
    shoff, = struct.unpack_from(">I", blob, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", blob, 0x2E)
    if not shoff or not shnum:
        return {}

    def header(i):
        return struct.unpack_from(">IIIIII", blob, shoff + i * shentsize)

    _n, _t, _f, _a, stroff, strsize = header(shstrndx)
    strtab = blob[stroff:stroff + strsize]
    out = {}
    for i in range(shnum):
        nameoff, typ, _flags, _addr, off, size = header(i)
        if typ != SHT_PROGBITS:
            continue
        end = strtab.find(b"\0", nameoff)
        name = strtab[nameoff:end].decode("ascii", "replace")
        if not name.startswith(POOL_SECTION_PREFIXES):
            continue
        out[name] = blob[off:off + size]
    return out


def _words(blob):
    n = len(blob) // 4
    return list(struct.unpack(">%dI" % n, blob[:n * 4])) if n else []


def compare_pool(unit, section, ours_blob, retail_blob):
    ours, retail = _words(ours_blob), _words(retail_blob)
    hits = sum(1 for x, y in zip(ours, retail) if x == y)
    co, cr = collections.Counter(ours), collections.Counter(retail)
    surplus, shortfall = co - cr, cr - co
    missing = sum(shortfall.values())
    extra = sum(surplus.values())
    mdelta = {}
    for w, n in surplus.items():
        mdelta[w] = n
    for w, n in shortfall.items():
        mdelta[w] = -n
    aligned = sum(b.size for b in
                  difflib.SequenceMatcher(None, ours, retail,
                                          autojunk=False).get_matching_blocks())
    return PoolStat(unit, section, hits, len(ours), len(retail), missing, extra,
                    aligned=aligned, mdelta=mdelta)


def obj_paths(wt, source_path):
    """(ours, retail carve) for a unit, from its report source_path."""
    stem = os.path.splitext(source_path)[0]
    if not stem.startswith("src/"):
        return None, None
    build = os.path.join(wt, "build", VERSION)
    return (os.path.join(build, stem + ".o"),
            os.path.join(build, "obj" + stem[3:] + ".o"))


def pool_fingerprint(wt, report):
    """Every unit's data sections, ours against the retail carve.

    Returns {(unit, section): PoolStat}, or None if this endpoint has no build
    tree to read (a bare --reports run), so the caller can say so rather than
    print a zero it did not earn.
    """
    if not wt or not os.path.isdir(os.path.join(wt, "build", VERSION)):
        return None
    stats = {}
    for name, unit in report.units.items():
        if not unit.source_path:
            continue
        ours_p, retail_p = obj_paths(wt, unit.source_path)
        if not ours_p or not (os.path.exists(ours_p) and os.path.exists(retail_p)):
            continue
        ours = elf_progbits(ours_p)
        retail = elf_progbits(retail_p)
        for sec in set(ours) | set(retail):
            st = compare_pool(name, sec, ours.get(sec, b""), retail.get(sec, b""))
            if st.retail or st.ours:
                stats[st.key] = st
    if not stats:
        raise SchemaError(
            "%s: pool fingerprint is empty -- no unit's objects could be read, "
            "so a clean pool verdict here would be a false clean" % wt)
    return stats


def diff_pools(pa, pb):
    """Rows where our pool moved AWAY from the carve, and rows where it moved
    toward it.

    A row is reported worse when the VALUE axis loses ground (the word
    shortfall or the surplus grows) or when the SHIFT-INVARIANT alignment
    drops. Raw positional agreement is printed but never gates: a pure size
    change slides the whole tail out of position without any word changing, and
    gating on that reports a correct fix as a regression."""
    worse, better = [], []
    if pa is None or pb is None:
        return worse, better
    for key in set(pa) | set(pb):
        sa, sb = pa.get(key), pb.get(key)
        if sa is None or sb is None:
            # A data section that appeared or vanished outright.
            seen = sb or sa
            zero = PoolStat(key[0], key[1], 0, 0, seen.retail, seen.retail, 0,
                            aligned=0, mdelta={})
            sa, sb = (sa or zero), (sb or zero)
        # Growth past the retail claim counts against us -- that is how a
        # freshly minted @N anon shows up -- but not when the same edit also
        # bought alignment, which is a section being filled in rather than
        # drifting.
        filling_in = sb.missing_nz < sa.missing_nz
        improved_any = (filling_in or sb.extra_nz < sa.extra_nz
                        or sb.unmatched < sa.unmatched)
        # A zero-word shortfall that grows on its own is still a loss; one that
        # grows while the row improves elsewhere is the alignment padding a
        # wrong-width literal used to force, and is not.
        zero_lost = sb.zero_delta < sa.zero_delta
        drifted = (sb.missing_nz > sa.missing_nz
                   or (sb.unmatched > sa.unmatched and not filling_in)
                   or (sb.extra_nz > sa.extra_nz and not filling_in)
                   or (zero_lost and not improved_any))
        if drifted:
            worse.append((sa, sb))
        elif improved_any:
            better.append((sa, sb))
    worse.sort(key=lambda r: (r[0].missing_nz - r[1].missing_nz,
                              r[0].unmatched - r[1].unmatched))
    better.sort(key=lambda r: (r[1].missing_nz - r[0].missing_nz,
                               r[1].unmatched - r[0].unmatched))
    return worse, better


class Delta:
    def __init__(self):
        self.regressed = []      # (key, before, after)
        self.improved = []
        self.lost = []           # Fn from A
        self.new = []            # Fn from B
        self.renames = []        # (lost Fn, new Fn) paired by va+size
        self.new_unscored = []   # keys unscored in B that were scored in A
        self.unscored_b = []     # every unscored key in B
        self.data = []           # (unit, before, after)
        self.demoted = []
        self.promoted = []
        self.sections = []       # (unit, section, before, after)
        self.pool_worse = []     # (PoolStat A, PoolStat B) -- rotated or lost
        self.pool_better = []
        self.pools_read = False  # False when neither endpoint had a build tree
        self.tree = {}

    @property
    def data_loss(self):
        return [r for r in self.data if r[2] < r[1]]

    @property
    def unexpected_unscored(self):
        return [k for k in self.unscored_b if k not in STUCK_UNSCORED]

    @property
    def red(self):
        """Rows that fail the gate. Demotion alone is policy, not failure --
        but it IS what blinds the DOL gate, so it is always printed."""
        return (bool(self.regressed) or bool(self.lost) or bool(self.new_unscored)
                or bool(self.data_loss) or bool(self.unexpected_unscored)
                or bool(self.pool_worse))


def pair_renames(lost, new):
    """A two-sided rename empties one key and fills another. Pair them by
    virtual address + size so a rename is not reported as a loss."""
    pairs = []
    by_va = {}
    for f in new:
        by_va.setdefault((f.va, f.size), []).append(f)
    rest_lost, matched_new = [], set()
    for f in lost:
        bucket = by_va.get((f.va, f.size))
        if bucket and f.va >= 0:
            cand = bucket.pop(0)
            pairs.append((f, cand))
            matched_new.add(id(cand))
        else:
            rest_lost.append(f)
    rest_new = [f for f in new if id(f) not in matched_new]
    return pairs, rest_lost, rest_new


def diff_reports(a, b):
    d = Delta()
    for key in ("fuzzy_match_percent", "matched_data", "matched_code",
                "total_data", "total_functions", "complete_units", "total_units"):
        va, vb = a.measures.get(key), b.measures.get(key)
        if va is None and vb is None:
            continue
        d.tree[key] = (va, vb)

    lost_fns, new_fns = [], []
    for k, fa in a.fns.items():
        fb = b.fns.get(k)
        if fb is None:
            lost_fns.append(fa)
            continue
        if fa.scored and not fb.scored:
            d.new_unscored.append(k)
        if fb.score < fa.score - EPS:
            d.regressed.append((k, fa.score, fb.score))
        elif fb.score > fa.score + EPS:
            d.improved.append((k, fa.score, fb.score))
    for k, fb in b.fns.items():
        if k not in a.fns:
            new_fns.append(fb)
        if not fb.scored:
            d.unscored_b.append(k)

    d.renames, d.lost, d.new = pair_renames(lost_fns, new_fns)
    # A rename that also moved the score is still a regression.
    for fa, fb in d.renames:
        if fb.score < fa.score - EPS:
            d.regressed.append(("%s -> %s" % (fa.key, fb.key), fa.score, fb.score))

    for name, ua in a.units.items():
        ub = b.units.get(name)
        if ub is None:
            continue
        if ua.matched_data != ub.matched_data:
            d.data.append((name, ua.matched_data, ub.matched_data))
        if ua.complete and not ub.complete:
            d.demoted.append(name)
        elif ub.complete and not ua.complete:
            d.promoted.append(name)
        for sec, (pa, _sa) in ua.sections.items():
            pb = ub.sections.get(sec, (0.0, 0))[0]
            if abs(pa - pb) > 1e-4:
                d.sections.append((name, sec, pa, pb))

    d.pools_read = a.pools is not None and b.pools is not None
    d.pool_worse, d.pool_better = diff_pools(a.pools, b.pools)
    return d


def render(d, a, b, out=sys.stdout):
    w = out.write
    w("=" * 78 + "\n")
    w("A: %s\nB: %s\n" % (a.label, b.label))
    w("=" * 78 + "\n")
    for k, (va, vb) in d.tree.items():
        try:
            fa, fb = float(va), float(vb)
            mark = "" if abs(fb - fa) < 1e-9 else ("  %+g" % (fb - fa))
        except (TypeError, ValueError):
            fa = fb = None
            mark = ""
        w("  %-20s %18s -> %-18s%s\n" % (k, va, vb, mark))

    w("\nREGRESSED %d\n" % len(d.regressed))
    for k, x, y in sorted(d.regressed, key=lambda r: r[1] - r[2], reverse=True):
        w("  REG   %-68s %8.3f -> %8.3f  (%+.3f)\n" % (k, x, y, y - x))
    w("IMPROVED %d\n" % len(d.improved))
    for k, x, y in sorted(d.improved, key=lambda r: r[2] - r[1], reverse=True)[:40]:
        w("  IMP   %-68s %8.3f -> %8.3f  (%+.3f)\n" % (k, x, y, y - x))
    w("LOST %d (unpaired)\n" % len(d.lost))
    for f in d.lost:
        w("  LOST  %-68s %8.3f  size=%d va=%d\n" % (f.key, f.score, f.size, f.va))
    w("NEW %d (unpaired)\n" % len(d.new))
    for f in d.new:
        w("  NEW   %-68s %8.3f  size=%d va=%d\n" % (f.key, f.score, f.size, f.va))
    if d.renames:
        w("RENAMES %d (paired by va+size, score held)\n" % len(d.renames))
        for fa, fb in d.renames:
            w("  REN   %s -> %s  (%.3f)\n" % (fa.key, fb.key, fb.score))
    w("NEW-UNSCORED %d\n" % len(d.new_unscored))
    for k in d.new_unscored:
        w("  UNSC* %s\n" % k)
    unexpected = d.unexpected_unscored
    w("UNSCORED in B: %d (%d expected PS islands, %d unexpected)\n"
      % (len(d.unscored_b), len(d.unscored_b) - len(unexpected), len(unexpected)))
    for k in unexpected:
        w("  UNSC  %s\n" % k)

    w("\nUNIT matched_data deltas %d (net %+d)\n"
      % (len(d.data), sum(y - x for _n, x, y in d.data)))
    for n, x, y in sorted(d.data, key=lambda r: r[2] - r[1]):
        w("  DATA  %-60s %8d -> %-8d (%+d)\n" % (n, x, y, y - x))
    w("DEMOTED %d  (complete true -> false: the DOL gate goes blind here)\n"
      % len(d.demoted))
    for n in d.demoted:
        w("  DEMOTE %s\n" % n)
    w("PROMOTED %d\n" % len(d.promoted))
    for n in d.promoted:
        w("  PROMOTE %s\n" % n)
    if d.sections:
        w("SECTION fuzzy deltas %d\n" % len(d.sections))
        for n, sec, x, y in sorted(d.sections, key=lambda r: r[3] - r[2])[:60]:
            w("  SECT  %-52s %-10s %7.3f -> %7.3f\n" % (n, sec, x, y))

    if not d.pools_read:
        w("\nPOOL word-diff: NOT RUN -- no build tree at one or both endpoints, "
          "so a rotation here would be invisible\n")
    else:
        w("\nPOOL word-diff (ours vs the retail carve, per data section) "
          "worse %d / better %d\n" % (len(d.pool_worse), len(d.pool_better)))
        for sa, sb in d.pool_worse[:60]:
            w("  POOL  %-52s %-10s misplaced %4d -> %-4d (%+d)  "
              "shortfall %d -> %d  surplus %d -> %d  [raw hits %d -> %d]\n"
              % (sb.unit, sb.section, sa.unmatched, sb.unmatched,
                 sb.unmatched - sa.unmatched, sa.missing_nz, sb.missing_nz,
                 sa.extra_nz, sb.extra_nz, sa.hits, sb.hits))
            w("        zero pad %+d -> %+d, raw missing %d -> %d, "
              "raw extra %d -> %d\n"
              % (sa.zero_delta, sb.zero_delta, sa.missing, sb.missing,
                 sa.extra, sb.extra))
            w("        value delta %s -> %s\n"
              % (sa.mdelta_str(), sb.mdelta_str()))
        for sa, sb in d.pool_better[:40]:
            w("  POOL+ %-52s %-10s misplaced %4d -> %-4d (%+d)  "
              "shortfall %d -> %d  surplus %d -> %d  [raw hits %d -> %d]\n"
              % (sb.unit, sb.section, sa.unmatched, sb.unmatched,
                 sb.unmatched - sa.unmatched, sa.missing_nz, sb.missing_nz,
                 sa.extra_nz, sb.extra_nz, sa.hits, sb.hits))
            w("        zero pad %+d -> %+d, raw missing %d -> %d, "
              "raw extra %d -> %d\n"
              % (sa.zero_delta, sb.zero_delta, sa.missing, sb.missing,
                 sa.extra, sb.extra))
            w("        value delta %s -> %s\n"
              % (sa.mdelta_str(), sb.mdelta_str()))
    w("\nVERDICT: %s\n" % ("RED" if d.red else "CLEAN"))
    return d


# ---------------------------------------------------------------------------
# build endpoints
# ---------------------------------------------------------------------------

def run(cmd, cwd, check=True, capture=True):
    p = subprocess.run(cmd, cwd=cwd, text=True,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.STDOUT if capture else None)
    if check and p.returncode != 0:
        raise BuildError("%s (in %s) exited %d\n%s"
                         % (" ".join(cmd), cwd, p.returncode, (p.stdout or "")[-4000:]))
    return p


def git(args, cwd=REPO, check=True):
    return run(["git"] + args, cwd=cwd, check=check)


def make_worktree(sha, path, quiet=False):
    if os.path.exists(path):
        raise BuildError("worktree path already exists: %s" % path)
    git(["worktree", "add", "--detach", path, sha])
    if not quiet:
        print("  worktree %s @ %s" % (path, sha))
    return path


def provision(wt, compilers, binutils, tools_src):
    """A fresh worktree has no build/. Give it the toolchain the main tree
    already downloaded rather than re-fetching it."""
    os.makedirs(os.path.join(wt, "build"), exist_ok=True)
    link = os.path.join(wt, "build", "binutils")
    if not os.path.exists(link):
        os.symlink(binutils, link)
    dst_tools = os.path.join(wt, "build", "tools")
    if not os.path.exists(dst_tools) and os.path.isdir(tools_src):
        shutil.copytree(tools_src, dst_tools, symlinks=True)
    # The retail DOL the forced-link gate hashes against.
    src_dol = os.path.join(REPO, "orig", VERSION, "sys", "main.dol")
    dst_dol = os.path.join(wt, "orig", VERSION, "sys", "main.dol")
    if os.path.exists(src_dol) and not os.path.exists(dst_dol):
        os.makedirs(os.path.dirname(dst_dol), exist_ok=True)
        shutil.copy2(src_dol, dst_dol)
    run([sys.executable, "configure.py",
         "--compilers", compilers, "--binutils", binutils], cwd=wt)


def report_path(wt):
    return os.path.join(wt, "build", VERSION, "report.json")


def assert_fresh(wt):
    """Guard against the A26-A31 frozen-report trap.

    A probe that fails leaves the .o DELETED; a later read of a stale
    report.json then reports the axis inert. Three assertions: the build has
    no work left, every unit's object exists, and report.json is not older
    than the newest object.
    """
    rp = report_path(wt)
    if not os.path.exists(rp):
        raise BuildError("no report.json in %s" % wt)
    dry = run(["ninja", "-n", "all_source"], cwd=wt)
    if "no work to do" not in (dry.stdout or ""):
        raise BuildError("build in %s is not up to date:\n%s"
                         % (wt, (dry.stdout or "")[:2000]))
    doc = json.load(open(rp))
    newest = 0.0
    missing = []
    for u in doc.get("units", []):
        sp = (u.get("metadata") or {}).get("source_path")
        if not sp:
            continue
        obj = os.path.join(wt, "build", VERSION, os.path.splitext(sp)[0] + ".o")
        if not os.path.exists(obj):
            missing.append(sp)
        else:
            newest = max(newest, os.path.getmtime(obj))
    if missing:
        raise BuildError("%d source objects are MISSING under %s (a failed probe "
                         "deletes the .o and freezes the report): %s"
                         % (len(missing), wt, ", ".join(missing[:8])))
    if os.path.getmtime(rp) + 1.0 < newest:
        raise BuildError("report.json in %s is OLDER than the newest object -- "
                         "it is frozen and must be regenerated" % wt)


def build(wt, quiet=False):
    if not quiet:
        print("  building %s ..." % wt)
    run(["ninja", "all_source"], cwd=wt)
    rp = report_path(wt)
    # Never read a report we did not just produce.
    if os.path.exists(rp):
        os.remove(rp)
    run(["ninja", os.path.relpath(rp, wt)], cwd=wt)
    assert_fresh(wt)
    return rp


def dol_gate(wt, quiet=False):
    """The forced-link gate, run in-process so a window scan cannot skip it.

    Deleting main.dol/main.elf first is not decoration: without the delete
    ninja has nothing to do and the check is a no-op. A MatchingFor DLL unit's
    literal pool links into main.dol's .sdata2, so a purge can GROW the DOL and
    leave staging red while `all_source` stays green.
    """
    dol = os.path.join(wt, "build", VERSION, "main.dol")
    elf = os.path.join(wt, "build", VERSION, "main.elf")
    for p in (dol, elf):
        if os.path.exists(p):
            os.remove(p)
    run(["ninja", os.path.join("build", VERSION, "ok")], cwd=wt)
    got = md5(dol)
    retail = os.path.join(wt, "orig", VERSION, "sys", "main.dol")
    want = md5(retail) if os.path.exists(retail) else None
    ok = want is None or got == want
    if not quiet:
        print("  forced link: main.dol md5 %s %s" % (
            got, "== retail" if ok else "!= retail %s  *** DOL IS RED ***" % want))
    return ok, got, want


def rebuild_unit(wt, obj_rel):
    run(["ninja", obj_rel], cwd=wt)
    rp = report_path(wt)
    if os.path.exists(rp):
        os.remove(rp)
    run(["ninja", os.path.relpath(rp, wt)], cwd=wt)
    return rp


# ---------------------------------------------------------------------------
# positive control -- build layer
# ---------------------------------------------------------------------------

def md5(path):
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def pick_control_sites(report, wt, limit=6):
    """Rank units we could perturb: real source we can find on disk, at least
    one function scoring above zero, smallest first so the rebuild is cheap."""
    cands = []
    for name, u in report.units.items():
        if not u.source_path or not u.source_path.endswith(".c"):
            continue
        src = os.path.join(wt, u.source_path)
        if not os.path.exists(src):
            continue
        fns = [f for f in report.fns.values() if f.unit == name and f.scored and f.score > 0]
        if not fns:
            continue
        size = sum(f.size for f in fns)
        if size < 0x80:
            continue
        if find_injection_point(src) is None:
            continue
        cands.append((size, name, u.source_path))
    if not cands:
        raise ControlError("no suitable unit to inject the control regression into")
    return [(n, p) for _s, n, p in sorted(cands)[:limit]]


# A C89 declaration: a type token followed by a declarator. This has to be told
# apart from a plain assignment, because MWCC rejects a statement placed among
# the declarations -- the mutation must land at the FIRST STATEMENT, which is
# also the position where it cannot be dead-code-eliminated.
RE_DECL = re.compile(
    rb"^\s*(?:static\s+|const\s+|volatile\s+|register\s+|unsigned\s+|signed\s+|struct\s+|union\s+)*"
    rb"[A-Za-z_]\w*\s+\**\s*[A-Za-z_]\w*\s*(?:\[[^\]]*\])?\s*(?:=|,|;)")


def find_injection_point(path):
    """Line index at which the control statement may legally be inserted.

    This tree is K&R, so a function body opens with a lone `{` in column 0 --
    but so does an array or struct initialiser, so require the preceding
    non-blank line to close a parameter list and carry no `=`. Then step past
    the declarations to the first statement.
    """
    with open(path, "rb") as fh:
        lines = fh.read().split(b"\n")
    for i, ln in enumerate(lines):
        if ln != b"{":
            continue
        j = i - 1
        while j >= 0 and not lines[j].strip():
            j -= 1
        if j < 0:
            continue
        prev = lines[j].strip()
        if not prev.endswith(b")") or b"=" in prev or prev.startswith(b"#"):
            continue
        k = i + 1
        while k < len(lines):
            s = lines[k].strip()
            if (not s or s.startswith(b"//") or s.startswith(b"/*")
                    or s.startswith(b"*") or s.startswith(b"#") or RE_DECL.match(s)):
                k += 1
                continue
            break
        if k < len(lines) and lines[k].strip() not in (b"}", b""):
            return k
    return None


def inject(path):
    """Insert the control statement at the first statement of a function."""
    k = find_injection_point(path)
    if k is None:
        raise ControlError("no injectable function body found in %s" % path)
    with open(path, "rb") as fh:
        lines = fh.read().split(b"\n")
    lines.insert(k, CONTROL_MUTATION.rstrip("\n").encode())
    with open(path, "wb") as fh:
        fh.write(b"\n".join(lines))
    return k


def positive_control(wt, baseline, quiet=False):
    """Prove, end to end, that a real loss reaches the verdict.

    Inject -> rebuild -> the differ MUST report a regression. Then restore ->
    rebuild -> the differ MUST report nothing. The second half is the negative
    control: it shows the RED came from the injection and not from noise.

    Candidate units are tried in turn, because a mutation that does not compile
    proves nothing either way; only an exhausted candidate list is a failure.
    """
    tried = []
    for unit, src_rel in pick_control_sites(baseline, wt):
        src = os.path.join(wt, src_rel)
        obj_rel = os.path.join("build", VERSION,
                               os.path.splitext(src_rel)[0] + ".o")
        original = open(src, "rb").read()
        before_md5 = hashlib.md5(original).hexdigest()
        if not quiet:
            print("  positive control: injecting a synthetic regression into %s (%s)"
                  % (src_rel, unit))
        built = False
        try:
            inject(src)
            rp = rebuild_unit(wt, obj_rel)
            built = True
            hurt = load_report(rp, label="control(injected)")
            d = diff_reports(baseline, hurt)
            moved_fn = [r for r in d.regressed if r[0].startswith(unit + "/")]
            if not moved_fn:
                raise ControlError(
                    "the injected regression in %s was NOT reported. This tool "
                    "cannot see a loss, so any zero it prints is meaningless."
                    % src_rel)
            if not quiet:
                k, x, y = moved_fn[0]
                print("    control seen: %s %.3f -> %.3f  (%d rows moved, "
                      "%d unit data rows)"
                      % (k, x, y, len(d.regressed), len(d.data)))
        except BuildError as exc:
            tried.append("%s (did not compile)" % src_rel)
            if not quiet:
                print("    %s did not compile with the mutation; trying the next unit"
                      % src_rel)
            built = False
            exc  # the mutation is ours, so a compile error is not a tool failure
        finally:
            with open(src, "wb") as fh:
                fh.write(original)
        if not built:
            rebuild_unit(wt, obj_rel)
            continue
        if md5(src) != before_md5:
            raise ControlError("failed to restore %s byte-for-byte" % src_rel)
        rp = rebuild_unit(wt, obj_rel)
        back = load_report(rp, label="control(restored)")
        d2 = diff_reports(baseline, back)
        if d2.red or d2.improved:
            raise ControlError(
                "after restoring %s the tree does not match its own baseline "
                "(%d regressed, %d improved, %d data rows) -- the measurement is "
                "not reproducible" % (src_rel, len(d2.regressed),
                                      len(d2.improved), len(d2.data)))
        if not quiet:
            print("    negative control: restored tree diffs clean against its "
                  "own baseline")
        return True
    raise ControlError("no candidate unit accepted the control mutation: %s"
                       % "; ".join(tried))


# ---------------------------------------------------------------------------
# positive control -- differ layer
# ---------------------------------------------------------------------------

def _synthetic_doc(nfns=1200):
    units = []
    for ui in range(4):
        fns = []
        for fi in range(nfns // 4):
            fns.append({"name": "fn_%d_%d" % (ui, fi), "size": "64",
                        "fuzzy_match_percent": 100.0,
                        "metadata": {"virtual_address": str(0x80000000 + ui * 0x10000 + fi * 64)}})
        units.append({
            "name": "main/main/u%d" % ui,
            "measures": {"fuzzy_match_percent": 100.0, "matched_data": "1000",
                         "total_data": "1000"},
            "sections": [{"name": ".sdata2", "size": "84", "fuzzy_match_percent": 100.0}],
            "metadata": {"complete": True, "source_path": "src/main/u%d.c" % ui},
            "functions": fns,
        })
    return {"version": 2,
            "measures": {"fuzzy_match_percent": 100.0, "matched_data": "4000",
                         "total_data": "4000", "total_functions": nfns,
                         "complete_units": 4, "total_units": 4},
            "units": units}


def self_test(real_report=None):
    """Validate the differ in BOTH directions against ground truth before it is
    allowed to gate anything."""
    ok = True

    def chk(label, cond, detail=""):
        nonlocal ok
        if not cond:
            ok = False
        print("  %-62s %s %s" % (label, "PASS" if cond else "*** FAIL ***", detail))

    base_doc = _synthetic_doc()
    base = parse_report(copy.deepcopy(base_doc), path="synthetic-A", label="A")

    # NEGATIVE: a report against itself must be silent. A differ that cries on
    # identical input is as useless as one that never cries.
    d = diff_reports(base, parse_report(copy.deepcopy(base_doc), path="synthetic-B"))
    chk("identical reports diff clean", not d.red and not d.improved and not d.data)

    # POSITIVE 1: a per-function fuzzy drop.
    doc = copy.deepcopy(base_doc)
    doc["units"][1]["functions"][3]["fuzzy_match_percent"] = 98.333
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("per-function fuzzy drop is REGRESSED",
        len(d.regressed) == 1 and abs(d.regressed[0][2] - 98.333) < 1e-6)
    chk("a per-function drop turns the verdict RED", d.red)

    # POSITIVE 2: unit matched_data loss -- the axis the purge commits moved and
    # a matched_code gate cannot see.
    doc = copy.deepcopy(base_doc)
    doc["units"][2]["measures"]["matched_data"] = "880"
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("unit matched_data loss is reported",
        d.data == [("main/main/u2", 1000, 880)] and d.data_loss)
    chk("data loss alone turns the verdict RED",
        d.red and not d.regressed, "(no per-fn row, still RED)")

    # POSITIVE 3: demotion. Harmless on its own, but it is what blinds the
    # forced-link / DOL gate, so it must always surface.
    doc = copy.deepcopy(base_doc)
    doc["units"][0]["metadata"]["complete"] = False
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("demotion is reported", d.demoted == ["main/main/u0"])

    # POSITIVE 4: a demoted unit that ALSO lost data -- the exact 21b90aff9f
    # shape, where demotion hides the loss from every other gate.
    doc = copy.deepcopy(base_doc)
    doc["units"][0]["metadata"]["complete"] = False
    doc["units"][0]["measures"]["matched_data"] = "900"
    doc["units"][0]["functions"][0]["fuzzy_match_percent"] = 99.5
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("demoted unit that lost data AND score is RED on all three axes",
        d.red and d.demoted and d.data_loss and d.regressed)

    # POSITIVE 5: a vanished function.
    doc = copy.deepcopy(base_doc)
    del doc["units"][3]["functions"][7]
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("a vanished function is LOST", len(d.lost) == 1 and d.red)

    # NEGATIVE: a two-sided rename empties one key and fills another at the same
    # address and size. That is not a loss.
    doc = copy.deepcopy(base_doc)
    doc["units"][3]["functions"][7]["name"] = "fn_renamed"
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("a va+size-paired rename is not a loss",
        not d.lost and not d.new and len(d.renames) == 1 and not d.red)

    # POSITIVE 6: a rename that also dropped the score is still a regression.
    doc = copy.deepcopy(base_doc)
    doc["units"][3]["functions"][7]["name"] = "fn_renamed"
    doc["units"][3]["functions"][7]["fuzzy_match_percent"] = 90.0
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("a rename that dropped score is still REGRESSED",
        len(d.regressed) == 1 and d.red)

    # POSITIVE 7: objdiff OMITS fuzzy_match_percent at 0.0, so a function that
    # fell to zero looks like a well-formed row with a missing key.
    doc = copy.deepcopy(base_doc)
    del doc["units"][1]["functions"][2]["fuzzy_match_percent"]
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("a function that fell to 0% (key omitted) is NEW-UNSCORED",
        len(d.new_unscored) == 1 and d.red)

    # POSITIVE 8: a section that fell to 0 with no other change (the .sdata2
    # 0/84 shape) is visible.
    doc = copy.deepcopy(base_doc)
    del doc["units"][1]["sections"][0]["fuzzy_match_percent"]
    d = diff_reports(base, parse_report(doc, path="synthetic-B"))
    chk("a section falling to 0% is reported",
        any(s[1] == ".sdata2" and s[3] == 0.0 for s in d.sections))

    # BLIND SPOT 3. Everything above is derived from a score, and a pool
    # rotation inside an already-NonMatching unit moves no score at all: the
    # section is already short of 100 so matched_data (all-or-nothing per
    # section) does not move, nothing is demoted, and not one instruction
    # changed. The pool word-diff is the only sensor that fires, so it gets its
    # own controls in both directions.
    carve = struct.pack(">8I", 1, 2, 3, 4, 5, 6, 7, 8)
    same = carve
    rotated = struct.pack(">8I", 5, 6, 7, 8, 1, 2, 3, 4)   # same multiset
    lost_word = struct.pack(">8I", 1, 2, 3, 4, 5, 6, 7, 99)

    base_pool = {("u", ".sdata2"): compare_pool("u", ".sdata2", same, carve)}
    chk("an exact pool scores every word as a positional hit",
        base_pool[("u", ".sdata2")].hits == 8
        and base_pool[("u", ".sdata2")].missing == 0)

    rot = compare_pool("u", ".sdata2", rotated, carve)
    chk("a rotation preserves the word multiset (missing/extra stay 0)",
        rot.missing == 0 and rot.extra == 0)
    worse, better = diff_pools(base_pool, {("u", ".sdata2"): rot})
    chk("a pool ROTATION at flat multiset is reported worse",
        len(worse) == 1 and worse[0][1].unmatched > 0 and not better)

    d = diff_reports(base, parse_report(copy.deepcopy(base_doc), path="synthetic-B"))
    d.pool_worse, d.pool_better = worse, better
    chk("a pool rotation alone turns the verdict RED", d.red)

    worse, better = diff_pools(
        base_pool, {("u", ".sdata2"): compare_pool("u", ".sdata2", lost_word, carve)})
    chk("a pool WORD LOSS is reported worse",
        len(worse) == 1 and worse[0][1].missing == 1)

    worse, better = diff_pools(
        {("u", ".sdata2"): rot}, base_pool)
    chk("a pool moving TOWARD the carve is better, not worse",
        not worse and len(better) == 1 and better[0][1].unmatched == 0)

    # Growth past the carve is the freshly-minted-@N tell and counts against
    # us, but only when it did not also buy positional agreement -- a section
    # being filled in grows too.
    grew_only = compare_pool("u", ".sdata2", carve + struct.pack(">I", 77), carve)
    worse, better = diff_pools(base_pool, {("u", ".sdata2"): grew_only})
    chk("growth past the carve that filled nothing in is worse",
        len(worse) == 1 and worse[0][1].extra_nz == 1)
    thin = {("u", ".sdata2"): compare_pool("u", ".sdata2", carve[:8], carve)}
    worse, better = diff_pools(thin, {("u", ".sdata2"): grew_only})
    chk("growth that filled the section in is better, not worse",
        not worse and len(better) == 1, [repr(x[1]) for x in worse])

    worse, better = diff_pools(base_pool, dict(base_pool))
    chk("an unchanged pool is silent", not worse and not better)

    # THE POSITIONAL-HITS TRAP (lane A84). `hits` used to gate, and a pure SIZE
    # change slides the whole tail out of position without one word changing.
    # Both controls below are built from the same size shift; only the value
    # axis tells them apart, so the sensor must read the value axis.

    # NEGATIVE CONTROL: our section carried one surplus word that retail does
    # not have (a literal compiled at the wrong width). Deleting it is a strict
    # improvement, yet it slides every later word by one and costs almost all
    # the raw positional agreement. This is A83's engine/0 .sdata2 row.
    surplus_head = struct.pack(">9I", 0x40840000, 1, 2, 3, 4, 5, 6, 7, 8)
    fixed = carve                       # the surplus word removed
    before = compare_pool("u", ".sdata2", surplus_head, carve)
    after = compare_pool("u", ".sdata2", fixed, carve)
    chk("a value-neutral size shift shreds RAW positional hits",
        before.hits == 0 and after.hits == 8 and before.unmatched == 1
        and after.unmatched == 0,
        "hits %d -> %d, misplaced %d -> %d"
        % (before.hits, after.hits, before.unmatched, after.unmatched))
    chk("deleting a surplus word empties the multiset surplus",
        before.extra == 1 and after.extra == 0 and after.missing == 0)
    worse, better = diff_pools({("u", ".sdata2"): before},
                               {("u", ".sdata2"): after})
    chk("a size shift that is value-NEUTRAL-or-better is NOT reported worse",
        not worse and len(better) == 1, [repr(x[1]) for x in worse])
    d = diff_reports(base, parse_report(copy.deepcopy(base_doc), path="synthetic-B"))
    d.pool_worse, d.pool_better = worse, better
    chk("a correct pool fix does not turn the verdict RED", not d.red)
    chk("the value delta is printed, not just the counts",
        before.mdelta_str() == "{0x40840000:+1}" and after.mdelta_str() == "{}")

    # And the same shift in reverse -- MINTING a surplus word at the head --
    # must still be caught, so the negative control is not just a blanket
    # amnesty for anything that changes length.
    worse, better = diff_pools({("u", ".sdata2"): after},
                               {("u", ".sdata2"): before})
    chk("minting a surplus word at the head is still reported worse",
        len(worse) == 1 and worse[0][1].extra == 1 and not better)

    # POSITIVE CONTROL: a real VALUE regression hiding inside a size change.
    # One word is both moved and corrupted, so the length shift is identical to
    # the negative control above and only the multiset separates them.
    corrupt = struct.pack(">8I", 1, 2, 3, 4, 5, 6, 7, 0xDEADBEEF)
    bad = compare_pool("u", ".sdata2", corrupt, carve)
    worse, better = diff_pools({("u", ".sdata2"): before},
                               {("u", ".sdata2"): bad})
    chk("a real value regression inside a size change is CAUGHT",
        len(worse) == 1 and worse[0][1].missing == 1
        and worse[0][1].mdelta.get(8) == -1
        and worse[0][1].mdelta.get(0xDEADBEEF) == 1)
    d = diff_reports(base, parse_report(copy.deepcopy(base_doc), path="synthetic-B"))
    d.pool_worse, d.pool_better = worse, better
    chk("a value regression turns the verdict RED", d.red)

    # The order axis still fires with the multiset flat: `aligned` is what
    # catches blind spot 3 now, and it must be shift-invariant.
    chk("a rotation still misplaces words at a flat multiset",
        rot.unmatched > 0 and rot.missing == 0 and rot.extra == 0)
    slid = compare_pool("u", ".sdata2", struct.pack(">I", 0x40840000) + carve,
                        carve)
    chk("a pure head insertion misplaces exactly the inserted word",
        slid.unmatched == 1 and slid.hits < 8)

    # ZERO PADDING. Deleting a wrong-width literal also deletes the alignment
    # zeros it forced, so the raw shortfall GROWS on a correct fix. This is the
    # exact residue of A83's engine/0 row, reproduced here at full scale.
    pad_carve = struct.pack(">6I", 0x40490fdb, 0, 0, 1, 2, 3)
    pad_before = compare_pool(          # surplus literal + the zeros it forced
        "u", ".sdata2", struct.pack(">5I", 0x40840000, 0, 0, 1, 2), pad_carve)
    pad_after = compare_pool(           # literal deleted, its padding with it
        "u", ".sdata2", struct.pack(">3I", 0, 1, 2), pad_carve)
    chk("deleting a wrong-width literal GROWS the raw shortfall (zeros only)",
        pad_after.missing > pad_before.missing
        and pad_after.zero_delta < pad_before.zero_delta
        and pad_after.missing_nz == pad_before.missing_nz
        and pad_after.extra_nz < pad_before.extra_nz,
        "raw %d -> %d, nz %d -> %d" % (pad_before.missing, pad_after.missing,
                                       pad_before.missing_nz,
                                       pad_after.missing_nz))
    worse, better = diff_pools({("u", ".sdata2"): pad_before},
                               {("u", ".sdata2"): pad_after})
    chk("a fix whose only new shortfall is ZERO PADDING is better, not worse",
        not worse and len(better) == 1, [repr(x[1]) for x in worse])

    # ...but a nonzero constant lost in the same shape is still caught, and so
    # is a lost 0.0f -- that one via the alignment axis, which no zero-word
    # amnesty touches.
    lost_const = compare_pool(
        "u", ".sdata2", struct.pack(">2I", 0, 1), pad_carve)
    worse, better = diff_pools({("u", ".sdata2"): pad_after},
                               {("u", ".sdata2"): lost_const})
    chk("a nonzero constant lost under the same shape is still WORSE",
        len(worse) == 1 and worse[0][1].missing_nz > pad_after.missing_nz)
    full = compare_pool("u", ".sdata2", pad_carve, pad_carve)
    lost_zero = compare_pool("u", ".sdata2",
                             struct.pack(">5I", 0x40490fdb, 0, 1, 2, 3),
                             pad_carve)
    worse, better = diff_pools({("u", ".sdata2"): full},
                               {("u", ".sdata2"): lost_zero})
    chk("a 0.0f deleted with NOTHING else improving is still caught",
        len(worse) == 1 and lost_zero.missing_nz == 0
        and lost_zero.zero_delta == -1, [repr(x[1]) for x in worse])

    chk("a report-only endpoint reports the pool diff as NOT RUN",
        diff_pools(None, base_pool) == ([], [])
        and not diff_reports(base, base).pools_read)

    # THE SCHEMA GUARD. This is the failure that produced the false zeros: a
    # loader reaching for function["measures"]["fuzzy_match_percent"] finds an
    # empty dict, scores every function the same sentinel, and reports zero
    # regressions against ANY input. The parser must refuse, not report clean.
    doc = copy.deepcopy(base_doc)
    for u in doc["units"]:
        for f in u["functions"]:
            f["measures"] = {"fuzzy_match_percent": f.pop("fuzzy_match_percent")}
    try:
        parse_report(doc, path="schema-drift")
        chk("schema drift (scores moved under 'measures') is REFUSED", False,
            "parsed a report it cannot score")
    except SchemaError:
        chk("schema drift (scores moved under 'measures') is REFUSED", True)

    for label, mutate in (
        ("empty units list", lambda dd: dd.__setitem__("units", [])),
        ("no top-level measures", lambda dd: dd.pop("measures")),
        ("truncated report (too few functions)",
         lambda dd: dd.__setitem__("units", dd["units"][:1])),
    ):
        doc = copy.deepcopy(base_doc)
        try:
            mutate(doc)
            parse_report(doc, path="broken")
            chk("%s is REFUSED" % label, False, "parsed anyway")
        except SchemaError:
            chk("%s is REFUSED" % label, True)
        except Exception as exc:  # a crash is not a refusal
            chk("%s is REFUSED" % label, False, repr(exc))

    # Ground truth: the real tree, if we were handed one.
    if real_report and os.path.exists(real_report):
        r = load_report(real_report, label="real")
        chk("real report parses", len(r.fns) > 9000, "%d functions" % len(r.fns))
        chk("real report's only unscored rows are the 3 PS islands",
            r.unscored == STUCK_UNSCORED, sorted(r.unscored - STUCK_UNSCORED) or "")
        d = diff_reports(r, r)
        chk("real report against itself is clean", not d.red and not d.improved)
        # And the same injection on real data.
        doc = json.load(open(real_report))
        for u in doc["units"]:
            if u.get("functions"):
                u["functions"][0]["fuzzy_match_percent"] = 1.0
                u["measures"]["matched_data"] = "0"
                break
        d = diff_reports(r, parse_report(doc, path="real-mutated"))
        chk("injected loss in the REAL report is caught",
            bool(d.regressed) and bool(d.data_loss) and d.red)

        # And the pool sensor against the real build tree, not a synthetic one:
        # it must read a real fingerprint, be silent against itself, and catch a
        # rotation injected into it.
        wt = os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(real_report))))
        try:
            pools = pool_fingerprint(wt, r)
        except (SchemaError, OSError) as exc:
            pools = None
            chk("real build tree yields a pool fingerprint", False, str(exc)[:70])
        if pools:
            chk("real build tree yields a pool fingerprint", True,
                "%d data sections" % len(pools))
            chk("real fingerprint against itself is silent",
                diff_pools(pools, dict(pools)) == ([], []))
            victim = max(pools.values(), key=lambda s: s.hits)
            spun = dict(pools)
            spun[victim.key] = PoolStat(victim.unit, victim.section, 0,
                                        victim.ours, victim.retail,
                                        victim.missing, victim.extra)
            worse, _b = diff_pools(pools, spun)
            chk("a rotation injected into the REAL fingerprint is caught",
                len(worse) == 1 and worse[0][1].key == victim.key)

    print("\nself-test: %s" % ("PASS" if ok else "FAIL"))
    return ok


# ---------------------------------------------------------------------------
# integrity scan
# ---------------------------------------------------------------------------

def integrity_scan(report, out=sys.stdout):
    """A unit marked complete must actually score 100 on every axis. A unit
    that claims completeness while scoring below it is a lie the DOL gate will
    eventually trip over."""
    bad = []
    for name, u in report.units.items():
        if not u.complete:
            continue
        fns = [f for f in report.fns.values() if f.unit == name]
        low = [f for f in fns if f.score < 100.0 - EPS]
        if low:
            bad.append((name, low))
    out.write("\nINTEGRITY: %d units marked complete while scoring < 100\n" % len(bad))
    for name, low in bad:
        out.write("  BAD   %-56s %s\n"
                  % (name, ", ".join("%s=%.3f" % (f.name, f.score) for f in low[:4])))
    return bad


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--self-test", action="store_true",
                      help="validate the differ against ground truth and exit")
    mode.add_argument("--reports", nargs=2, metavar=("A", "B"),
                      help="diff two report.json files that already exist")
    mode.add_argument("--commits", nargs=2, metavar=("A", "B"),
                      help="full rebuild at both shas, then diff")
    mode.add_argument("--head", action="store_true",
                      help="shorthand for --commits HEAD^ HEAD")
    mode.add_argument("--integrity-only", metavar="REPORT",
                      help="scan one report.json for units marked complete "
                           "while scoring below 100, and exit")
    ap.add_argument("--work-dir", default="/private/tmp",
                    help="where throwaway worktrees are created")
    ap.add_argument("--prefix", default="sdgate",
                    help="worktree name prefix")
    ap.add_argument("--keep", action="store_true",
                    help="keep the throwaway worktrees for follow-up probing")
    ap.add_argument("--reuse", nargs=2, metavar=("SHA", "DIR"), action="append",
                    default=[], help="an already-built worktree to use for a sha")
    ap.add_argument("--compilers", default=os.path.join(REPO, "build", "compilers"))
    ap.add_argument("--binutils", default=os.path.join(REPO, "build", "binutils"))
    ap.add_argument("--tools", default=os.path.join(REPO, "build", "tools"))
    ap.add_argument("--no-positive-control", action="store_true",
                    help="skip the control; the verdict is then UNPROVEN")
    ap.add_argument("--integrity", action="store_true",
                    help="also scan endpoint B for complete units scoring < 100")
    ap.add_argument("--no-dol-gate", action="store_true",
                    help="skip the forced-link / DOL byte-identity check at B")
    ap.add_argument("--real-report", default=os.path.join(
        REPO, "build", VERSION, "report.json"),
        help="a real report.json for --self-test to validate against")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if self_test(args.real_report) else 3

    if args.integrity_only:
        r = load_report(args.integrity_only, label=args.integrity_only)
        print("%s: tree %s  matched_data %s  %s/%s units complete  %d functions"
              % (args.integrity_only, r.measures.get("fuzzy_match_percent"),
                 r.measures.get("matched_data"), r.measures.get("complete_units"),
                 r.measures.get("total_units"), len(r.fns)))
        unexpected = r.unscored - STUCK_UNSCORED
        print("UNSCORED: %d (%d unexpected)%s"
              % (len(r.unscored), len(unexpected),
                 "" if not unexpected else " " + ", ".join(sorted(unexpected))))
        bad = integrity_scan(r)
        return 1 if (bad or unexpected) else 0

    if args.reports:
        a = load_report(args.reports[0], label=args.reports[0])
        b = load_report(args.reports[1], label=args.reports[1])
        # A report.json sits at <tree>/build/GSAE01/report.json, so its tree is
        # three levels up. When both are still on disk the pool word-diff runs
        # here too; when they are not, render() says so instead of reading zero.
        for rep, path in ((a, args.reports[0]), (b, args.reports[1])):
            wt = os.path.dirname(os.path.dirname(os.path.dirname(
                os.path.abspath(path))))
            try:
                rep.pools = pool_fingerprint(wt, rep)
            except SchemaError:
                rep.pools = None
        if not args.no_positive_control:
            print("differ-layer control:")
            if not self_test(None):
                print("\nPOSITIVE CONTROL FAILED -- verdict withheld.")
                return 3
            print("")
        else:
            print("WARNING: control skipped; this verdict is UNPROVEN.\n")
        d = render(diff_reports(a, b), a, b)
        if args.integrity:
            integrity_scan(b)
        return 1 if d.red else 0

    if args.head:
        sha_b = git(["rev-parse", "HEAD"]).stdout.strip()
        sha_a = git(["rev-parse", "HEAD^"]).stdout.strip()
    else:
        sha_a = git(["rev-parse", args.commits[0]]).stdout.strip()
        sha_b = git(["rev-parse", args.commits[1]]).stdout.strip()
    print("A = %s\nB = %s\n" % (sha_a, sha_b))

    print("differ-layer control:")
    if not self_test(None):
        print("\nPOSITIVE CONTROL FAILED -- verdict withheld.")
        return 3
    print("")

    reuse = dict(args.reuse)
    made = []
    reports = {}
    try:
        for label, sha in (("A", sha_a), ("B", sha_b)):
            wt = reuse.get(sha)
            if wt:
                print("%s: reusing %s" % (label, wt))
                assert_fresh(wt)
            else:
                wt = os.path.join(args.work_dir, "%s_%s_%s"
                                  % (args.prefix, label, sha[:10]))
                make_worktree(sha, wt)
                made.append(wt)
                provision(wt, args.compilers, args.binutils, args.tools)
                build(wt)
            rp = report_path(wt)
            rep = load_report(rp, label="%s (%s)" % (sha[:10], wt))
            rep.pools = pool_fingerprint(wt, rep)
            print("  %s: pool fingerprint over %d data sections" % (label, len(rep.pools)))
            reports[label] = (rep, wt)

        if not args.no_positive_control:
            print("\nbuild-layer control at endpoint B:")
            positive_control(reports["B"][1], reports["B"][0])
            print("")
        else:
            print("\nWARNING: build-layer control skipped; verdict is UNPROVEN.\n")

        dol_ok = True
        if not args.no_dol_gate:
            print("forced-link gate at endpoint B:")
            dol_ok, _got, _want = dol_gate(reports["B"][1])
            print("")

        d = render(diff_reports(reports["A"][0], reports["B"][0]),
                   reports["A"][0], reports["B"][0])
        if not dol_ok:
            print("VERDICT OVERRIDDEN: RED -- main.dol no longer matches retail\n")
        if args.integrity:
            integrity_scan(reports["B"][0])
        return 1 if (d.red or not dol_ok) else 0
    except ControlError as exc:
        print("\nPOSITIVE CONTROL FAILED: %s" % exc)
        return 3
    finally:
        if not args.keep:
            for wt in made:
                shutil.rmtree(wt, ignore_errors=True)
                git(["worktree", "prune"], check=False)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BuildError, SchemaError) as _exc:
        print("\nERROR: %s" % _exc)
        sys.exit(2)
