/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* JUNO (Clipper 0.8): the poly synth of the Roland Juno-6 / 60 / 106, one of the most loved sounds there is (its
 * idea: one DCO with a saw, a pulse whose width the LFO moves, a sub oscillator an octave down and noise; a high-pass
 * in four positions; a 24 dB resonant low-pass; and the BBD chorus, I, II or both). No code or sample of Roland's.
 * EDIT 1  WAVE  PW  SUB  NOIS        EDIT 2  HPF  CUT  RES  CHOR
 *   WAVE  SAW, PLS (the pulse), S+P (both, as the 106's two buttons)
 *   PW    the pulse's width, 50 % (square) .. 5 %; SHAPE (LFO DEST SHP: the PWM, MW / AT SHP, the matrix) moves it
 *   SUB   the square an octave down; NOIS white noise
 *   HPF   0 a little bass boost, 1 flat, 2 and 3 a high-pass at ~240 and ~720 Hz (the 106's four positions)
 *   CUT RES  the 24 dB ladder (dsp.c, Zavalishin's), keyboard tracking half; ENV DEST FLT its envelope
 *   CHOR  OFF, I (0.5 Hz, wide), II (0.8 Hz, deeper), I+II (8 Hz, shallow: the 60's both buttons): stereo, after the
 *         sound's INSERT and FILTER (fx.c juno_chorus: one per part; the INSERT's JUNO types give it to any sound)
 * The oscillators are band-limited (polyBLEP: dsp.c osc_saw / osc_pulse).
 * Voice state: ph[0] the DCO, ph[1] the sub, ph[2] the noise; s[0..3] the ladder, s[4] the HPF's low-pass, s[5] the
 * bass boost's */
enum { JW_SAW, JW_PLS, JW_SP };
static const char *const N_JWAVE[] = {"SAW", "PLS", "S+P"};
static const char *const N_JHPF[] = {"0", "1", "2", "3"};
static const char *const N_JCHOR[] = {"OFF", "I", "II", "I+II"};

static void juno_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->env && !v->env_out) {                    /* from silence: the filter clear (a DCO: its phase runs on) */
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = v->s[4] = v->s[5] = 0;
        if (!v->ph[2])
            v->ph[2] = 0x2545F491u + v->note * 2654435761u;
    }
}

static void juno_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, ph = v->ph[0], phs = v->ph[1], nz = v->ph[2], inc = m->inc, incs = inc >> 1;
    uint32_t wave = (uint32_t)clamp(p[P_E0], 0, 2), hpf = (uint32_t)clamp(p[P_E4], 0, 3);
    int32_t w = clamp(p[P_E1] * 256 + (m->shape - (64 << 8)) * 2, 0, 127 * 256);   /* 0 square .. 127 * 256 narrow */
    uint32_t pw = 0x80000000u - (uint32_t)w * 59000u;                               /* 50 % .. ~5 % */
    int32_t sub = p[P_E2] * 200, nois = p[P_E3] * 160, hp = v->s[4], bb = v->s[5];
    int32_t cut = (p[P_E5] << 8) + m->cutoff + ((v->pitch16 - 60 * 16) << 3);
    int32_t ls[4] = {v->s[0], v->s[1], v->s[2], v->s[3]};
    ladder_t lad;
    ladder_coef(&lad, cut, p[P_E6]);
    for (i = 0; i < n; i++) {
        int32_t s, y, a;
        if (wave == JW_SAW)
            s = osc_saw(ph, inc);
        else if (wave == JW_PLS)
            s = osc_pulse(ph, inc, pw);
        else
            s = (osc_saw(ph, inc) + osc_pulse(ph, inc, pw)) * 5 >> 3;
        if (sub)
            s += mulq15(osc_pulse(phs, incs, 0x80000000u), sub);
        if (nois) {
            nz = nz * 1664525u + 1013904223u;
            s += mulq15((int32_t)(nz >> 16) - 32768, nois);
        }
        ph += inc;
        phs += incs;
        s = s * 5 >> 3;                              /* (four sources: headroom) */
        if (hpf != 1u) {                             /* 0: bass boost (a low shelf ~120 Hz); 2, 3: a one-pole high-pass */
            if (!hpf) {
                bb += ((s - bb) * 560) >> 15;
                s += bb >> 1;
            } else {
                hp += ((s - hp) * (hpf == 2u ? 1120 : 3360)) >> 15;
                s -= hp;
            }
        }
        y = ladder_tick(&lad, s, ls) >> 1;
        a = y < 0 ? -y : y;
        if (a > 16000) {
            a = 16000 + (softclip((a - 16000) * 2) >> 1);
            y = y < 0 ? -a : a;
        }
        out[i] += mulq15(mulq15(y << 1, amp_at(m, i)), VOICE_FS) << 1;
    }
    v->ph[0] = ph;
    v->ph[1] = phs;
    v->ph[2] = nz;
    v->s[0] = ls[0], v->s[1] = ls[1], v->s[2] = ls[2], v->s[3] = ls[3];
    v->s[4] = hp;
    v->s[5] = bb;
}

