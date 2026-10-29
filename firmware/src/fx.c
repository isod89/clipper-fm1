/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Effects: per-track DIST insert, then sends into three shared buses (chorus, tempo delay, reverb).
 * Stereo dry mix; the chorus and the reverb come back in stereo, the delay in the middle. */
#if CLIPPER_SYNTH                /* Clipper: the line at half the rate (22.05 kHz, its repeats are low-passed anyway):
                                  * the same 1.49 s in half the memory (64 KB for the wavetables) */
#define DLY_LEN 32768u
#define DLY_SPAN (2u * DLY_LEN)  /* samples the line holds */
#else
#define DLY_LEN 65536u           /* 1.49 s: 1/4 down to 41 BPM, 1/8 dotted down to 31 (clamped below) */
#define DLY_SPAN DLY_LEN
#endif
#define CHO_LEN 2048u
#define PART_MAX 880000                                 /* (1.0) a part's sum, ~27 x full scale: x LEVEL_Q12[127] (9713)
                                                         * >> 2 stays in 32 bits */
#define MIX_MAX 2000000                                 /* (1.0) ~61 x full scale: far into the limiter, never past 32 bits */
static int16_t dly_buf[DLY_LEN] __attribute__((section(".pool")));
static int16_t cho_buf[CHO_LEN] __attribute__((section(".pool")));
/* the reverb: two input diffusers, then four delay lines mixed by a Hadamard matrix (a feedback delay
 * network: every echo feeds all four, so it thickens instead of ringing like a comb), damped in the
 * loop, one line slowly modulated (no metallic tone on long tails); left and right take different lines */
#define REV_MOD 12               /* samples the modulated line moves (+-) */
static const uint16_t REV_LINE[4] = {1559, 1931, 2389, 2791};   /* 35..63 ms, coprime */
static const uint16_t REV_AP[2] = {556, 441};
#if CLIPPER_SYNTH                /* (1.0) in the pool, which has room in the synth (48 KB free): .lowram put line 0, the
                                  * left channel's own, right above the second core's stacks */
static int16_t rev_line[1559 + 1931 + 2389 + 2791 + REV_MOD + 2] __attribute__((section(".pool")));
#else
static int16_t rev_line[1559 + 1931 + 2389 + 2791 + REV_MOD + 2] __attribute__((section(".lowram")));   /* (the pool is
                                                                      * full; .lowram: the SRAM below the RAM region, app.ld) */
#endif
static int16_t rev_ap[556 + 441] __attribute__((section(".pool")));
static struct {
    uint32_t dly_w, cho_w, cho_ph, rev_ph;
    int32_t dly_lp, dly_acc;
    uint16_t line_i[4], ap_i[2];
    int32_t line_lp[4];
    uint32_t dl_cur, dl_old;            /* (0.9) the delay's TIME now and the one it fades from (DL_XF samples) */
    uint32_t dl_x;                      /* .. samples into that fade (DL_XF: none) */
    int32_t cdep;                       /* (0.9) the chorus's DEPTH now (glides: a jump moved its taps at once) */
#if CLIPPER_SYNTH
    uint32_t dm_cur, dm_old, dm_x;      /* (1.0) the delay's MODE now, the one it fades from, samples into that fade */
    uint32_t tw_ph, tf_ph;              /* (1.0) TAPE: the wow's and the flutter's phases */
    int32_t tp0, tpd;                   /* .. the swing of the tap (Q8 samples, more delay) at the block's start, a step */
#endif
} fx;
#define DL_XF 1024u                     /* a new delay TIME: the old tap fades out as the new one fades in, 23 ms (a jump
                                         * of the read point was a crack) */

/* DIST: low cut -> drive (1x..8x, exponential) -> asymmetric soft clip
 * (a little bias = even harmonics) -> tone low-pass that closes with drive ->
 * make-up gain. State per part (track_t dist_*). */
/* (Clipper 0.9) DIST turned on or off while the part sounds: the dry and the driven cross-fade over 2.9 ms (SL_SLOPE;
 * it switched at once: a crack on a held chord); a part starting from silence (fresh): at once, as before, bit for bit */
static void track_dist(track_t *t, int32_t *b, uint32_t n, uint32_t fresh)
{
    int32_t d = t->p[P_DIST], i, g, k, mk, bias = 2400, b0, tw = d ? 32768 : 0, w = t->dist_w;
    if (!d && (!w || fresh)) {
        t->dist_on = 0;
        t->dist_w = 0;
        return;
    }
    if (d)
        t->dist_last = (int16_t)d;
    else
        d = t->dist_last;                               /* (fading out: the amount it had) */
    if (!t->dist_on) {                                  /* coming on: no stale high-pass state (a thump) */
        t->dist_on = 1;
        t->dist_hp = b[0];
        t->dist_lp1 = t->dist_lp2 = 0;
        w = fresh ? 32768 : 0;
    }
    g = 4096 + d * d * 2;                                /* Q12: 1x .. ~9x, gentle at first */
    k = 32000 - d * 95;                                  /* tone: transparent at low drive .. ~3 kHz, Q15 */
    mk = 30000 - d * 120;                                /* make-up */
    b0 = softclip(bias);
    for (i = 0; i < (int32_t)n; i++) {
        int32_t x = b[i], y;
        t->dist_hp += (x - t->dist_hp + 64) >> 7;           /* ~55 Hz low cut: keep the bass out of the clipper */
        x = clamp(x - t->dist_hp, -230000, 230000);         /* (x >> 2) * g fits 32 bits; the clip is flat out there */
        y = softclip(MULQ12R(x, g, 2) + bias) - b0;         /* (as (x >> 2) * g >> 10 in 32 bits: no overflow for loud
                                                             * poly; SLOOP Pro #4: rounded, no step of 4 x the drive) */
        t->dist_lp1 += mulq15(y - t->dist_lp1, k);         /* two poles: tames the fizz */
        t->dist_lp2 += mulq15(t->dist_lp1 - t->dist_lp2, k);
        y = mulq15(t->dist_lp2, mk);
        if (w != tw)
            w += clamp(tw - w, -SL_SLOPE, SL_SLOPE);
        b[i] = w == 32768 ? y : b[i] + (int32_t)(((int64_t)(y - b[i]) * w) >> 15);
    }
    t->dist_w = w;
    if (!w)
        t->dist_on = 0;                                 /* (faded out: off) */
}

/* master: peak limiter in front of the soft clipper, a 35 ms hold then ~46 ms release (SLOOP 2.6), threshold where tanh is still nearly linear,
 * so chords get quieter instead of crushed. SLOOP 2.6: it looks one block ahead (lim_block): the output is the mix
 * CTL samples late (0.73 ms) and the gain glides over each block to what the loudest sample of it and of the next one
 * needs, so a hot kick or bass is already turned down when it comes. Before, the gain caught up in ~4 samples and the
 * first ones went through the tanh knee at up to 4x: a crackle on the beats with the MASTER up (in headphones most) */
#define LIM_T 18000
typedef struct {
    int32_t dl[CTL], dr[CTL];           /* the mix of the last block (the delay) */
    uint32_t w;                         /* its oldest sample (the next out) */
    int32_t env;                        /* the peak the gain fits under LIM_T (LIM_T: none) */
    int32_t g;                          /* the gain at the end of the last block, Q15 (32768: 1) */
    uint32_t hold;                      /* blocks before the release starts (LIM_HOLD after a peak near env) */
} lim_t;
/* (2.6, a whistle on loud basses) the release waits LIM_HOLD blocks after the last peak near the envelope: a held bass
 * (down to ~29 Hz, a period under 35 ms) keeps one gain, instead of the gain dipping and creeping back at every cycle
 * (a sawtooth at the bass's pitch with a corner at every block: tones around 1.4 kHz and up) */
#define LIM_HOLD 48u
#define LIM_REL 11                      /* the release after the hold: ~46 ms (12 before the hold: ~93 ms; the same loudness) */
static lim_t lim_dac = {.env = LIM_T, .g = 32768}, lim_usb = {.env = LIM_T, .g = 32768};
/* (Clipper 0.9) the gain's way over a block: an S curve (smoothstep at (i + 1) / 32), not a straight line: no corner in
 * the gain at the blocks' joints (a straight ramp put one at each, a tick on every loud attack). Each block still ends
 * at the gain its look-ahead asks for; the samples going out were in the last block's look-ahead (g0 fits them) */
static const uint16_t LIM_S[CTL] = {94, 368, 810, 1408, 2150, 3024, 4018, 5120, 6318, 7600, 8954, 10368, 11830, 13328,
                                    14850, 16384, 17918, 19440, 20938, 22400, 23814, 25168, 26450, 27648, 28750, 29744,
                                    30618, 31360, 31958, 32400, 32674, 32768};
/* l, r: n (<= CTL) samples in; out: the n samples of CTL before, limited (before the knee) */
static void lim_block(lim_t *s, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, w = s->w;
    int32_t pk = 0, a, g0 = s->g, g1, dg, e = s->env;
    for (i = 0; i < CTL; i++) {                         /* the samples going out now and the ones after them (the
                                                         * rest of the delay, then this block): CTL ahead */
        a = s->dl[i] < 0 ? -s->dl[i] : s->dl[i], pk = a > pk ? a : pk;
        a = s->dr[i] < 0 ? -s->dr[i] : s->dr[i], pk = a > pk ? a : pk;
    }
    for (i = 0; i < n; i++) {
        a = l[i] < 0 ? -l[i] : l[i], pk = a > pk ? a : pk;
        a = r[i] < 0 ? -r[i] : r[i], pk = a > pk ? a : pk;
    }
    if ((int64_t)pk * 17 >= (int64_t)e * 16)           /* a peak within ~0.5 dB of the envelope: the hold again */
        s->hold = LIM_HOLD;
    if (s->hold)
        s->hold--;
    else if (e > LIM_T)                                 /* the release (after the hold), a block at a time */
        e -= (int32_t)((((int64_t)(e - LIM_T) * (int32_t)n) >> LIM_REL) + 1);
    if (e < LIM_T)
        e = LIM_T;
    if (pk > e)
        e = pk, s->hold = LIM_HOLD;
    s->env = e;
    g1 = e > LIM_T ? (int32_t)(((uint32_t)LIM_T << 15) / (uint32_t)e) : 32768;
    s->g = g1;
    dg = (g1 - g0) / (int32_t)(n ? n : 1u);             /* (towards 0: the gain never above both ends' bound) */
    for (i = 0; i < n; i++) {
        int32_t xl = s->dl[w], xr = s->dr[w], g = n == CTL ? g0 + (int32_t)(((int64_t)(g1 - g0) * LIM_S[i]) >> 15)
                                                           : g0 + dg * (int32_t)(i + 1u);
        s->dl[w] = l[i], s->dr[w] = r[i];
        w = (w + 1u) & (CTL - 1u);
        if (i + 1u == n)
            g = g1;
        if (g < 32768) {                                /* (64-bit: |x| may be far above Q15. 2.6 fix: x >> 4 first cut
                                                         * the limited signal to 12 bits, a whistle on loud basses) */
            xl = (int32_t)(((int64_t)xl * g) >> 15);
            xr = (int32_t)(((int64_t)xr * g) >> 15);
        }
        l[i] = xl, r[i] = xr;
    }
    s->w = w;
}
static volatile uint8_t fx_lowcut;     /* settings: 12 dB/oct ~110 Hz for the small speaker */
static int32_t lc_l1, lc_l2, lc_r1, lc_r2, dc_l, dc_r, dce_l, dce_r;

/* DC blocker (~2 Hz), always on: a leaky integrator of the input (Q6 state) subtracted from it.
 * The >> 12 step keeps its remainder (error feedback, 0..4095) and adds it to the next one, so no
 * part of the step is lost: the state follows the input exactly, down to 0 after the sound stops.
 * (It was rounded, and a rounded step of (x - dc) / 4096 stops moving at |x - dc| < 2048: a
 * constant offset of up to +-31 stayed at the output after silence.) */
static inline int32_t dc_block(int32_t x, int32_t *dc, int32_t *err)
{
    int32_t e = (x << 6) - *dc + *err, d = e >> 12;
    *err = e - (d << 12);
    *dc += d;
    return x - ((*dc + 32) >> 6);
}

static int32_t lce[4];
static inline int32_t lowcut1(int32_t x, int32_t *lc, int32_t *err)   /* x minus its one-pole low-pass */
{
    int32_t e = x - *lc + *err, d = e >> 6;
    *err = e - (d << 6);
    *lc += d;
    return x - *lc;
}

