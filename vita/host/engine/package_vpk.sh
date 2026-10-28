#!/bin/bash
# package_vpk.sh -- pack an eboot.bin built by build_game_vita.sh into an installable .vpk.
#
#   bash package_vpk.sh <eboot.bin> <out.vpk>        (MSYS2 bash, VitaSDK on PATH)
#
# The app's name and title id are generated here with vita-mksfoex, never copied from a .sfo file:
# the 2026-10-28 release candidate was packed from a stale .sfo pulled off a dev console and showed
# the old dev title "Stairfax Temperatures (RHI)" under its bubble. The check at the end reads the
# title back out of the finished .vpk.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <eboot.bin> <out.vpk>"; exit 2; }
EBOOT=$1; VPK=$2
HERE=$(cd "$(dirname "$0")" && pwd); SYS=$HERE/../../sce_sys
export PATH=${VITASDK:-/usr/local/vitasdk}/bin:$PATH
TITLE="Star Fox Adventures"; TITLE_ID=STFX00001
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
vita-mksfoex -s TITLE_ID=$TITLE_ID -s APP_VER=01.00 "$TITLE" "$TMP/param.sfo"
vita-pack-vpk -s "$TMP/param.sfo" -b "$EBOOT" \
  -a "$SYS/icon0.png=sce_sys/icon0.png" -a "$SYS/pic0.png=sce_sys/pic0.png" \
  -a "$SYS/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png" \
  -a "$SYS/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png" \
  -a "$SYS/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml" \
  "$VPK"
python - "$VPK" "$TITLE" "$TITLE_ID" <<'EOF'
import sys, struct, zipfile
vpk, want, want_id = sys.argv[1:]
d = zipfile.ZipFile(vpk).read('sce_sys/param.sfo')
_, _, kt, dt, n = struct.unpack('<4sIIII', d[:20]); f = {}
for i in range(n):
    ko, fmt, ln, _, do = struct.unpack('<HHIII', d[20 + i * 16:36 + i * 16])
    f[d[kt + ko:d.index(b'\0', kt + ko)].decode()] = d[dt + do:dt + do + ln].rstrip(b'\0')
got = (f[b'TITLE'.decode()].decode(), f['STITLE'].decode(), f['TITLE_ID'].decode())
if got != (want, want, want_id): sys.exit(f'package_vpk: the .vpk says {got}, expected ({want!r}, {want!r}, {want_id!r})')
print(f'{vpk}: "{got[0]}" ({got[2]})')
EOF
