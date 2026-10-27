#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Host tests (no hardware). Run from the repo root after ./build.sh:
#   tests/run_tests.sh
#
# Regression suite (tests/regress.c, tests/target_budget.py; details at the top of regress.c):
#   golden renders  every engine x preset, the drum kit, voice modes, FX sends, a 4-track mix: one hash
#                   each in tests/golden.txt. A change of the sound fails with the list of renders.
#   health          clipping, DC, peak level, voices free after the release, silence at the end.
#   CPU             cost / sample per preset and mix, relative to the idle + drums mix (tests/cpu_baseline.txt,
#                   +25 %; counted by the kernel or under callgrind, else timed at +35 %), ns printed;
#                   target: loop instructions of the render functions in build/felucca.dis
#                   (tests/target_budget.txt, +10 %; exact, static).
#   voices          the budget of 8, steal fades, MONO / LEGATO / UNISON keep their note, the VOICE cap,
#                   no hanging notes on any MIDI / key routing.
# FM6 (tests/fm6_test.c): the 6-operator FM engine (firmware/src/eng_fm6.c, fm6_core.c, fm6_bank.c): the 32
#                   algorithms' carriers, the operator envelopes ending the voice, retrigger, DC / clipping, the
#                   eight macros, the patch formats (packed, SysEx), the 6-voice cap, the patch bank on a
#                   simulated NOR; demos in build/fm6_demo/.
# After an intended change of the sound: GOLDEN_UPDATE=1 sh tests/run_tests.sh, review the diff
# of tests/golden.txt, commit it with the change. After an intended change of the cost (or a new
# compiler): BUDGET_UPDATE=1 (rewrites cpu_baseline.txt and target_budget.txt). VERBOSE=1: every render.
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
# SLOOP's groovebox tests (four tracks, drums, song) build with CLIPPER_SYNTH=0; Clipper's own synth mode
# (CLIPPER_SYNTH=1, the firmware's default) has its tests in the section "Clipper synth" below
CCS="${CC:-cc} -O1 -Wall -Wno-unused-function"
CC="$CCS -DCLIPPER_SYNTH=0"
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }

# HOST_ONLY=1: without the JieLi toolchain (no build/felucca.fwsc): the generated headers are made here and the
# tests that need the built package (update entry, update loader, target cost of the render loops) are skipped
if [ -n "$HOST_ONLY" ]; then
    mkdir -p build/gen
    for g in font:felucca_font icons:felucca_icons samples:felucca_samples drumkits:felucca_drumkits logo:sloop_logo; do
        [ -f "build/gen/${g#*:}.h" ] || python3 "tools/gen_${g%%:*}.py" "build/gen/${g#*:}.h" >/dev/null
    done
else
    [ -f build/felucca.fwsc ] || { echo "run ./build.sh first (or HOST_ONLY=1)"; exit 1; }
fi
# the generated headers the FM6 engine needs (tools/build.py generate() makes them too; no Pillow needed)
mkdir -p build/gen
[ build/gen/felucca_tables.h -nt tools/gen_tables.py ] || python3 tools/gen_tables.py build/gen/felucca_tables.h
[ build/gen/felucca_fm6.h -nt tools/gen_fm6_patches.py ] || python3 tools/gen_fm6_patches.py build/gen/felucca_fm6.h
python3 tools/gen_wavetables.py build/gen/felucca_wavetables.h >/dev/null   # (cached: only when the script changed)

$CC -o "$OUT/storage_test" tests/storage_test.c
run "flash storage (A/B, torn writes)" "$OUT/storage_test"

$CC -o "$OUT/recovery_test" tests/recovery_test.c
run "application USB recovery and boot-loop guard" "$OUT/recovery_test"