/* the output stage: linear up to KNEE (a clean low end: no tanh harmonics on a loud sine), above it
 * a tanh knee with the same slope at the joint, to full scale */
#define KNEE 16384
static inline int32_t knee(int32_t x)
{
    int32_t a = x < 0 ? -x : x;
    if (a <= KNEE)
        return x;
    a = KNEE + (softclip((a - KNEE) * 2) >> 1);
    return x < 0 ? -a : a;
}

/* the USB audio input at full level (menu USB AUDIO = FULL): the same output stage with its own state,
 * fed as if MASTER were all the way up, so the recording level does not follow the knob. The DAC path
 * (master_pre, lim_dac) is untouched. audio.c sets usb_full_now for a render while the computer records. */
static uint8_t usb_full;                 /* menu USB AUDIO: 0 MASTER (follows the knob), 1 FULL (panel.c lights_word) */
static uint8_t usb_full_now;             /* this render fills usb_out (audio.c) */
static int32_t usb_out[2u * CTL];
/* the visualiser's input (audio.c fills the scope with it): the mix as if MASTER were all the way up, so the
 * picture does not follow the volume knob, even at 0 (2.4). The same soft knee as the output, no limiter state */
static int16_t vis_tap[2u * CTL];
static struct { int32_t dc[2], dce[2], lc[4], lce[4]; } uo;
static void master_pre_usb(int32_t *l, int32_t *r)      /* (USB AUDIO = FULL: its own state) */
{
    *l = dc_block(*l, &uo.dc[0], &uo.dce[0]);
    *r = dc_block(*r, &uo.dc[1], &uo.dce[1]);
    if (fx_lowcut) {
        *l = lowcut1(*l, &uo.lc[0], &uo.lce[0]);
        *l = lowcut1(*l, &uo.lc[1], &uo.lce[1]);
        *r = lowcut1(*r, &uo.lc[2], &uo.lce[2]);
        *r = lowcut1(*r, &uo.lc[3], &uo.lce[3]);
    }
}

/* the output stage before the limiter: the DC blocker, the low cut */
static inline void master_pre(int32_t *l, int32_t *r)
{
    *l = dc_block(*l, &dc_l, &dce_l);
    *r = dc_block(*r, &dc_r, &dce_r);
    if (fx_lowcut) {                  /* two one-pole high-passes, error feedback as dc_block (the */
        *l = lowcut1(*l, &lc_l1, &lce[0]);          /* rounded step stopped at |x - lc| < 32: an offset) */
        *l = lowcut1(*l, &lc_l2, &lce[1]);
        *r = lowcut1(*r, &lc_r1, &lce[2]);
        *r = lowcut1(*r, &lc_r2, &lce[3]);
    }
}



static uint32_t delay_samples(void)
{
    uint32_t s = dly_samples((uint32_t)song.g[G_DTIME]);
    return s < 16u ? 16u : s >= DLY_SPAN - 4u ? DLY_SPAN - 4u : s;
}

#if CLIPPER_SYNTH
/* Clipper 0.2: a plate reverb (G_RTYPE = PLATE), after J. Dattorro, "Effect Design, Part 1" (JAES 45/9, 1997): a
 * pre-delay (G_RPRE), a bandwidth low-pass and four input diffusers, then a tank of two halves feeding each other (a
 * figure eight); in each half a modulated diffuser (moving reflections: long tails do not ring), a delay, the damping
 * low-pass (DAMP), the decay (SIZE), a second diffuser and a delay. The output is taken from seven taps across the
 * tank a side, as in the paper: dense from the start, wide, smooth.
 * It runs at half the rate (22.05 kHz, as the Lexicon 224 ran at 20 kHz): the paper's lengths (29.76 kHz) x 0.741,
 * 33 KB of lines (the punch-in ring's memory, gone in the synth); two input samples averaged into one tank step, the
 * output between the last two steps. */
enum { PL_AP1, PL_AP2, PL_AP3, PL_AP4, PL_MA, PL_DA1, PL_AA, PL_DA2, PL_MB, PL_DB1, PL_AB, PL_DB2, PL_PRE, PL_N };
#define PL_EXC 12                 /* the modulated diffusers' excursion (+- samples) */
static const uint16_t PL_LEN[PL_N] = {105, 79, 281, 205, 498 + PL_EXC + 2, 3299, 1334, 2756, 673 + PL_EXC + 2, 3124,
                                      1968, 2343, 2206};
#define PL_TOTAL (105 + 79 + 281 + 205 + 512 + 3299 + 1334 + 2756 + 687 + 3124 + 1968 + 2343 + 2206)
static int16_t pl_buf[PL_TOTAL] __attribute__((section(".pool")));
static struct {
    uint16_t base[PL_N], w[PL_N];
    int32_t bw, dmp[2], end[2], y0l, y0r, y1l, y1r, acc;
    uint32_t lph;
    uint8_t half, ready;
} pl;

static void plate_clear(void)
{
    uint32_t k, b = 0;
    for (k = 0; k < PL_TOTAL; k++)
        pl_buf[k] = 0;
    for (k = 0; k < PL_N; k++) {
        pl.base[k] = (uint16_t)b;
        pl.w[k] = 0;
        b += PL_LEN[k];
    }
    pl.bw = pl.dmp[0] = pl.dmp[1] = pl.end[0] = pl.end[1] = 0;
    pl.y0l = pl.y0r = pl.y1l = pl.y1r = pl.acc = 0;
    pl.half = 0;
    pl.ready = 1;
}
/* line k: d samples back (1 = the newest, PL_LEN[k] the oldest) */
static inline int32_t pl_tap(uint32_t k, uint32_t d)
{
    uint32_t i = pl.w[k] + PL_LEN[k] - d;
    return pl_buf[pl.base[k] + (i >= PL_LEN[k] ? i - PL_LEN[k] : i)];
}
static inline void pl_put(uint32_t k, int32_t x)
{
    pl_buf[pl.base[k] + pl.w[k]] = (int16_t)clamp(x, -32768, 32767);
    if (++pl.w[k] >= PL_LEN[k])
        pl.w[k] = 0;
}
static inline int32_t pl_ap(uint32_t k, int32_t x, int32_t c)  /* a diffuser: an allpass over line k, c Q15 */
{
    int32_t d = pl_tap(k, PL_LEN[k]), v = x - mulq15(d, c);
    pl_put(k, v);
    return d + mulq15(v, c);
}
static inline int32_t pl_mod_ap(uint32_t k, int32_t x, int32_t c, int32_t pos)   /* pos: the delay, Q8 samples */
{
    uint32_t di = (uint32_t)pos >> 8;
    int32_t a = pl_tap(k, di), b = pl_tap(k, di + 1u), d = a + (((b - a) * (pos & 255)) >> 8), v = x - mulq15(d, c);
    pl_put(k, v);
    return d + mulq15(v, c);
}
/* (1.0) SHIMMER: the tank's output an octave up, back into its input (the Eno / Lanois shimmer): a delay line read
 * twice as fast as it is written by two taps half a window apart, each faded in and out (a triangle), at 22.05 kHz */
#define PS_LEN 1024u                                   /* 46 ms */
#define PS_WIN 896u                                    /* the window the taps sweep, samples */
static int16_t ps_buf[PS_LEN] __attribute__((section(".pool")));
static struct { uint32_t w, ph; int32_t lp; } ps;
static int32_t shimmer_step(int32_t in)
{
    uint32_t k;
    int32_t y = 0;
    ps_buf[ps.w & (PS_LEN - 1u)] = (int16_t)clamp(in, -32768, 32767);
    for (k = 0; k < 2u; k++) {                         /* delay = WIN - phase: it shrinks one a step (twice the speed) */
        uint32_t ph = (ps.ph + k * (PS_WIN / 2u)) % PS_WIN, d = PS_WIN - ph + 2u;
        int32_t g = ph < PS_WIN / 2u ? (int32_t)(ph * 65536u / PS_WIN) : (int32_t)((PS_WIN - ph) * 65536u / PS_WIN);
        y += (ps_buf[(ps.w - d) & (PS_LEN - 1u)] * g) >> 15;   /* (the two triangles sum to 1) */
    }
    ps.w++;
    ps.ph = (ps.ph + 1u) % PS_WIN;
    ps.lp += mulq15(y - ps.lp, 16000);                 /* (a little softer: the octave of a bright tail is harsh) */
    return ps.lp;
}
/* one tank step (22.05 kHz) on x: the left and right outputs. (1.0) bw: the input's bandwidth (HALL darker), shim:
 * the octave-up share of the tail fed back in (SHIMMER), Q15 */
static void plate_step(int32_t x, int32_t decay, int32_t damp, uint32_t pre, int32_t bw, int32_t shim, int32_t *yl,
                       int32_t *yr)
{
    int32_t a, b, ma, mb;
    pl_put(PL_PRE, x);
    x = pre ? pl_tap(PL_PRE, pre) : x;
    if (shim)
        x += mulq15(shimmer_step((pl.end[0] + pl.end[1]) >> 1), shim);
    pl.bw += mulq15(x - pl.bw, bw);                             /* bandwidth (~6 kHz at 22.05; HALL ~3.5 kHz) */
    x = pl_ap(PL_AP1, pl.bw, 24576);                            /* 0.75 */
    x = pl_ap(PL_AP2, x, 24576);
    x = pl_ap(PL_AP3, x, 20480);                                /* 0.625 */
    x = pl_ap(PL_AP4, x, 20480);
    pl.lph += 194783u;                                          /* ~1 Hz at 22.05 kHz */
    ma = ((498 << 8) + ((osc_sine(pl.lph) * PL_EXC) >> 7));
    mb = ((673 << 8) + ((osc_sine(pl.lph + 0x40000000u) * PL_EXC) >> 7));
    a = pl_mod_ap(PL_MA, x + mulq15(pl.end[1], decay), -22938, ma);   /* -0.7: the decay diffusion */
    b = pl_mod_ap(PL_MB, x + mulq15(pl.end[0], decay), -22938, mb);
    pl_put(PL_DA1, a);
    pl_put(PL_DB1, b);
    a = pl_tap(PL_DA1, PL_LEN[PL_DA1]);
    b = pl_tap(PL_DB1, PL_LEN[PL_DB1]);
    pl.dmp[0] += mulq15(a - pl.dmp[0], damp);
    pl.dmp[1] += mulq15(b - pl.dmp[1], damp);
    a = pl_ap(PL_AA, mulq15(pl.dmp[0], decay), 16384);          /* 0.5 */
    b = pl_ap(PL_AB, mulq15(pl.dmp[1], decay), 16384);
    pl_put(PL_DA2, a);
    pl_put(PL_DB2, b);
    pl.end[0] = pl_tap(PL_DA2, PL_LEN[PL_DA2]);
    pl.end[1] = pl_tap(PL_DB2, PL_LEN[PL_DB2]);
    /* (1.0, comments: the plate crackled, on the left) the paper's tap 1913 on its 1800-sample diffuser is past the
     * line's end: x 0.741 it read 1417 back on a 1334-sample line, 83 samples old or (the index wrapping) the line
     * before's: a click a turn of the line, on the left only. Read inside it, the same share of the line as the
     * right's AB 910 / 1968 tap: 1250 */
    *yl = pl_tap(PL_DA1, 197) + pl_tap(PL_DA1, 2203) - pl_tap(PL_AA, 1250) + pl_tap(PL_DA2, 1479) -
          pl_tap(PL_DB1, 1474) - pl_tap(PL_AB, 139) - pl_tap(PL_DB2, 790);
    *yr = pl_tap(PL_DB1, 262) + pl_tap(PL_DB1, 2687) - pl_tap(PL_AB, 910) + pl_tap(PL_DB2, 1980) -
          pl_tap(PL_DA1, 1564) - pl_tap(PL_AA, 248) - pl_tap(PL_DA2, 90);
}
#ifndef SH_DECAY
#define SH_DECAY 27000
#define SH_DAMP 28000
#define SH_GAIN 8192
#endif
/* the plate over a block: rev_in (the reverb sends) -> + wet_l, wet_r */
static void plate_run(const int32_t *rev_in, int32_t *wet_l, int32_t *wet_r, uint32_t n, int32_t rg0, int32_t rg1)
{
    int32_t rt = song.g[G_RTYPE], hall = rt >= RT_HALL;
    int32_t decay = hall ? 16384 + song.g[G_RSIZE] * 104          /* (1.0) HALL: 0.5 .. 0.9, RT60 ~2 .. 10 s */
                         : 6554 + song.g[G_RSIZE] * 155;        /* 0.2 .. 0.8: RT60 ~0.8 .. 5 s (twice a half) */
    int32_t damp = 32767 - song.g[G_RDAMP] * 193;               /* the damping low-pass: open .. 0.25 */
    if (rt == RT_SHIMMER) {                                     /* (the octave line feeds the tank: kept under a loop gain
                                                                 * of 1, the top always damped a little) */
        decay = decay > SH_DECAY ? SH_DECAY : decay;
        damp = damp > SH_DAMP ? SH_DAMP : damp;
    }
    uint32_t pre = (uint32_t)clamp(song.g[G_RPRE] + (hall ? 18 : 0), 0, 100) * 2205u / 100u;   /* ms -> 22.05 kHz
                                                                 * samples (HALL: 18 ms more, a big room's) */
    int32_t bw = hall ? 18000 : 26000, shim = rt == RT_SHIMMER ? SH_GAIN : 0;   /* (SHIMMER: some of the tail an octave up) */
    uint32_t i;
    static uint32_t pre_cur;                                    /* (1.0) the pre-delay glides (a jump: a step in) */
    if (!pl.ready)
        plate_clear(), pre_cur = pre;
    for (i = 0; i < n; i++) {
        int32_t x = mulq15(rev_in[i], 13000), wl, wr;           /* (as the room's input) */
        int32_t g = rg0 + (((rg1 - rg0) * (int32_t)i) >> CTL_LOG2);
        if (!pl.half) {
            pl.acc = x;
            wl = (pl.y0l + pl.y1l) >> 1;                        /* between the last two steps */
            wr = (pl.y0r + pl.y1r) >> 1;
        } else {
            int32_t yl, yr;
            pre_cur += pre_cur < pre ? 1u : pre_cur > pre ? (uint32_t)-1 : 0u;
            plate_step((pl.acc + x) >> 1, decay, damp, pre_cur, bw, shim, &yl, &yr);
            pl.y0l = pl.y1l, pl.y0r = pl.y1r;
            pl.y1l = (yl * 19661) >> 14, pl.y1r = (yr * 19661) >> 14;   /* x 1.2: the paper's 0.6, as loud as ROOM */
            wl = pl.y0l, wr = pl.y0r;
        }
        if (g != 32767)
            wl = mulq15(wl, g), wr = mulq15(wr, g);
        wet_l[i] += wl;
        wet_r[i] += wr;
        pl.half ^= 1u;
    }
}
#endif

