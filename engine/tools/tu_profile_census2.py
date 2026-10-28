"""Second-pass profile census: probes expressed as DELTAS from the unit's own
configured -opt/-inline setting rather than as an absolute profile.

Pass 1 pinned every probe to `nopeephole,noschedule,...`, which silently moved
two axes at once for the 216 units the build compiles at plain -O4,p.  Here each
probe adds exactly one pass-disable token to (or changes the level of) whatever
the unit is configured with, so a divergence names one knob.

Same classification and output shape as tu_profile_census.py.
"""
from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tu_profile_census as C
from fnbytes import find_unit, objdump_path
from function_objdump import load_units

REPO = C.REPO

ADD_TOKENS = ["nopropagation", "nocse", "noloopinvariants", "nolifetimes",
              "nostrength", "nodead", "nopeephole", "noschedule"]
LEVELS = ["level=1", "level=2", "level=3", "level=4"]
INLINES = ["noauto", "off", "auto", "auto,deferred"]


def unit_profiles(flags: list[str]) -> dict[str, tuple[str | None, str | None]]:
    opt = flags[flags.index("-opt") + 1] if "-opt" in flags else ""
    cur = [t for t in opt.split(",") if t]
    inl = None
    for i, x in enumerate(flags):
        if x == "-inline":
            inl = flags[i + 1]
    out: dict[str, tuple[str | None, str | None]] = {"as-configured": (None, None)}
    for t in ADD_TOKENS:
        if t in cur:
            continue
        out["+" + t] = (",".join(cur + [t]), None)
    base = [t for t in cur if not t.startswith("level=")]
    for lv in LEVELS:
        if lv in cur or (lv == "level=4" and not any(t.startswith("level=") for t in cur)):
            continue
        out["@" + lv] = (",".join(base + [lv]), None)
    for i in INLINES:
        if i != inl:
            out["inline:" + i] = (None, i)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("units", nargs="+")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    units = load_units(REPO / "build/GSAE01/config.json")
    objdump = objdump_path()
    sink = open(args.out, "w") if args.out else sys.stdout
    for q in args.units:
        try:
            unit = find_unit(units, q)
        except LookupError as exc:
            print(json.dumps({"unit": q, "status": "NOUNIT", "detail": str(exc)}),
                  file=sink, flush=True)
            continue
        obj_rel = unit["object"].replace("build/GSAE01/obj/", "build/GSAE01/src/")
        try:
            flags, _ = C.ninja_cflags(obj_rel)
        except LookupError as exc:
            print(json.dumps({"unit": unit["name"], "status": "NOFLAGS"}), file=sink, flush=True)
            continue
        profs = unit_profiles(flags)
        saved = C.PROFILES
        C.PROFILES = profs
        try:
            res = C.probe_unit(unit, objdump, list(profs))
        finally:
            C.PROFILES = saved
        res["configured"] = shlex.join(
            [flags[i] + " " + flags[i + 1] for i, x in enumerate(flags) if x in ("-opt", "-inline")])
        print(json.dumps(res), file=sink, flush=True)
    if args.out:
        sink.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
