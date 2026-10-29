/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* MACRO: four macro-oscillator models in the spirit of Mutable Instruments Plaits (Emilie Gillet, MIT): one MODEL,
 * three macro knobs (HARM, TIMB, MORPH) that each model reads its own way, a low-pass and a low-pass gate (Clipper 0.2).
 * Written for Clipper (fixed point, the FM-1's budget); the ideas are Plaits', not its code.
 *   CHORD   four oscillators on a chord: HARM the chord (OCT 5TH SUS4 m m7 m9 m11 6/9 M9 M7 M), TIMB the inversion,
 *           MORPH saw -> square
 *   SWARM   eight oscillators spread around the note (HARM: up to a semitone each way), each drifting on its own
 *           slow LFO (TIMB), MORPH saw -> sine
 *   ADDTV   additive, harmonics 1..16 under a moving bump: HARM its centre, TIMB its width, MORPH odd .. even
 *           (harmonics above 20 kHz are left out: no aliasing)
 *   SPEECH  a glottal source through three formants gliding along a word (HARM: AI AU OI UA WOW YEAH AEIOU MAMA),
 *           TIMB the size of the voice (the formants -0.5 .. +0.5 octave), MORPH a position in the word (0..63) or,
 *           above, the word spoken from each note at a speed
 * EDIT 2  CUT RES (not on SPEECH: the formants are its filter), DEC LPG: the low-pass gate (Plaits' "vactrol"): a
 *         decay from each note (DEC) closes the level and the cutoff together, LPG how much (0: the ADSR alone)
 * Voice state: ph[0..2], s[2..6] the phases (SWARM 8, CHORD 4), s[0..1] the low-pass (SPEECH: s[0..5] the three
 * formants, s[6] the word's position), s[7] the gate's envelope. */
static const char *const N_MAC_MODEL[] = {"CHORD", "SWARM", "ADDTV", "SPEECH"};
static const int8_t MAC_CHORD[11][4] = {{0, 12, -12, 24}, {0, 7, 12, 19}, {0, 5, 7, 12}, {0, 3, 7, 12}, {0, 3, 7, 10},
                                        {0, 3, 10, 14}, {0, 3, 10, 17}, {0, 4, 9, 14}, {0, 4, 11, 14}, {0, 4, 7, 11},
                                        {0, 4, 7, 12}};
static const uint32_t MAC_SEMI[49] = {   /* 2^(s / 12) Q16, s = -12 .. 36 */
    32768, 34716, 36781, 38968, 41285, 43740, 46341, 49097, 52016, 55109, 58386, 61858, 65536, 69433, 73562, 77936,
    82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072, 138866, 147123, 155872, 165140, 174960,
    185364, 196386, 208064, 220436, 233544, 247431, 262144, 277732, 294247, 311744, 330281, 349920, 370728, 392772,
    416128, 440872, 467088, 494862, 524288};
/* SPEECH: vowels as cutoff indices (0..127 << 8, as the filters'), F1 F2 F3 (Peterson & Barney; W Y M approximate) */
enum { VA, VE, VI, VO, VU, VW, VY, VM };
static const uint16_t MAC_VOWEL[8][3] = {{16527, 18602, 22775}, {14869, 21313, 22859}, {11377, 22446, 23862},
                                         {15246, 17253, 22711}, {11922, 17435, 22332}, {11922, 15926, 22239},
                                         {10978, 22239, 23845}, {10978, 18156, 22689}};
static const uint8_t MAC_WORD[8][6] = {   /* count, vowels */
    {2, VA, VI}, {2, VA, VU}, {2, VO, VI}, {2, VU, VA}, {3, VW, VA, VU}, {3, VY, VE, VA}, {5, VA, VE, VI, VO, VU},
    {4, VM, VA, VM, VA}};
static uint32_t mac_t[NPART];                    /* blocks played, per part (macro_block): the swarm's drift */
static void macro_block(track_t *t) { mac_t[(uint32_t)(t - trk) % NPART]++; }

static void macro_note_on(track_t *t, voice_t *v)
{
    uint32_t k;
    int fresh = !v->env && !v->env_out;
    if (t->p[P_E0] % 4 == 3) {                       /* SPEECH: the word from its start; the formants clear from silence */
        v->s[6] = 0;
        if (fresh)
            for (k = 0; k < 6u; k++)
                v->s[k] = 0;
    } else if (fresh) {                              /* the phases spread, the low-pass clear */
        for (k = 0; k < 3u; k++)
            v->ph[k] = k * 0x2E8BA2E9u;
        for (k = 2; k < 7u; k++)
            v->s[k] = (int32_t)(k * 0x1D89D89Du);
        v->s[0] = v->s[1] = 0;
    }
    v->s[7] = 32767;                                 /* the gate opens */
}

/* the phase slots: 0..2 ph[], 3..7 s[2..6] */
static inline uint32_t *mac_ph(voice_t *v, uint32_t k) { return k < 3u ? &v->ph[k] : (uint32_t *)&v->s[k - 1u]; }
static inline uint32_t mac_inc(uint32_t inc, int32_t semi) { return (uint32_t)(((uint64_t)inc * MAC_SEMI[clamp(semi, -12, 36) + 12]) >> 16); }

/* (after Clipper 1.2) UNISON (voice.c unison_spread): the voice's oscillators moved by r (Q32) of a turn of its note,
 * each by its own pitch's share (CHORD: its interval; SWARM: the note's), as if started that much sooner */
static void macro_spread(track_t *t, voice_t *v, uint32_t r)
{
    const int16_t *p = t->p;
    uint32_t j;
    switch ((uint32_t)p[P_E0] % 4u) {
    case 0: {
        const int8_t *c = MAC_CHORD[p[P_E1] * 11 / 128];
        int32_t inv = p[P_E2] * 5 / 128;
        for (j = 0; j < 4u; j++)
            *mac_ph(v, j) += (uint32_t)(((uint64_t)r * MAC_SEMI[clamp(c[j] + ((int32_t)j < inv ? 12 : 0), -12, 36) + 12]) >> 16);
        break;
    }
    case 1:
        for (j = 0; j < 8u; j++)
            *mac_ph(v, j) += r;
        break;
    default:                                         /* ADDTV, SPEECH: ph[0] */
        v->ph[0] += r;
        break;
    }
}

static void mac_chord(voice_t *v, int32_t *acc, uint32_t n, uint32_t inc, int32_t harm, int32_t timb, int32_t morph)
{
    const int8_t *c = MAC_CHORD[harm * 11 / 128];
    int32_t inv = timb * 5 / 128, x = morph * 258;
    uint32_t j, i;
    for (j = 0; j < 4u; j++) {
        uint32_t *pp = mac_ph(v, j), ph = *pp, in = mac_inc(inc, c[j] + ((int32_t)j < inv ? 12 : 0));
        if (in > 0x70000000u)                        /* (above ~19 kHz: left out) */
            continue;
        for (i = 0; i < n; i++, ph += in)
            acc[i] += mulq15(str_reg(ph, in, x), 10923);
        *pp = ph;
    }
}

static void mac_swarm(voice_t *v, int32_t *acc, uint32_t n, uint32_t inc, uint32_t part, int32_t harm, int32_t timb,
                      int32_t morph)
{
    uint32_t j, i, t = mac_t[part];
    int32_t x = morph * 258;
    for (j = 0; j < 8u; j++) {
        /* spread: (2j - 7) / 7 of up to 100 cents; drift: a slow sine of its own (0.1 .. 0.5 Hz), TIMB deep */
        int32_t base = (2 * (int32_t)j - 7) * harm * 100 / (7 * 127);                     /* cents */
        int32_t dr = (osc_sine(t * (300000u + j * 97001u) + j * 0x2468ACE1u) * timb) >> 16;   /* ~ +-64 cents x 1/2 */
        int32_t fq = (base + (dr * harm) / 127) * 38;                                       /* Q16 */
        uint32_t *pp = mac_ph(v, j), ph = *pp, in = inc + (uint32_t)(((int64_t)(int32_t)inc * fq) >> 16);
        for (i = 0; i < n; i++, ph += in) {
            int32_t a = hy_saw(ph, in);
            acc[i] += mulq15(x ? a + mulq15(osc_sine(ph) - a, x) : a, 7000);
        }
        *pp = ph;
    }
}

static void mac_additive(voice_t *v, int32_t *acc, uint32_t n, uint32_t inc, int32_t harm, int32_t timb, int32_t morph)
{
    int32_t a[16], c = 256 + harm * 15 * 256 / 127, w = 154 + timb * 6 * 256 / 127, sum = 0, g;   /* Q8 */
    uint32_t h, i, ph0 = v->ph[0];
    for (h = 1; h <= 16u; h++) {
        int32_t d = (((int32_t)h << 8) - c) * 256 / w;                                   /* Q8 */
        a[h - 1] = (int32_t)(2147418112u / (uint32_t)(65536 + d * d));                 /* 1 / (1 + d^2), Q15 (32 bits:
                                                                                         * no 64-bit divide on the FM-1) */
        if (h > 1u)                                                                     /* odd .. even */
            a[h - 1] = mulq15(a[h - 1], (h & 1u) ? clamp(2 * (127 - morph) * 129, 0, 32767) : clamp(2 * morph * 129, 0, 32767));
        if (inc && h > 0x73333333u / inc)                                               /* above 0.45 FS */
            a[h - 1] = 0;
        sum += a[h - 1];
    }
    g = sum > 0 ? (int32_t)(29491u * 32768u / (uint32_t)sum) : 0;                       /* the peak ~0.9 */
    for (h = 1; h <= 16u; h++) {
        int32_t ah = mulq15(a[h - 1], g);
        uint32_t ph = ph0 * h, in = inc * h;
        if (ah < 100)                                                                   /* (under -50 dB: the
                                                                                         * time goes elsewhere) */
            continue;
        for (i = 0; i < n; i++, ph += in)
            acc[i] += mulq15(osc_sine(ph), ah);
    }
    v->ph[0] = ph0 + inc * n;
}

static void mac_speech(voice_t *v, int32_t *acc, uint32_t n, uint32_t inc, int32_t harm, int32_t timb, int32_t morph)
{
    int32_t src[CTL], b[CTL];                        /* (0.7: on the stack: the voices render on two cores) */
    const uint8_t *w = MAC_WORD[harm * 8 / 128];
    uint32_t nv = w[0], i, k, seg;
    int32_t pos, fr, f[3];
    hsvf_t c;
    for (k = 0; k < 6u; k++)                         /* (from another model: its phases are no filter states) */
        v->s[k] = clamp(v->s[k], -150000, 150000);
    if (morph < 64)                                  /* a position in the word, held */
        pos = morph * 65535 / 63;
    else {                                           /* spoken: the word in ~1.5 s .. 0.2 s */
        pos = clamp(v->s[6], 0, 65535) + (morph - 63) * 12;   /* (a switch from SWARM: s[6] was a phase) */
        if (pos > 65535)
            pos = 65535;
        v->s[6] = pos;
    }
    seg = (uint32_t)pos * (nv - 1u) >> 16;
    fr = (int32_t)(((uint32_t)pos * (nv - 1u)) & 0xFFFFu);
    for (k = 0; k < 3u; k++) {
        int32_t a = MAC_VOWEL[w[1 + seg]][k], z = MAC_VOWEL[w[1 + (seg + 1u < nv ? seg + 1u : seg)]][k];
        f[k] = a + (int32_t)(((int64_t)(z - a) * fr) >> 16) + (timb - 64) * 28;        /* (+-0.5 octave) */
    }
    {   /* the source: a band-limited saw, its top softened (one pole), a little breath */
        uint32_t ph = v->ph[0];
        int32_t lp = (int32_t)v->ph[2], nz = (int32_t)v->ph[1] | 1;
        for (i = 0; i < n; i++, ph += inc) {
            lp += (hy_saw(ph, inc) - lp) >> 1;
            src[i] = (lp >> 2) + (((int32_t)(noise32(&nz) >> 20) - 2048) >> 1);
        }
        v->ph[0] = ph, v->ph[2] = (uint32_t)lp, v->ph[1] = (uint32_t)nz;
    }
    for (k = 0; k < 3u; k++) {                       /* the formants: band-passes (Q ~7), F1 F2 F3 at 1, 0.6, 0.35 */
        static const int32_t G[3] = {4 * 592, 4 * 355, 4 * 207};
        for (i = 0; i < n; i++)
            b[i] = src[i];
        hsvf_coef(&c, f[k], hy_damp(127));
        hsvf_run(&c, b, n, &v->s[2 * k], 1);
        for (i = 0; i < n; i++)
            acc[i] += (b[i] * G[k]) >> 12;
    }
}

static void macro_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t acc[CTL];                                /* (0.7: on the stack: two cores) */
    uint32_t model = (uint32_t)p[P_E0] % 4u, part = (uint32_t)(t - trk) % NPART, i;
    int32_t env = v->s[7], lpg = p[P_E7] * 258, shut, gain, cut;
    if (n > CTL)
        return;
    env = (int32_t)(((int64_t)env * acid_block_k(30u + (uint32_t)p[P_E6] * 3u / 4u)) >> 16);   /* the gate's decay */
    v->s[7] = env;
    shut = mulq15(lpg, 32767 - env);                 /* how closed the gate is */
    gain = 32767 - shut;
    for (i = 0; i < n; i++)
        acc[i] = 0;
    switch (model) {
    case 0: mac_chord(v, acc, n, m->inc, p[P_E1], p[P_E2], p[P_E3]); break;
    case 1: mac_swarm(v, acc, n, m->inc, part, p[P_E1], p[P_E2], p[P_E3]); break;
    case 2: mac_additive(v, acc, n, m->inc, p[P_E1], p[P_E2], p[P_E3]); break;
    default: mac_speech(v, acc, n, m->inc, p[P_E1], p[P_E2], p[P_E3]); break;
    }
    if (model != 3u) {                               /* the low-pass, closing with the gate (up to 3 octaves) */
        int32_t ic1 = v->s[0], ic2 = v->s[1];
        tsvf_t flt;
        cut = (p[P_E4] << 8) + m->cutoff - ((shut * 10767) >> 15);
        tsvf_coef(&flt, cut, p[P_E5]);
        for (i = 0; i < n; i++)
            acc[i] = tsvf_lp(&flt, acc[i] >> 1, &ic1, &ic2) << 1;
        v->s[0] = ic1;
        v->s[1] = ic2;
    }
    for (i = 0; i < n; i++) {
        int32_t y = acc[i] >> 1;
        if (y > 16000 || y < -16000)
            y = soft_knee(y, 16000);
        out[i] += mulq15(mulq15(mulq15(y << 1, gain), amp_at(m, i)), VOICE_FS) << 2;   /* (+6 dB: the gate's
                                                                                     * stabs and plucks) */
    }
}

static const preset_t MACRO_PRESETS[] = {
    /* {MODEL, HARM, TIMB, MORPH, CUT, RES, DEC, LPG}, {A D S R}, ENV -> FILTER, mono */
    {"CHORD STAB", {0, 50, 20, 20, 100, 10, 60, 90}, {0, 90, 100, 40}, 0, 0, FX(0, 20, 30, 40)},
    {"MAJ7 KEYS", {0, 105, 50, 60, 96, 10, 70, 70}, {0, 90, 100, 50}, 0, 0, FX(0, 20, 20, 40)},
    {"M9 PAD", {0, 64, 70, 30, 84, 10, 0, 0}, {80, 100, 120, 90}, 0, 0, FX(0, 40, 20, 65)},
    {"SWARM", {1, 70, 80, 0, 110, 10, 0, 0}, {40, 100, 120, 80}, 0, 0, FX(0, 20, 20, 50)},
    {"SWARM SUB", {1, 30, 40, 90, 90, 20, 0, 0}, {10, 90, 120, 50}, 0, 1, FX(0, 0, 0, 20), XP(P_TRANS + 1, -12)},
    {"ADDITIVE", {2, 30, 40, 64, 127, 0, 0, 0}, {10, 90, 110, 60}, 0, 0, FX(0, 20, 20, 40)},
    {"HARMONIUM", {2, 10, 80, 30, 127, 0, 0, 0}, {20, 90, 120, 40}, 0, 0, FX(0, 30, 0, 40)},
    {"BELL TONE", {2, 100, 10, 100, 127, 0, 70, 127}, {0, 110, 0, 100}, 0, 0, FX(0, 0, 30, 60)},
    {"PING", {0, 0, 0, 0, 110, 30, 40, 127}, {0, 90, 0, 60}, 0, 0, FX(0, 0, 30, 40)},
    {"YEAH", {3, 80, 64, 90, 127, 0, 0, 0}, {5, 90, 110, 40}, 0, 1, FX(0, 0, 20, 30)},
    {"WOW BASS", {3, 66, 40, 100, 127, 0, 0, 0}, {0, 90, 110, 30}, 0, 1, FX(10, 0, 0, 10), XP(P_TRANS + 1, -12)},
    {"VOWELS", {3, 100, 70, 30, 127, 0, 0, 0}, {20, 90, 120, 60}, 0, 0, FX(0, 30, 10, 50)},
    /* Clipper 0.3 */
    {"HOUSE CHRD", {0, 52, 30, 30, 90, 20, 50, 60}, {0, 90, 100, 40}, 0, 0, FX(0, 20, 30, 40)},
    {"SUS PAD", {0, 28, 60, 10, 90, 10, 0, 0}, {80, 100, 120, 90}, 0, 0, FX(0, 40, 20, 65)},
    {"6/9 KEYS", {0, 87, 40, 40, 100, 10, 60, 60}, {0, 90, 100, 50}, 0, 0, FX(0, 20, 20, 40)},
    {"POWER CHRD", {0, 16, 0, 100, 90, 20, 0, 0}, {0, 80, 110, 40}, 0, 0, FX(30, 0, 10, 30)},
    {"M11 PAD", {0, 75, 80, 20, 84, 10, 0, 0}, {80, 100, 120, 90}, 0, 0, FX(0, 40, 20, 66)},
    {"SWARM PAD", {1, 90, 100, 20, 100, 10, 0, 0}, {90, 100, 120, 95}, 0, 0, FX(0, 20, 20, 60)},
    {"SWARM LEAD", {1, 30, 30, 0, 110, 20, 0, 0}, {2, 80, 110, 40}, 0, 1, FX(0, 0, 26, 30), XP(P_GLIDE + 1, 30)},
    {"ORGAN ADD", {2, 20, 60, 50, 127, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 30, 0, 30)},
    {"ADD BELL", {2, 80, 20, 90, 127, 0, 60, 127}, {0, 110, 0, 100}, 0, 0, FX(0, 0, 30, 60)},
    {"TALK BASS", {3, 40, 50, 100, 127, 0, 0, 0}, {0, 90, 110, 30}, 0, 1, FX(10, 0, 0, 10), XP(P_TRANS + 1, -12)},
    {"MAMA", {3, 120, 64, 100, 127, 0, 0, 0}, {5, 90, 110, 40}, 0, 0, FX(0, 10, 20, 40)},
    {"OI LEAD", {3, 24, 70, 90, 127, 0, 0, 0}, {5, 90, 110, 40}, 0, 1, FX(0, 0, 24, 30), XP(P_GLIDE + 1, 30)},
};

static const engine_t ENG_MACRO = {
    "MACRO", {"MODEL", "LPG"},
    {
        {"MODL", F_ENUM, 0, 3, 0, N_MAC_MODEL, 0},
        {"HARM", F_PCT, 0, 127, 64, 0, 0},
        {"TIMB", F_PCT, 0, 127, 64, 0, 0},
        {"MRPH", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 110, 0, 0},
        {"RES", F_PCT, 0, 127, 10, 0, 0},
        {"DEC", F_PCT, 0, 127, 60, 0, 0},
        {"LPG", F_PCT, 0, 127, 0, 0, 0},
    },
    MACRO_PRESETS, sizeof(MACRO_PRESETS) / sizeof(MACRO_PRESETS[0]), 1, macro_note_on, macro_render,
    0x07FF, {P_E1, P_E2, P_E3, P_E4}, 0, 0, 0, macro_block,
};