#if CLIPPER_SYNTH
/* (1.0) the half-rate line read q (Q8) samples back: entry e ~ sample 2e, linear between entries */
static inline int32_t dly_rd(uint32_t q)
{
    uint32_t s = (fx.dly_w << 8) - q, e = s >> 9, fr = (s >> 1) & 255u;
    int32_t a = dly_buf[e & (DLY_LEN - 1u)], b = dly_buf[(e + 1u) & (DLY_LEN - 1u)];
    return a + (((b - a) * (int32_t)fr) >> 8);
}
/* (1.0) the taps of a MODE at a TIME d, sample i of the block: left, right, and the one fed back */
static inline void dly_taps(uint32_t m, uint32_t d, uint32_t i, int32_t *l, int32_t *r, int32_t *f)
{
    if (m == DM_PING) {
        uint32_t d2 = 2u * d;
        if (d2 > DLY_SPAN - 4u)
            d2 = DLY_SPAN - 4u;
        *l = dly_rd(d << 8);
        *r = *f = dly_rd(d2 << 8);
    } else if (m == DM_TAPE) {
        uint32_t q = (d << 8) + (uint32_t)(fx.tp0 + fx.tpd * (int32_t)i);
        if (q > ((DLY_SPAN - 4u) << 8))
            q = (DLY_SPAN - 4u) << 8;
        *l = *r = *f = dly_rd(q);
    } else {
        uint32_t p = fx.dly_w - d, e = p >> 1;
        int32_t a = dly_buf[e & (DLY_LEN - 1u)];
        *l = *r = *f = (p & 1u) ? (a + dly_buf[(e + 1u) & (DLY_LEN - 1u)]) >> 1 : a;
    }
}
#define TW_INC 1558299u                 /* (a block's turn: 0.5 Hz, the wow) */
#define TF_INC 20257887u                /* (6.5 Hz, the flutter) */
#endif

static inline int32_t lp_step(int32_t d, int32_t k)   /* (1.0) mulq15(d, k), d past 16 bits in 64 */
{
    return d > 65535 || d < -65535 ? (int32_t)(((int64_t)d * k) >> 15) : mulq15(d, k);
}
/* process the three buses for one block; sends in, wet out (stereo). The LFOs (chorus, reverb line)
 * are computed per block and ramped: no sine per sample */
static void fx_buses(const int32_t *cho_in, const int32_t *dly_in, const int32_t *rev_in, int32_t *wet_l,
                     int32_t *wet_r, uint32_t n)
{
    uint32_t i, k, dl = delay_samples(), dlo;
    int32_t fb = song.g[G_DFDBK] * 230, col = 2000 + song.g[G_DCOLOR] * 240;
    int32_t dmix = song.g[G_DMIX] * 258;
    int32_t g = 17000 + song.g[G_RSIZE] * 104, lpk = 32767 - song.g[G_RDAMP] * 200;   /* loop gain (RT60 ~0.4..4 s), damping */
    int32_t cdepth = song.g[G_CDEPTH] * 6, cdep0;
    int32_t ca0, ca1, cb0, cb1, ma, mb, dca, dcb;
    int plate = CLIPPER_SYNTH && song.g[G_RTYPE] >= RT_PLATE;   /* Clipper: the plate (1.0: HALL, SHIMMER: its tank)
                                                                 * instead of the room */
    const uint32_t L0 = REV_LINE[0] + REV_MOD + 2u, B1 = L0, B2 = B1 + REV_LINE[1], B3 = B2 + REV_LINE[2];
    int32_t rg0 = 32767, rg1 = 32767;                   /* (1.0) the reverb's share over the block (a change of type) */
#if CLIPPER_SYNTH
    {   /* a change between the room and the plate's tank (PLATE, HALL, SHIMMER share it: they change on the fly): the
         * old one fades out over 6 blocks (1.0: it was cut, a click on a ringing tail), then the other starts from
         * silence (no old tail bursting out of its lines) */
        static int fam_used = -1, was_shim;
        static uint8_t rv_fade;
        int shim = song.g[G_RTYPE] == RT_SHIMMER;
        if (fam_used < 0)
            fam_used = plate;
        if (plate != fam_used) {
            if (!rv_fade)
                rv_fade = 6;
            rg0 = rv_fade * 5461, rg1 = (rv_fade - 1) * 5461;   /* (32767 / 6) */
            plate = fam_used;                           /* (this block: the old one) */
            if (!--rv_fade) {
                if (fam_used)
                    for (k = 0; k < NELEM(rev_line); k++)
                        rev_line[k] = 0;
                else
                    pl.ready = 0;
                fam_used = !fam_used;
            }
        }
        if (shim && !was_shim) {                        /* (the octave line from silence) */
            memset(ps_buf, 0, sizeof ps_buf);
            ps.lp = 0;
        }
        was_shim = shim;
    }
#endif
    if (!fx.dl_cur)
        fx.dl_cur = dl, fx.dl_x = DL_XF, fx.cdep = cdepth;
    if (dl != fx.dl_cur && fx.dl_x >= DL_XF)            /* (0.9) a new TIME: the fade starts (one at a time) */
        fx.dl_old = fx.dl_cur, fx.dl_cur = dl, fx.dl_x = 0;
    dl = fx.dl_cur, dlo = fx.dl_old;
#if CLIPPER_SYNTH
    {   /* (1.0) a new MODE: the fade starts (one at a time); TAPE's swing at both ends of the block */
        uint32_t dm = song.g[G_DMODE] <= DM_TAPE ? song.g[G_DMODE] : DM_MONO;
        int32_t dw = (int32_t)((dl * 197u) >> 8) + 1536, t1;   /* (wow: 0.3 % of TIME + 6 samples; flutter: 2) */
        if (fx.dm_x == 0 && fx.dm_old == fx.dm_cur)
            fx.dm_x = DL_XF;                            /* (from the start: nothing to fade) */
        if (dm != fx.dm_cur && fx.dm_x >= DL_XF)
            fx.dm_old = fx.dm_cur, fx.dm_cur = dm, fx.dm_x = 0;
        fx.tp0 = dw + 512 + ((dw * osc_sine(fx.tw_ph)) >> 15) + ((512 * osc_sine(fx.tf_ph)) >> 15);
        fx.tw_ph += TW_INC, fx.tf_ph += TF_INC;
        t1 = dw + 512 + ((dw * osc_sine(fx.tw_ph)) >> 15) + ((512 * osc_sine(fx.tf_ph)) >> 15);
        fx.tpd = (t1 - fx.tp0) >> CTL_LOG2;
    }
#endif
    cdep0 = fx.cdep;                                    /* (0.9) DEPTH glides: ~60 ms over its range */
    fx.cdep += clamp(cdepth - fx.cdep, -10, 10);
    {   /* the chorus' two read points (Q8 samples back) and line 0's extra length, at both ends of the block */
        int32_t s0 = osc_sine(fx.cho_ph), s1, m0 = osc_sine(fx.rev_ph), m1;
        fx.cho_ph += LFO_INC[song.g[G_CRATE] & 127];
        fx.rev_ph += LFO_INC[30];
        s1 = osc_sine(fx.cho_ph);
        m1 = osc_sine(fx.rev_ph);
        ca0 = (400 << 8) + ((s0 + 32768) * cdep0 >> 8), ca1 = (400 << 8) + ((s1 + 32768) * fx.cdep >> 8);
        cb0 = (400 << 8) + ((32767 - s0) * cdep0 >> 8), cb1 = (400 << 8) + ((32767 - s1) * fx.cdep >> 8);
        ma = (REV_MOD << 8) + ((m0 * REV_MOD) >> 7), mb = (REV_MOD << 8) + ((m1 * REV_MOD) >> 7);
        dca = (ca1 - ca0) >> CTL_LOG2, dcb = (cb1 - cb0) >> CTL_LOG2;
    }
    for (i = 0; i < n; i++) {
        int32_t yl, yr, a, o0, o1, o2, o3;
#if !CLIPPER_SYNTH
        int32_t x;
#endif
        /* chorus: two modulated short delays, 5..15 ms, the LFO half a turn apart: left and right move apart */
        cho_buf[fx.cho_w & (CHO_LEN - 1u)] = (int16_t)clamp(cho_in[i] >> 1, -32768, 32767);
        {
            int32_t r0 = ca0 + dca * (int32_t)i, r1 = cb0 + dcb * (int32_t)i;
            uint32_t i0 = (uint32_t)r0 >> 8, i1 = (uint32_t)r1 >> 8;
            int32_t c0 = cho_buf[(fx.cho_w - i0) & (CHO_LEN - 1u)], c1 = cho_buf[(fx.cho_w - i0 - 1u) & (CHO_LEN - 1u)];
            int32_t d0 = cho_buf[(fx.cho_w - i1) & (CHO_LEN - 1u)], d1 = cho_buf[(fx.cho_w - i1 - 1u) & (CHO_LEN - 1u)];
            yl = (c0 + (((c1 - c0) * (r0 & 255)) >> 8)) << 1;
            yr = (d0 + (((d1 - d0) * (r1 & 255)) >> 8)) << 1;
        }
        fx.cho_w++;
        /* delay with a low-passed feedback (in the middle) */
#if CLIPPER_SYNTH
        {   /* half rate: an entry is the mean of two samples (entry e ~ sample 2e); a position between two entries
             * reads between them. (1.0) MONO: one tap in the middle; PING: the echoes left (TIME) and right (2 x
             * TIME), the feedback from the right; TAPE: the tap swung by wow and flutter, the feedback saturating.
             * A change of MODE: the old one's taps fade out as the new one's fade in (DL_XF) */
            int32_t xl, xr, xf, v, ol, or_, of;
            uint32_t m = fx.dm_cur;
            dly_taps(m, dl, i, &xl, &xr, &xf);
            if (fx.dl_x < DL_XF) {                      /* (0.9) the old TIME's taps fading out */
                dly_taps(m, dlo, i, &ol, &or_, &of);
                xl = ol + (((xl - ol) * (int32_t)fx.dl_x) >> 10);
                xr = or_ + (((xr - or_) * (int32_t)fx.dl_x) >> 10);
                xf = of + (((xf - of) * (int32_t)fx.dl_x) >> 10);
                fx.dl_x++;
            }
            if (fx.dm_x < DL_XF) {                      /* (1.0) the old MODE's taps fading out */
                dly_taps(fx.dm_old, dl, i, &ol, &or_, &of);
                xl = ol + (((xl - ol) * (int32_t)fx.dm_x) >> 10);
                xr = or_ + (((xr - or_) * (int32_t)fx.dm_x) >> 10);
                xf = of + (((xf - of) * (int32_t)fx.dm_x) >> 10);
                fx.dm_x++;
            }
            fx.dly_lp += mulq15(xf - fx.dly_lp, col);
            v = (dly_in[i] >> 1) + mulq15(fx.dly_lp, fb);
            if (m == DM_TAPE)
                v = softclip(v);                        /* (a tape's: the repeats round off as they pile up) */
            if (fx.dly_w & 1u)
                dly_buf[(fx.dly_w >> 1) & (DLY_LEN - 1u)] = (int16_t)clamp((fx.dly_acc + v) >> 1, -32768, 32767);
            else
                fx.dly_acc = v;
            fx.dly_w++;
            yl += mulq15(xl << 1, dmix);
            yr += mulq15(xr << 1, dmix);
        }
#else
        x = dly_buf[(fx.dly_w - dl) & (DLY_LEN - 1u)];
        if (fx.dl_x < DL_XF) {                          /* (0.9) the old TIME's tap fading out */
            int32_t xo = dly_buf[(fx.dly_w - dlo) & (DLY_LEN - 1u)];
            x = xo + (((x - xo) * (int32_t)fx.dl_x) >> 10);
            fx.dl_x++;
        }
        fx.dly_lp += mulq15(x - fx.dly_lp, col);
        dly_buf[fx.dly_w & (DLY_LEN - 1u)] =
            (int16_t)clamp((dly_in[i] >> 1) + mulq15(fx.dly_lp, fb), -32768, 32767);
        fx.dly_w++;
        x = mulq15(x << 1, dmix);
        yl += x;
        yr += x;
#endif
        /* reverb: two diffusers, then the four lines (Clipper: or the plate, after the loop) */
        if (plate) {
            wet_l[i] = yl;
            wet_r[i] = yr;
            continue;
        }
        a = mulq15(rev_in[i], 13000);
        {
            int16_t *c = rev_ap;
            for (k = 0; k < 2u; k++) {
                int32_t b = c[fx.ap_i[k]], v = a + (b >> 1);
                c[fx.ap_i[k]] = (int16_t)clamp(v, -32768, 32767);
                a = b - (v >> 1);
                if (++fx.ap_i[k] >= REV_AP[k])
                    fx.ap_i[k] = 0;
                c += REV_AP[k];
            }
        }
        {
            int16_t *c = rev_line;
            int32_t s0, s1, d0, d1, r = ma + (((mb - ma) * (int32_t)i) >> CTL_LOG2);   /* (between the two: never below 0) */
            uint32_t ri = fx.line_i[0] + ((uint32_t)r >> 8), rj;
            if (ri >= L0)                                   /* (the oldest sample is at line_i: reading */
                ri -= L0;                                   /* past it shortens line 0 by 0..2 REV_MOD) */
            rj = ri + 1u >= L0 ? 0u : ri + 1u;
            o0 = c[ri] + (((c[rj] - c[ri]) * (r & 255)) >> 8);
            o1 = c[B1 + fx.line_i[1]];
            o2 = c[B2 + fx.line_i[2]];
            o3 = c[B3 + fx.line_i[3]];
            s0 = o0 + o1, d0 = o0 - o1, s1 = o2 + o3, d1 = o2 - o3;   /* Hadamard / 2: each feeds all four */
            /* (1.0) lp_step: the lines at full scale (SIZE and the sends up, DAMP 0) swing the step past 65536, and
             * x 32767 overflowed 32 bits: the tail burst into a crackle. Within 65536, as before, bit for bit */
            fx.line_lp[0] += lp_step(((s0 + s1) >> 1) - fx.line_lp[0], lpk);
            fx.line_lp[1] += lp_step(((d0 + d1) >> 1) - fx.line_lp[1], lpk);
            fx.line_lp[2] += lp_step(((s0 - s1) >> 1) - fx.line_lp[2], lpk);
            fx.line_lp[3] += lp_step(((d0 - d1) >> 1) - fx.line_lp[3], lpk);
            c[fx.line_i[0]] = (int16_t)clamp(mulq15(fx.line_lp[0], g) + a, -32768, 32767);
            c[B1 + fx.line_i[1]] = (int16_t)clamp(mulq15(fx.line_lp[1], g) - a, -32768, 32767);
            c[B2 + fx.line_i[2]] = (int16_t)clamp(mulq15(fx.line_lp[2], g) + a, -32768, 32767);
            c[B3 + fx.line_i[3]] = (int16_t)clamp(mulq15(fx.line_lp[3], g) - a, -32768, 32767);
            if (++fx.line_i[0] >= L0) fx.line_i[0] = 0;
            if (++fx.line_i[1] >= REV_LINE[1]) fx.line_i[1] = 0;
            if (++fx.line_i[2] >= REV_LINE[2]) fx.line_i[2] = 0;
            if (++fx.line_i[3] >= REV_LINE[3]) fx.line_i[3] = 0;
        }
        if (rg0 != 32767 || rg1 != 32767) {             /* (1.0) fading out (a change of type) */
            int32_t g = rg0 + (((rg1 - rg0) * (int32_t)i) >> CTL_LOG2);
            o0 = mulq15(o0, g), o1 = mulq15(o1, g), o2 = mulq15(o2, g), o3 = mulq15(o3, g);
        }
        wet_l[i] = yl + o0 + o2;
        wet_r[i] = yr + o1 - o3;
    }
#if CLIPPER_SYNTH
    if (plate)
        plate_run(rev_in, wet_l, wet_r, n, rg0, rg1);
#endif
}

