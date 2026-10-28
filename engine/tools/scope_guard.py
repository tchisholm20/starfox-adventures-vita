#!/usr/bin/env python3
"""Refuse a scan whose scope matched nothing.

A screen that answers `scanned=0 ... =0` and exits 0 gives a typo'd unit name
the same shape of answer as a clean tree, and the operator reads it as clean.
Nine screens in this directory did exactly that; `tools/vacuity_audit.py
--family scope` is the mutation control that found them and keeps them honest.

An unfiltered run is never guarded -- scanning the whole tree and finding
nothing IS the answer there.  Only an explicit filter that selects no unit is
an error, because the caller asked about something that does not exist.
"""
import sys


def require_nonempty(filters, names, tool=None):
    """Exit 2 if `filters` is non-empty and matches none of `names`."""
    if not filters:
        return
    for n in names:
        if any(f in n for f in filters):
            return
    who = tool or (sys.argv[0].rsplit("/", 1)[-1] if sys.argv else "screen")
    sys.stderr.write(
        "%s: no units matched %r -- refusing to report an EMPTY SCOPE as "
        "clean\n" % (who, list(filters)))
    raise SystemExit(2)
