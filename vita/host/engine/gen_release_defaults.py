"""Emit the C table of release settings files (exact bytes) for vita_tool_main.c's first-boot installer."""
import os, sys

root = sys.argv[1]   # .../StarFoxAdventures-Vita-<ver>/ux0/data
rows = []
for d in ('stairfax', 'STFX00001'):
    for f in sorted(os.listdir(os.path.join(root, d))):
        p = os.path.join(root, d, f)
        if not os.path.isfile(p):
            continue
        b = open(p, 'rb').read()
        if len(b) > 64:
            sys.exit(f'unexpected large settings file {p} ({len(b)} B)')
        parts = []
        for c in b:
            if 32 <= c < 127 and chr(c) not in '"\\':
                parts.append(chr(c))
            else:
                parts.append('" "\\x%02x" "' % c)   # close/reopen the literal so a following hex digit is not swallowed
        lit = ('"' + ''.join(parts) + '"').replace('"" ', '').replace(' ""', '')
        rows.append(f'    {{ "ux0:data/{d}/{f}", {lit}, {len(b)} }},')
print('\n'.join(rows))
print(f'/* {len(rows)} files */', file=sys.stderr)