/* one block of the whole mix (shared with tests/hostsim.c): events -> each part
 * -> dist -> SLICER -> level / pan / sends -> drums (-> SLICER) -> buses -> master; out: stereo Q15 */
static void events_block(uint32_t n);                    /* seq.c */
static int32_t send_c[CTL], send_d[CTL], send_r[CTL], wet_l[CTL], wet_r[CTL], mix_l[CTL], mix_r[CTL], part_buf[CTL];

/* ---- mute / solo: a track that goes silent fades out over ~6 ms (and back in) */
#define MUTE_STEP 4096                                  /* Q15 per block: 8 blocks */
static int32_t gain_next(track_t *t)                    /* the track's mute gain at the end of this block */
{
    int32_t to = trk_silent(t) ? 32767 : 0, a = t->att;
    t->att = a < to ? (to - a > MUTE_STEP ? a + MUTE_STEP : to) : (a - to > MUTE_STEP ? a - MUTE_STEP : to);
    return 32767 - t->att;
}

/* ---- DUCK: every kick (the drum track's KICK / KICK 2) dips the synth parts, which come back over an
 * eighth note: depth G_DUCK, the curve (1 - t / T)^2. drum_on sets drums.kick. */
static struct {
    uint32_t t;                                         /* units since the kick */
    int32_t g0, g1;                                     /* the parts' gain at the block start, end (Q15) */
} duck = {0xFFFFFFFFu, 32767, 32767};

static void duck_block(uint32_t adv)
{
    int32_t depth = CLIPPER_SYNTH ? 0 : song.g[G_DUCK] * 258, x;   /* (Clipper: its slot is the reverb TYPE) */
    uint32_t len = BEAT_U / 2u;
    duck.g0 = duck.g1;
    if (drums.kick) {
        drums.kick = 0;
        duck.t = 0;
    }
    if (!depth || duck.t >= len) {
        duck.g1 = 32767;
        return;
    }
    x = 32767 - (int32_t)((duck.t << 10) / (len >> 5));      /* 1 - t / T, Q15 (t < len < 2^21) */
    if (x < 0)
        x = 0;
    duck.g1 = 32767 - mulq15(depth, mulq15(x, x));
    duck.t = duck.t + adv < duck.t ? 0xFFFFFFFFu : duck.t + adv;
}

/* ---- the DJ filter: v < 0 a low-pass closing, > 0 a high-pass opening, 0 off. The cutoff glides
 * to the knob (no zipper); at 0 it opens fully, then the filter is bypassed. On the master (G_FILT)
 * and, SLOOP 2.4, on each track (P_TFLT: a synth part's mono signal, the drum track's left, right,
 * reverb send and, 2.5, delay send) */
typedef struct {
    int32_t cut;                                        /* now, 0..127 << 8 (CUTOFF_HZ index) */
    int8_t mode;                                        /* -1 LP, 1 HP, 0 off */
    uint8_t seed;                                       /* (SLOOP 2.6) bit per channel: the LP's state from its next sample */
    int32_t z[4][2];                                    /* the SVF states, per channel */
} djf_t;
static djf_t djf, tflt[NTRK];

/* the knob v -> this block's coefficients in *c; 0 = bypassed (nothing to do) */
static int djf_block(djf_t *f, int32_t v, tsvf_t *c)
{
    int32_t to;
    if ((v < 0 && f->mode > 0) || (v > 0 && f->mode < 0))
        v = 0;                                          /* (Clipper 0.9) from one side to the other (a CC, the editor):
                                                         * open this side first, then the other from open (a jump from
                                                         * the low-pass to the high-pass was a crack) */
    if (v < 0 && f->mode >= 0) {                        /* (switching side: from open) */
        f->mode = -1;
        f->cut = 127 << 8;
        memset(f->z, 0, sizeof f->z);
        f->seed = 15;                                   /* (SLOOP 2.6) the low-pass starts where the signal is: from rest
                                                         * its output began at 0 under a sounding note (a click) */
    } else if (v > 0 && f->mode <= 0) {
        f->mode = 1;
        f->cut = 0;
        memset(f->z, 0, sizeof f->z);
    }
    if (!f->mode)
        return 0;
    to = f->mode < 0 ? (v < 0 ? (127 << 8) + v * 90 * 4 : 127 << 8) : (v > 0 ? v * 90 * 4 : 0);
    f->cut += clamp(to - f->cut, -384, 384);            /* ~1.5 index a block */
    if (!v && f->cut == to) {
        f->mode = 0;                                    /* fully open again: off */
        return 0;
    }
    tsvf_coef(c, f->cut, 40);
    return 1;
}
/* n samples of channel ch through the filter (x within +-140000) */
static void djf_run(djf_t *f, const tsvf_t *c, int32_t *b, uint32_t n, uint32_t ch)
{
    uint32_t i;
    int32_t *z = f->z[ch];
    if ((f->seed >> ch) & 1u) {
        f->seed &= (uint8_t)~(1u << ch);
        if (f->mode < 0 && n) {
            z[0] = 0;                                   /* at rest on this input: the output is it (DC: exact) */
            z[1] = clamp(b[0], -140000, 140000);
        }
    }
    for (i = 0; i < n; i++) {
        int32_t x = clamp(b[i], -140000, 140000), y = tsvf_lp(c, x, &z[0], &z[1]);
        b[i] = f->mode < 0 ? y : x - y;
    }
}
/* a track's filter on a signal at 4x its level (b >> 2 into the filter's range, back << 2) */
static void tflt_run(djf_t *f, const tsvf_t *c, int32_t *b, uint32_t n, uint32_t ch)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        b[i] >>= 2;
    djf_run(f, c, b, n, ch);
    for (i = 0; i < n; i++)
        b[i] <<= 2;
}