$CC -o "$OUT/arranger_test" tests/arranger_test.c
run "song order, timing, repeats and missing scenes" "$OUT/arranger_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/song_audio_test" tests/song_audio_test.c -lm
run "song: four simultaneous tracks, scene transition and stop" "$OUT/song_audio_test" "$OUT/song-demo.wav"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/song_ui_test" tests/song_ui_test.c -lm
run "song screen: commands, load (OCT+ twice), display bounds" "$OUT/song_ui_test" "$OUT/song-screen.ppm"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/studio_drums_test" tests/studio_drums_test.c -lm
run "drum lanes, kit audio, metronome, record arm, free take" "$OUT/studio_drums_test" "$OUT/drum-styles.wav"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/seq2_test" tests/seq2_test.c -lm
run "sequencer 2.0: no drift, ratchets, roll, erase / undo, ghost / hard, chords, mute / solo, nudge, locks" "$OUT/seq2_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/hold_rec_test" tests/hold_rec_test.c -lm
run "SLOOP 2.8.3: a note held under others (live, recorded, overdubbed, free take), MIDI in overflow" "$OUT/hold_rec_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/len_test" tests/len_test.c -lm
run "SLOOP 2.8.3: SEQ > NOTES, LEN under later notes, MONO / STEP FULL, shorter, delete" "$OUT/len_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/drum_steal_test" tests/drum_steal_test.c -lm
run "SLOOP 2.8.3: drum voices on a busy beat, every kit: no kick cut by another sound" "$OUT/drum_steal_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/chop_attack_test" tests/chop_attack_test.c -lm
run "SLOOP 2.8.3: a sample played again starts from silence with its ATK (POLY same note, MONO, LEGATO, UNISON), no click" "$OUT/chop_attack_test"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/drumkit_test" tests/drumkit_test.c -lm
run "synthesised drum kits: every kit x sound bounded, audible, finite, levels, cost" "$OUT/drumkit_test" "$OUT/drum-kits.wav" "$OUT/drum-kits.txt"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/userkit_test" tests/userkit_test.c -lm
run "user drum kits (KIT USR1..USR3): a user slot's sounds on the drum lanes" "$OUT/userkit_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/dsyn_test" tests/dsyn_test.c -lm
run "SYN1..SYN4 drum kits and the editor's DRUM SYNTH commands (v10)" "$OUT/dsyn_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/punch_test" tests/punch_test.c -lm
run "punch-in FX: 16 effects, bounded, dry after release, FX-held keys" "$OUT/punch_test" "$OUT/punch-fx.wav"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/ui_pages_test" tests/ui_pages_test.c -lm
run "live UI: pages, layers (punch, steps, erase, roll, key, mix), holds, drums, REC, fuzz" "$OUT/ui_pages_test" "$OUT"
# no divide by 0 (the FM-1 runs with the div0 trap off, hal/fm1_irq.h: a real one would give a wrong value
# silently): the UI fuzz, the sequencer, the projects and a minute of random live use, with UBSan
UBSAN="${CC_UB:-cc} -DCLIPPER_SYNTH=0 -O1 -w -fsanitize=integer-divide-by-zero -fno-sanitize-recover=integer-divide-by-zero -Ibuild/gen -Ifirmware/src -Ifirmware/hal"
$UBSAN -o "$OUT/ui_pages_ub" tests/ui_pages_test.c -lm && $UBSAN -o "$OUT/seq2_ub" tests/seq2_test.c -lm &&
    $UBSAN -o "$OUT/project_ub" tests/project_test.c -lm && $UBSAN -o "$OUT/soak_ub" tests/soak_test.c -lm || fail=1
mkdir -p "$OUT/ub"
run "no divide by zero (UBSan): UI fuzz, sequencer, projects, a minute of live use" \
    sh -c "'$OUT/ui_pages_ub' '$OUT/ub' >/dev/null && '$OUT/seq2_ub' >/dev/null && '$OUT/project_ub' >/dev/null && '$OUT/soak_ub' 1 >/dev/null && echo 'no divide by zero'"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/soak_test" tests/soak_test.c -lm
