/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* FMX: six operators, each its own wave and its own way of being modulated, on any routing (Clipper 0.6, after a
 * request for "the Korg opsix on the FM-1": the ideas, not any code). FM6 stays the DX7 (its patches, its exact
 * engine); FMX is the larger instrument:
 *   - WAVE per operator: SINE TRI SAW SQR HALF ABS QUART PULSE SIN8 NOISE (fmx_tables.h: SAW SQR PULSE band-limited
 *     twice, the high notes on the 8-harmonic copy; HALF ABS QUART the OPL's, SIN8 a sine on 16 steps)
 *   - MODE per operator: PM (phase modulation, the DX7's "FM"), FM (true, linear, through-zero frequency
 *     modulation: the modulators move the frequency, a DC in them bends the pitch), RING (the operator's wave times
 *     what modulates it). FIX: a fixed frequency (1 Hz .. ~9.8 kHz) instead of a ratio of the note
 *   - ROUTING: any operator into any other, each connection its own amount (mod[to][from], 0..127; an operator into
 *     itself: feedback); any operator to the output, its own level (out[]). The 32 DX7 algorithms and 8 more are
 *     templates (ALG): the patch keeps what they filled in, and every connection can be changed after
 *   - per operator: RATIO (x0.5, x1 .. x31) and FINE (x1.00 .. x1.99), LEVEL (0.75 dB a step), an ADSR, VEL
 *     (velocity to its level), KSC (its level down above C4: high notes less bright), DTN (-7..7)
 *   - per patch: a pitch envelope (PEG +-24 semitones, its decay PTIM), LEVEL
 * The patch is 128 bytes, every one 0..127: one slot of the FM6 bank holds an FMX patch as well (fm6_bank.c, a bit
 * in the bank's spare word: FM6 sees an empty slot there), and an FMX sound on a slot holding a DX7 voice plays it,
 * converted (fmx_from_dx7: the algorithm, ratios, levels, envelopes, feedback). PTCH: F1..F16 the factory patches,
 * B1..B27 the bank.
 * On the FM-1: EDIT 1 PTCH ALG MOD FB, EDIT 2 TIME DTN VEL KTRK (the patch through them: fmx_compile), and the FMX
 * pages (ui_synth.c, shown on this engine only): OP (OP WAVE MODE RATIO), OP 2 (FINE LEVEL VEL KSC), OP ENV
 * (ATK DEC SUS REL), ROUTE (TO FROM AMT OUT), FMX (DTN FIX PEG PTIM) and STORE (SLOT STORE): the part's own patch,
 * changed at once; STORE writes it to the bank (the song stopped: a flash write silences the audio a moment).
 * Per sample, per voice: the six operators from 6 to 1, an operator modulated by a higher one hears its output of
 * this sample, by a lower one (or itself) its last (itself: the mean of its last two, the DX7's); everything else
 * (envelopes, levels, increments) once a control tick, the levels ramped over it.
 * The operator envelopes are the voice's amplitude (engine_t.ownenv): a voice ends when its carriers' have. */
#include "fmx_tables.h"

#define FMX_NOP 6u
#define FMX_NFACTORY 16u
#define FMX_NSLOT (FMX_NFACTORY + FM6_BANK_N)   /* PTCH: F1..F16, B1..B27 */
enum { FXM_PM, FXM_FM, FXM_RING, FXM_N };
#define FXM_FIX 4u               /* mode bit: a fixed frequency */
enum { FXW_SINE, FXW_TRI, FXW_SAW, FXW_SQR, FXW_HALF, FXW_ABS, FXW_QUART, FXW_PULSE, FXW_SIN8, FXW_NOISE,
       FXW_SAW8, FXW_SQR8, FXW_PULS8 };
_Static_assert(FXW_PULS8 + 1u == FMX_WN && FXW_NOISE + 1u == FMX_NWAVE, "fmx_tables.h's waves");

typedef struct {                 /* 12 bytes, each 0..127 */
    uint8_t wave;                /* FXW_SINE .. FXW_NOISE */
    uint8_t mode;                /* FXM_* | FXM_FIX */
    uint8_t coarse;              /* 0 = x0.5, 1..31 (FIX: 0..3 = 1, 10, 100, 1000 Hz) */
    uint8_t fine;                /* 0..99: x1.00 .. x1.99 (FIX: x10^(fine / 100)) */
    uint8_t level;               /* 0..127: 0.75 dB a step (0: off) */
    uint8_t adsr[4];             /* voice.c's ADSR scales */
    uint8_t vel;                 /* velocity -> level, 0..127 */
    uint8_t ksc;                 /* level down per octave above C4, 0..127 (~6 dB an octave at 127) */
    uint8_t dtn;                 /* 0..14, 7 = in tune */
} fmx_op_t;
typedef struct {
    fmx_op_t op[FMX_NOP];        /* operator 1 first */
    uint8_t mod[FMX_NOP][FMX_NOP];   /* mod[to][from], 0..127 */
    uint8_t out[FMX_NOP];        /* to the output, 0..127 */
    uint8_t alg;                 /* the template it came from, 1..FMX_NALG (0: none) */
    uint8_t peg, ptime;          /* the pitch envelope: 64 none, +-24 semitones; its decay */
    uint8_t level;               /* 100 = unity */
    char name[10];
} fmx_patch_t;
_Static_assert(sizeof(fmx_patch_t) == 128u && __builtin_offsetof(fmx_patch_t, mod) == 72u && __builtin_offsetof(fmx_patch_t, out) == 108u &&
               __builtin_offsetof(fmx_patch_t, alg) == 114u && __builtin_offsetof(fmx_patch_t, level) == 117u && __builtin_offsetof(fmx_patch_t, name) == 118u,
               "an FMX patch fills an FM6 bank record (the editor's layout: web/editor.html FX)");
_Static_assert(FM6_PACKED == 128u, "the bank's records");

/* ---------------------------------------------------------------------------------------- factory patches --- */
#define OP(w, md, c, f, l, a, d, s, r, ve, k) {w, md, c, f, l, {a, d, s, r}, ve, k, 7}
#define NOOP {0, 0, 1, 0, 0, {0, 0, 0, 0}, 0, 0, 7}
static const fmx_patch_t FMX_FACTORY[FMX_NFACTORY] = {
    {   /* F1: an electric piano, the tine (2 > 1 at 14x) over the body (4 > 3) */
        {OP(FXW_SINE, FXM_PM, 1, 0, 127, 0, 88, 0, 70, 50, 20), OP(FXW_SINE, FXM_PM, 14, 0, 96, 0, 54, 0, 50, 90, 60),
         OP(FXW_SINE, FXM_PM, 1, 0, 120, 0, 90, 30, 70, 40, 20), OP(FXW_SINE, FXM_PM, 1, 0, 100, 0, 80, 20, 60, 70, 40),
         NOOP, NOOP},
        {{0, 100}, {0}, {0, 0, 0, 100}, {0}, {0}, {0}}, {100, 0, 90, 0, 0, 0}, 5, 64, 0, 100, "FMX EP"},
    {   /* F2: a bell (3 > 1 at 3.5x, 2 rings at 7x) */
        {OP(FXW_SINE, FXM_PM, 1, 0, 127, 0, 100, 0, 96, 30, 10), OP(FXW_SINE, FXM_PM, 3, 50, 104, 0, 92, 0, 90, 50, 30),
         OP(FXW_SINE, FXM_PM, 7, 0, 90, 0, 84, 0, 80, 60, 30), NOOP, NOOP, NOOP},
        {{0, 100, 60}, {0, 0, 80}, {0}, {0}, {0}, {0}}, {110, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "FMX BELL"},
    {   /* F3: a bass, a saw carrier phase-modulated by a sine an octave down */
        {OP(FXW_SAW, FXM_PM, 1, 0, 120, 0, 70, 60, 30, 40, 0), OP(FXW_SINE, FXM_PM, 0, 0, 100, 0, 56, 10, 30, 80, 20),
         NOOP, NOOP, NOOP, NOOP},
        {{0, 90}, {0, 80}, {0}, {0}, {0}, {0}}, {110, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "SAW BASS"},
    {   /* F4: brass, three modulators into one, slow on the bright ones */
        {OP(FXW_SINE, FXM_PM, 1, 0, 127, 20, 80, 110, 40, 30, 10), OP(FXW_SINE, FXM_PM, 1, 0, 96, 34, 76, 90, 40, 60, 20),
         OP(FXW_SINE, FXM_PM, 1, 1, 80, 28, 70, 80, 40, 50, 20), OP(FXW_SINE, FXM_PM, 2, 0, 70, 30, 70, 70, 40, 50, 20),
         NOOP, NOOP},
        {{0, 100, 70, 50}, {0, 70}, {0}, {0}, {0}, {0}}, {110, 0, 0, 0, 0, 0}, 35, 64, 0, 100, "FMX BRASS"},
    {   /* F5: an organ, six sines added (32: all carriers) */
        {OP(FXW_SINE, FXM_PM, 0, 0, 120, 0, 0, 127, 20, 0, 0), OP(FXW_SINE, FXM_PM, 1, 0, 124, 0, 0, 127, 20, 0, 0),
         OP(FXW_SINE, FXM_PM, 2, 0, 116, 0, 0, 127, 20, 0, 0), OP(FXW_SINE, FXM_PM, 3, 0, 108, 0, 0, 127, 20, 0, 0),
         OP(FXW_SINE, FXM_PM, 4, 0, 112, 0, 0, 127, 20, 0, 0), OP(FXW_SINE, FXM_PM, 8, 0, 100, 0, 40, 0, 20, 0, 0)},
        {{0}, {0}, {0}, {0}, {0}, {0, 0, 0, 0, 0, 40}}, {60, 70, 60, 50, 50, 40}, 32, 64, 0, 100, "FMX ORGAN"},
    {   /* F6: ring bells: two sines ringing each other (RING), a 3rd harmonic on top */
        {OP(FXW_SINE, FXM_RING, 1, 0, 127, 0, 96, 0, 90, 40, 10), OP(FXW_SINE, FXM_PM, 2, 41, 120, 0, 90, 0, 88, 0, 0),
         OP(FXW_SINE, FXM_RING, 3, 0, 120, 0, 84, 0, 80, 30, 10), OP(FXW_SINE, FXM_PM, 5, 13, 120, 0, 80, 0, 80, 0, 0),
         NOOP, NOOP},
        {{0, 127}, {0}, {0, 0, 0, 127}, {0}, {0}, {0}}, {100, 0, 70, 0, 0, 0}, 0, 64, 0, 100, "RING BELLS"},
    {   /* F7: true FM: a half-wave modulator bends the pitch as it modulates (FM mode), a growl */
        {OP(FXW_SINE, FXM_FM, 1, 0, 120, 0, 80, 90, 30, 30, 0), OP(FXW_HALF, FXM_PM, 1, 0, 100, 10, 70, 70, 40, 60, 10),
         OP(FXW_SINE, FXM_PM, 2, 0, 80, 0, 60, 40, 30, 60, 20), NOOP, NOOP, NOOP},
        {{0, 90}, {0, 0, 60}, {0, 0, 90}, {0}, {0}, {0}}, {110, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "TRUE FM"},
    {   /* F8: a chip lead: SIN8 and a square, a little feedback */
        {OP(FXW_SIN8, FXM_PM, 1, 0, 120, 0, 60, 100, 30, 20, 0), OP(FXW_SQR, FXM_PM, 1, 0, 70, 0, 50, 60, 30, 40, 0),
         NOOP, NOOP, NOOP, NOOP},
        {{0, 60}, {0, 60}, {0}, {0}, {0}, {0}}, {110, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "CHIP LEAD"},
    {   /* F9: OPL-style keys: quarter and half sines */
        {OP(FXW_QUART, FXM_PM, 1, 0, 124, 0, 84, 30, 50, 40, 10), OP(FXW_HALF, FXM_PM, 2, 0, 90, 0, 66, 20, 40, 60, 20),
         NOOP, NOOP, NOOP, NOOP},
        {{0, 90}, {0}, {0}, {0}, {0}, {0}}, {115, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "OPL KEYS"},
    {   /* F10: a pad, two detuned stacks, slow */
        {OP(FXW_SINE, FXM_PM, 1, 0, 120, 70, 90, 110, 80, 0, 0), OP(FXW_TRI, FXM_PM, 2, 0, 70, 90, 100, 90, 80, 0, 20),
         OP(FXW_SINE, FXM_PM, 1, 1, 120, 70, 90, 110, 80, 0, 0), OP(FXW_SINE, FXM_PM, 3, 0, 60, 96, 100, 80, 80, 0, 20),
         NOOP, NOOP},
        {{0, 80}, {0}, {0, 0, 0, 80}, {0}, {0}, {0}}, {90, 0, 90, 0, 0, 0}, 5, 64, 0, 100, "FMX PAD"},
    {   /* F11: a kick: a sine dropping from two octaves up (PEG), a click of noise */
        {OP(FXW_SINE, FXM_PM, 1, 0, 127, 0, 70, 0, 50, 30, 0), OP(FXW_NOISE, FXM_PM, 1, 0, 70, 0, 20, 0, 10, 50, 0),
         NOOP, NOOP, NOOP, NOOP},
        {{0, 20}, {0}, {0}, {0}, {0}, {0}}, {127, 0, 0, 0, 0, 0}, 0, 112, 72, 100, "FM KICK"},
    {   /* F12: a metal hit: a cascade at inharmonic ratios */
        {OP(FXW_SINE, FXM_PM, 1, 0, 127, 0, 74, 0, 70, 40, 0), OP(FXW_SINE, FXM_PM, 1, 41, 100, 0, 70, 0, 66, 60, 0),
         OP(FXW_SINE, FXM_PM, 3, 17, 100, 0, 66, 0, 60, 60, 0), OP(FXW_SINE, FXM_PM, 5, 73, 90, 0, 60, 0, 60, 60, 0),
         NOOP, NOOP},
        {{0, 110}, {0, 0, 100}, {0, 0, 0, 90}, {0, 0, 0, 80}, {0}, {0}}, {110, 0, 0, 0, 0, 0}, 33, 64, 0, 100, "METAL HIT"},
    {   /* F13: a sync-like lead: true FM on a saw, the modulator at 2x */
        {OP(FXW_SAW, FXM_FM, 1, 0, 116, 0, 70, 100, 30, 20, 0), OP(FXW_SINE, FXM_PM, 2, 0, 90, 0, 80, 60, 30, 60, 10),
         NOOP, NOOP, NOOP, NOOP},
        {{0, 70}, {0}, {0}, {0}, {0}, {0}}, {105, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "FM SYNC"},
    {   /* F14: a pluck: 3 modulators, short, the carriers two octaves apart */
        {OP(FXW_SINE, FXM_PM, 1, 0, 124, 0, 72, 0, 50, 40, 10), OP(FXW_SINE, FXM_PM, 3, 0, 104, 0, 50, 0, 40, 70, 30),
         OP(FXW_SINE, FXM_PM, 4, 0, 110, 0, 66, 0, 46, 40, 10), OP(FXW_TRI, FXM_PM, 1, 0, 90, 0, 44, 0, 40, 70, 30),
         NOOP, NOOP},
        {{0, 100}, {0}, {0, 0, 0, 90}, {0}, {0}, {0}}, {100, 0, 70, 0, 0, 0}, 5, 64, 0, 100, "FMX PLUCK"},
    {   /* F15: a wobble bass: a fifth up modulator in true FM, feedback */
        {OP(FXW_SINE, FXM_PM, 0, 0, 124, 0, 70, 110, 30, 20, 0), OP(FXW_SINE, FXM_FM, 1, 50, 100, 0, 70, 90, 30, 40, 0),
         NOOP, NOOP, NOOP, NOOP},
        {{0, 100}, {0, 100}, {0}, {0}, {0}, {0}}, {120, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "FM WOBBLE"},
    {   /* F16: init: one sine */
        {OP(FXW_SINE, FXM_PM, 1, 0, 127, 0, 0, 127, 30, 0, 0), NOOP, NOOP, NOOP, NOOP, NOOP},
        {{0}, {0}, {0}, {0}, {0}, {0}}, {110, 0, 0, 0, 0, 0}, 0, 64, 0, 100, "INIT"},
};
#undef OP
#undef NOOP
/* (fixed with SLOOP 2.8.3's port) the names end with a 0: params.c param_format counts an F_INT value's names up to
 * it. Without it the count ran past the list into whatever came after (FM KICK, ALG past 32: a FELUCCA CRASH, the
 * screen glitched; tests/param_names_test.c) */
static const char *const N_FMX_PATCH[FMX_NSLOT + 1u] = {
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16",
    "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "B10", "B11", "B12", "B13", "B14", "B15", "B16", "B17",
    "B18", "B19", "B20", "B21", "B22", "B23", "B24", "B25", "B26", "B27", 0};
static const char *const N_FMX_MODE[] = {"PM", "FM", "RING"};
static const char *const N_FMX_FIX[] = {"RATIO", "FIX"};

/* ---------------------------------------------------------------------------------------- the parts' patches --- */
static fmx_patch_t fmx_cur[NPART];             /* the patch each part plays (the ISR reads it, the UI edits it) */
static volatile uint8_t fmx_dirty[NPART];      /* the UI (or the editor) changed it: fmx_compile at the next block */
static int16_t fmx_slot_seen[NPART];           /* PTCH last loaded + 1 (0: none yet) */
static int (*fmx_bank_store)(uint32_t k, const uint8_t *pk);   /* fm6_bank.c: patch pk into bank slot k (0 ok) */

typedef struct {                 /* the patch through the macros (the ISR's, rebuilt when either changes) */
    uint8_t ok, n[FMX_NOP], src[FMX_NOP][FMX_NOP];   /* per operator: its modulators */
    int16_t amt[FMX_NOP][FMX_NOP];                  /* .. their amounts (Q7: 128 = the template's full) */
    int16_t outg[FMX_NOP];                          /* to the output, Q15 */
    uint8_t carriers, wave[FMX_NOP], mode[FMX_NOP], adsr[FMX_NOP][4];
    uint32_t ratio[FMX_NOP];                        /* Q16 of the note's increment (FIX: the increment itself) */
    int16_t e[8];                                   /* P_E0..P_E7 it was made with */
    uint32_t gen;
} fmx_eff_t;
static fmx_eff_t fmx_eff[NPART];

typedef struct {
    uint32_t ph[FMX_NOP];
    int32_t e[FMX_NOP];          /* envelopes, Q24 */
    int32_t g[FMX_NOP];          /* the operators' gains at the end of the last tick, Q15 */
    int16_t y1[FMX_NOP], y2[FMX_NOP];   /* their last two outputs */
    uint8_t st[FMX_NOP];
    uint8_t live;
    int32_t peg;                 /* the pitch envelope, 1/4096 semitone */
    uint32_t rnd;
} fmx_voice_t;
typedef struct {
    fmx_voice_t v[NVOICE];
} fmx_part_t;

/* the ratio of operator o (Q16 of the note's increment), or (FIX) its increment */
static uint32_t fmx_ratio(const fmx_op_t *o)
{
    uint32_t f = o->fine > 99u ? 99u : o->fine;
    if (o->mode & FXM_FIX) {                         /* 10^(coarse & 3) x 10^(fine / 100) Hz -> an increment */
        static const uint32_t HZ_INC[4] = {97391u, 973911u, 9739112u, 97391116u};   /* 2^32 x 1, 10, 100, 1000 / FS */
        return (uint32_t)(((uint64_t)HZ_INC[o->coarse & 3u] * FMX_P10[f]) >> 16);
    }
    {
        uint32_t c = o->coarse > 31u ? 31u : o->coarse;
        uint32_t r = c ? c << 16 : 1u << 15;         /* 0: x0.5 */
        r += (uint32_t)(((uint64_t)r * f) * 655u >> 16);   /* x (1 + fine / 100): 655 / 65536 ~ 1 / 100 */
        r += (uint32_t)(((int32_t)(o->dtn > 14u ? 14 : o->dtn) - 7) * (int32_t)(r >> 11));   /* DTN: ~0.85 cents */
        return r;
    }
}

/* the effective patch of part tr: its patch, the template ALG (E1 > 0), MOD (E2), FB (E3), TIME (E4), DTN (E5) */
static void fmx_compile(uint32_t tr, const int16_t *e)
{
    const fmx_patch_t *p = &fmx_cur[tr];
    fmx_eff_t *f = &fmx_eff[tr];
    uint32_t d, s, alg = (uint32_t)clamp(e[1], 0, (int32_t)FMX_NALG);
    int32_t modk = clamp(e[2], 0, 127), fb = clamp(e[3], -64, 63), tm = clamp(e[4], -64, 63), dt = clamp(e[5], 0, 127);
    uint8_t car = 0;
    for (d = 0; d < FMX_NOP; d++)
        car |= (uint8_t)((p->out[d] ? 1u : 0u) << d);
    if (alg)
        car = FMX_ALG[alg - 1u][6];
    f->carriers = car;
    for (d = 0; d < FMX_NOP; d++) {
        const fmx_op_t *o = &p->op[d];
        uint32_t k;
        f->n[d] = 0;
        for (s = 0; s < FMX_NOP; s++) {
            int32_t a = p->mod[d][s];
            if (alg)                                     /* a template: its connections, the patch's amount if it
                                                          * has one there, else 100 (feedback: 60) */
                a = (FMX_ALG[alg - 1u][d] >> s) & 1u ? (a ? a : s == d ? 60 : 100) : 0;
            if (!a)
                continue;
            a = s == d ? a + fb : a * modk >> 6;         /* FB moves the feedback, MOD the rest (64 = as it is) */
            if (a <= 0)
                continue;
            if (a > 255)
                a = 255;
            f->src[d][f->n[d]] = (uint8_t)s;
            f->amt[d][f->n[d]++] = (int16_t)(s == d ? a : a << 3);   /* (feedback: an eighth of the scale, the DX7's:
                                                          * 127 a quarter turn, its FB 7) */
        }
        f->outg[d] = (int16_t)((car >> d) & 1u ? (alg ? 100 : p->out[d]) * 258 : 0);
        f->wave[d] = o->wave < FMX_NWAVE ? o->wave : FXW_SINE;
        f->mode[d] = o->mode;
        f->ratio[d] = fmx_ratio(o);
        if (dt && !(o->mode & FXM_FIX))                  /* DTN: spread, odd operators up, even ones down */
            f->ratio[d] += (uint32_t)((d & 1u ? -1 : 1) * (int32_t)(f->ratio[d] >> 14) * dt / 32);
        for (k = 0; k < 4u; k++) {
            int32_t v = o->adsr[k];
            if (!((car >> d) & 1u) && k != 2u)           /* TIME: the modulators' times (the brightness's shape) */
                v += tm;
            f->adsr[d][k] = (uint8_t)clamp(v, 0, 127);
        }
    }
    for (d = 0; d < 8u; d++)
        f->e[d] = e[d];
    f->ok = 1;
    f->gen++;
}

/* a DX7 voice (fm6_unpack's 155 bytes) as an FMX patch */
static void fmx_from_dx7(const uint8_t *v, fmx_patch_t *p)
{
    uint32_t k, d, s, alg = v[FP_ALG] & 31u;
    memset(p, 0, sizeof *p);
    for (k = 0; k < FMX_NOP; k++) {
        const uint8_t *o = v + (5u - k) * FP_OP;         /* (the voice keeps operator 6 first) */
        fmx_op_t *q = &p->op[k];
        int32_t r1 = o[FP_R1], r2 = o[FP_R1 + 1], r3 = o[FP_R1 + 2], r4 = o[FP_R1 + 3], l3 = o[FP_L1 + 2];
        q->wave = FXW_SINE;
        q->mode = (uint8_t)(FXM_PM | (o[FP_MODE] ? FXM_FIX : 0u));
        q->coarse = (uint8_t)(o[FP_FC] & 31u);
        q->fine = (uint8_t)(o[FP_FF] > 99u ? 99u : o[FP_FF]);
        q->level = (uint8_t)(o[FP_OL] ? clamp(127 - (99 - o[FP_OL]), 0, 127) : 0);
        q->adsr[0] = (uint8_t)clamp((99 - r1) * 110 / 99, 0, 127);   /* a fast rate: a short time */
        q->adsr[1] = (uint8_t)clamp((99 - (r2 < r3 ? r2 : r3)) * 120 / 99, 0, 127);
        q->adsr[2] = (uint8_t)clamp(l3 * 127 / 99, 0, 127);
        q->adsr[3] = (uint8_t)clamp((99 - r4) * 120 / 99, 0, 127);
        q->vel = (uint8_t)((o[FP_KVS] & 7u) * 18u);
        q->ksc = (uint8_t)((o[FP_RC] == 0u || o[FP_RC] == 1u) ? o[FP_RD] * 127u / 99u : 0u);   /* -LIN, -EXP */
        q->dtn = (uint8_t)(o[FP_DET] > 14u ? 14u : o[FP_DET]);
    }
    for (d = 0; d < FMX_NOP; d++) {
        for (s = 0; s < FMX_NOP; s++)
            if ((FMX_ALG[alg][d] >> s) & 1u)
                p->mod[d][s] = (uint8_t)(s == d ? (v[FP_FB] & 7u) * 18u : 120u);
        p->out[d] = (uint8_t)((FMX_ALG[alg][6] >> d) & 1u ? 100u : 0u);
    }
    p->alg = (uint8_t)(alg + 1u);
    p->peg = 64;
    p->level = 100;
    for (k = 0; k < 10u; k++)
        p->name[k] = (char)(v[FP_NAME + k] >= 32u && v[FP_NAME + k] <= 126u ? v[FP_NAME + k] : ' ');
}

/* every byte in its range (a patch from the editor or the bank) */
static void fmx_sanitize(fmx_patch_t *p)
{
    uint32_t k, s;
    for (k = 0; k < FMX_NOP; k++) {
        fmx_op_t *o = &p->op[k];
        uint8_t *b = (uint8_t *)o;
        for (s = 0; s < sizeof *o; s++)
            b[s] &= 127u;
        if (o->wave >= FMX_NWAVE)
            o->wave = FXW_SINE;
        if ((o->mode & 3u) >= FXM_N)
            o->mode = (uint8_t)(o->mode & FXM_FIX);
        o->mode &= 3u | FXM_FIX;
        if (o->coarse > 31u)
            o->coarse = 31;
        if (o->fine > 99u)
            o->fine = 99;
        if (o->dtn > 14u)
            o->dtn = 14;
        for (s = 0; s < FMX_NOP; s++)
            p->mod[k][s] &= 127u;
        p->out[k] &= 127u;
    }
    if (p->alg > FMX_NALG)
        p->alg = 0;
    p->peg &= 127u, p->ptime &= 127u, p->level &= 127u;
    for (k = 0; k < 10u; k++)
        if ((uint8_t)p->name[k] < 32u || (uint8_t)p->name[k] > 126u)
            p->name[k] = ' ';
}

/* PTCH s -> patch: F1..F16 factory, B1..B27 the bank (an FMX record as it is, a DX7 voice converted, an empty
 * slot: INIT) */
static void fmx_slot_patch(uint32_t s, fmx_patch_t *p)
{
    uint8_t pk[FM6_PACKED];
    if (s < FMX_NFACTORY) {
        *p = FMX_FACTORY[s];
        return;
    }
    s -= FMX_NFACTORY;
    if (s >= FM6_BANK_N || !fm6_bank_read || fm6_bank_read(s, pk)) {   /* (fm6_bank.c's: the record as it is) */
        *p = FMX_FACTORY[FMX_NFACTORY - 1u];
        return;
    }
    if ((fm6_bank_fmx >> s) & 1u) {
        memcpy(p, pk, sizeof *p);
        fmx_sanitize(p);
    } else {
        uint8_t v[FP_SIZE + 1u];
        fm6_unpack(pk, v);
        fmx_from_dx7(v, p);
    }
}

/* once a block, part t: PTCH turned -> its patch; the patch or the macros changed -> fmx_eff rebuilt */
static void fmx_block(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk), k;
    int16_t s;
    if (tr >= NPART)
        return;
    s = (int16_t)clamp(t->p[P_E0], 0, (int32_t)FMX_NSLOT - 1);
    if (((fmx_reload >> tr) & 1u) && !t->xf_on) {     /* a sound loaded (after the old one's fade): its patch */
        fmx_reload &= (uint8_t)~(1u << tr);
        fmx_slot_seen[tr] = 0;
    }
    if (fmx_slot_seen[tr] != s + 1 && (!fmx_slot_seen[tr] || t->dk_free)) {   /* (0.9: PTCH turned under a sound:
                                                         * when the part's duck holds it silent, fx.c part_duck) */
        fmx_slot_seen[tr] = (int16_t)(s + 1);
        fmx_slot_patch((uint32_t)s, &fmx_cur[tr]);
        fmx_dirty[tr] = 1;
    }
    for (k = 0; k < 8u; k++)
        if (fmx_eff[tr].e[k] != t->p[P_E0 + k])
            fmx_dirty[tr] = 1;
    if (fmx_dirty[tr] || !fmx_eff[tr].ok) {
        fmx_dirty[tr] = 0;
        fmx_compile(tr, &t->p[P_E0]);
    }
}

static fmx_voice_t *fmx_voice(track_t *t, voice_t *v)
{
    fmx_part_t *P = (fmx_part_t *)eng_arena_of(t, ENGI_FMX);
    return P ? &P->v[(uint32_t)(v - t->v) % NVOICE] : 0;
}

static void fmx_note_on(track_t *t, voice_t *v)
{
    fmx_voice_t *x = fmx_voice(t, v);
    uint32_t tr = (uint32_t)(t - trk), k;
    if (!x)
        return;
    if (tr < NPART)
        fmx_block(t);                                /* (the patch of its sound: a note can come before a block) */
    if (!x->live || (!v->env && !v->env_out)) {      /* a fresh voice: from rest; a retrigger keeps its phases (a voice
                                                      * given up, voice.c voice_kill, ended without fmx_done: as FM6) */
        memset(x, 0, sizeof *x);
        x->rnd = 0x9E3779B9u * ((uint32_t)v->note + 1u);
    }
    x->live = 1;
    for (k = 0; k < FMX_NOP; k++)
        x->st[k] = 1;                                /* (attack: from where the envelope is) */
    x->peg = tr < NPART ? ((int32_t)fmx_cur[tr].peg - 64) * 3 * 4096 / 8 : 0;   /* +-24 semitones at 0 / 127 */
}

/* (after Clipper 1.2) UNISON (voice.c unison_spread): the voice's operators moved together by r (Q32) of a turn of its
 * note, each by its own increment's share: the relative phases, the timbre, kept */
static void fmx_spread(track_t *t, voice_t *v, uint32_t r)
{
    fmx_voice_t *x = fmx_voice(t, v);
    uint32_t tr = (uint32_t)(t - trk), k, base;
    if (!x || tr >= NPART)
        return;
    base = pitch_inc((uint32_t)clamp(v->pitch16, 0, 2047));
    for (k = 0; k < FMX_NOP; k++)                    /* (FIX: r / base samples of its own increment; per note) */
        x->ph[k] += fmx_eff[tr].mode[k] & FXM_FIX ? (uint32_t)(((uint64_t)r * fmx_eff[tr].ratio[k]) / (base ? base : 1u))
                                                  : (uint32_t)(((uint64_t)r * fmx_eff[tr].ratio[k]) >> 16);
}

static int fmx_done(track_t *t, voice_t *v)
{
    fmx_voice_t *x = fmx_voice(t, v);
    uint32_t tr = (uint32_t)(t - trk), k, car;
    if (!x || !x->live || tr >= NPART)
        return 1;
    car = fmx_eff[tr].carriers;
    for (k = 0; k < FMX_NOP; k++)
        if (((car >> k) & 1u) && x->st[k])
            return 0;
    x->live = 0;
    return 1;
}

/* one sample of wave w at phase ph (the 8-harmonic copy for SAW SQR PULSE above ~460 Hz: hi) */
static inline int32_t fmx_wave(uint32_t w, uint32_t ph, int hi, uint32_t *rnd)
{
    const int16_t *T;
    uint32_t i;
    int32_t a, b;
    switch (w) {
    case FXW_SINE:
        return sine_i(ph);
    case FXW_NOISE:
        *rnd = *rnd * 1664525u + 1013904223u;
        return (int32_t)(*rnd >> 16) - 32768;
    case FXW_SAW:
        T = FMX_WAVE[hi ? FXW_SAW8 : FXW_SAW];
        break;
    case FXW_SQR:
        T = FMX_WAVE[hi ? FXW_SQR8 : FXW_SQR];
        break;
    case FXW_PULSE:
        T = FMX_WAVE[hi ? FXW_PULS8 : FXW_PULSE];
        break;
    default:
        T = FMX_WAVE[w];
        break;
    }
    i = ph >> 22;
    a = T[i];
    b = T[(i + 1u) & (FMX_TN - 1u)];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}

static void fmx_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    uint32_t tr = (uint32_t)(t - trk), i, k, base;
    fmx_voice_t *x = fmx_voice(t, v);
    const fmx_eff_t *f;
    const fmx_patch_t *pt;
    uint32_t inc[FMX_NOP];
    int32_t g0[FMX_NOP], dg[FMX_NOP], vel = v->vel, ktk = clamp(t->p[P_E7], -64, 63);
    int32_t vmod = clamp(t->p[P_E6], -64, 63), keyo = (int32_t)v->note - 60;
    uint8_t hi[FMX_NOP];
    uint32_t act = 0;                                /* the operators sounding this tick */
    int16_t amt[FMX_NOP][FMX_NOP];
    int32_t shp = (m->shape - (64 << 8)) >> 8;      /* SHAPE (LFO / ENV DEST SHP, MW / AT SHP, the matrix): the depth */
    if (!x || tr >= NPART)
        return;
    f = &fmx_eff[tr];
    pt = &fmx_cur[tr];
    if (!f->ok)
        return;
    /* the pitch envelope: decays to 0 at PTIM */
    if (x->peg) {
        x->peg -= (int32_t)(((int64_t)x->peg * ENV_EXP[pt->ptime & 127u]) >> 16);
        if (x->peg > -64 && x->peg < 64)
            x->peg = 0;
    }
    base = x->peg ? hy_inc(m->pitch16, 0, m->fine + x->peg) : m->inc;
    for (k = 0; k < FMX_NOP; k++) {
        const fmx_op_t *o = &pt->op[k];
        int32_t e = hy_env(&x->e[k], &x->st[k], f->adsr[k], v->gate), lv, g;
        uint32_t j;
        for (j = 0; j < f->n[k]; j++)
            amt[k][j] = (int16_t)(f->src[k][j] == k || !shp ? f->amt[k][j] : clamp(f->amt[k][j] * (64 + shp) >> 6, 0, 2040));
        int car = (f->carriers >> k) & 1u;
        lv = o->level;
        if (o->ksc && keyo > 0)                      /* KSC: level down above C4 */
            lv -= (int32_t)o->ksc * keyo / 96;
        if (!car)                                    /* VEL (E6) and KTRK (E7): the modulators' depth */
            lv += (vmod * (vel - 64)) / 64 - ktk * keyo / 24;
        g = FMX_LVL[clamp(lv, 0, 127)];
        g = (g * e) >> 15;
        if (o->vel)                                  /* VEL: soft notes quieter (this operator) */
            g = (int32_t)(((int64_t)g * (32768 - (int32_t)o->vel * (127 - vel) * 2)) >> 15);
        g0[k] = x->g[k];
        dg[k] = (g - g0[k]) / (int32_t)n;
        x->g[k] = g;
        inc[k] = f->mode[k] & FXM_FIX ? f->ratio[k] : (uint32_t)(((uint64_t)base * f->ratio[k]) >> 16);
        if (inc[k] > 0x7FFFFFFFu)
            inc[k] = 0x7FFFFFFFu;
        hi[k] = inc[k] > 44700000u;                      /* (~460 Hz: the 8-harmonic copy) */
        if (g0[k] | g)
            act |= 1u << k;
        else                                         /* silent the whole tick: its phase runs on, nothing else */
            x->ph[k] += inc[k] * n, x->y1[k] = x->y2[k] = 0;
    }
    for (i = 0; i < n; i++) {
        int32_t y[FMX_NOP], acc, s = 0;
        uint32_t j;
        for (k = FMX_NOP; k-- > 0;) {
            uint32_t ph = x->ph[k];
            int32_t w;
            if (!((act >> k) & 1u)) {
                y[k] = 0;
                continue;
            }
            acc = 0;
            for (j = 0; j < f->n[k]; j++) {
                uint32_t q = f->src[k][j];
                int32_t in = q > k ? y[q] : q == k ? (x->y1[q] + x->y2[q]) >> 1 : x->y1[q];
                acc += in * amt[k][j];
            }
            switch (f->mode[k] & 3u) {
            case FXM_FM:                             /* the frequency moves: +-4x at full */
                x->ph[k] = ph + inc[k] + (uint32_t)(int32_t)(((int64_t)(int32_t)(inc[k] >> 1) * acc) >> 22);
                w = fmx_wave(f->wave[k], ph, hi[k], &x->rnd);
                break;
            case FXM_RING:                           /* the wave times what modulates it */
                w = fmx_wave(f->wave[k], ph, hi[k], &x->rnd);
                if (f->n[k])
                    w = (w * clamp(acc >> 10, -32767, 32767)) >> 15;
                x->ph[k] = ph + inc[k];
                break;
            default:                                 /* PM: +-2 turns at full */
                w = fmx_wave(f->wave[k], ph + ((uint32_t)acc << 8), hi[k], &x->rnd);
                x->ph[k] = ph + inc[k];
                break;
            }
            w = (w * (g0[k] + dg[k] * (int32_t)i)) >> 15;
            y[k] = w;
            if (f->outg[k])
                s += (w * f->outg[k]) >> 15;
        }
        for (k = 0; k < FMX_NOP; k++)
            if ((act >> k) & 1u)
                x->y2[k] = x->y1[k], x->y1[k] = (int16_t)clamp(y[k], -32767, 32767);
        s = (s * pt->level) >> 8;                    /* (level 100: ~0.4: two carriers at full stay in range) */
        if (s > 20000 || s < -20000)
            s = soft_knee(s, 20000);
        out[i] += mulq15(mulq15(s << 1, amp_at(m, i)), VOICE_FS) << 1;
    }
}

static const preset_t FMX_PRESETS[] = {
    /* {PTCH, ALG, MOD, FB, TIME, DTN, VEL, KTRK}, {A D S R} (ownenv: unused), ENV -> FILTER, mono */
    {"FMX EP", {0, 0, 64, 0, 0, 0, 10, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 40, 20, 35)},
    {"FMX BELL", {1, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 10, 30, 70)},
    {"SAW FM BASS", {2, 0, 64, 0, 0, 0, 10, 0}, {0, 0, 127, 0}, 0, 1, FX(0, 0, 10, 10)},
    {"FMX BRASS", {3, 0, 64, 0, 0, 0, 10, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 20, 20, 40)},
    {"FMX ORGAN", {4, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 0, FX(10, 40, 0, 30)},
    {"RING BELLS", {5, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 20, 30, 60)},
    {"TRUE FM GRWL", {6, 0, 64, 0, 0, 0, 10, 0}, {0, 0, 127, 0}, 0, 1, FX(10, 0, 10, 20)},
    {"CHIP FM LEAD", {7, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 1, FX(0, 10, 20, 20), XP(P_GLIDE + 1, 15)},
    {"OPL KEYS", {8, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 20, 20, 30)},
    {"FMX PAD", {9, 0, 64, 0, 0, 30, 0, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 50, 30, 70)},
    {"FM KICK", {10, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 1, FX(0, 0, 0, 10)},
    {"METAL HIT", {11, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 0, 20, 50)},
    {"FM SYNC LEAD", {12, 0, 64, 0, 0, 0, 10, 0}, {0, 0, 127, 0}, 0, 1, FX(0, 10, 20, 30), XP(P_GLIDE + 1, 20)},
    {"FMX PLUCK", {13, 0, 64, 0, 0, 0, 10, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 20, 30, 40)},
    {"FM WOBBLE", {14, 0, 64, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 1, FX(10, 0, 0, 10), XP(P_LRATE + 1, 70, P_LD_SHP + 1, 40)},
    {"CASCADE", {3, 33, 48, 20, 0, 0, 10, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 20, 20, 40)},
};

static const engine_t ENG_FMX = {
    "FMX", {"FMX", "SHAPE"},
    {
        {"PTCH", F_INT, 0, FMX_NSLOT - 1, 0, N_FMX_PATCH, 0},
        {"ALG", F_INT, 0, FMX_NALG, 0, N_FMX_ALG, 0},
        {"MOD", F_PCT, 0, 127, 64, 0, 0},
        {"FB", F_BIPCT, -64, 63, 0, 0, 0},
        {"TIME", F_BIPCT, -64, 63, 0, 0, 0},
        {"DTN", F_PCT, 0, 127, 0, 0, 0},
        {"VEL", F_BIPCT, -64, 63, 0, 0, 0},
        {"KTRK", F_BIPCT, -64, 63, 0, 0, 0},
    },
    FMX_PRESETS, sizeof(FMX_PRESETS) / sizeof(FMX_PRESETS[0]), -1, fmx_note_on, fmx_render,
    0xFD20, {P_E0, P_E1, P_E2, P_E3}, 0, 0, 0, fmx_block,
    .ownenv = 1, .done = fmx_done,
};
