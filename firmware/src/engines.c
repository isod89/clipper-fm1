/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Engine table: order = PRESETS browsing order (and the engine numbers of the editor protocol). */
#include "dsp.c"
/* SLOOP 2.5: the big per-part state of GRAIN, FM6 and PHYS shares one arena per synth part (a part plays one
 * engine at a time: on a switch the old engine fades out, its voices end, then the new one starts, voice.c
 * engine_block). The first engine to ask for it after another one gets it cleared, as at power-on. */
static void *eng_arena_of(const track_t *t, uint32_t eng);
static inline int32_t voice_amp(int32_t s, const vmod_t *m, uint32_t i) { return mulq15(mulq15(s, amp_at(m, i)), VOICE_FS); }
static inline int32_t soft_knee(int32_t y, int32_t k)   /* linear up to k, then only the peaks saturate */
{
    int32_t a = y < 0 ? -y : y;
    if (a <= k)
        return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}
/* 0.7: the second core renders some of a part's voices (voice.c track_render) into vs_buf while this core renders the
 * others: an engine's scratch that is not on the stack is one per core, picked by C1_IX(out) (1: the second core's) */
static int32_t vs_buf[CTL] __attribute__((aligned(8)));
#if CLIPPER_SYNTH                       /* (1.0) SPREAD: a voice alone into a scratch first (voice.c vrender), one a core */
static int32_t vs_tmp[2][CTL] __attribute__((aligned(8)));
#define C1_IX(out) ((out) == vs_buf || (out) == vs_tmp[1] ? 1u : 0u)
#else
#define C1_IX(out) ((out) == vs_buf ? 1u : 0u)
#endif
/* (0.9) the filter MODEL the engines with a low-pass of their own play (dsp.c fmod_*): P_FMODEL after the short duck
 * a change of it brings (fx.c mix_part1: no click), SVF in the groovebox */
static inline int32_t fm_model(const track_t *t)
{
#if CLIPPER_SYNTH
    return t->fm_eff ? t->fm_eff : clamp(t->p[P_FMODEL], FM_SVF, FM_LAST);
#else
    (void)t;
    return FM_SVF;
#endif
}
/* (pitch_inc: felucca_tables.h, SLOOP 2.5: from the top octave) */
/* (after Clipper 1.2) UNISON's start-phase spread (voice.c unison_spread): r (Q32: a turn of the note at pitch p16,
 * 1/16 semitone) in turns of an oscillator d16 (1/16 semitone) above it, the share of the same time; per note */
static uint32_t uni_turns(uint32_t r, int32_t p16, int32_t d16)
{
    uint32_t b = pitch_inc((uint32_t)clamp(p16, 0, 2047));
    return b ? (uint32_t)(((uint64_t)r * pitch_inc((uint32_t)clamp(p16 + d16, 0, 2047))) / b) : r;
}

#include "eng_analog.c"
#include "eng_digital.c"
#include "eng_phase.c"
#include "eng_lofi.c"
#include "eng_sample.c"
#include "eng_formant.c"
#include "eng_trio.c"
#include "eng_drawbar.c"
#include "eng_grain.c"
#include "eng_fm6.c"            /* FM6: 6-operator FM, msfa ported (fm6_core.c, Apache-2.0); SLOOP 2.4 */
#include "eng_phys.c"           /* PHYS: physical models, from Felucca 1.0 (DaisySP, Rings: MIT); SLOOP 2.5 */
#include "eng_noise.c"          /* NOISE: from Felucca 1.0; SLOOP 2.5 */
#include "eng_hybrid.c"         /* HYBRID: the Clipper voice (3 oscillators, 2 filters, the matrix) */
#if CLIPPER_SYNTH
#include "eng_wave.c"           /* WAVE: wavetables (Clipper 0.2) */
#include "eng_acid.c"           /* ACID: a 303-style bass line */
#include "eng_strings.c"        /* STRINGS: a string machine */
#include "eng_macro.c"          /* MACRO: chords, swarm, additive, speech (after Plaits) */
#include "eng_vps.c"            /* VPS: vector phaseshaping (Clipper 0.5) */
#include "eng_terrain.c"        /* TERRAIN: wave terrain, lite (Clipper 0.5) */
#include "eng_fmx.c"            /* FMX: six operators, waves, PM / FM / RING, any routing (Clipper 0.6) */
#include "eng_wseq.c"           /* WSEQ: wave sequencing (Clipper 0.6) */
#include "eng_juno.c"           /* JUNO: the Juno poly, its chorus in fx.c (Clipper 0.8) */
#include "eng_vector.c"         /* VECTOR: vector synthesis (Clipper 0.8) */
#endif