run "soak: ${SOAK_MIN:-10} minutes of random live use (bounded, no hanging voices, idle after stop)" "$OUT/soak_test" "${SOAK_MIN:-10}"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/font_test" tests/font_test.c
run "the screen font at 1 bit a pixel: every glyph, the same pixels as before" "$OUT/font_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/stress_test" tests/stress_test.c -lm
run "stress (2.4): FONT_L = FONT_S at 2x, hard random use of every 2.4 addition, clean stop" "$OUT/stress_test" "${STRESS_FRAMES:-40000}"
# the same under AddressSanitizer + UBSan: any read or write out of bounds stops it (left shifts of negative values
# and the FM6 phase's wrap-around are left out: the DSP's two's complement idioms, as every compiler builds them)
ASAN="${CC_UB:-cc} -DCLIPPER_SYNTH=0 -O1 -g -w -fsanitize=address,undefined -fno-sanitize=shift-base,signed-integer-overflow -fno-sanitize-recover=all -Ibuild/gen -Ifirmware/src -Ifirmware/hal"
if $ASAN -o "$OUT/stress_asan" tests/stress_test.c -lm 2>/dev/null; then
    run "stress under ASan + UBSan (no access out of bounds)" "$OUT/stress_asan" "${STRESS_ASAN_FRAMES:-15000}" 7
else
    echo "(stress under ASan: this compiler has no AddressSanitizer, skipped)"
fi

# Clipper synth (CLIPPER_SYNTH=1, what the FM-1 runs): one instrument, HOME = the sound, no tracks / drums / song
SYN="$CCS -O2 -w -DCLIPPER_SYNTH=1 -Ibuild/gen -Ifirmware/src -Ifirmware/hal"
mkdir -p "$OUT/clipper"
$SYN -o "$OUT/clipper_ui_test" tests/clipper_ui_test.c -lm
run "Clipper synth: HOME, presets, keys, pages, one track, MIDI omni, no drums / song" "$OUT/clipper_ui_test" "$OUT/clipper"
$SYN -o "$OUT/clipper_hold_test" tests/hold_rec_test.c -lm
run "Clipper synth (SLOOP 2.8.3): a note held under others, POLY and UNI2, live / recorded / overdub / free take, MIDI burst" "$OUT/clipper_hold_test"
$SYN -o "$OUT/clipper_chop_test" tests/chop_attack_test.c -lm
run "Clipper synth (SLOOP 2.8.3): a sample played again starts from silence with its ATK, no click" "$OUT/clipper_chop_test"
$SYN -o "$OUT/clipper_stress" tests/stress_test.c -lm
run "Clipper synth: stress (hard random use, clean stop)" "$OUT/clipper_stress" "${STRESS_FRAMES:-40000}"
$SYN -o "$OUT/clipper_soak" tests/soak_test.c -lm
run "Clipper synth: soak, ${SOAK_MIN:-10} minutes of random live use" "$OUT/clipper_soak" "${SOAK_MIN:-10}"
$SYN -o "$OUT/filter_test" tests/filter_test.c -lm
run "Clipper 0.2 filters: the MOOG ladder (response, resonance, no dead band, bounded), soft SVF states, ANALOG MODL" "$OUT/filter_test"
$SYN -o "$OUT/alias_test" tests/alias_test.c -lm
run "Clipper 0.2 anti-aliasing: every oscillator's folded partials below 15 kHz (TRI, SYNC, FOLD, FM, feedback)" "$OUT/alias_test"
$SYN -o "$OUT/reverb_test" tests/reverb_test.c -lm
run "Clipper 0.2 plate reverb: decay, damping, width, level, pre-delay, stable, type switch" "$OUT/reverb_test" "$OUT/clipper/reverbs.wav"
$SYN -o "$OUT/wave_test" tests/wave_test.c -lm
mkdir -p build/wave_demo
run "Clipper 0.2 WAVE: band-limited tables (no aliasing), building, POS, DTN / SUB, user tables, bounded, demos" "$OUT/wave_test" build/wave_demo
SYN_UB="${CC_UB:-cc} -DCLIPPER_SYNTH=1 -O1 -g -w -fsanitize=address,undefined -fno-sanitize=shift-base,signed-integer-overflow -fno-sanitize-recover=all -Ibuild/gen -Ifirmware/src -Ifirmware/hal"
$SYN -o "$OUT/engines2_test" tests/engines2_test.c -lm
mkdir -p build/eng2_demo
run "Clipper 0.2 ACID (accent, sweep, slide), STRINGS (registers, ensemble), MACRO (chords, swarm, additive, speech, LPG)" "$OUT/engines2_test" build/eng2_demo
mkdir -p "$OUT/synth3"
$SYN -o "$OUT/synth3_test" tests/synth3_test.c -lm
run "Clipper 0.3: master EQ and COMP, poly unison, VEL / MW / AT, LAYER and SPLIT, SWAP, MORPH, RANDOM, the pages (0.4: MATRIX, LFO 2, ENV 2)" "$OUT/synth3_test" "$OUT/synth3"
if $SYN_UB -o "$OUT/synth3_ub" tests/synth3_test.c -lm 2>/dev/null; then
    mkdir -p "$OUT/synth3_asan"
    run "Clipper 0.3 under ASan + UBSan" sh -c "'$OUT/synth3_ub' '$OUT/synth3_asan' >/dev/null && echo clean"
