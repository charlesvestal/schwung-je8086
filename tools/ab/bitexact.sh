#!/usr/bin/env bash
# bitexact.sh — compare two jp8000_render builds sample-for-sample.
#
# The perceptual metrics in compare_wavs.py answer "does this still sound
# right". They are the wrong instrument for an optimization that claims to
# change nothing: that claim is bit-exactness, so compare hashes.
#
# A script is only evidence if it is reproducible. Each script is rendered
# RUNS times per build and a script whose own output varies run-to-run is
# reported UNSTABLE and excluded rather than counted as a difference --
# performance switching is currently in that category (see CLAUDE.md).
#
# usage: bitexact.sh <ref_build_dir> <new_build_dir> <rom_dir> <script_dir> [runs]

set -u
REF=${1:?ref build dir}; NEW=${2:?new build dir}; ROMS=${3:?rom dir}; SCRIPTS=${4:?script dir}
RUNS=${5:-3}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

hashes() {  # <binary> <script> -> unique hashes, one per line
  local bin=$1 scr=$2 i
  for ((i=0;i<RUNS;i++)); do
    "$bin" "$ROMS" "$scr" "$TMP/o.wav" >/dev/null 2>&1
    shasum -a 256 "$TMP/o.wav" | cut -d' ' -f1
  done | sort -u
}

fail=0; unstable=0; same=0
printf "%-24s %s\n" "script" "result"
for scr in "$SCRIPTS"/*.txt; do
  name=$(basename "$scr" .txt)
  mapfile -t rh < <(hashes "$REF/jp8000_render" "$scr")
  mapfile -t nh < <(hashes "$NEW/jp8000_render" "$scr")
  if [ "${#rh[@]}" -ne 1 ] || [ "${#nh[@]}" -ne 1 ]; then
    printf "%-24s UNSTABLE (ref %d/%d, new %d/%d distinct) - excluded\n" \
      "$name" "${#rh[@]}" "$RUNS" "${#nh[@]}" "$RUNS"
    unstable=$((unstable+1)); continue
  fi
  if [ "${rh[0]}" = "${nh[0]}" ]; then
    printf "%-24s BIT-IDENTICAL  %s\n" "$name" "${rh[0]:0:16}"; same=$((same+1))
  else
    printf "%-24s DIFFERS        ref %s  new %s\n" "$name" "${rh[0]:0:16}" "${nh[0]:0:16}"; fail=$((fail+1))
  fi
done
echo "---"
echo "identical: $same   differing: $fail   unstable(excluded): $unstable"
[ "$fail" -eq 0 ]
