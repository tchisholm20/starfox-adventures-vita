"""Sweep TU-level compiler profiles for whole units and rank them by residual.

Per-function optimization pragmas are banned (see CLAUDE.md); the only sanctioned
way to give a translation unit a different optimization profile is a TU-level
cflag entry in `configure.py`.  Finding the profile that actually helps means
trying them.  This tool does that mechanically: it takes a unit's REAL ninja
compile command, mutates the flags, compiles to a scratch directory, and scores
the result with `tools/resid_rank.py`'s link-equivalence screen.

    python3 tools/flag_sweep.py main/objprint.c              # 26 single profiles
    python3 tools/flag_sweep.py --pairs main/objprint.c      # 21 -opt subflag pairs
    python3 tools/flag_sweep.py --versions main/objprint.c   # compiler versions only
    python3 tools/flag_sweep.py --all --jobs 8               # every NonMatching unit
    python3 tools/flag_sweep.py --list                       # show profile names

Nothing is written to the tree: `configure.py` is never touched and the unit's
real `.o` is never overwritten.  A hit here is a LEAD, not a landing.

!! TWO-METRIC RULE -- WHY THIS TOOL PRINTS TWO NUMBERS !!

    resid   raw bytes plus relocation differences mwld would still have to
            reconcile (`bytes + 4*relocs`, from tools/resid_rank.py).  Only
            residual ZERO flips a unit to `complete`.
    fuzzy   the unit's real `fuzzy_match_percent`, computed by running
            objdiff-cli over a one-unit throwaway project whose base object is
            the candidate build.  It reproduces report.json's number exactly.

    The two move INDEPENDENTLY and routinely in OPPOSITE directions -- one
    observed sweep cut a unit's residual 4259 -> 1803 while fuzzy collapsed
    99.478 -> 74.51.  Optimization-disabling flags are especially prone to it.

    A profile is landable only if it improves one metric without regressing the
    other.  Verdicts printed per hit:

        FLIP        residual reached zero -- the unit can become `complete`
        FUZZY       fuzzy up, residual not worse -- land it
        FUZZY-ONLY  fuzzy up but residual worse -- land it only once the unit is
                    known to be unflippable anyway (e.g. it carries an .sdata2
                    retail's `.o` does not have, so the pool can never be
                    claimed); otherwise it trades away the flip
        RESID-ONLY  residual down but fuzzy down too -- REJECT, it is a trap

    Even a FLIP/FUZZY hit must be confirmed by editing `configure.py`,
    rebuilding and regenerating a FULL `report.json` before it is committed.
"""
from __future__ import annotations

