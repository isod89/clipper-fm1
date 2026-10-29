/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* VPS: vector phaseshaping (Clipper 0.5; Felucca discussion #145, after J. Kleimola, V. Lazzarini, J. Timoney and
 * V. Valimaki, "Vector Phaseshaping Synthesis", DAFx-11: the idea, not any code). A sine read through a bent phase:
 * the phase phi (0..1) is mapped by a line through the inflection point (D, V): phi < D goes 0 -> V, phi >= D goes
 * V -> 1, and the output is -cos(2 pi P(phi)). V below 1: a pulse-like, phase-distorted wave (CZ style); V above 1:
 * V cycles squeezed into the first D of the period, a formant whose centre is V x f0 and whose width D sets, as a
 * hard sync or a vowel sweep does. A non-integer V would leave a cut-off cycle at D (a bright spike): the output is
 * the crossfade of floor(V) and floor(V) + 1, so V sweeps smoothly and every cycle is whole. A few multiplies and two
 * sine reads a sample: no table, no filter bank.
 * EDIT 1  D  V  VENV  OSC2        EDIT 2  CUT  RES  DTN  SUB
 *   D     the inflection's phase (2 % .. 98 %); SHAPE (LFO / ENV DEST SHP, MW / AT SHP, the matrix) moves it
 *   V     its height, 0 .. 7.9 cycles; VENV the amp envelope on it (+-4 cycles: a formant sweep with each note)
 *   OSC2  a second VPS oscillator, D mirrored (1 - D), detuned by DTN (0 .. 100 cents): wider, a second formant
 *   SUB   a sine an octave down
 *   CUT RES  a 12 dB low-pass (keyboard ~40 %), after them
 * High notes: V is held under ~8 kHz / f0 (a formant above it would only alias).
 * Voice state: ph[0..2] osc 1, osc 2, the sub; s[0..1] the SVF; s[2] the DC blocker (V < 1 and a short D: a DC). */
typedef struct {
    int32_t d, s1, s2, v;        /* the inflection's phase (Q16), the two slopes (Q8 of turns per turn), its V (Q8) */
} vps_seg_t;

static void vps_seg(vps_seg_t *g, int32_t d16, int32_t v8)   /* 32-bit divides: twice a block per oscillator */
{
    g->d = d16;
    g->v = v8;
    g->s1 = (v8 << 16) / d16;
    g->s2 = (256 - v8) * 65536 / (65536 - d16);
}
/* the bent phase of ph, as a sine phase (P mod 1 in 32 bits) */
static inline uint32_t vps_ph(const vps_seg_t *g, uint32_t ph)
{
    int32_t f = (int32_t)(ph >> 16), p;
    if (f < g->d)
        p = (f * g->s1) >> 8;                        /* (< V x 65536 <= 2^27) */
    else
        p = (g->v << 8) + (((f - g->d) * g->s2) >> 8);   /* (|(f - d) s2| < |1 - V| x 65536) */
    return (uint32_t)p << 16;                        /* (the whole turns fall off: a sine phase) */
}
/* one VPS sample: floor(V) and floor(V) + 1 crossed by V's fraction (fr, Q8) */
static inline int32_t vps_out(const vps_seg_t *a, const vps_seg_t *b, int32_t fr, uint32_t ph)
{
    int32_t y = osc_sine(vps_ph(a, ph) - 0x40000000u);   /* -cos */
    if (fr)
        y += ((osc_sine(vps_ph(b, ph) - 0x40000000u) - y) * fr) >> 8;
    return y;
}

static void vps_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->env && !v->env_out) {
        v->ph[0] = v->ph[2] = 0;
        v->ph[1] = 0x40000000u;                      /* (osc 2 a quarter on: no comb when they line up) */
        v->s[0] = v->s[1] = v->s[2] = 0;
    }
}

