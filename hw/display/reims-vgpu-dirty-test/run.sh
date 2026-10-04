#!/bin/sh
# Build and run the dirty-adapter model check. No QEMU build required.
#   ./run.sh [max-length]          check the shim as it is
#   ./run.sh --mutant NAME         build a deliberately wrong shim; must FAIL
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
here=$(cd "$(dirname "$0")" && pwd)
shim="$here/../reims-vgpu-dirty.c"
out=${TMPDIR:-/tmp}/reims-vgpu-dirty-test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
mutant=
if [ "${1:-}" = "--mutant" ]; then mutant=$2; shift 2; fi
src="$shim"
case "$mutant" in
  "") ;;
  arm_at_track)   # a "synchronous baseline": readable the instant it is tracked
    sed 's|s->arm_at = d->harvests + 1;|s->arm_at = 0; s->gen = 1;|' "$shim" > "$out/shim.c"; src="$out/shim.c" ;;
  no_unlogged)    # arm in the harvest that turned logging on
    sed 's|if (s->gen == 0 \&\& unlogged) {|if (0) {|' "$shim" > "$out/shim.c"; src="$out/shim.c" ;;
  no_hit_stamp)   # forget which pages the arming harvest absorbed
    sed 's|s->page_gen\[g_array_index(hit, size_t, h)\] = s->gen;|(void)h;|' "$shim" > "$out/shim.c"; src="$out/shim.c" ;;
  no_sync)        # read the bitmap without folding the hypervisor's bits in
    sed 's|    memory_global_dirty_log_sync(false);|    (void)0;|' "$shim" > "$out/shim.c"; src="$out/shim.c" ;;
  *) echo "unknown mutant $mutant" >&2; exit 2 ;;
esac
cc -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-but-set-variable -I"$here/fake" -I"$here/.." \
   -DSHIM_SRC="\"$src\"" \
   $(pkg-config --cflags glib-2.0) "$here/test.c" "$here/fake/fake_qemu.c" \
   $(pkg-config --libs glib-2.0) -lpthread -o "$out/t"
if [ -n "$mutant" ]; then
  # A mutant is judged by the model check alone: the directed cases state the
  # intended mechanism and would fail it for the wrong reason.
  "$out/t" "${1:-6}" model-only
else
  "$out/t" "$@"
fi