fi
$SYN -o "$OUT/synth4_test" tests/synth4_test.c -lm
run "Clipper 0.4: the matrix on every engine (LFO 2, ENV 2, VEL, KEY, RAND, MW, AT, SLIDE), MPE, projects, user presets" "$OUT/synth4_test"
if $SYN_UB -o "$OUT/synth4_ub" tests/synth4_test.c -lm 2>/dev/null; then
    run "Clipper 0.4 under ASan + UBSan" sh -c "'$OUT/synth4_ub' >/dev/null && echo clean"
fi
mkdir -p build/eng3_demo
$SYN -o "$OUT/synth6_test" tests/synth6_test.c -lm
run "Clipper 0.6: an INSERT per sound (bypass bit for bit, every type, fades, tail, the layer's own, projects, user presets, SLOOP 2.6's), MULTI (the layer on its own channel)" "$OUT/synth6_test"
if $SYN_UB -o "$OUT/synth6_ub" tests/synth6_test.c -lm 2>/dev/null; then
    run "Clipper 0.6 under ASan + UBSan" sh -c "'$OUT/synth6_ub' >/dev/null && echo clean"
fi
$SYN -o "$OUT/fmx_test" tests/fmx_test.c -lm
mkdir -p build/fmx_demo
run "Clipper 0.6 FMX: presets in tune, waves, PM / FM / RING, routing, 40 templates, PEG, bank (FMX and DX7 slots), DX7 converted" "$OUT/fmx_test" build/fmx_demo
if $SYN_UB -o "$OUT/fmx_ub" tests/fmx_test.c -lm 2>/dev/null; then
    run "Clipper 0.6 FMX under ASan + UBSan" sh -c "'$OUT/fmx_ub' >/dev/null && echo clean"
fi
$SYN -o "$OUT/wseq_test" tests/wseq_test.c -lm
mkdir -p build/wseq_demo
run "Clipper 0.6 WSEQ: steps at the tempo and their pitches, FWD / BACK / PING / ONCE, PITCH, START, XFADE, no click at a cut" "$OUT/wseq_test" build/wseq_demo
mkdir -p "$OUT/fmx_ui"
$SYN -o "$OUT/fmx_ui_test" tests/fmx_ui_test.c -lm
run "Clipper 0.6 FMX on the FM-1: its pages on FMX only, OP / ROUTE / templates / a sound loaded" "$OUT/fmx_ui_test" "$OUT/fmx_ui"
mkdir -p "$OUT/hist_ui"
$SYN -o "$OUT/hist_ui_test" tests/hist_ui_test.c -lm -lpthread
run "Clipper 0.7 on the FM-1: UNDO (steps, back, forward), COMPARE, EDITED, a sound loaded, RANDOM by section and FMX's operators, FAV, KBD HOLD, 15 arp modes, the arp kept" "$OUT/hist_ui_test" "$OUT/hist_ui"
$SYN -o "$OUT/split_test" tests/split_test.c -lm -lpthread
run "Clipper 0.7, the voices on both cores: every preset, the same output with and without the split (one core, inline)" "$OUT/split_test"
run "Clipper 0.7, the voices on both cores: core 1 as a thread (CORES=2)" env CORES=2 "$OUT/split_test"
run "Clipper 0.7, the voices on both cores: an erratic core 1 (CORES=2 CHAOS=5)" env CORES=2 CHAOS=5 "$OUT/split_test" 16
run "Clipper 0.7, the voices on both cores: a lost job (held, its voices here, once)" env CPU1_TIMEOUT_AT=40 "$OUT/split_test" 16
if ${CC_TSAN:-gcc} -O1 -g -w -fsanitize=thread -DCLIPPER_SYNTH=1 -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/split_tsan" tests/split_test.c -lm -lpthread 2>/dev/null; then
    run "Clipper 0.7, the voices on both cores under ThreadSanitizer (CORES=2: no race)" env CORES=2 TSAN_OPTIONS=halt_on_error=1 "$OUT/split_tsan" 12