typedef union {
    gr_part_t grain;
    fm6_note_t fm6[FM6_POLY];
    uint32_t phys[PHYS_ARENA / 4u];      /* eng_phys.c: two SYMP slots or three small ones */
    hy_part_t hybrid;                    /* eng_hybrid.c: the voices */
#if CLIPPER_SYNTH
    fmx_part_t fmx;                      /* eng_fmx.c: the voices */
#endif
} eng_arena_t;
static eng_arena_t eng_arena[NPART] __attribute__((section(".pool")));
static uint8_t eng_arena_own[NPART];            /* the engine + 1 whose state is in it, 0 = none */
#ifdef ARENA_STATS
static uint32_t eng_arena_claims, eng_arena_bad;   /* host tests: claims, accesses by an engine the part does not play */
#endif
static void *eng_arena_of(const track_t *t, uint32_t eng)
{
    uint32_t p = (uint32_t)(t - trk);
    if (p >= NPART)
        return 0;
#ifdef ARENA_STATS
    if (t->engine != eng)
        eng_arena_bad++;
#endif
    if (eng_arena_own[p] != eng + 1u) {
        memset(&eng_arena[p], 0, sizeof eng_arena[p]);
        eng_arena_own[p] = (uint8_t)(eng + 1u);
#ifdef ARENA_STATS
        eng_arena_claims++;
#endif
    }
    return &eng_arena[p];
}
#if FELUCCA_SLICE
#include "eng_slice.c"
#endif

static const engine_t *const ENGINES[NENGINES] = {&ENG_ANALOG, &ENG_DIGITAL, &ENG_PHASE, &ENG_LOFI, &ENG_SAMPLE,
                                                    &ENG_FORMANT, &ENG_TRIO, &ENG_DRAWBAR, &ENG_GRAIN,
                                                    &ENG_FM6,    /* 9 (ENGI_FM6): always; SLICE after it (core.h) */
                                                    &ENG_PHYS, &ENG_NOISE, &ENG_HYBRID,
#if FELUCCA_SLICE
                                                    &ENG_SLICE,
#endif
#if CLIPPER_SYNTH
                                                    &ENG_WAVE,   /* 13 (ENGI_WAVE) */
                                                    &ENG_ACID, &ENG_STRINGS, &ENG_MACRO,   /* 14, 15, 16 */
                                                    &ENG_VPS, &ENG_TERRAIN,                /* 17, 18 */
                                                    &ENG_FMX, &ENG_WSEQ,                   /* 19, 20 */
                                                    &ENG_JUNO, &ENG_VECTOR,                /* 21, 22 */
#endif
};
#if CLIPPER_SYNTH
_Static_assert(ENGI_WAVE == 13u && ENGI_ACID == 14u && ENGI_STRINGS == 15u && ENGI_MACRO == 16u && ENGI_VPS == 17u &&
               ENGI_TERRAIN == 18u && ENGI_FMX == 19u && ENGI_WSEQ == 20u && ENGI_JUNO == 21u && ENGI_VECTOR == 22u,
               "ENGINES[] order (Clipper)");
/* (eng_wave.c) the table of the instrument: track 1's, else its layer's (one table in RAM: two WAVE sounds share
 * track 1's); while a sound fades out after a change (voice.c sound_fade_begin) its table stays */
static int32_t wt_wanted(void)
{
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        const track_t *t = &trk[k];
        if (k && (t->p[P_LYMODE] != LY_LAYER && t->p[P_LYMODE] != LY_SPLIT && t->p[P_LYMODE] != LY_MULTI))
            break;
        if (t->xf_on && ENGINES[t->engine % NENGINES] == &ENG_WAVE)
            return wt_have;
        if (ENGINES[t->eng_req % NENGINES] == &ENG_WAVE)
            return wt_tab_eff[k] ? wt_tab_eff[k] - 1 : (int32_t)((uint32_t)t->p[P_E0] % WT_NALL);   /* (0.9: after the duck) */
    }
    return -1;
}
#endif
_Static_assert(ENGI_FM6 == 9u, "ENGINES[ENGI_FM6] is FM6");
_Static_assert(ENGI_GRAIN == 8u && ENGI_PHYS == 10u && ENGI_NOISE == 11u && ENGI_HYBRID == 12u && ENGI_SLICE == 13u, "ENGINES[] order");

/* every factory sound as loud as the others: a level trim per preset, 1/2 dB, measured on a phrase
 * that fits the sound (tools/level_presets.py writes preset_trim.h); a track keeps it in P_ED_FX */
#include "preset_trim.h"
#if CLIPPER_SYNTH
#include "preset_trim_clipper.h"
#endif
static int16_t preset_trim(uint32_t e, uint32_t pi)
{
    if (CLIPPER_SYNTH && (ENGINES[e % NENGINES] == &ENG_SAMPLE || ENGINES[e % NENGINES] == &ENG_GRAIN))
        return 0;                                    /* (Clipper: the user's own samples, no factory level) */
#if CLIPPER_SYNTH
    if (e >= PTC_FIRST)                              /* Clipper's own engines: their table (preset_trim_clipper.h) */
        return e - PTC_FIRST < PTC_ENGINES && pi < PTC_MAX ? PRESET_TRIM_CL[e - PTC_FIRST][pi] : 0;
#endif
    return e < PT_ENGINES && pi < PT_MAX ? PRESET_TRIM[e][pi] : 0;
}
