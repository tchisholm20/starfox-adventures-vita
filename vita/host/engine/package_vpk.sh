#!/bin/bash
# package_vpk.sh -- pack an eboot.bin built by build_game_vita.sh into an installable .vpk.
#
#   bash package_vpk.sh <eboot.bin> <out.vpk> [shaders_dir]        (MSYS2 bash, VitaSDK on PATH)
#
# shaders_dir (optional) holds pack.bin and gxm/*.gxp: pre-compiled shaders. pack.bin ships as app0:shaders/pack.bin; the .gxp files are packed
# into ONE app0:shaders/gxm.pak by pack_gxp.py (2026-11-05: 2028 loose files made the vpk install take 10+ minutes on an SD2VITA).
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
  # the saved pack index (the launch skips its 1.4 s scan); optional: absent = the game scans once and saves it to the card
  if [ -f "$SHD/pack.bin.idx" ]; then ARGS+=(-a "$SHD/pack.bin.idx=shaders/pack.bin.idx"); fi
  for g in "$SHD"/gxm/*.gxp; do NGXP=$((NGXP + 1)); done
  [ "$NGXP" -gt 0 ] || { echo "package_vpk: no .gxp files in $SHD/gxm"; exit 3; }
fi
if [ "$NGXP" -gt 0 ]; then
  python "$HERE/pack_gxp.py" "$SHD/gxm" "$TMP/gxm.pak"
  ARGS+=(-a "$TMP/gxm.pak=shaders/gxm.pak")
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
loose = sum(1 for x in names if x.startswith('shaders/gxm/'))
have = 0
if 'shaders/gxm.pak' in names:
    pk = z.read('shaders/gxm.pak'); magic, ver, have = struct.unpack_from('<III', pk)
    if magic != 0x314B5847 or ver != 1: sys.exit('package_vpk: shaders/gxm.pak has a bad header')
if have != ngxp or loose or (want_pack and 'shaders/pack.bin' not in names):
    sys.exit(f'package_vpk: the .vpk holds {have} packed shaders (expected {ngxp}), {loose} loose shader files (expected 0), pack.bin {"present" if "shaders/pack.bin" in names else "MISSING"}')
print(f'{vpk}: "{got[0]}" ({got[2]}), {have} pre-compiled shaders{", pack.bin" if want_pack else ""}')
EOF
