"""devcfg.py -- the developer's own machine paths (dev_defaults.env, KEY=VALUE lines), or nothing.

dev_defaults.env is NOT part of the public source tree: there every path comes from the environment (BUILDING.md) and dev() is None.
Importing this never fails and never needs STFX_SRC (stfx_paths.py does, so tools that must run without it import this instead).
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__)).replace(chr(92), '/')
_D = None


def dev(key):
    global _D
    if _D is None:
        _D = {}
        try:
            for l in open(HERE + '/dev_defaults.env', encoding='utf-8'):
                l = l.split('#')[0].strip()
                if '=' in l:
                    k, v = l.split('=', 1)
                    _D[k.strip()] = v.strip()
        except OSError:
            pass
    return _D.get(key)


def need(name, val):
    if not val:
        raise SystemExit('%s is not set: export %s=<path> (see BUILDING.md)' % (name, name))
    return val
