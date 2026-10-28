"""Repo-wide census of units whose functions disagree on optimizer profile.

For each unit it recompiles the whole TU under a set of candidate -opt/-inline
profiles and records, per function, which profiles reproduce the retail bytes.
A unit is classified as

  CLEAN         every function already matches as-configured
  FLAG          some single profile matches strictly more functions than the
                configured one and breaks none of the ones that match today
  CONFLICT      no single profile dominates: function A only matches under P,
                function B only matches under Q, P != Q
  STRUCTURAL    functions still short of retail under every profile, but no
                profile disagreement (a source defect, not a flag defect)

Gates on extracted function bytes; a failed compile is ERR, never a match.
Output is JSON on stdout (one object per unit) for downstream ranking.

Sweep result (all 102 units carrying an imperfect function, 16 absolute
profiles here plus 20 per-unit delta profiles in tu_profile_census2.py):

    CLEAN         0
    FLAG          0      no unit is fixed outright by a different TU flag
    CONFLICT     10      functions of one unit want different profiles
    STRUCTURAL   92      no profile reaches retail: a source defect

Adjudicating the 10 conflicts against the retail DOL (tools/interning_adjudicate.py
plus tools/tu_split_geometry.py) gives 8 proven-single, 2 undecided, 0 boundary
errors, so a profile conflict is NOT a boundary-error detector: it is evidence
that one object really does carry two optimisation shapes.  Do not split on it.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fnbytes import find_unit, objdump_path
from function_objdump import load_units

REPO = Path(__file__).resolve().parent.parent

PROFILES: dict[str, tuple[str | None, str | None]] = {
    "as-configured": (None, None),
    "L4": ("nopeephole,noschedule", None),
    "L3": ("nopeephole,noschedule,level=3", None),
    "L2": ("nopeephole,noschedule,level=2", None),
    "L1": ("nopeephole,noschedule,level=1", None),
    "noprop": ("nopeephole,noschedule,nopropagation", None),
    "nocse": ("nopeephole,noschedule,nocse", None),
    "noloopinv": ("nopeephole,noschedule,noloopinvariants", None),
    "nolifetimes": ("nopeephole,noschedule,nolifetimes", None),
    "nostrength": ("nopeephole,noschedule,nostrength", None),
    "nodead": ("nopeephole,noschedule,nodead", None),
    "L4+noauto": ("nopeephole,noschedule", "noauto"),
    "L3+noauto": ("nopeephole,noschedule,level=3", "noauto"),
    "L1+noauto": ("nopeephole,noschedule,level=1", "noauto"),
    "noprop+noauto": ("nopeephole,noschedule,nopropagation", "noauto"),
    "nocse+noauto": ("nopeephole,noschedule,nocse", "noauto"),
}

SYM_RE = re.compile(r"^[0-9a-f]+ <(.+)>:$")
INSN_RE = re.compile(r"^\s*[0-9a-f]+:\t((?:[0-9a-f]{2} )+)\s*\t")


def ninja_cflags(obj_path: str) -> tuple[list[str], str]:
    text = ninja_cflags.text  # type: ignore[attr-defined]
    for cand in (obj_path, obj_path.replace("/", "\\")):
        idx = text.find(f"build {cand}: ")
        if idx != -1:
            break
    else:
        raise LookupError(f"no build statement for {obj_path}")
    end = len(text)
    for m in re.finditer(r"^(?:build|rule) ", text[idx + 1:], re.M):
        end = idx + 1 + m.start()
        break
    block = text[idx:end].replace("$\n", " ")
    cflags = re.search(r"^\s*cflags\s*=\s*(.*)$", block, re.M)
    mwv = re.search(r"^\s*mw_version\s*=\s*(\S+)\s*$", block, re.M)
    if not cflags or not mwv:
        raise LookupError(f"could not recover cflags for {obj_path}")
    return shlex.split(cflags.group(1)), mwv.group(1)


ninja_cflags.text = (REPO / "build.ninja").read_text()  # type: ignore[attr-defined]


def substitute(flags: list[str], opt: str | None, inline: str | None) -> list[str]:
    out = list(flags)
    for key, val in (("-opt", opt), ("-inline", inline)):
        if val is None:
            continue
        if key in out:
            out[out.index(key) + 1] = val
        else:
            out += [key, val]
    return out


def compile_probe(src: str, flags: list[str], mwv: str, outdir: Path) -> Path | None:
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [] if os.name == "nt" else [str(REPO / "build/tools/wibo")]
    cmd += [
        str(REPO / "build/tools/sjiswrap.exe"),
        str(REPO / f"build/compilers/{mwv}/mwcceppc.exe"),
        *flags, "-c", src, "-o", str(outdir),
    ]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    obj = outdir / (Path(src).stem + ".o")
    if obj.is_file() and proc.returncode == 0:
        return obj
    return None


def dump_functions(objdump: Path, obj: Path) -> dict[str, bytes]:
    """symbol -> concatenated instruction bytes, one objdump call for the object."""
    proc = subprocess.run(
        [str(objdump), "-M", "gekko", "-drz", "--section=.text", str(obj)],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        return {}
    out: dict[str, bytearray] = {}
    cur: bytearray | None = None
    for line in proc.stdout.splitlines():
        m = SYM_RE.match(line)
        if m:
            cur = out.setdefault(m.group(1), bytearray())
            continue
        if cur is None:
            continue
        mi = INSN_RE.match(line)
        if mi:
            cur += bytes.fromhex(mi.group(1).replace(" ", ""))
    return {k: bytes(v) for k, v in out.items() if v}


def probe_unit(unit: dict, objdump: Path, profiles: list[str]) -> dict:
    src = unit["name"]
    if not (REPO / src).is_file():
        src = "src/" + src
    if not (REPO / src).is_file():
        return {"unit": unit["name"], "status": "NOSRC"}
    target = REPO / unit["object"]
    obj_rel = unit["object"].replace("build/GSAE01/obj/", "build/GSAE01/src/")
    try:
        flags, mwv = ninja_cflags(obj_rel)
    except LookupError as exc:
        return {"unit": unit["name"], "status": "NOFLAGS", "detail": str(exc)}

    want = dump_functions(objdump, target)
    if not want:
        return {"unit": unit["name"], "status": "NOTARGET"}

    stem = Path(src).stem
    matches: dict[str, list[str]] = {s: [] for s in want}
    ok_profiles: list[str] = []
    for pname in profiles:
        opt, inline = PROFILES[pname]
        outdir = REPO / "build/tu_profile_census" / stem / pname
        obj = compile_probe(src, substitute(flags, opt, inline), mwv, outdir)
        if obj is None:
            continue
        got = dump_functions(objdump, obj)
        if not got:
            continue
        ok_profiles.append(pname)
        for sym, bs in want.items():
            if got.get(sym) == bs:
                matches[sym].append(pname)
        try:
            obj.unlink()
        except OSError:
            pass

    cfg = "as-configured"
    sizes = {s: len(b) for s, b in want.items()}
    cfg_ok = {s for s, ps in matches.items() if cfg in ps}
    any_ok = {s for s, ps in matches.items() if ps}
    never = {s for s in want if not matches[s]}

    # what would each profile buy, and what would it break
    per_profile = {}
    for p in ok_profiles:
        got = {s for s, ps in matches.items() if p in ps}
        per_profile[p] = {
            "n": len(got),
            "gain": sorted(got - cfg_ok),
            "loss": sorted(cfg_ok - got),
            "gain_bytes": sum(sizes[s] for s in got - cfg_ok),
            "loss_bytes": sum(sizes[s] for s in cfg_ok - got),
        }

    fixable = sorted(any_ok - cfg_ok)
    dominating = [p for p, d in per_profile.items()
                  if p != cfg and not d["loss"] and d["gain"]]
    if not fixable:
        status = "CLEAN" if not never else "STRUCTURAL"
    elif dominating:
        status = "FLAG"
    else:
        status = "CONFLICT"

    return {
        "unit": unit["name"],
        "status": status,
        "nfuncs": len(want),
        "cfg_ok": len(cfg_ok),
        "fixable": fixable,
        "fixable_bytes": sum(sizes[s] for s in fixable),
        "never": len(never),
        "never_bytes": sum(sizes[s] for s in never),
        "dominating": dominating,
        "per_profile": per_profile,
        "matches": {s: ps for s, ps in matches.items() if ps != [cfg] and ps},
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("units", nargs="*")
    ap.add_argument("--profiles", default=",".join(PROFILES))
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    profiles = [p for p in args.profiles.split(",") if p in PROFILES]
    units = load_units(REPO / "build/GSAE01/config.json")
    objdump = objdump_path()

    sink = open(args.out, "w") if args.out else sys.stdout
    for q in args.units:
        try:
            unit = find_unit(units, q)
        except LookupError as exc:
            print(json.dumps({"unit": q, "status": "NOUNIT", "detail": str(exc)}), file=sink, flush=True)
            continue
        res = probe_unit(unit, objdump, profiles)
        print(json.dumps(res), file=sink, flush=True)
    if args.out:
        sink.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
