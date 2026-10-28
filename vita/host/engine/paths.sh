# paths.sh -- sourced by build_game_vita.sh and compile_all.sh: the three roots, from the environment.
#   V       projects/stairfax-vita, derived from this file's location (STFX_VITA overrides)
#   R       the engine tree  (STFX_SRC; dev_defaults.env on the developer's machine, nothing in the public tree)
#   OUT     the build output (OUT; same)
#   C3      the C3 hook headers dir (STFX_C3; default: <V>/../gxaot/release/c3)
# See BUILDING.md. dev_defaults.env is excluded from the public tree: there STFX_SRC and OUT must be exported.
STFX_HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
_stfx_dev() { sed -n "s/^$1=//p" "$STFX_HERE/dev_defaults.env" 2>/dev/null | head -1 | tr -d '\r'; }
V=${STFX_VITA:-$(cd "$STFX_HERE/../.." && pwd)}
R=${STFX_SRC:-$(_stfx_dev STFX_SRC)}
OUT=${OUT:-$(_stfx_dev OUT)}
[ -n "$R" ] || { echo "STFX_SRC is not set: export STFX_SRC=<engine tree> (see BUILDING.md)"; exit 2; }
[ -n "$OUT" ] || { echo "OUT is not set: export OUT=<build output dir> (see BUILDING.md)"; exit 2; }
C3=${STFX_C3:-$V/../gxaot/release/c3}
export STFX_SRC=$R OUT