#if CLIPPER_SYNTH
/* INSERT (Clipper 0.6: SLOOP 2.6's, after Felucca 1.5 by Leo Kuroshita, Discussions #78 and #177): one effect per
 * sound (the instrument and its layer each their own, kept with the sound: presets, projects), after DIST and before
 * the sound's FILTER, with a dry / wet MIX. TYPE (P_ITYPE, N_ITYPE; stored, append-only) and its three values A B C
 * (P_IA .. P_IC, what they mean: params.c ins_desc):
 *   SOFT HARD FOLD FUZZ  DRIVE (1x .. ~13x, squared), TONE (two one-pole low-passes at F_CUTOFF), LEVEL (F_DB, as the
 *                        track's LEVEL: 112 is 0 dB). A slow low cut in front (~27 Hz), a DC blocker after (FUZZ is
 *                        lopsided). SOFT tanh, HARD a flat clip at a voice's level, FOLD a triangle folder (the wave
 *                        reflected at +-16384: every fold more harmonics), FUZZ a hard top and a soft, halved bottom;
 *   CRUSH                BITS (1 .. 16, of a voice's full scale), RATE (a held sample, 689 Hz .. 44.1 kHz, INS_HOLD:
 *                        aliasing as the old samplers), LPF (two one-pole low-passes at F_CUTOFF after them);
 *   PHASR                RATE (F_LFOHZ), DEPTH (the sweep around ~850 Hz: 0 still .. the whole 150 Hz .. 4.8 kHz),
 *                        FDBK (0 .. 0.7): four all-pass stages, mono, per track;
 *   FLANG CHOR           RATE, DEPTH, FDBK: a 256-sample line per track (5.8 ms); FLANG swept 0.25 .. 5 ms (a squared
 *                        triangle), feedback 0 .. 0.85; CHOR a sine around 3.2 ms (+-2.3 ms at DEPTH 127), feedback
 *                        0 .. 0.4. A DC blocker on the swept copy (a clipped loud input is lopsided).
 * The swept ones are the pedals': their wet is (dry + swept) / 2, the notches need both, so MIX 100 % is the whole
 * effect and less a lighter one. MIX (P_IMIX) blends dry and wet linearly; MIX 0 or TYPE OFF: the track bit for bit
 * as before (nothing runs). MIX glides (2.9 ms, SL_SLOPE); a TYPE change fades the old one out, clears the state,
 * fades the new one in; so does a start from MIX 0 (no stale line or filter).
 * State in RAM (.bss): 580 B a sound; noinline, its own loops */
enum { IT_OFF, IT_SOFT, IT_HARD, IT_FOLD, IT_FUZZ, IT_CRUSH, IT_PHASER, IT_FLANGER, IT_CHORUS,
       IT_JUNO1, IT_JUNO2, IT_JUNO12, IT_N };   /* (0.8: the JUNO chorus, juno_chorus below: the INSERT passes them) */
_Static_assert(IT_N == sizeof N_ITYPE / sizeof N_ITYPE[0], "the INSERT's types as params.c names them");
_Static_assert(P_IA == P_ITYPE + 1 && P_IC == P_ITYPE + 3 && P_IMIX == P_ITYPE + 4, "the INSERT's values in order");
#define IL_LEN 256u                     /* FLANG / CHOR: the line, samples (a power of 2: masked) */
#define PH_ST 4u                        /* PHASR: all-pass stages (two notches, the classic pedal's) */
static const uint8_t INS_HOLD[16] = {64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};   /* CRUSH RATE */
/* PHASR: the stages' coefficient over the sweep (Q14), 33 points */
static const int16_t PH_A[33] = {
    -16038, -15998, -15955, -15906, -15853, -15793, -15727, -15653, -15572, -15481, -15381, -15270, -15147, -15012,
    -14862, -14696, -14514, -14313, -14092, -13849, -13582, -13290, -12970, -12621, -12240, -11825, -11375, -10886,
    -10356, -9784, -9166, -8500, -7783,
};
typedef struct {
    int32_t w;                          /* the wet share now, Q15 */
    uint8_t type;                       /* the type running */
    uint8_t cnt;                        /* CRUSH: samples left of the held one */
    uint16_t wp;                        /* FLANG / CHOR: the line's next write */
    uint16_t fill;                      /* (0.9) FLANG / CHOR: samples before the wet comes in (the line full first) */
    uint32_t ph;                        /* the sweep's phase */
    int32_t hp, lp1, lp2, dc, hold;     /* low cut (Q8), the two low-passes, DC blocker (Q8), CRUSH's held sample */
    int32_t px[PH_ST], py[PH_ST], fb;   /* PHASR: each stage's last input and output, the fed back */
    int16_t line[IL_LEN];               /* FLANG / CHOR, at a quarter (headroom) */
} ins_t;
/* (in .bss, not the pool: tools/build.py keeps 8 KiB of the pool spare; RAM has room) */
static ins_t ins[NPART];                         /* the parts (Clipper: the instrument and its layer) */

static inline int32_t ins_mixq(int32_t m) { return m >= 127 ? 32768 : m <= 0 ? 0 : m * 258; }   /* MIX -> Q15 */
static inline int32_t ins_mix(int32_t x, int32_t y, int32_t w) { return w == 32768 ? y : x + mulq16(y - x, (uint32_t)w << 1); }
static inline uint32_t ins_tri(uint32_t q) { uint32_t t = q >> 16; return t < 32768u ? t << 1 : (65535u - t) << 1; }
/* the one-pole low-pass (topology-preserving) coefficient of cutoff index c, Q16: G = g / (1 + g), g = tan(pi fc / FS) */
static int32_t ins_lpk(int32_t c)
{
    uint32_t g = SVF_G[clamp(c, 0, 127)];
    return (int32_t)((g << 16) / (4096u + g));
}
/* the drives' and CRUSH's shape of v (a voice's full scale 32768; v already through DRIVE) */
static inline int32_t ins_shape(uint32_t ty, int32_t v, uint32_t sh)
{
    switch (ty) {
    case IT_SOFT:
        return softclip(v);
    case IT_HARD:
        return clamp(v, -24000, 24000);
    case IT_FOLD: {
        int32_t u = (int32_t)((uint32_t)(v + 16384) & 0xFFFFu) - 32768;   /* a triangle of period 65536: +-16384 */
        u = 16384 - (u < 0 ? -u : u);
        return u + (u >> 1);                            /* x 1.5 (no divide) */
    }
    case IT_FUZZ:
        return v > 0 ? (v > 20000 ? 20000 : v) : softclip(v) >> 1;
    case IT_CRUSH:                                      /* sh: 16 - BITS; to the nearest step */
        return sh ? ((v + (1 << (sh - 1u))) >> sh) << sh : v;
    default:
        return v;
    }
}
static inline uint32_t ins_bits_sh(int32_t a) { return 15u - (uint32_t)(clamp(a, 0, 127) >> 3); }   /* BITS 1..16 */

/* SOFT .. FUZZ: low cut -> DRIVE -> shape -> TONE -> LEVEL -> DC blocker; CRUSH: held sample -> bits -> LPF. The state
 * in locals for the block (b may alias it as far as the compiler knows: no load / store a sample), written back */
static __attribute__((noinline)) void ins_drive(ins_t *s, const int16_t *p, int32_t *b, uint32_t n, int32_t tw)
{
    uint32_t i, ty = s->type, sh = ins_bits_sh(p[P_IA]);
    uint32_t hold = INS_HOLD[clamp(p[P_IB], 0, 127) >> 3];
    int32_t a = clamp(p[P_IA], 0, 127), g = 4096 + a * a * 3;   /* DRIVE, Q12: 1x .. 12.8x, gentle at first */
    int32_t w = s->w, lp1 = s->lp1, lp2 = s->lp2, x, y, v;
    if (ty == IT_CRUSH) {
        uint32_t k = (uint32_t)ins_lpk(p[P_IC]), cnt = s->cnt;
        int32_t hd = s->hold;
        for (i = 0; i < n; i++) {
            x = b[i];
            w += clamp(tw - w, -SL_SLOPE, SL_SLOPE);
            if (!cnt) {
                hd = ins_shape(IT_CRUSH, clamp(x, -262144, 262143), sh);
                cnt = hold;
            }
            cnt--;
            y = hd << 4;                                /* (x16: the low cutoffs' rounding leaves no offset) */
            v = mulq16(y - lp1, k);                     /* LPF: two one-pole low-passes */
            y = v + lp1;
            lp1 = y + v;
            v = mulq16(y - lp2, k);
            y = v + lp2;
            lp2 = y + v;
            b[i] = ins_mix(x, (y + 8) >> 4, w);
        }
        s->cnt = (uint8_t)cnt;
        s->hold = hd;
    } else {
        uint32_t k = (uint32_t)ins_lpk(p[P_IB]);
        int32_t lvl = LEVEL_Q12[p[P_IC] & 127], hp = s->hp, dc = s->dc;
        for (i = 0; i < n; i++) {
            x = b[i];
            w += clamp(tw - w, -SL_SLOPE, SL_SLOPE);
            hp += ((x << 8) - hp) >> 8;                 /* ~27 Hz low cut (Q8: no offset left by the rounding) */
            v = clamp(x - (hp >> 8), -230000, 230000);
            y = ins_shape(ty, ((v >> 3) * g) >> 9, 0) << 4;   /* (>> 3 first: 32 bits; x16 as CRUSH) */
            v = mulq16(y - lp1, k);                     /* TONE: two one-pole low-passes */
            y = v + lp1;
            lp1 = y + v;
            v = mulq16(y - lp2, k);
            y = v + lp2;
            lp2 = y + v;
            y = (((y + 8) >> 4) * lvl) >> 12;           /* LEVEL */
            dc += ((y << 8) - dc) >> 9;                 /* DC blocker, ~14 Hz (FUZZ's lopsided halves) */
            b[i] = ins_mix(x, y - (dc >> 8), w);
        }
        s->hp = hp;
        s->dc = dc;
    }
    s->w = w;
    s->lp1 = lp1;
    s->lp2 = lp2;
}

/* PHASR, FLANG, CHOR: the sweep at RATE (one LFO per state), DEPTH, FDBK; the state in locals for the block */
static __attribute__((noinline)) void ins_swept(ins_t *s, const int16_t *p, int32_t *b, uint32_t n, int32_t tw)
{
    uint32_t i, inc = LFO_INC[clamp(p[P_IA], 0, 127)] / CTL, dep = (uint32_t)clamp(p[P_IB], 0, 127);
    int32_t c = clamp(p[P_IC], 0, 127), w = s->w, dc = s->dc;
    if (s->type == IT_PHASER) {                         /* the coefficients a block at a time (at its middle) */
        uint32_t tr = ins_tri(s->ph + (n >> 1) * inc), q = 32768u + (uint32_t)(((int32_t)tr - 32768) * (int32_t)dep / 127);
        int32_t a = PH_A[q >> 11] + (((PH_A[(q >> 11) + 1u] - PH_A[q >> 11]) * (int32_t)(q & 2047u)) >> 11);
        int32_t fbk = c * 180, fb = s->fb;              /* 0 .. 0.7, Q15 */
        int32_t x0 = s->px[0], x1 = s->px[1], x2 = s->px[2], x3 = s->px[3];
        int32_t y0 = s->py[0], y1 = s->py[1], y2 = s->py[2], y3 = s->py[3];
        _Static_assert(PH_ST == 4u, "ins_swept: four all-pass stages");
        for (i = 0; i < n; i++) {
            int32_t x = b[i], u = clamp((x + 4) >> 3, -16383, 16383) + ((fb * fbk + 16384) >> 15), y;
            w += clamp(tw - w, -SL_SLOPE, SL_SLOPE);
            /* y = a (u - y1) + u1, four times */
            y = clamp(x0 + ((a * (u - y0) + 8192) >> 14), -65535, 65535), x0 = u, y0 = y, u = y;
            y = clamp(x1 + ((a * (u - y1) + 8192) >> 14), -65535, 65535), x1 = u, y1 = y, u = y;
            y = clamp(x2 + ((a * (u - y2) + 8192) >> 14), -65535, 65535), x2 = u, y2 = y, u = y;
            y = clamp(x3 + ((a * (u - y3) + 8192) >> 14), -65535, 65535), x3 = u, y3 = y;
            fb = clamp(y, -32767, 32767);
            y <<= 2;                                    /* (y: at an eighth) */
            dc += ((y << 8) - dc) >> 9;                 /* DC blocker, ~14 Hz (a clipped loud input is lopsided) */
            b[i] = ins_mix(x, (x >> 1) + y - (dc >> 8), w);   /* (dry + all-passed) / 2 */
        }
        s->px[0] = x0, s->px[1] = x1, s->px[2] = x2, s->px[3] = x3;
        s->py[0] = y0, s->py[1] = y1, s->py[2] = y2, s->py[3] = y3;
        s->fb = fb;
        s->ph += n * inc;
    } else {
        int32_t chorus = s->type == IT_CHORUS, fbk = c * (chorus ? 103 : 219);   /* 0 .. 0.4 / 0 .. 0.85, Q15 */
        uint32_t dq = dep * 516u, ph = s->ph, wp = s->wp; /* DEPTH, Q16 */
        int16_t *line = s->line;
        for (i = 0; i < n; i++, ph += inc) {
            int32_t x = b[i], r0, r1, v, fr;
            uint32_t d;                                 /* the delay, Q16 samples */
            if (s->fill)
                s->fill--;                              /* (0.9: an empty line read back: a step when the sound reaches
                                                         * the delay; the wet waits for the line) */
            if (!s->fill)
                w += clamp(tw - w, -SL_SLOPE, SL_SLOPE);
            if (chorus) {
                d = (uint32_t)((140 << 16) + ((osc_sine(ph) * (int32_t)dep * 50) >> 5));   /* 3.2 +- 2.3 ms */
            } else {
                uint32_t tr = ins_tri(ph), sw = (tr * tr) >> 16;   /* longer near the short end */
                d = (11u << 16) + 209u * (((sw >> 1) * dq) >> 15);  /* 0.25 .. 5 ms */
            }
            r0 = (int32_t)(wp - (d >> 16)) & (int32_t)(IL_LEN - 1u);
            r1 = (r0 - 1) & (int32_t)(IL_LEN - 1u);
            fr = (int32_t)((d >> 1) & 0x7FFFu);
            v = line[r0] + (((line[r1] - line[r0]) * fr) >> 15);
            line[wp] = (int16_t)clamp((x >> 2) + softclip(mulq15(v, fbk)), -32767, 32767);
            wp = (wp + 1u) & (IL_LEN - 1u);
            v <<= 1;                                    /* (v: at a quarter) */
            dc += ((v << 8) - dc) >> 9;                 /* DC blocker (the line's clip on a loud lopsided wave) */
            b[i] = ins_mix(x, (x >> 1) + v - (dc >> 8), w);   /* (dry + delayed) / 2 */
        }
        s->ph = ph;
        s->wp = (uint16_t)wp;
    }
    s->w = w;
    s->dc = dc;
}

