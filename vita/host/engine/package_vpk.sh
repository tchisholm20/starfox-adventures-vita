#!/bin/bash
# package_vpk.sh -- pack an eboot.bin built by build_game_vita.sh into an installable .vpk.
#
#   bash package_vpk.sh <eboot.bin> <out.vpk> [shaders_dir]        (MSYS2 bash, VitaSDK on PATH)
#
# shaders_dir (optional) holds pack.bin and gxm/*.gxp: pre-compiled shaders, packed as app0:shaders/.
# The game reads them when the memory-card cache (ux0:data/STFX00001/) does not have a shader, so a
# player needs only the vpk and a disc image. Without it, every shader compiles on the console.
#
# The app's name and title id are generated here with vita-mksfoex, never copied from a .sfo file:
# the 2026-10-28 release candidate was packed from a stale .sfo pulled off a dev console and showed
# the old dev title "Stairfax Temperatures (RHI)" under its bubble. The check at the end reads the
# title (and the shader count) back out of the finished .vpk.
set -euo pipefail
[ $# -eq 2 ] || [ $# -eq 3 ] || { echo "usage: $0 <eboot.bin> <out.vpk> [shaders_dir]"; exit 2; }
EBOOT=$1; VPK=$2; SHD=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd); SYS=$HERE/../../sce_sys
export PATH=${VITASDK:-/usr/local/vitasdk}/bin:$PATH
TITLE="Star Fox Adventures"; TITLE_ID=STFX00001
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
vita-mksfoex -s TITLE_ID=$TITLE_ID -s APP_VER=01.00 "$TITLE" "$TMP/param.sfo"
ARGS=(-a "$SYS/icon0.png=sce_sys/icon0.png" -a "$SYS/pic0.png=sce_sys/pic0.png"
      -a "$SYS/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png"
      -a "$SYS/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png"
      -a "$SYS/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml")
NGXP=0
if [ -n "$SHD" ]; then
  [ -f "$SHD/pack.bin" ] || { echo "package_vpk: $SHD/pack.bin missing"; exit 3; }
  ARGS+=(-a "$SHD/pack.bin=shaders/pack.bin")
  for g in "$SHD"/gxm/*.gxp; do ARGS+=(-a "$g=shaders/gxm/$(basename "$g")"); NGXP=$((NGXP + 1)); done
  [ "$NGXP" -gt 0 ] || { echo "package_vpk: no .gxp files in $SHD/gxm"; exit 3; }
fi
vita-pack-vpk -s "$TMP/param.sfo" -b "$EBOOT" "${ARGS[@]}" "$VPK"
python - "$VPK" "$TITLE" "$TITLE_ID" "$NGXP" "${SHD:+1}" <<'EOF'
import sys, struct, zipfile
vpk, want, want_id, ngxp, want_pack = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5] == '1'
z = zipfile.ZipFile(vpk); names = z.namelist()
d = z.read('sce_sys/param.sfo')
_, _, kt, dt, n = struct.unpack('<4sIIII', d[:20]); f = {}
for i in range(n):
    ko, fmt, ln, _, do = struct.unpack('<HHIII', d[20 + i * 16:36 + i * 16])
    f[d[kt + ko:d.index(b'\0', kt + ko)].decode()] = d[dt + do:dt + do + ln].rstrip(b'\0')
got = (f['TITLE'].decode(), f['STITLE'].decode(), f['TITLE_ID'].decode())
if got != (want, want, want_id): sys.exit(f'package_vpk: the .vpk says {got}, expected ({want!r}, {want!r}, {want_id!r})')
have = sum(1 for x in names if x.startswith('shaders/gxm/') and x.endswith('.gxp'))
if have != ngxp or (want_pack and 'shaders/pack.bin' not in names):
    sys.exit(f'package_vpk: the .vpk holds {have} shaders (expected {ngxp}), pack.bin {"present" if "shaders/pack.bin" in names else "MISSING"}')
print(f'{vpk}: "{got[0]}" ({got[2]}), {have} pre-compiled shaders{", pack.bin" if want_pack else ""}')
EOF
