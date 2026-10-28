"""stfx_paths.py -- the three roots every engine tool uses, overridable by environment.

    STFX_SRC  the engine tree the game is built from      (no built-in default; dev_defaults.env on the developer's machine)
    OUT       the build output directory                  (same)
    STFX_ISO  the Rev 1 disc image (STAIRFAX_ISO also read) (default: <STFX_SRC>/orig/GSAE01/sfa_rev1.iso)
    STFX_VITA the projects/stairfax-vita directory         (default: derived from this file's location)

A second tree builds with a different STFX_SRC / OUT and never touches the first tree's objects.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__)).replace('\\', '/')


def _win(p):
    """MSYS hands Windows python '/d/x' unless path conversion ran; normalise to 'D:/x'."""
    p = p.replace('\\', '/').rstrip('/')
    if len(p) >= 3 and p[0] == '/' and p[2] == '/' and p[1].isalpha():
        p = p[1].upper() + ':' + p[2:]
    return p


def _posix(p):
    return '/' + p[0].lower() + p[2:] if len(p) >= 2 and p[1] == ':' else p


import devcfg
from devcfg import dev, need   # dev_defaults.env (developer machine only) / "is it set"


# projects/stairfax-vita (this file is host/engine/stfx_paths.py); STFX_VITA overrides
VITA = _win(os.environ.get('STFX_VITA') or os.path.normpath(os.path.join(HERE, '..', '..')))
VITA_POSIX = _posix(VITA)
SRC = _win(need('STFX_SRC', os.environ.get('STFX_SRC') or dev('STFX_SRC')))       # D:/... form
SRC_POSIX = _posix(SRC)                                   # /d/... form (for sh / compile commands)
OUT = _win(need('OUT', os.environ.get('OUT') or dev('OUT')))
ISO = _win(os.environ.get('STFX_ISO') or os.environ.get('STAIRFAX_ISO') or dev('STFX_ISO') or (SRC + '/orig/GSAE01/sfa_rev1.iso'))
# The retail revision the game source is compiled AS (compile_all.sh passes -DVERSION_<this>), and
# so the map whose layout the code was written against. Upstream's source is per-revision since
# 2026-09 and its GSAE01_rev1 map is complete; the pre-merge fork compiled Rev 0 source (no
# conditionals existed) and translated Rev 0 addresses to the Rev 1 disc (rev_map.py).
# Default follows the TREE: upstream's post-merge layout (src/MSL_C/ exists) is compiled as Rev 1;
# the pre-merge tree has no VERSION_ conditionals and was written against the Rev 0 map.
VERSION = os.environ.get('STFX_VERSION',
                         'GSAE01_rev1' if os.path.isdir(SRC + '/src/MSL_C') else 'GSAE01')
MAP = SRC + '/config/' + VERSION + '/symbols.txt'


def rebase(path):
    """Map a path recorded under the DEFAULT tree (either spelling, or the `$SRC/` token) onto SRC."""
    p = path.replace('\\', '/')
    if p.startswith('$SRC/'):
        return SRC + '/' + p[5:]
    old_root = dev('DEFAULT_SRC')
    if old_root:
        for old in (old_root + '/', _posix(old_root) + '/'):
            if p.startswith(old):
                return SRC + '/' + p[len(old):]
    return p