/* the INSERT of the settings p on one channel b through state s: 1 while it runs (or fades out); 0: dry, bit for bit */
static __attribute__((noinline)) int ins_run(ins_t *s, const int16_t *p, int32_t *b, uint32_t n, int fresh)
{
    int32_t ty = clamp(p[P_ITYPE], 0, IT_N - 1), tw;
    if (ty >= IT_JUNO1)
        ty = IT_OFF;                                    /* the JUNO chorus: not here (juno_chorus, after the FILTER) */
    if (s->type != ty && !s->w)                         /* faded out: the new type from rest */
        s->type = (uint8_t)ty, s->fill = 0;
    tw = s->type == ty && ty ? ins_mixq(p[P_IMIX]) : 0;
    if (!s->w && !s->fill) {
        if (!tw)
            return 0;                                   /* dry: bit for bit */
        memset(s, 0, sizeof *s);                        /* starting: no stale line or filter */
        s->type = (uint8_t)ty;
        if ((ty == IT_FLANGER || ty == IT_CHORUS) && !fresh)
            s->fill = IL_LEN;                           /* (the part was silent: the empty line is its past, no wait) */
    }
    if (s->type >= IT_PHASER)
        ins_swept(s, p, b, n, tw);
    else
        ins_drive(s, p, b, n, tw);
    return s->w != 0 || s->fill != 0;                   /* (filling: running, the wet still out) */
}
/* a part's INSERT on its block b (mix_part): nothing while TYPE is OFF or MIX is 0 and faded */
static void track_insert(track_t *t, int32_t *b, uint32_t n, int fresh)
{
    t->ins_run = (uint8_t)ins_run(&ins[(uint32_t)(t - trk) % NPART], t->p, b, n, fresh);
}
/* the INSERT at rest at once (mix_part: TYPE turned OFF while the part was silent, its fade never ran) */
static __attribute__((noinline)) void ins_rest(track_t *t)
{
    memset(&ins[(uint32_t)(t - trk) % NPART], 0, sizeof(ins_t));
    t->ins_run = 0;
}

/* ---- the JUNO chorus (Clipper 0.8): the stereo BBD chorus of the Juno-60 / 106, its idea and its published
 * measurements (no code of anyone's): one line, two taps whose delays a triangle LFO sweeps in opposite directions,
 * each tap low-passed as a BBD's, left = dry + tap 1, right = dry + tap 2.
 *   I     0.513 Hz, 1.66 .. 5.35 ms        II  0.863 Hz, the same range        I+II  9.75 Hz, 3.3 .. 3.7 ms
 * Its mode: the JUNO engine's CHOR, else the INSERT's types JUNO1 JUNO2 JUN12 (any sound; then MIX the wet, A WIDTH,
 * B TONE, C NOISE: the BBD's hiss). Mode changes fade out (2.9 ms), clear the line, fill it (5.8 ms), fade in. 532 B a
 * part (.bss);
 * the stereo half (left - right) in jc_side for mix_part1's level loop; off: nothing runs */
#define JC_LEN 256u                     /* the line: 5.8 ms (a power of 2: masked), at a quarter (headroom) */
typedef struct {
    int32_t w;                          /* the wet now, Q15 */
    uint32_t ph, nz;                    /* the LFO's phase; the hiss */
    int32_t l1, l2;                     /* each tap's low-pass */
    uint16_t wp;                        /* the line's next write */
    uint8_t mode;                       /* running: 1 I, 2 II, 3 I+II */
    uint16_t fill;                      /* samples left before it fades in (the line full of the sound first) */
    uint8_t idle;                       /* the part was silent (mix_part1 did not run it): a new mode at once */
    int16_t line[JC_LEN];
} jc_t;
static jc_t jc[NPART];
static int32_t jc_side[CTL];
static const uint32_t JC_INC[3] = {49962u, 84048u, 949577u};   /* 0.513, 0.863, 9.75 Hz at 44.1 kHz */
static int32_t jc_mode(const track_t *t, int32_t *wet, int32_t *width, int32_t *tone, int32_t *hiss)
{
    const int16_t *p = t->p;
    int32_t m = t->engine == ENGI_JUNO ? clamp(p[P_E7], 0, 3) : 0;
    *wet = 32768, *width = 127, *tone = 100, *hiss = 0;
    if (!m && p[P_ITYPE] >= IT_JUNO1 && p[P_ITYPE] <= IT_JUNO12) {
        m = p[P_ITYPE] - IT_JUNO1 + 1;
        *wet = ins_mixq(p[P_IMIX]);
        *width = clamp(p[P_IA], 0, 127), *tone = clamp(p[P_IB], 0, 127), *hiss = clamp(p[P_IC], 0, 127);
    }
    return *wet ? m : 0;
}
/* the part silent: a chorus whose mode is no longer asked for stops at once (nothing to fade: the next note dry) */
static void jc_idle(const track_t *t)
{
    jc_t *c = &jc[(uint32_t)(t - trk) % NPART];
    int32_t a, b, d, e;
    c->idle = 1;
    if (c->mode && jc_mode(t, &a, &b, &d, &e) != c->mode)
        c->w = 0, c->mode = 0;
}
/* the chorus on the part's block b (mono in, its mid back in b, its side in jc_side): 1 when it ran */
static __attribute__((noinline)) int juno_chorus(track_t *t, int32_t *b, uint32_t n)
{
    jc_t *c = &jc[(uint32_t)(t - trk) % NPART];
    int32_t wet, width, tone, hiss, m = jc_mode(t, &wet, &width, &tone, &hiss), tw, w, l1, l2, k;
    uint32_t i, ph, wp, inc, base, span;
    int16_t *line = c->line;
    if (c->mode != m && (!c->w || c->idle)) {           /* faded out (or silent): the new mode from rest */
        memset(c, 0, sizeof *c);
        c->mode = (uint8_t)m;
        c->nz = 0x2545F491u;
        c->fill = JC_LEN;                               /* (an empty line read back: a step when the sound reaches it) */
    }
    c->idle = 0;
    if (!c->mode)
        return 0;
    tw = c->mode == m ? wet : 0;
    k = ins_lpk(tone) >> 1;                             /* Q15 */
    inc = JC_INC[c->mode - 1u];
    if (c->mode == 3u)
        base = 9535488u, span = 18u;                    /* 145.5 + 0 .. 17.7 samples (Q16) */
    else
        base = 4797235u, span = 163u;                   /* 73.2 + 0 .. 163 samples */
    w = c->w, ph = c->ph, wp = c->wp, l1 = c->l1, l2 = c->l2;
    width = width * 258;
    hiss = hiss * hiss / 96;                          /* (a gentle curve: 96 a faint hiss, 127 the BBD's) */
    for (i = 0; i < n; i++, ph += inc) {
        int32_t x = b[i], tr = (int32_t)ins_tri(ph), v1, v2, r0, fr, g, md, sd;
        uint32_t d1 = base + span * (uint32_t)tr, d2 = base + span * (uint32_t)(65535 - tr);
        if (c->fill)
            c->fill--;
        else
            w += clamp(tw - w, -SL_SLOPE, SL_SLOPE);
        r0 = (int32_t)(wp - (d1 >> 16)) & (int32_t)(JC_LEN - 1u), fr = (int32_t)((d1 >> 1) & 0x7FFFu);
        v1 = line[r0] + (((line[(r0 - 1) & (int32_t)(JC_LEN - 1u)] - line[r0]) * fr) >> 15);
        r0 = (int32_t)(wp - (d2 >> 16)) & (int32_t)(JC_LEN - 1u), fr = (int32_t)((d2 >> 1) & 0x7FFFu);
        v2 = line[r0] + (((line[(r0 - 1) & (int32_t)(JC_LEN - 1u)] - line[r0]) * fr) >> 15);
        l1 += ((v1 - l1) * k) >> 15;                    /* the BBD's dark */
        l2 += ((v2 - l2) * k) >> 15;
        {
            int32_t in = clamp(x >> 3, -32767, 32767);  /* (1.0: an eighth, was a quarter: 8 loud voices clipped) */
            if (hiss) {
                c->nz = c->nz * 1664525u + 1013904223u;
                in += (((int32_t)(c->nz >> 16) - 32768) * hiss) >> 16;
            }
            line[wp] = (int16_t)clamp(in, -32767, 32767);
        }
        wp = (wp + 1u) & (JC_LEN - 1u);
        g = 32768 - ((w * 9830) >> 15);                 /* 1 dry .. 0.7 at full wet */
        /* (1.0: 64-bit products: a loud chord's x passes 2^16, and x * g wrapped round: a crack at a chord change) */
        md = x + (int32_t)(((int64_t)((l1 + l2) << 2) * w) >> 15);   /* dry + (tap 1 + tap 2) / 2 (the taps at an eighth) */
        sd = (int32_t)(((int64_t)(((int64_t)((l1 - l2) << 2) * w) >> 15) * width) >> 15);   /* (tap 1 - tap 2) / 2 */
        b[i] = (int32_t)(((int64_t)md * g) >> 15);
        jc_side[i] = (int32_t)(((int64_t)sd * g) >> 15);
    }
    c->w = w, c->ph = ph, c->wp = (uint16_t)wp, c->l1 = l1, c->l2 = l2;
    if (!w && c->mode != m)
        c->mode = 0;                                    /* (faded out: the next block takes the new one) */
    return 1;
}
#endif

/* (0.7, SLOOP 2.7's, after JIANT FM 0.6.8, github.com/juanjiant-bit/JIANT) LEVEL and PAN glide: a knob's detent (or a
 * new sound's trim) moves a quarter of the way each block of CTL samples (0.73 ms), ~3 ms in all, instead of a step a
 * detent. The last few units: at once */
static int32_t mix_glide(int32_t from, int32_t to)
{
    int32_t d = to - from;
    return d <= 8 && d >= -8 ? to : from + d / 4;
}

#if CLIPPER_SYNTH
/* Clipper 0.9, the part's duck: what cannot change under a sounding part without a crack (another filter MODEL, another
 * WAVE table, which is built in the voices' own memory, another FMX patch: PTCH) waits for it: the part fades out over
 * 2.9 ms (an S curve), holds silent while the change lands (a table built: a few ms), fades in over 2.9 ms. A silent part:
 * at once; one fading out for a sound loaded: after its fade. part_pending: what waits; part_apply: lands it (FMX: fmx_block loads
 * its patch while dk_free) */