import argparse
import itertools
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(os.environ.get("SFA_ROOT", Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(ROOT / "tools"))

import resid_rank as RR  # noqa: E402

OBJDIFF = ROOT / "build/tools/objdiff-cli"

# -opt subflags worth toggling; the pairwise mode takes combinations of these.
OPT_SUBFLAGS = ["nocse", "nodeadstore", "nolifetimes", "noloopinvariants",
                "nostrength", "nopropagation", "nodead"]
# The versions the SFA build plausibly used; swept alongside the flag profiles.
VERSIONS = ["1.2.5n", "1.3", "1.3.2", "2.0", "2.5", "2.6", "2.7"]
# The rest of build/compilers/GC, swept only by --versions.
VERSIONS_EXTRA = ["1.0", "1.1", "1.1p1", "1.2.5", "1.3.2r", "2.0p1",
                  "3.0a3", "3.0a3.2", "3.0a3.3", "3.0a3.4", "3.0a3p1",
                  "3.0a5", "3.0a5.2"]


def add_opt(*extras):
    def m(toks):
        toks = list(toks)
        if "-opt" in toks:
            i = toks.index("-opt")
            cur = toks[i + 1].split(",")
            toks[i + 1] = ",".join(cur + [e for e in extras if e not in cur])
        else:
            toks = toks[:1] + ["-opt", ",".join(extras)] + toks[1:]
        return toks
    return m


def del_opt(sub):
    def m(toks):
        toks = list(toks)
        if "-opt" in toks:
            i = toks.index("-opt")
            parts = [p for p in toks[i + 1].split(",") if p != sub]
            if parts:
                toks[i + 1] = ",".join(parts)
            else:
                del toks[i:i + 2]
        return toks
    return m


def set_olevel(level):
    def m(toks):
        toks = [t for t in toks if not re.match(r"^-O[0-4]", t)]
        return toks[:1] + [level] + toks[1:]
    return m


def set_inline(value):
    def m(toks):
        toks = list(toks)
        while "-inline" in toks:
            i = toks.index("-inline")
            del toks[i:i + 2]
        if value:
            toks = toks[:1] + ["-inline", value] + toks[1:]
        return toks
    return m


def set_version(ver):
    pat = re.compile(r"build/compilers/GC/[^/]+/")
    def m(toks):
        return [pat.sub("build/compilers/GC/%s/" % ver, t) for t in toks]
    return m


def _single_profiles():
    p = [("+" + s, add_opt(s)) for s in OPT_SUBFLAGS]
    p += [("-nopeephole", del_opt("nopeephole")), ("+nopeephole", add_opt("nopeephole")),
          ("-noschedule", del_opt("noschedule")), ("+noschedule", add_opt("noschedule"))]
    p += [(lvl.lstrip("-").replace(",", ""), set_olevel(lvl))
          for lvl in ("-O4,p", "-O3,p", "-O2,p", "-O4")]
    p += [("inl_noauto", set_inline("noauto")), ("inl_auto", set_inline("auto")),
          ("inl_nodef", set_inline("noauto,deferred")), ("inl_off", set_inline("off"))]
    return p


def _version_profiles(extra=False):
    vs = VERSIONS + VERSIONS_EXTRA if extra else VERSIONS
    return [("v" + v, set_version(v)) for v in vs]


def _pair_profiles():
    return [("+".join(c), add_opt(*c)) for c in itertools.combinations(OPT_SUBFLAGS, 2)]


def profiles_for(mode):
    if mode == "pairs":
        return _pair_profiles()
    if mode == "versions":
        return _version_profiles(extra=True)
    return _single_profiles() + _version_profiles()


def base_command(obj):
    """The unit's real mwcc invocation, straight out of ninja."""
    out = subprocess.run(["ninja", "-t", "commands", obj], cwd=ROOT,
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "mwcceppc.exe" in line:
            return line.split("&&")[0].strip()
    return None


def fuzzy_of(candidate, unit_entry, workdir):
    """Real fuzzy_match_percent for one candidate object, via objdiff-cli.

    A one-unit throwaway project with base_path pointed at the candidate build
    reproduces report.json's number for that unit exactly.
    """
    entry = dict(unit_entry)
    entry["target_path"] = str(ROOT / entry["target_path"])
    entry["base_path"] = str(candidate)
    proj = os.path.join(workdir, "proj")
    os.makedirs(proj, exist_ok=True)
    with open(os.path.join(proj, "objdiff.json"), "w") as fh:
        json.dump({"min_version": "2.0.0-beta.5", "units": [entry]}, fh)
    out = os.path.join(workdir, "one.json")
    subprocess.run([str(OBJDIFF), "report", "generate", "-p", proj, "-o", out],
                   capture_output=True, text=True)
    try:
        with open(out) as fh:
            return json.load(fh)["units"][0]["measures"].get("fuzzy_match_percent", 0.0)
    except Exception:
        return None


def compile_and_score(cmd, unit, mutate, target, workdir, unit_entry):
    toks = shlex.split(cmd, posix=os.name != "nt")
    if os.name == "nt":
        toks = [
            token[1:-1]
            if len(token) >= 2 and token[0] == token[-1] and token[0] in {'"', "'"}
            else token
            for token in toks
        ]
    for i, t in enumerate(toks):
        if t == "-o":
            toks[i + 1] = str(workdir) + "/"
    toks = [t for t in toks if t != "-MMD"]
    toks = mutate(toks)
    for f in os.listdir(workdir):
        if f.endswith(".o"):
            os.remove(os.path.join(workdir, f))
    subprocess.run(toks, cwd=ROOT, capture_output=True, text=True)
    obj = os.path.join(workdir, os.path.basename(unit)[:-2] + ".o")
    if not os.path.exists(obj):
        return None
    try:
        screen = RR.screen(obj, target)
        nbytes, nrelocs = screen[:2]
    except Exception:
        return None
    fz = fuzzy_of(obj, unit_entry, workdir) if unit_entry else None
    return nbytes + 4 * nrelocs, nbytes, nrelocs, fz


def verdict(base, got):
    """FLIP / FUZZY / FUZZY-ONLY / RESID-ONLY, per the two-metric rule."""
    if got[0] == 0:
        return "FLIP"
    if base[3] is None or got[3] is None:
        return "RESID-ONLY"
    up = got[3] > base[3] + 1e-6
    if got[0] < base[0]:
        return "FUZZY" if got[3] >= base[3] - 1e-6 else "RESID-ONLY"
    if got[0] == base[0]:
        return "FUZZY" if up else None
    return "FUZZY-ONLY" if up else None


def sweep_unit(unit, obj, target, profs, top, unit_entry, keep_traps):
    cmd = base_command(obj)
    if not cmd:
        return f"{unit}: NO COMPILE COMMAND"
    workdir = tempfile.mkdtemp(prefix="flagsweep.")
    try:
        base = compile_and_score(cmd, unit, lambda t: t, target, workdir, unit_entry)
        if base is None:
            return f"{unit}: BASELINE BUILD FAILED"
        hits = []
        for name, mutate in profs:
            got = compile_and_score(cmd, unit, mutate, target, workdir, unit_entry)
            if got is None:
                continue
            if got[0] >= base[0] and not (got[3] and base[3] and got[3] > base[3] + 1e-6):
                continue
            v = verdict(base, got)
            if v is None or (v == "RESID-ONLY" and not keep_traps):
                continue
            rank = {"FLIP": 0, "FUZZY": 1, "FUZZY-ONLY": 2}.get(v, 3)
            hits.append((rank, -(got[3] or 0), got[0], name, got, v))
        hits.sort()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    bf = "%.5f" % base[3] if base[3] is not None else "?"
    body = "  ".join(
        "%s[%s resid=%d fuzzy=%s]" % (n, v, g[0], "%.5f" % g[3] if g[3] is not None else "?")
        for _, _, _, n, g, v in hits[:top]) or "-"
    return f"{unit}: base resid={base[0]}({base[1]}b/{base[2]}r) fuzzy={bf}\n    {body}"


def objdiff_units():
    with open(ROOT / "objdiff.json") as fh:
        return {u["base_path"].replace("\\", "/"): u for u in json.load(fh).get("units", [])
                if u.get("base_path")}


def resolve(selectors, only_incomplete):
    entries = objdiff_units()
    for name, ours, target, (done, _fz, _s1) in RR.units(tuple(selectors)):
        if only_incomplete and done:
            continue
        obj = str(Path(ours).relative_to(ROOT))
        yield name, obj, target, entries.get(obj.replace("\\", "/"))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("units", nargs="*", help="unit name or substring, e.g. main/objprint.c")
    ap.add_argument("--all", action="store_true",
                    help="sweep every NonMatching unit instead of a named list")
    ap.add_argument("--pairs", action="store_true",
                    help="sweep the 21 pairwise -opt subflag combinations")
    ap.add_argument("--versions", action="store_true",
                    help="sweep compiler versions only")
    ap.add_argument("--top", type=int, default=5, help="hits to print per unit")
    ap.add_argument("--jobs", type=int, default=1, help="units to sweep in parallel")
    ap.add_argument("--list", action="store_true", help="print profile names and exit")
    ap.add_argument("--traps", action="store_true",
                    help="also print RESID-ONLY hits (residual down, fuzzy down)")
    args = ap.parse_args()

    mode = "pairs" if args.pairs else "versions" if args.versions else "single"
    profs = profiles_for(mode)
    if args.list:
        print(" ".join(n for n, _ in profs))
        return
    if not args.units and not args.all:
        ap.error("name at least one unit, or pass --all")

    targets = list(resolve(args.units, args.all))
    if not targets:
        ap.error("no unit matched")

    def work(t):
        return sweep_unit(t[0], t[1], t[2], profs, args.top, t[3], args.traps)

    if args.jobs > 1:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for line in pool.map(work, targets):
                print(line)
                sys.stdout.flush()
    else:
        for t in targets:
            print(work(t))
            sys.stdout.flush()


if __name__ == "__main__":
    main()
