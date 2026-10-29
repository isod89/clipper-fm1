#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The editor protocol / external input fuzz (tests/editor_fuzz.c, tests/editor_host.c) of Clipper's synth build
# (CLIPPER_SYNTH=1) under AddressSanitizer + UBSan, then the reproducers of what it found (tests/editor_repro.c).
#   sh tests/editor_fuzz.sh [ITER] [SEED]     (from the repo root; needs build/gen)
#   KNOWN_DRLVL=1: tolerate the EQ LOW written past its range by TRACK_MIX (editor_repro 2) until that fix is in
set -e
cd "$(dirname "$0")/.."
OUT=build/host/edfz
mkdir -p "$OUT/gen"
# SMP_DATA read through a pointer: the user sample slots are addressed as offsets from it (one flash), which
# -fsanitize=bounds would take for an overflow of the array; AddressSanitizer still checks every byte
{ echo '#define SMP_DATA (smp_data_v)'; sed 's/^static const uint8_t SMP_DATA\[/static const uint8_t SMP_DATA_ARR[/' build/gen/felucca_samples.h;
  echo 'static const uint8_t *volatile smp_data_v = SMP_DATA_ARR;'; } > "$OUT/gen/felucca_samples.h"
CCF="${CC:-cc} -DCLIPPER_SYNTH=1 ${BOARD:-} -DKNOWN_DRLVL=${KNOWN_DRLVL:-0} -O1 -g -w -fsanitize=address,undefined \
    -fno-sanitize=shift-base,signed-integer-overflow -fno-sanitize-recover=all -I$OUT/gen -Ibuild/gen -Ifirmware/src -Ifirmware/hal"
$CCF -o "$OUT/editor_fuzz" tests/editor_fuzz.c -lm
N=${1:-20000}
S=${2:-1}
"$OUT/editor_fuzz" 0 $((N * 10)) "$S"        # single frames, every command (HYBRID, FMX, MOD SEQ, TUNING too), edges
"$OUT/editor_fuzz" 1 "$N" "$S"               # sequences: uploads (samples, wavetables), restores, presets, patches, UI, audio
"$OUT/editor_fuzz" 2 "$N" "$S"               # USB-MIDI (MPE, RPN, CC 16..20, program change) and TRS bytes in
"$OUT/editor_fuzz" 3 $((N / 20)) "$S"        # boots from a damaged flash (a child each)
"$OUT/editor_fuzz" 4 1 "$S"                  # every command over every engine / track / id / step
"$OUT/editor_fuzz" 5 "$N" "$S"               # MIDI in only, then no note may be left held
"$OUT/editor_fuzz" 6 $((N / 100)) "$S"       # backup / section / autosave round trips give the project and the sound back
# the reproducers: all must pass
$CCF -o "$OUT/editor_repro" tests/editor_repro.c -lm
for r in 1 2 3 4; do "$OUT/editor_repro" $r; done