static uint32_t part_pending(track_t *t, int32_t fmw)
{
    uint32_t k = (uint32_t)(t - trk) % NPART, w = 0;
    if (!t->fm_eff)
        t->fm_eff = (uint8_t)fmw;
    w |= fmw != t->fm_eff;
    if (ENGINES[t->engine % NENGINES] == &ENG_WAVE) {
        int16_t tab = (int16_t)((uint32_t)t->p[P_E0] % WT_NALL + 1u);
        if (!wt_tab_eff[k])
            wt_tab_eff[k] = tab;
        w |= (uint32_t)(wt_tab_eff[k] != tab) << 1;
    }
    if (ENGINES[t->engine % NENGINES] == &ENG_FMX && fmx_slot_seen[k] &&
        fmx_slot_seen[k] != clamp(t->p[P_E0], 0, (int32_t)FMX_NSLOT - 1) + 1 && !((fmx_reload >> k) & 1u))
        w |= 4u;
    return w;
}
static void part_apply(track_t *t, int32_t fmw)
{
    uint32_t k = (uint32_t)(t - trk) % NPART;
    if (t->fm_eff && t->fm_eff != fmw && ENGINES[t->engine % NENGINES] == &ENG_VECTOR) {
        uint32_t i;                                     /* (after Clipper 1.2) VECTOR keeps one set of filter states for
                                                         * every MODEL (eng_vector.c s[0..3]): a ladder's (x 4096) read by
                                                         * an SVF burst at full scale; cleared while the part is silent */
        for (i = 0; i < NVOICE; i++)
            t->v[i].s[0] = t->v[i].s[1] = t->v[i].s[2] = t->v[i].s[3] = 0;
    }
    t->fm_eff = (uint8_t)fmw;
    if (ENGINES[t->engine % NENGINES] == &ENG_WAVE)
        wt_tab_eff[k] = (int16_t)((uint32_t)t->p[P_E0] % WT_NALL + 1u);
    else
        wt_tab_eff[k] = 0;
}
static void part_duck(track_t *t, int32_t *b, int32_t *b2, uint32_t n, int32_t fmw)   /* (1.0: b2, SPREAD's side, too) */
{
    uint32_t k;
    if (t->dk_phase == 2u) {                            /* held silent until the change has landed (50 ms at most) */
        uint32_t p = (uint32_t)(t - trk) % NPART;
        int ready = !part_pending(t, fmw) && (ENGINES[t->engine % NENGINES] != &ENG_WAVE || wt_have == wt_tab_eff[p] - 1);
        for (k = 0; k < n; k++)
            b[k] = 0;
        if (b2)
            for (k = 0; k < n; k++)
                b2[k] = 0;
        if (ready || ++t->dk_hold > 68u)
            t->dk_phase = 3, t->dk_pos = 0, t->dk_hold = 0;
        return;
    }
    for (k = 0; k < n; k++) {
        int32_t x = t->dk_phase == 1u ? 32768 - t->dk_pos * 256 : t->dk_pos * 256, x2 = (x * x) >> 15;
        int32_t g = 3 * x2 - 2 * ((x2 * x) >> 15);      /* smoothstep, Q15 */
        if (t->dk_pos >= 128u) {
            if (t->dk_phase == 1u) {
                b[k] = 0;
                if (b2)
                    b2[k] = 0;
            }
            continue;
        }
        b[k] = (int32_t)(((int64_t)b[k] * g) >> 15);
        if (b2)
            b2[k] = (int32_t)(((int64_t)b2[k] * g) >> 15);
        t->dk_pos++;
    }
    if (t->dk_pos >= 128u) {
        t->dk_phase = t->dk_phase == 1u ? 2u : 0u;      /* (out: held from the next block, the change lands there) */
        t->dk_pos = 0, t->dk_hold = 0;
    }
}
#endif

/* one synth part into the dry mix and the sends; a part with no voice sounding costs
 * the LFO tick and a cleared buffer only (after the DIST tail has run out) */
static void mix_part1(track_t *t, uint32_t n)
{
    int32_t *b = part_buf;
    uint32_t i, fresh = !t->tail;                       /* (0.9) the part was silent: a change takes effect at once */
    int32_t g0 = 32767 - t->att, g1 = gain_next(t);
#if CLIPPER_SYNTH
    int32_t fmw = clamp(t->p[P_FMODEL], FM_SVF, FM_LAST);
    if (part_pending(t, fmw)) {                         /* (0.9) a change that would crack the sound: the duck first */
        if (fresh)
            part_apply(t, fmw);                         /* (silent: at once) */
        else if (t->xf_on)
            ;                                           /* (a sound loaded: the old one fades out as it was, the change
                                                         * lands after, the part silent) */
        else if (!t->dk_phase)
            t->dk_phase = 1, t->dk_pos = 0;
        else if (t->dk_phase == 3u)
            t->dk_phase = 1, t->dk_pos = (uint8_t)(128u - t->dk_pos);   /* (from where the fade-in got to) */
    }
    t->dk_free = (uint8_t)(fresh || t->dk_phase == 2u);
    if (t->dk_phase == 2u)
        part_apply(t, fmw);
    if (track_render(t, b, n))
        t->tail = t->p[P_ITYPE] ? 64 : 16, t->ins_xt = 0;  /* blocks of DIST / INSERT state to run out after the last
                                                         * voice (the INSERT's: 46 ms, a flanger's feedback) */
    else if ((!t->tail || !(t->p[P_DIST] | t->p[P_ITYPE] | jc[(uint32_t)(t - trk) % NPART].w) || !--t->tail) &&
             !slicer_busy(t) &&
             !(t->ins_run && t->ins_res > 16u && t->ins_xt < 255u && ++t->ins_xt)) {   /* (the INSERT's DC blocker
                                                         * run out first, up to 255 blocks more: no step) */
        if (t->ins_run && !t->p[P_ITYPE])
            ins_rest(t);                                /* TYPE OFF while silent: no fade-out left for the next note */
        jc_idle(t);                                     /* (0.8: the JUNO chorus the same) */
        part_apply(t, fmw);                             /* (0.9: silent: every change at once) */
        t->dk_phase = 0, t->dk_pos = 0;
        slicer_track(t, 0, n);
        return;
    }
#else
    if (track_render(t, b, n))
        t->tail = 16;                                   /* blocks of DIST state to run out after the last voice */
    else if ((!t->tail || !t->p[P_DIST] || !--t->tail) && !slicer_busy(t)) {
        slicer_track(t, 0, n);                          /* (the SLICER's step clock runs on) */
        return;
    }
#endif
#if CLIPPER_SYNTH
    if (t->dk_phase)
        part_duck(t, b, part_side_on ? part_side : 0, n, fmw);
#endif
    if (!g0 && !g1) {                                   /* silent (MUTE / SOLO): the voices run, nothing is heard */
        slicer_track(t, 0, n);
        return;
    }
    {
        int32_t lvl = LEVEL_Q12[t->p[P_LEVEL] ? clamp(t->p[P_LEVEL] + t->p[P_ED_FX], 1, 127) : 0], pan = t->p[P_PAN];   /* (+ the sound's trim: 1/2 dB steps, as LEVEL's) */
#if CLIPPER_SYNTH
        {   /* (1.0) the expression (CC 11): x (cc + 1)^2 / 128^2, 127 unchanged */
            int32_t ex = 128 - part_ctl[ctl_part(t)].exoff;
            lvl = (lvl * ex * ex) >> 14;
        }
        pan = clamp(pan + t->panmod, -64, 64);         /* Clipper 0.4: + the matrix's PAN (voice.c) */
#endif
        int32_t gl = 4096 - (pan > 0 ? pan * 64 : 0), gr = 4096 + (pan < 0 ? pan * 64 : 0);
        int32_t c = t->p[P_CHOR] * 258, d = t->p[P_DLY] * 258, r = t->p[P_REV] * 258, pk = t->peak;
#if CLIPPER_SYNTH
        int32_t sa = 0;                                 /* (1.0) |the part| before DIST and the INSERT (SPREAD's side
                                                         * follows their level: they run on the mid) */
        {   /* (1.0) SPACE: more reverb and delay */
            int32_t sp = clamp(t->p[P_MAC4], 0, 127);
            r = clamp(t->p[P_REV] + sp, 0, 127) * 258, d = clamp(t->p[P_DLY] + sp / 2, 0, 127) * 258;
        }
#endif
        int32_t xmax, c0, d0, r0, dsc, dsd, dsr;
        int32_t ga = mulq15(g0, duck.g0), gb = mulq15(g1, duck.g1);   /* mute x duck, ramped over the block */
        if (!t->mix_on || fresh)                        /* (0.9) the sends glide as LEVEL does (a jump: a crack in the
                                                         * chorus's and the delay's wet) */
            t->snd_c = c, t->snd_d = d, t->snd_r = r;
        c0 = t->snd_c, d0 = t->snd_d, r0 = t->snd_r;
        t->snd_c = mix_glide(c0, c), t->snd_d = mix_glide(d0, d), t->snd_r = mix_glide(r0, r);
        dsc = t->snd_c - c0, dsd = t->snd_d - d0, dsr = t->snd_r - r0;
        xmax = c0 > d0 ? c0 : d0;
        xmax = xmax > r0 ? xmax : r0;
        xmax = xmax > c ? xmax : c, xmax = xmax > d ? xmax : d, xmax = xmax > r ? xmax : r;
        xmax = 0x7FFFFFFF / (xmax | 1);                 /* sends: loud chords at a high LEVEL */
#if CLIPPER_SYNTH
        if (part_side_on && (t->p[P_DIST] | t->dist_w | t->p[P_ITYPE] | t->ins_run))
            for (i = 0; i < n; i++)
                sa += b[i] < 0 ? -(b[i] >> 6) : b[i] >> 6;
#endif
        track_dist(t, b, n, fresh);
#if CLIPPER_SYNTH
        if (t->p[P_ITYPE] | t->ins_run) {
            track_insert(t, b, n, fresh);               /* the INSERT (0.6): off, nothing */
            t->ins_res = (uint16_t)(b[n - 1u] < 0 ? (-b[n - 1u] > 65535 ? 65535 : -b[n - 1u])
                                                  : (b[n - 1u] > 65535 ? 65535 : b[n - 1u]));   /* (its tail's end) */
        }
#endif
        slicer_track(t, b, n);                          /* slicer.c: before the level, pan and sends */
        {
            tsvf_t fc;                                  /* the track's FILTER (P_TFLT), after the SLICER */
            djf_t *f = &tflt[(uint32_t)(t - trk) % NTRK];
            if (djf_block(f, t->p[P_TFLT], &fc)) {
                tflt_run(f, &fc, b, n, 0);
#if CLIPPER_SYNTH
                if (part_side_on)                       /* (1.0) SPREAD's side through the same filter (its own state) */
                    tflt_run(f, &fc, part_side, n, 1);
#endif
            }
        }
#if CLIPPER_SYNTH
        if (part_side_on && sa) {                       /* (1.0) the side as loud as DIST / the INSERT left the mid */
            int32_t sb = 0, g, g0s = t->sd_g ? t->sd_g : 32768;
            for (i = 0; i < n; i++)
                sb += b[i] < 0 ? -(b[i] >> 6) : b[i] >> 6;
            while (sb > 65535 || sa > 65535)            /* (32 bits: no 64-bit divide) */
                sb >>= 1, sa >>= 1;
            g = (int32_t)(((uint32_t)sb << 15) / (uint32_t)(sa + 1));
            g = clamp(g, 0, 65535);
            t->sd_g = g;
            for (i = 0; i < n; i++) {
                int32_t gi = g0s + (((g - g0s) * (int32_t)i) >> CTL_LOG2);
                part_side[i] = (int32_t)(((int64_t)part_side[i] * gi) >> 15);
            }
        } else {
            t->sd_g = 0;
        }
        const int32_t *side = juno_chorus(t, b, n) ? jc_side : 0;   /* (0.8) the JUNO chorus: stereo */
        if (part_side_on) {                             /* (1.0) + SPREAD's */
            if (side)
                for (i = 0; i < n; i++)
                    part_side[i] += jc_side[i];
            side = part_side;
        }
#endif
        int32_t lvl0, dl, gl0, dgl, gr0, dgr;          /* (0.7, after SLOOP 2.7) LEVEL (and a new sound's trim) and PAN
                                                         * glide (mix_glide), ramped sample by sample inside the block */
        if (!t->mix_on)
            t->lvl = lvl, t->gpl = gl, t->gpr = gr, t->mix_on = 1;
        lvl0 = t->lvl, gl0 = t->gpl, gr0 = t->gpr;
        t->lvl = mix_glide(lvl0, lvl), t->gpl = mix_glide(gl0, gl), t->gpr = mix_glide(gr0, gr);
        dl = (t->lvl - lvl0) >> CTL_LOG2, dgl = t->gpl - gl0, dgr = t->gpr - gr0;
        for (i = 0; i < n; i++) {
            int32_t lv = lvl0 + dl * (int32_t)i;       /* (SLOOP Pro #4: rounded, all of b, in 32 bits as the >> 3 / >> 2
                                                         * first were: they floored the part to 2..9 LSB) */
#if NVOICE > 8                                          /* Clipper: 12 / 16 loud voices: one more bit of headroom */
            int32_t bi = clamp(b[i], -PART_MAX * 2, PART_MAX * 2), x = MULQ12R(bi, lv, 3), a;
#else
            int32_t bi = clamp(b[i], -PART_MAX, PART_MAX), x = MULQ12R(bi, lv, 2), a;   /* (pre-shift: 8 loud voices,
                                                         * 1.0: bounded: x LEVEL 127 within 32 bits) */
#endif
            int32_t xs, g = ga + (((gb - ga) * (int32_t)i) >> CTL_LOG2);
            if (g < 32767) {                            /* (always, at rest: mulq15(32767, 32767) is 32766) */
                g >>= 3;                                /* (Q12: no 32-bit overflow) */
                x = MULQ12R(x, g, 4);
            }
            a = x < 0 ? -x : x;
            xs = clamp(x, -xmax, xmax);                 /* sends: mulq15 would overflow */
            if (a > pk)
                pk = a;
            c = c0 + ((dsc * (int32_t)i) >> CTL_LOG2), d = d0 + ((dsd * (int32_t)i) >> CTL_LOG2);
            r = r0 + ((dsr * (int32_t)i) >> CTL_LOG2);
            if (c)
                send_c[i] += mulq15(xs, c);
            if (d)
                send_d[i] += mulq15(xs, d);
            if (r)
                send_r[i] += mulq15(xs, r);
            gl = gl0 + ((dgl * (int32_t)i) >> CTL_LOG2), gr = gr0 + ((dgr * (int32_t)i) >> CTL_LOG2);   /* (0.7) */
#if CLIPPER_SYNTH
            if (side) {                                 /* the chorus's left - right, through the same level */
                int32_t si = side[i];
#if NVOICE > 8
                int32_t sd = MULQ12R(si, lv, 3);
#else
                int32_t sd = MULQ12R(si, lv, 2);
#endif
                if (g < 32767)
                    sd = MULQ12R(sd, g, 4);             /* (g: Q12 above) */
                mix_l[i] += ((x + sd) * (gl >> 4) + 128) >> 8;
                mix_r[i] += ((x - sd) * (gr >> 4) + 128) >> 8;
                continue;
            }
#endif
            mix_l[i] += (x * (gl >> 4) + 128) >> 8;     /* (x may pass 2^19: PAN's Q12 to Q8, exact at rest: 64 a step;
                                                         * #4: x >> 4 floored the part to 16 LSB) */
            mix_r[i] += (x * (gr >> 4) + 128) >> 8;
        }
        t->peak = pk;
    }
}