else
    echo "(split under ThreadSanitizer: this compiler has none, skipped)"
fi
$SYN -o "$OUT/synth8_test" tests/synth8_test.c -lm
run "Clipper 0.8: JUNO (presets, tune, PW, HPF, the chorus I / II / I+II, no click, as an INSERT), VECTOR (corners, MOVE), the MOD SEQ (KEY, SYNC, LEN, SMTH, to CUT / AMP, projects, user presets), SPICE / DICE (arp, sequencer)" "$OUT/synth8_test"
if $SYN_UB -o "$OUT/synth8_ub" tests/synth8_test.c -lm 2>/dev/null; then
    run "Clipper 0.8 under ASan + UBSan" sh -c "'$OUT/synth8_ub' >/dev/null && echo clean"
fi
mkdir -p "$OUT/mseq_ui"
$SYN -o "$OUT/mseq_ui_test" tests/mseq_ui_test.c -lm -lpthread
run "Clipper 0.8 / 0.9 on the FM-1: MOD SEQ pages (lane, step, value, SHAPE), MOD SEQ 2, SPICE, UNDO of a step, the layer SWAP, JUNO / VECTOR in the browser, TUNING, VINTG, MODL, TURNG's labels" "$OUT/mseq_ui_test" "$OUT/mseq_ui"
$SYN -o "$OUT/synth9_test" tests/synth9_test.c -lm
run "Clipper 0.9: the filter MODELs (SVF MOOG MS20 DIODE SEM NOTCH BP HP: bounded, responses, switch without a click), VINTAGE, the TUNING (JUST, 19-EDO, RAST, a USER scale), the TURNG arp and LOOP, projects and user presets" "$OUT/synth9_test"
if $SYN_UB -o "$OUT/synth9_ub" tests/synth9_test.c -lm 2>/dev/null; then
    run "Clipper 0.9 under ASan + UBSan" sh -c "'$OUT/synth9_ub' >/dev/null && echo clean"
fi
$SYN -o "$OUT/synth10_test" tests/synth10_test.c -lm
run "Clipper 1.0: SPREAD (left = right at 0, the middle kept), the matrix's 12 slots, LFO 3, ENV 2 LOOP, the MACROS (and CC 16..19), HALL and SHIMMER, the delay's PING and TAPE (a change of MODE: no click), projects and user presets (the tail), the pages" "$OUT/synth10_test"
if $SYN_UB -o "$OUT/synth10_ub" tests/synth10_test.c -lm 2>/dev/null; then
    run "Clipper 1.0 under ASan + UBSan" sh -c "'$OUT/synth10_ub' >/dev/null && echo clean"
fi
$SYN -o "$OUT/clean_test" tests/clean_test.c -lm
run "Clipper 1.0 audio: no click or crackle (55 cases: presets of every engine, steals, mute, level, pan, INSERT, DIST, the sends, FILT, MODL, CHOR, VINTAGE, delay TIME and MODE, reverb SIZE and TYPE, EQ, WAVE TABL, FMX PTCH, engine switch, TUNING, LAYER, MOD SEQ, SPREAD, the MACROS)" "$OUT/clean_test"
if $SYN_UB -o "$OUT/clean_ub" tests/clean_test.c -lm 2>/dev/null; then
    run "Clipper 0.9 audio under ASan + UBSan" sh -c "'$OUT/clean_ub' >/dev/null && echo clean"