static void vps_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    vps_seg_t a1, b1, a2, b2;
    int32_t d16 = 1310 + p[P_E0] * 495 + (m->shape - (64 << 8)) * 2;
    int32_t v8 = p[P_E1] * 16 + ((p[P_E2] * m->envq15) >> 11), vmax = 2032, fr, d2;
    int32_t mix2 = p[P_E3] * 258, sub = p[P_E7] * 258, ic1 = v->s[0], ic2 = v->s[1], dc = v->s[2];
    int32_t over = m->pitch16 - 83 * 16;
    uint32_t i, ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], inc = m->inc, inc2 = inc, inc_s = inc >> 1;
    tsvf_t flt;
    if (over > 0) {                                  /* V under ~8 kHz / f0: halved an octave up */
        uint32_t oc = (uint32_t)over / 192u, vo = oc >= 11u ? 0u : 2032u >> oc;
        vmax = (int32_t)(vo - vo * ((uint32_t)over % 192u) / 384u);
    }
    v8 = clamp(v8, 0, vmax);
    d16 = clamp(d16, 1310, 64226);
    d2 = 65536 - d16;
    fr = v8 & 255;
    vps_seg(&a1, d16, v8 & ~255);
    vps_seg(&b1, d16, (v8 & ~255) + 256);
    if (mix2) {
        int32_t fq = p[P_E6] * 30;                   /* cents x ~38 (Q16): 0 .. ~100 cents */
        inc2 = inc + (uint32_t)(((uint64_t)inc * (uint32_t)fq) >> 16);
        vps_seg(&a2, d2, v8 & ~255);
        vps_seg(&b2, d2, (v8 & ~255) + 256);
    }
    tsvf_coef(&flt, (p[P_E4] << 8) + m->cutoff + ((v->pitch16 - 60 * 16) << 3), p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t y = vps_out(&a1, &b1, fr, ph0), x;
        if (mix2)
            y += mulq15(vps_out(&a2, &b2, fr, ph1), mix2);
        if (sub)
            y += mulq15(osc_sine(ph2), sub);
        ph0 += inc;
        ph1 += inc2;
        ph2 += inc_s;
        dc += (y - (dc >> 8));                       /* DC blocker (~27 Hz): dc is 256 x its low-pass */
        x = (y - (dc >> 8)) * 5 >> 3;                /* (three sources: headroom) */
        x = tsvf_lp(&flt, x, &ic1, &ic2);
        if (x > 16000 || x < -16000)
            x = soft_knee(x, 16000);
        out[i] += mulq15(mulq15(x << 1, amp_at(m, i)), VOICE_FS) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = dc;
}

static const preset_t VPS_PRESETS[] = {
    /* {D, V, VENV, OSC2, CUT, RES, DTN, SUB}, {A D S R}, ENV -> FILTER, mono */
    {"VPS LEAD", {40, 40, 0, 0, 112, 10, 0, 0}, {0, 80, 110, 40}, 0, 1, FX(0, 0, 20, 30), XP(P_GLIDE + 1, 20)},
    {"FORMANT PAD", {70, 56, 16, 80, 100, 10, 30, 0}, {70, 100, 120, 90}, 0, 0, FX(0, 20, 10, 60), XP(P_LD_SHP + 1, 12)},
    {"VPS BASS", {20, 24, 30, 0, 80, 30, 0, 90}, {0, 70, 90, 30}, 10, 1, FX(10, 0, 0, 10)},
    {"SYNC SWEEP", {50, 64, 40, 0, 120, 0, 0, 0}, {0, 90, 100, 40}, 0, 1, FX(0, 0, 24, 30), XP(P_LRATE + 1, 40, P_LD_SHP + 1, 30)},
    {"VOX PLUCK", {80, 72, 30, 60, 110, 20, 10, 0}, {0, 76, 24, 56}, 0, 0, FX(0, 0, 20, 40)},
    {"HOLLOW KEYS", {36, 26, 10, 100, 104, 10, 5, 40}, {0, 80, 70, 50}, 0, 0, FX(0, 20, 10, 40)},
    {"DIGI BRASS", {60, 40, 40, 50, 105, 15, 20, 30}, {10, 80, 100, 40}, 10, 0, FX(0, 10, 10, 40)},
    {"VPS BELL", {90, 100, 0, 90, 127, 0, 80, 0}, {0, 90, 0, 90}, 0, 0, FX(0, 0, 20, 60)},
    {"WOBBLE VPS", {30, 48, 0, 0, 90, 30, 0, 80}, {0, 80, 110, 30}, 0, 1, FX(20, 0, 0, 10), XP(P_LRATE + 1, 70, P_LD_SHP + 1, 50)},
    {"VPS CHOIR", {64, 80, 10, 100, 96, 10, 40, 0}, {80, 100, 120, 100}, 0, 0, FX(0, 30, 10, 70), XP(P_LRATE + 1, 25, P_LD_SHP + 1, 8)},
};

/* (after Clipper 1.2) UNISON (voice.c unison_spread): oscillators 1, 2 (DTN up) and the sub moved by r (Q32)
 * of a turn of the note, each by its share (fixed phases at each note: every voice in phase) */
static void vps_spread(track_t *t, voice_t *v, uint32_t r)
{
    v->ph[0] += r;
    v->ph[1] += r + (uint32_t)(((uint64_t)r * (uint32_t)(t->p[P_E6] * 30)) >> 16);
    v->ph[2] += r >> 1;
}

static const engine_t ENG_VPS = {
    "VPS", {"VECTOR", "TONE"},
    {
        {"D", F_PCT, 0, 127, 50, 0, 0},
        {"V", F_INT, 0, 127, 32, 0, 0},
        {"VENV", F_BIPCT, -64, 63, 0, 0, 0},
        {"OSC2", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 110, 0, 0},
        {"RES", F_PCT, 0, 127, 10, 0, 0},
        {"DTN", F_PCT, 0, 127, 20, 0, 0},
        {"SUB", F_PCT, 0, 127, 0, 0, 0},
    },
    VPS_PRESETS, sizeof(VPS_PRESETS) / sizeof(VPS_PRESETS[0]), 1, vps_note_on, vps_render,
    0xFB2C, {P_E0, P_E1, P_E2, P_E4}, 0, 0, 0, 0,
};