/* Clipper 0.3: a sound fading out after a change renders, mixes and sends with its own values (voice.c
 * sound_fade_begin) */
static void mix_part(track_t *t, uint32_t n)
{
#if CLIPPER_SYNTH
    if (t->xf_snd && t->xf_on && t->xf) {
        int16_t keep[P_E0];
        memcpy(keep, t->p, sizeof keep);
        memcpy(t->p, t->p_old, sizeof keep);
        mix_part1(t, n);
        memcpy(t->p, keep, sizeof keep);
        return;
    }
#endif
    mix_part1(t, n);
}

/* ---- DUST: the master through an old sampler and a record. G_DUST 0..127 turns up together: drive
 * into a soft clip, a lower sample rate (held samples, 44.1 -> 11 kHz), fewer bits (15 -> 8), a
 * one-pole low-pass (open -> ~3 kHz), a little hiss and crackle. The hiss and the crackle are the
 * record turning: they fade in with PLAY and out (~0.1 s) at STOP, so a stopped SLOOP is silent.
 * Stereo, ~25 ops a sample. */
static struct {
    int32_t hl, hr, hn;                                 /* held samples, samples left to hold */
    int32_t ll, lr;                                     /* low-pass states */
    int32_t rnd, click;                                 /* noise state, a crackle decaying */
    int32_t bed;                                        /* hiss / crackle level, Q15: 0 stopped, 32767 playing */
} dust = {0, 0, 0, 0, 0, 0x2545F491, 0, 0};

static int32_t crush_bits(int32_t v, int32_t shift)    /* fewer bits, rounded toward 0: no DC from tails */
{
    return v >= 0 ? (v >> shift) << shift : -((-v >> shift) << shift);
}

static void dust_process(int32_t *l, int32_t *r, uint32_t n)
{
    int32_t d = song.g[G_DUST], hold, shift, a, drive, hiss, i, bed0, bed1;
    uint32_t pc;
    if (!d) {
        dust.bed = 0;
        return;
    }
    hold = 1 + d * 3 / 127;
    shift = d / 18;
    a = 32767 - d * 165;                                /* one-pole coefficient, Q15 */
    drive = 4096 + d * 24;                              /* Q12: 1x .. 1.75x */
    hiss = d * 2;
    pc = (uint32_t)d * 7u;                              /* crackle: chance per sample, x 2^-22 */
    bed0 = dust.bed;                                    /* ~0.1 s from 0 to full (238 a block of 32) */
    bed1 = dust.bed = clamp(dust.bed + (song.playing ? 238 : -238), 0, 32767);
    for (i = 0; i < (int32_t)n; i++) {
        int32_t x = l[i], y = r[i];
        int32_t bed = bed0 + (((bed1 - bed0) * i) >> CTL_LOG2);
        uint32_t nz = noise32(&dust.rnd);
        if (--dust.hn <= 0) {                           /* sample and hold, then the bits */
            dust.hn = hold;
            dust.hl = softclip(((x >> 2) * drive) >> 10);
            dust.hr = softclip(((y >> 2) * drive) >> 10);
            if (shift) {
                dust.hl = crush_bits(dust.hl, shift);
                dust.hr = crush_bits(dust.hr, shift);
            }
        }
        dust.ll += mulq15(dust.hl - dust.ll, a);
        dust.lr += mulq15(dust.hr - dust.lr, a);
        if ((nz >> 10) < pc)                            /* a speck of dust */
            dust.click = mulq15(((int32_t)(nz & 0x3FFu) - 512) * d / 8, bed);
        x = dust.ll + dust.click + mulq15(((int32_t)(nz >> 16) - 32768) * hiss >> 15, bed);
        y = dust.lr + dust.click + mulq15(((int32_t)(nz & 0xFFFFu) - 32768) * hiss >> 15, bed);
        dust.click -= dust.click >> 2;
        l[i] = x;
        r[i] = y;
    }
}

static void djf_process(int32_t *l, int32_t *r, uint32_t n)
{
    tsvf_t c;
    if (!djf_block(&djf, song.g[G_FILT], &c))
        return;
    djf_run(&djf, &c, l, n, 0);
    djf_run(&djf, &c, r, n, 1);
}

#include "punch.c"            /* PUNCH-IN FX on the whole mix (FX held + a white key) */
#if CLIPPER_SYNTH
#include "bus.c"              /* Clipper 0.3: the master EQ and compressor */
#endif
static int32_t master_cur = -1;                        /* the volume knob, ramped per sample (no zipper) */
static void mix_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t m0, m1;
    for (i = 0; i < n; i++)
        send_c[i] = send_d[i] = send_r[i] = mix_l[i] = mix_r[i] = 0;
    events_block(n);
    duck_block(n * (uint32_t)song.g[G_BPM]);
    for (i = 0; i < NPART; i++)
        mix_part(&trk[i], n);
#if !CLIPPER_SYNTH || BOARD_SMK37                                  /* (Clipper 1.2: no drum machine: nothing to render; its sounds are
                                                     * out of the image) */
    drums.a0 = TDRUM->att;                              /* the drum track's mute / solo fade */
    drums.a1 = 32767 - gain_next(TDRUM);
    {
        static int32_t dl[CTL], dr[CTL], dv[CTL], dd[CTL];
        tsvf_t fc;
        djf_t *f = &tflt[TRK_DRUM];
        if (n <= CTL && djf_block(f, TDRUM->p[P_TFLT], &fc)) {   /* the drum track's FILTER: left, right, the sends */
            for (i = 0; i < n; i++)
                dl[i] = dr[i] = dv[i] = dd[i] = 0;
            slicer_drums(dl, dr, dv, dd, n);
            tflt_run(f, &fc, dl, n, 0);
            tflt_run(f, &fc, dr, n, 1);
            tflt_run(f, &fc, dv, n, 2);
            tflt_run(f, &fc, dd, n, 3);
            for (i = 0; i < n; i++) {
                mix_l[i] += dl[i];
                mix_r[i] += dr[i];
                send_r[i] += dv[i];
                send_d[i] += dd[i];
            }
        } else {
            slicer_drums(mix_l, mix_r, send_r, send_d, n);   /* drums_render, through the SLICER when on */
        }
    }
#endif
    click_render(mix_l, mix_r, n);                      /* the metronome: its own voice (drums.c), any kit, any mute */
    fx_buses(send_c, send_d, send_r, wet_l, wet_r, n);
    for (i = 0; i < n; i++) {
        mix_l[i] += wet_l[i];
        mix_r[i] += wet_r[i];
    }
    dust_process(mix_l, mix_r, n);
    punch_process(mix_l, mix_r, n);
    djf_process(mix_l, mix_r, n);
#if CLIPPER_SYNTH
    bus_process(mix_l, mix_r, n);
#endif
    m1 = (int32_t)song.master_q12;
    m0 = master_cur < 0 ? m1 : master_cur;
    master_cur = m1;
    if (n > CTL)
        n = CTL;                                        /* (the buffers' size) */
    {
        static int32_t ol[CTL], orr[CTL], ul[CTL], ur[CTL];
        static uint8_t usb_was;
        for (i = 0; i < n; i++) {
            int32_t m = m0 + (((m1 - m0) * (int32_t)i) >> CTL_LOG2);
            int32_t xl = clamp(mix_l[i], -MIX_MAX, MIX_MAX), xr = clamp(mix_r[i], -MIX_MAX, MIX_MAX);
            ol[i] = MULQ12R(xl, m, 2);                 /* (1.0: bounded first: x MASTER in 32 bits; #4: rounded, */
            orr[i] = MULQ12R(xr, m, 2);                /* (x >> 2) * m >> 10 floored to 4 LSB) */
            master_pre(&ol[i], &orr[i]);
            if ((i & 1u)) {                             /* the visualiser: every other sample, as audio.c reads it */
                vis_tap[2u * i] = (int16_t)knee(mix_l[i]);
                vis_tap[2u * i + 1u] = (int16_t)knee(mix_r[i]);
            }
            if (usb_full_now) {                         /* USB AUDIO = FULL: MASTER all the way up (4096) */
                ul[i] = mix_l[i], ur[i] = mix_r[i];     /* (#4: x 4096 / 4096, every bit; it was floored to 4 LSB) */
                master_pre_usb(&ul[i], &ur[i]);
            }
        }
        lim_block(&lim_dac, ol, orr, n);
        for (i = 0; i < n; i++) {
            out[2u * i] = knee(ol[i]);
            out[2u * i + 1u] = knee(orr[i]);
        }
        if (usb_full_now && !usb_was) {                 /* (a new recording: no old block in its delay) */
            memset(&lim_usb, 0, sizeof lim_usb);
            lim_usb.env = LIM_T, lim_usb.g = 32768, lim_usb.hold = 0;
        }
        usb_was = usb_full_now;
        if (usb_full_now) {
            lim_block(&lim_usb, ul, ur, n);
            for (i = 0; i < n; i++) {
                usb_out[2u * i] = knee(ul[i]);
                usb_out[2u * i + 1u] = knee(ur[i]);
            }
        }
    }
}