fi
$SYN -o "$OUT/fx_sweep_test" tests/fx_sweep_test.c -lm
run "Clipper 1.0 effects: a sine through every reverb type, delay MODE and TIME, chorus, swept INSERT, JUNO chorus and the master at their extremes: no click, left as clean as right" "$OUT/fx_sweep_test"
if $SYN_UB -o "$OUT/fx_sweep_ub" tests/fx_sweep_test.c -lm 2>/dev/null; then
    run "Clipper 1.0 effects under ASan + UBSan (no line read out of its array)" sh -c "'$OUT/fx_sweep_ub' >/dev/null && echo clean"
fi
$SYN -o "$OUT/engines3_test" tests/engines3_test.c -lm
run "Clipper 0.5 VPS (vector phaseshaping: pitch, formant, D, no DC, no aliasing, no step) and TERRAIN (every surface pitched, RAD, DRIFT, no aliasing), bounded, cost" "$OUT/engines3_test" build/eng3_demo
if $SYN_UB -o "$OUT/engines3_ub" tests/engines3_test.c -lm 2>/dev/null; then
    run "Clipper 0.5 engines under ASan + UBSan" sh -c "'$OUT/engines3_ub' >/dev/null && echo clean"
fi
$SYN -o "$OUT/sloop26_fixes" tests/sloop26_fixes_test.c -lm
run "Clipper 0.4.1: SLOOP 2.6's fixes (look-ahead limiter, filter start, FM6 patch after the fade, arp #191, wheels)" "$OUT/sloop26_fixes"
if $SYN_UB -o "$OUT/sloop26_ub" tests/sloop26_fixes_test.c -lm 2>/dev/null; then
    run "Clipper 0.4.1 fixes under ASan + UBSan" sh -c "'$OUT/sloop26_ub' >/dev/null && echo clean"
fi
if $SYN_UB -o "$OUT/engines2_asan" tests/engines2_test.c -lm 2>/dev/null; then
    run "Clipper 0.2 new engines under ASan + UBSan" sh -c "'$OUT/engines2_asan' >/dev/null && echo clean"
fi
if $SYN_UB -o "$OUT/clipper_ui_asan" tests/clipper_ui_test.c -lm 2>/dev/null; then
    mkdir -p "$OUT/clipper_asan"
    run "Clipper synth: UI under ASan + UBSan" sh -c "'$OUT/clipper_ui_asan' '$OUT/clipper_asan' >/dev/null && echo clean"
fi
if $SYN_UB -o "$OUT/param_names_asan" tests/param_names_test.c -lm 2>/dev/null; then
    run "every engine's EDIT values as shown, under ASan (the FMX ALG / PTCH names ended with a 0: FM KICK ALG past 32 crashed)" "$OUT/param_names_asan"
fi

$CC -o "$OUT/upreset_test" tests/upreset_test.c
run "user presets (UP_PUT parser, bank round trip, versions)" "$OUT/upreset_test"

$CC -o "$OUT/midi_uart_test" tests/midi_uart_test.c
run "TRS MIDI parser" "$OUT/midi_uart_test"
$CC -Ifirmware/hal -o "$OUT/encoder_test" tests/encoder_test.c
run "knobs: one click = one step (slow, fast, pauses, bounce)" "$OUT/encoder_test"
HALF=$(sed -n 's/^#define HALF_FRAMES \([0-9]*\).*/\1/p' firmware/src/core.h)
$CC -DT_CDC=1 -DHALF_FRAMES=$HALF -o "$OUT/uac_test" tests/uac_test.c -lm
run "USB audio input: descriptors (with CDC), ring and packets" "$OUT/uac_test"
$CC -DT_CDC=0 -DHALF_FRAMES=$HALF -o "$OUT/uac_test_nocdc" tests/uac_test.c -lm
run "USB audio input: descriptors (without CDC), ring and packets" "$OUT/uac_test_nocdc"
$CC -DT_CDC=2 -DHALF_FRAMES=$HALF -o "$OUT/uac_test_seroff" tests/uac_test.c -lm
run "USB audio input: descriptors (CDC built in, menu USB SERIAL OFF), ring and packets" "$OUT/uac_test_seroff"
run "USB SERIAL OFF: the descriptors of a build without CDC, byte for byte" \
    sh -c "[ \"\$(UAC_DUMP=1 '$OUT/uac_test_seroff' | tail -n 2)\" = \"\$(UAC_DUMP=1 '$OUT/uac_test_nocdc' | tail -n 2)\" ] && echo same"