static const preset_t JUNO_PRESETS[] = {
    /* {WAVE, PW, SUB, NOIS, HPF, CUT, RES, CHOR}, {A D S R}, ENV -> FILTER, mono; extras: the LFO on the PW */
    {"JUNO 106 PAD", {JW_SP, 40, 40, 0, 1, 88, 20, 1}, {70, 100, 115, 95}, 10, 0, FX(0, 0, 10, 40),
     XP(P_LRATE + 1, 28, P_LD_SHP + 1, 40, P_VINT + 1, 24)},
    {"JUNO BRASS", {JW_SAW, 0, 30, 0, 1, 62, 30, 0}, {14, 82, 100, 50}, 40, 0, FX(0, 0, 10, 30)},
    {"JUNO BASS", {JW_PLS, 50, 110, 0, 0, 58, 40, 0}, {0, 70, 70, 26}, 36, 1, FX(0, 0, 0, 8), XP(P_GLIDE + 1, 10)},
    {"JUNO STRINGS", {JW_SAW, 0, 0, 0, 2, 96, 10, 2}, {80, 100, 120, 96}, 0, 0, FX(0, 0, 10, 50),
     XP(P_LRATE + 1, 34, P_LD_SHP + 1, 30)},
    {"POLY KEYS", {JW_SP, 30, 20, 0, 1, 70, 24, 1}, {0, 88, 60, 60}, 30, 0, FX(0, 0, 14, 30)},
    {"SYNTHWAVE LD", {JW_SAW, 0, 60, 0, 1, 90, 30, 1}, {2, 80, 110, 50}, 10, 1, FX(0, 0, 30, 30),
     XP(P_GLIDE + 1, 24, P_LRATE + 1, 70, P_LD_PIT + 1, 3)},
    {"80S HITS", {JW_SP, 20, 50, 0, 1, 56, 50, 1}, {0, 64, 0, 40}, 50, 0, FX(0, 0, 20, 30)},
    {"JUNO PIANO", {JW_PLS, 60, 30, 0, 2, 50, 20, 1}, {0, 90, 0, 70}, 50, 0, FX(0, 0, 10, 30)},
    {"DREAM PAD", {JW_SP, 50, 30, 10, 2, 92, 30, 3}, {90, 100, 120, 110}, 0, 0, FX(0, 0, 20, 70),
     XP(P_LRATE + 1, 20, P_LD_SHP + 1, 50, P_VINT + 1, 40)},
    {"SUB BASS 106", {JW_PLS, 0, 127, 0, 0, 40, 10, 0}, {0, 80, 110, 20}, 10, 1, FX(0, 0, 0, 4)},
    {"SYNTH BRASS 2", {JW_SP, 10, 40, 0, 1, 54, 36, 2}, {20, 86, 96, 56}, 46, 0, FX(0, 0, 12, 34)},
    {"ARP PLUCK", {JW_SAW, 0, 40, 0, 2, 44, 50, 1}, {0, 60, 0, 40}, 56, 0, FX(0, 0, 30, 30),
     ARP(1, 2, 2, 70)},
    {"NOISE SWEEP", {JW_SAW, 0, 0, 127, 3, 40, 90, 3}, {60, 110, 100, 100}, 50, 0, FX(0, 0, 30, 60),
     XP(P_LRATE + 1, 10, P_LD_FLT + 1, 50)},
    {"JUNO ORGAN", {JW_PLS, 100, 90, 0, 1, 100, 0, 2}, {0, 30, 127, 20}, 0, 0, FX(0, 0, 0, 30)},
    {"SAW LEAD 106", {JW_SAW, 0, 30, 0, 1, 82, 44, 0}, {0, 80, 100, 40}, 24, 1, FX(10, 0, 26, 24),
     XP(P_GLIDE + 1, 20)},
    {"HOOVER JUNO", {JW_PLS, 80, 60, 0, 1, 76, 20, 3}, {0, 90, 110, 50}, 0, 1, FX(10, 0, 10, 30),
     XP(P_LRATE + 1, 60, P_LD_SHP + 1, 63, P_GLIDE + 1, 40)},
    /* 0.9: the arp's TURNG (a Turing machine over the held notes: hold a chord), VINTAGE */
    {"TURING JUNO", {JW_SAW, 0, 20, 0, 2, 64, 36, 1}, {0, 62, 24, 40}, 44, 0, FX(0, 0, 34, 40), ARP(15, 2, 2, 60),
     XP(P_SPICE + 1, 16, P_SLOOP + 1, 8, P_VINT + 1, 30)},
};

/* (after Clipper 1.2) UNISON (voice.c unison_spread): the DCO and the sub (an octave down) moved by r (Q32) of a
 * turn of the note, the noise from its own place (the note's seed: every voice the same) */
static void juno_spread(track_t *t, voice_t *v, uint32_t r)
{
    (void)t;
    v->ph[0] += r;
    v->ph[1] += r >> 1;
    v->ph[2] ^= r;
}

static const engine_t ENG_JUNO = {
    "JUNO", {"DCO", "VCF"},
    {
        {"WAVE", F_ENUM, 0, 2, JW_SP, N_JWAVE, 0},
        {"PW", F_PCT, 0, 127, 30, 0, 0},
        {"SUB", F_PCT, 0, 127, 40, 0, 0},
        {"NOIS", F_PCT, 0, 127, 0, 0, 0},
        {"HPF", F_ENUM, 0, 3, 1, N_JHPF, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 20, 0, 0},
        {"CHOR", F_ENUM, 0, 3, 1, N_JCHOR, 0},
    },
    JUNO_PRESETS, sizeof(JUNO_PRESETS) / sizeof(JUNO_PRESETS[0]), 1, juno_note_on, juno_render,
    0x4D7F, {P_E5, P_E6, P_E1, P_E7}, 0, 0, 0, 0,
};