uac_in_app() { ${CC%% *} -E -Ibuild/gen -Ifirmware/hal -Ifirmware/src firmware/src/felucca.c 2>/dev/null | grep -q uac_service; }
run "USB audio input: built into the firmware (FELUCCA_UAC set before usb.c)" uac_in_app

if [ -z "$HOST_ONLY" ]; then
$CC -o "$OUT/ota_test" tests/ota_test.c
run "M-UPGRADE entry" "$OUT/ota_test" build/felucca.fwsc

head -c 200000 build/felucca.bin > "$OUT/old_app.bin"
python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
$CC -o "$OUT/ldr_test" tests/ldr_test.c
run "update loader: other app -> this build" "$OUT/ldr_test" "$OUT/old.fwsc" build/felucca.fwsc
else
    echo "== skip update entry and loader (HOST_ONLY: no build/felucca.fwsc)"
fi

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/hostsim" tests/hostsim.c -lm
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/scale_test" tests/scale_test.c -lm
run "scales: white-key mapping and note lifecycle" "$OUT/scale_test"
run "DSP render (ANALOG preset 0)" "$OUT/hostsim" 0 0 1 "$OUT/render.wav"
mkdir -p build/tracks_demo
run "TRACKS: 4-track pattern, live recording (lengths, swing), voice budget, engine switch, cost" env TRACKS=build/tracks_demo "$OUT/hostsim" 0 0 1 "$OUT/tracks.wav"
$CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/project_test" tests/project_test.c -lm
run "project formats (FUN4 / FUN3 / FUN2 / FUN1 -> FUN5), capture / apply, autosave" "$OUT/project_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/slicer_test" tests/slicer_test.c -lm
mkdir -p build/slicer_demo
run "SLICER: no clicks, timing, sync with the sequencer, STUT, cost, demos" "$OUT/slicer_test" build/slicer_demo
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/fm6_test" tests/fm6_test.c -lm
mkdir -p build/fm6_demo
run "FM6: algorithms, envelopes, retrigger, DC, clipping, macros, patch formats, voices, the bank, demos" "$OUT/fm6_test" build/fm6_demo
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/hybrid_test" tests/hybrid_test.c -lm
mkdir -p build/hybrid_demo
run "HYBRID (Clipper): oscillators, filters, matrix, controllers, patches, cost, demos" "$OUT/hybrid_test" build/hybrid_demo
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/hybrid_editor_test" tests/hybrid_editor_test.c -lm
run "HYBRID editor commands (v11): HY_GET, HY_PUT, ranges, bad requests" "$OUT/hybrid_editor_test" "$OUT/hy_factory.json"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/regress" tests/regress.c -lm
# the CPU budget: counted by the kernel on macOS; elsewhere under callgrind when valgrind is there (exact, ~45 s;
# SKIP_CPU_VALGRIND=1 to time instead, which is only a rough check)
if [ -z "$SKIP_CPU_VALGRIND" ] && command -v valgrind >/dev/null 2>&1; then export CPU_VALGRIND=1; fi
run "regression: golden renders, health, voices, CPU budget" "$OUT/regress" tests/golden.txt tests/cpu_baseline.txt
# SLICE (tests/slice_test.c) needs a FELUCCA_SLICE=1 build; the engine is not built by default

if [ -z "$HOST_ONLY" ]; then
run "regression: target cost of the render loops" python3 tests/target_budget.py \
    build/felucca.dis tests/target_budget.txt
fi

run "installer CLI (fm1_install.py) against a simulated FM-1" python3 tests/install_test.py

if command -v node >/dev/null 2>&1; then
    run "web pages: editor protocol, samples, packages, update protocol" node web/test_web.mjs
else
    echo "== skip web tests (no node)"
fi

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
