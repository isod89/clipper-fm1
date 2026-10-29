/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* Clipper 1.0 (CLIPPER_SYNTH 1): SPREAD, the matrix's 12 slots, LFO 3, ENV 2 LOOP, the MACROS, HALL and SHIMMER,
 * the delay's PING and TAPE, and where they are kept.
 *   SPREAD   0: left = right, bit for bit; up: the voices apart (left != right), the middle ((L + R) / 2) as at 0
 *   MATRIX   slots 7..12 move the sound; an AMT of 0 there: bit for bit as no slot; the sources LFO 3 and MAC1..4
 *   LFO 3    in a slot to the level: the level swings (FREE), and from each note's start (KEY)
 *   ENV 2    LOOP: past its sustain the attack again (the level keeps moving); ONE: it settles
 *   MACROS   BRITE brighter / darker, TIMBR moves the shape, MOTN a vibrato, SPACE more reverb; CC 16..19 (every
 *            engine but HYBRID, whose own four they stay)
 *   REVERB   ROOM, PLATE, HALL, SHIMMER: bounded, dying away; HALL longer than PLATE; SHIMMER an octave up in its tail
 *   DELAY    PING: the first echo left (TIME), the second right (2 x TIME); TAPE: bounded at full feedback, its echoes
 *            swung (not MONO's); a change of MODE under a sounding line: no click
 *   STORE    projects (every 1.0 value of both sounds, the delay's MODE; with track 3's steps there: kept beside the
 *            project, the steps' nudges kept; saved so by 1.0 .. 1.2: their defaults; a 0.9 project: the defaults);
 *            user presets (every value back, the sources 16..19 of slots 1..6 by their fifth bit; the record SLOOP
 *            reads the same with or without the 1.0 values; no tail: the defaults)
 *   PAGES    VOICE 3, LFO 3, ENV 2 LOOP, MACROS, DLY 2, the matrix's slots 7..12
 *   FIX      the JUNO chorus on a loud chord (8 voices at 127): no crack */
#define main hostsim_main
#include <stddef.h>
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
static const uint8_t TRK_DEF[NPART][2] = {{ENGI_HYBRID, 0}, {1, 0}, {4, 5}};   /* (ui.c, Clipper) */
static uint32_t trk_def_engine(uint32_t i) { return i < NPART ? TRK_DEF[i][0] : 0u; }
#include "../firmware/src/project.c"
#define UP_HOST 1
#include "../firmware/src/upreset.c"

static int fails;
static void ok(int c, const char *what)
{
    printf("synth10: %-96s %s\n", what, c ? "ok" : "FAIL");
    fails += !c;
}

#define NCAP (4096u * 24u)
static int32_t cl[NCAP], cr[NCAP];
static uint32_t ncap;
static uint64_t hash;
static void run(uint32_t nb)
{
    static int32_t o[CTL * 2];
    uint32_t b, i;
    ncap = 0;
    hash = 1469598103934665603ull;
    for (b = 0; b < nb; b++) {
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++) {
            if (ncap < NCAP) {
                cl[ncap] = o[2 * i];
                cr[ncap++] = o[2 * i + 1];
            }
            hash = (hash ^ (uint32_t)o[2 * i]) * 1099511628211ull;
            hash = (hash ^ (uint32_t)o[2 * i + 1]) * 1099511628211ull;
        }
    }
}
static void quiet(void)
{
    uint32_t i, k;
    for (i = 0; i < NPART; i++)
        trk_all_off(&trk[i]);
    run(400);
    for (i = 0; i < NPART; i++)
        for (k = 0; k < NVOICE; k++)
            trk[i].v[k].active = trk[i].v[k].gate = 0, trk[i].v[k].stage = 0, trk[i].v[k].env = trk[i].v[k].env_out = 0;
    run(300);
}
static void plain(track_t *t, uint32_t eng, uint32_t pi)
{
    uint32_t k;
    host_preset(t, eng, pi);
    t->p[P_DIST] = t->p[P_CHOR] = t->p[P_DLY] = t->p[P_REV] = 0;
    t->p[P_LD_PIT] = t->p[P_LD_FLT] = t->p[P_LD_SHP] = t->p[P_LD_AMP] = t->p[P_ED_PIT] = t->p[P_ED_FLT] = 0;
    t->p[P_ATK] = 0, t->p[P_SUS] = 127, t->p[P_REL] = 10, t->p[P_VOICE] = V_POLY, t->p[P_PAN] = 0, t->p[P_GLIDE] = 0;
    t->p[P_MWDST] = t->p[P_ATDST] = MD_OFF;
    t->p[P_AMODE] = 0, t->p[P_VINT] = 0, t->p[P_FMODEL] = FM_SVF;
    t->p[P_ITYPE] = 0, t->p[P_IMIX] = 127;
    for (k = 0; k < MX_SLOTS; k++)
        t->p[mx_pid(k) + 2u] = 0;
    for (k = P_SPREAD; k < P_E0; k++)
        t->p[k] = TP[k].def;
}
static double rms_of(const int32_t *x, uint32_t a, uint32_t n)
{
    double e = 0;
    uint32_t i;
    for (i = a; i < a + n && i < ncap; i++)
        e += (double)x[i] * x[i];
    return sqrt(e / n);
}
static double rms(uint32_t a, uint32_t n) { return rms_of(cl, a, n); }
static double drms(uint32_t a, uint32_t n)             /* the first difference's: the top end */
{
    double e = 0;
    uint32_t i;
    for (i = a + 1u; i < a + n && i < ncap; i++)
        e += (double)(cl[i] - cl[i - 1]) * (cl[i] - cl[i - 1]);
    return sqrt(e / n);
}
static int bounded(void)
{
    uint32_t i;
    for (i = 0; i < ncap; i++)
        if (cl[i] >= (1 << 23) || cl[i] <= -(1 << 23) || cr[i] >= (1 << 23) || cr[i] <= -(1 << 23))
            return 0;
    return 1;
}
/* the block levels' largest / smallest over [a, a + n) (blocks of 512): how much the level swings */
static double swing(uint32_t a, uint32_t n)
{
    double mx = 0, mn = 1e30;
    uint32_t b;
    for (b = a; b + 512u <= a + n; b += 512u) {
        double r = rms(b, 512);
        mx = r > mx ? r : mx;
        mn = r < mn ? r : mn;
    }
    return mx / (mn + 1.0);
}
static const uint8_t CH[4] = {48, 55, 60, 64};
static void chord(track_t *t)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        trk_note_on(t, CH[i], 100);
}

static void t_spread(void)
{
    track_t *t = &trk[0];
    static int32_t ml[NCAP];
    double m0, mid, side;
    uint32_t i, same = 1, apart = 0;
    quiet();
    plain(t, 0, 0);
    chord(t);
    run(80);
    for (i = 0; i < ncap; i++) {
        same &= cl[i] == cr[i];
        ml[i] = cl[i];
    }
    ok(same, "SPREAD 0: left = right, bit for bit");
    m0 = rms_of(ml, 2048, ncap - 2048);
    quiet();
    plain(t, 0, 0);
    t->p[P_SPREAD] = 127;
    chord(t);
    run(80);
    {
        double e = 0, s = 0;
        for (i = 2048; i < ncap; i++) {
            double mm = 0.5 * ((double)cl[i] + cr[i]), d = 0.5 * ((double)cl[i] - cr[i]);
            e += mm * mm, s += d * d;
            apart += cl[i] != cr[i];
        }
        mid = sqrt(e / (ncap - 2048)), side = sqrt(s / (ncap - 2048));
    }
    printf("synth10:   SPREAD: mid %.0f (at 0: %.0f), side %.0f\n", mid, m0, side);
    ok(apart > (ncap - 2048) / 2u && side > 0.15 * mid && bounded(), "SPREAD 127: the voices apart (left != right)");
    ok(fabs(20 * log10(mid / m0)) < 1.0, "SPREAD 127: the middle as at 0 (within 1 dB)");
    {   /* SPREAD down again: the side gone, no jump */
        t->p[P_SPREAD] = 0;
        run(600);                                       /* (the master's per-channel state settling) */
        run(20);
        same = 1;
        for (i = 0; i < ncap; i++)
            same &= abs(cl[i] - cr[i]) <= 2;
        {
            int32_t mx = 0;
            uint32_t first = ncap;
            for (i = 0; i < ncap; i++) {
                int32_t d = abs(cl[i] - cr[i]);
                if (d && first == ncap)
                    first = i;
                mx = d > mx ? d : mx;
            }
            printf("synth10:   SPREAD back to 0: largest |L - R| %d, from sample %u\n", mx, first);
        }
        ok(same, "SPREAD back to 0: left = right again (the master's state settled: within 2 of 2^23)");
    }
    trk_all_off(t);
}

static void t_matrix(void)
{
    track_t *t = &trk[0];
    static track_t save;
    static int32_t ref[CTL * 96], out[CTL * 96];
    uint32_t k, i, moved = 1, zero = 1, rs;
    quiet();
    plain(t, 0, 0);
    t->p[P_LRATE] = 100;
    trk_note_on(t, 57, 100), run(2);
    save = *t, rs = rng_state;                         /* (the part rendered from the same state each time) */
    for (i = 0; i < 96u; i++)
        track_render(t, ref + i * CTL, CTL);
    for (k = MX_SLOTS6; k < MX_SLOTS; k++) {
        uint32_t id = mx_pid(k), a, same = 1;
        for (a = 0; a < 2u; a++) {
            *t = save, rng_state = rs;
            t->p[id] = MS_LFO1, t->p[id + 1u] = MT_PITCH, t->p[id + 2u] = (int16_t)(a ? 50 : 0);
            for (i = 0; i < 96u; i++)
                track_render(t, out + i * CTL, CTL);
            same = 1;
            for (i = 0; i < CTL * 96u; i++)
                same &= out[i] == ref[i];
            if (a)
                moved &= !same;
            else
                zero &= same;
        }
    }
    *t = save, rng_state = rs;
    ok(zero, "MATRIX: slots 7..12 at AMT 0: bit for bit as no slot");
    ok(moved, "MATRIX: slots 7..12 move the sound (LFO 1 > PITCH)");
    ok(MX_SLOTS == 12u && MS_NSRC == 20u && !strcmp(N_MSRC[MS_LFO3], "LFO3") && !strcmp(N_MSRC[MS_MAC4], "MAC4"),
       "MATRIX: 12 slots; LFO 3 and MAC1..4 among the sources");
    {   /* a macro as a source: MAC3 > AMP, its level follows the knob */
        double lo, hi;
        quiet();
        plain(t, 0, 0);
        t->p[P_M12SRC] = MS_MAC3, t->p[P_M12DST] = MT_AMP, t->p[P_M12AMT] = -63;
        t->p[P_MAC3] = 0;
        trk_note_on(t, 57, 100);
        run(60);
        lo = rms(1024, ncap - 1024);
        t->p[P_MAC3] = 0;                               /* (MOTN's own vibrato aside: MAC3 0 vs 127 under AMT) */
        quiet();
        plain(t, 0, 0);
        t->p[P_M12SRC] = MS_MAC3, t->p[P_M12DST] = MT_AMP, t->p[P_M12AMT] = -63;
        t->p[P_MAC3] = 127;
        trk_note_on(t, 57, 100);
        run(60);
        hi = rms(1024, ncap - 1024);
        printf("synth10:   MAC3 > AMP -63: %.0f at 0, %.0f at 127\n", lo, hi);
        ok(hi < 0.7 * lo, "MATRIX: MAC3 in slot 12 (to AMP, -63): the level down as the knob goes up");
    }
    trk_all_off(t);
}

static void t_lfo3_env2(void)
{
    track_t *t = &trk[0];
    double s0, s1, s2;
    quiet();
    plain(t, 0, 0);
    t->p[P_M7SRC] = MS_LFO3, t->p[P_M7DST] = MT_AMP, t->p[P_M7AMT] = 0, t->p[P_L3RATE] = 90;
    trk_note_on(t, 57, 100);
    run(2500);
    s0 = swing(4096, ncap - 4096);
    quiet();
    plain(t, 0, 0);
    t->p[P_M7SRC] = MS_LFO3, t->p[P_M7DST] = MT_AMP, t->p[P_M7AMT] = 63, t->p[P_L3RATE] = 90;
    trk_note_on(t, 57, 100);
    run(2500);
    s1 = swing(4096, ncap - 4096);
    quiet();
    plain(t, 0, 0);
    t->p[P_M7SRC] = MS_LFO3, t->p[P_M7DST] = MT_AMP, t->p[P_M7AMT] = 63, t->p[P_L3RATE] = 90, t->p[P_L3MODE] = L2_KEY;
    trk_note_on(t, 57, 100);
    run(2500);
    s2 = swing(4096, ncap - 4096);
    printf("synth10:   LFO 3 > AMP: swing %.2f (AMT 0), %.2f (FREE), %.2f (KEY)\n", s0, s1, s2);
    ok(s0 < 1.15 && s1 > 1.6 && s2 > 1.6 && bounded(), "LFO 3: in slot 7 to AMP the level swings (FREE and KEY)");
    {   /* ENV 2 LOOP: ENV 2 > AMP (-63: the level dips as it rises), an attack and a decay to a low sustain */
        double one, loop;
        uint32_t lp;
        for (lp = 0; lp < 2u; lp++) {
            quiet();
            plain(t, 0, 0);
            t->p[P_E2ATK] = 50, t->p[P_E2DEC] = 60, t->p[P_E2SUS] = 10, t->p[P_E2REL] = 30, t->p[P_E2LOOP] = (int16_t)lp;
            t->p[P_M8SRC] = MS_ENV2, t->p[P_M8DST] = MT_AMP, t->p[P_M8AMT] = -63;
            trk_note_on(t, 57, 100);
            run(3000);
            if (lp)
                loop = swing(ncap / 2u, ncap / 2u);
            else
                one = swing(ncap / 2u, ncap / 2u);
        }
        printf("synth10:   ENV 2 > AMP, second half: swing %.2f (ONE), %.2f (LOOP)\n", one, loop);
        ok(one < 1.15 && loop > 1.5 && bounded(), "ENV 2 LOOP: the level keeps moving (ONE: it settles)");
    }
    trk_all_off(t);
}

static void t_macros(void)
{
    track_t *t = &trk[0];
    double dark, bright, r0, r1;
    uint64_t h0;
    quiet();
    plain(t, 0, 0);
    t->p[P_E4] = 60;
    t->p[P_MAC1] = -64;
    chord(t);
    run(80);
    dark = drms(2048, ncap - 2048) / (rms(2048, ncap - 2048) + 1);
    quiet();
    plain(t, 0, 0);
    t->p[P_E4] = 60;
    t->p[P_MAC1] = 63;
    chord(t);
    run(80);
    bright = drms(2048, ncap - 2048) / (rms(2048, ncap - 2048) + 1);
    printf("synth10:   BRITE: top end %.3f (-64), %.3f (+63)\n", dark, bright);
    ok(bright > 1.5 * dark && bounded(), "MACROS: BRITE +63 brighter than -64");
    quiet();
    plain(t, 0, 0);
    chord(t);
    run(60);
    h0 = hash;
    quiet();
    plain(t, 0, 0);
    t->p[P_MAC2] = 50;
    chord(t);
    run(60);
    ok(hash != h0 && bounded(), "MACROS: TIMBR moves the sound");
    quiet();
    plain(t, 0, 0);
    t->p[P_MAC3] = 127;
    chord(t);
    run(60);
    ok(hash != h0 && bounded(), "MACROS: MOTN moves the sound (vibrato, LFO 3 on the cutoff)");
    {   /* SPACE: the reverb send up: more tail after the notes */
        uint32_t s;
        double tail[2];
        for (s = 0; s < 2u; s++) {
            quiet();
            plain(t, 0, 0);
            t->p[P_REV] = 20, t->p[P_MAC4] = (int16_t)(s ? 127 : 0);
            song.g[G_RTYPE] = RT_PLATE, song.g[G_RSIZE] = 90;
            chord(t);
            run(40);
            trk_all_off(t);
            run(60);
            tail[s] = rms(1024, 40u * CTL);
            (void)r0, (void)r1;
        }
        printf("synth10:   SPACE: tail %.0f (0), %.0f (127)\n", tail[0], tail[1]);
        ok(tail[1] > 2.0 * tail[0], "MACROS: SPACE 127: more reverb in the tail");
    }
    {   /* CC 16..19 */
        track_t *h = &trk[0];
        t = &trk[1];
        int16_t e4 = h->p[P_E4];
        t->eng_req = 0;
        part_ctl_cc(t, 16, 127), part_ctl_cc(t, 17, 0), part_ctl_cc(t, 18, 90), part_ctl_cc(t, 19, 5);
        ok(t->p[P_MAC1] == 63 && t->p[P_MAC2] == -64 && t->p[P_MAC3] == 90 && t->p[P_MAC4] == 5,
           "MACROS: CC 16..19 turn them (BRITE TIMBR centred at 64)");
        h->eng_req = ENGI_HYBRID;
        h->p[P_MAC1] = 0;
        part_ctl_cc(h, 16, 100);
        ok(h->p[P_E4] == 100 && h->p[P_MAC1] == 0, "MACROS: HYBRID's CC 16..19 its own four, as before");
        h->p[P_E4] = e4;
        t = h;
    }
    trk_all_off(t);
}

/* the buses alone: an input into one send, the wet out */
#define FXN (FS * 7u)
static int32_t wl[FXN], wr[FXN];
static void fx_run(uint32_t nblk, const int32_t *din, const int32_t *rin, uint32_t at)
{
    static int32_t z[CTL], d[CTL], r[CTL];
    uint32_t b, i;
    for (b = 0; b < nblk; b++) {
        for (i = 0; i < CTL; i++) {
            uint32_t s = b * CTL + i;
            d[i] = din && s >= at && s - at < FXN ? din[s - at] : 0;
            r[i] = rin && s >= at && s - at < FXN ? rin[s - at] : 0;
        }
        fx_buses(z, d, r, wl + b * CTL, wr + b * CTL, CTL);
    }
}
static double erg(const int32_t *x, uint32_t a, uint32_t n)
{
    double e = 0;
    uint32_t i;
    for (i = a; i < a + n; i++)
        e += (double)x[i] * x[i];
    return e / n;
}
static double goertzel(const int32_t *x, uint32_t a, uint32_t n, double f)
{
    double re = 0, im = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / n);
        re += x[a + i] * w * cos(2 * M_PI * f * i / FS);
        im += x[a + i] * w * sin(2 * M_PI * f * i / FS);
    }
    return sqrt(re * re + im * im) / n;
}
static int32_t inb[FXN];
static void t_reverb(void)
{
    static const char *const NM[4] = {"ROOM", "PLATE", "HALL", "SHIMMER"};
    double late[4], oct[4];
    uint32_t ty, i, good = 1;
    song.g[G_DMIX] = 0, song.g[G_DFDBK] = 0, song.g[G_CDEPTH] = 0;
    song.g[G_RSIZE] = 90, song.g[G_RDAMP] = 30, song.g[G_RPRE] = 10;
    memset(inb, 0, sizeof inb);
    for (i = 0; i < FS / 5u; i++)                       /* a 440 Hz burst, 0.2 s */
        inb[i] = (int32_t)(12000.0 * sin(2 * M_PI * 440.0 * i / FS) * (i < 200u ? i / 200.0 : 1.0));
    for (ty = RT_ROOM; ty <= RT_SHIMMER; ty++) {
        uint32_t b0 = FS / CTL, nb = FXN / CTL;
        double e1, e3;
        song.g[G_RTYPE] = (int16_t)ty;
        fx_run(b0, 0, 0, 0);                            /* (a change of family: its fade, then silence) */
        fx_run(nb, 0, inb, 0);
        for (i = 0; i < nb * CTL; i++)
            good &= wl[i] < (1 << 23) && wl[i] > -(1 << 23) && wr[i] < (1 << 23) && wr[i] > -(1 << 23);
        e1 = erg(wl, FS / 2u, FS / 2u);
        e3 = erg(wl, FS * 3u, FS / 2u);
        late[ty] = 10 * log10((e3 + 1) / (e1 + 1));
        oct[ty] = goertzel(wl, FS * 2u, 8192, 880.0) / (goertzel(wl, FS * 2u, 8192, 440.0) + 1e-9);
        printf("synth10:   %-8s 3 s vs 0.5 s: %6.1f dB; at 2 s, 880 / 440 Hz: %.3f\n", NM[ty], late[ty], oct[ty]);
        good &= late[ty] < -3.0;
    }
    ok(good, "REVERB: ROOM PLATE HALL SHIMMER bounded, dying away");
    ok(late[RT_HALL] > late[RT_PLATE] + 3.0, "REVERB: HALL's tail longer than PLATE's (the same SIZE)");
    ok(oct[RT_SHIMMER] > 3.0 * oct[RT_HALL] && oct[RT_SHIMMER] > 0.1, "REVERB: SHIMMER an octave up in its tail");
    {   /* the worst case: SIZE 127, DAMP 0 (SHIMMER: the octave line must not keep the tank going) */
        uint32_t nb = FXN / CTL;
        good = 1;
        song.g[G_RSIZE] = 127, song.g[G_RDAMP] = 0, song.g[G_RPRE] = 0;
        for (ty = RT_HALL; ty <= RT_SHIMMER; ty++) {
            double e2, e6;
            song.g[G_RTYPE] = (int16_t)ty;
            fx_run(FS / CTL, 0, 0, 0);
            fx_run(nb, 0, inb, 0);
            e2 = erg(wl, FS * 2u, FS), e6 = erg(wl, FS * 6u, FS - CTL);
            printf("synth10:   %-8s SIZE 127 DAMP 0: 6 s vs 2 s %6.1f dB\n", NM[ty], 10 * log10((e6 + 1) / (e2 + 1)));
            good &= e6 < 0.1 * e2;
        }
        ok(good, "REVERB: HALL and SHIMMER at SIZE 127, DAMP 0: still dying away (no runaway)");
        song.g[G_RSIZE] = 90, song.g[G_RDAMP] = 30;
    }
    song.g[G_RTYPE] = RT_PLATE;
    fx_run(FS / CTL, 0, 0, 0);
}

static uint32_t peak_at(const int32_t *x, uint32_t a, uint32_t n)
{
    uint32_t i, at = a;
    int32_t m = -1;
    for (i = a; i < a + n; i++)
        if ((x[i] < 0 ? -x[i] : x[i]) > m)
            m = x[i] < 0 ? -x[i] : x[i], at = i;
    return at;
}
static void t_delay(void)
{
    uint32_t dl, nb = FXN / CTL, i;
    song.g[G_RTYPE] = RT_PLATE;
    song.g[G_BPM] = 120, song.g[G_DTIME] = 1, song.g[G_DMIX] = 127, song.g[G_DFDBK] = 0, song.g[G_DCOLOR] = 127;
    dl = delay_samples();
    memset(inb, 0, sizeof inb);
    inb[0] = inb[1] = 20000;
    song.g[G_DMODE] = DM_PING;
    fx_run(FS / CTL, 0, 0, 0);
    fx_run(nb / 2u, inb, 0, 0);
    {
        uint32_t l1 = peak_at(wl, dl / 2u, dl), r1 = peak_at(wr, dl + dl / 2u, dl);
        double el = erg(wl, l1 - 8u, 16), er = erg(wr, l1 - 8u, 16), el2 = erg(wl, r1 - 8u, 16);
        printf("synth10:   PING: TIME %u: left's echo at %u, right's at %u\n", dl, l1, r1);
        ok(l1 + 4u >= dl && l1 <= dl + 4u && r1 + 4u >= 2u * dl && r1 <= 2u * dl + 4u && er < 0.01 * el && el2 < 0.01 * el,
           "DELAY: PING: the first echo left at TIME, the second right at 2 x TIME");
    }
    {   /* TAPE at full feedback: bounded; its echoes swung, not MONO's */
        static int32_t mono[FXN];
        uint32_t m, good = 1, diff = 0;
        for (i = 0; i < FXN; i++)
            inb[i] = i < FS / 4u ? (int32_t)(15000.0 * sin(2 * M_PI * 660.0 * i / FS)) : 0;
        for (m = DM_MONO; m <= DM_TAPE; m += DM_TAPE) {
            song.g[G_DMODE] = (int16_t)m, song.g[G_DFDBK] = 127;
            fx_run(2u * FS / CTL, 0, 0, 0);
            fx_run(nb, inb, 0, 0);
            for (i = 0; i < nb * CTL; i++) {
                good &= wl[i] < (1 << 22) && wl[i] > -(1 << 22);
                if (m == DM_MONO)
                    mono[i] = wl[i];
                else
                    diff += abs(wl[i] - mono[i]) > 200;
            }
        }
        ok(good && diff > FS / 2u, "DELAY: TAPE: bounded at full feedback, its echoes swung (not MONO's)");
    }
    {   /* a change of MODE under a sounding line: no click */
        int32_t dmax = 0, dsw = 0;
        song.g[G_DFDBK] = 100, song.g[G_DMODE] = DM_MONO;
        for (i = 0; i < FXN; i++)
            inb[i] = (int32_t)(12000.0 * sin(2 * M_PI * 220.0 * i / FS));
        fx_run(FS / CTL, inb, 0, 0);
        for (i = delay_samples() + 2000u; i < FS; i++) {   /* (past the echo's onset) */
            int32_t d = wl[i] - 2 * wl[i - 1] + wl[i - 2];
            d = d < 0 ? -d : d;
            dmax = d > dmax ? d : dmax;
        }
        song.g[G_DMODE] = DM_PING;
        fx_run(FS / 4u / CTL, inb + FS, 0, 0);
        for (i = 2; i < FS / 4u - FS / 4u % CTL; i++) {
            int32_t d = wl[i] - 2 * wl[i - 1] + wl[i - 2];
            d = d < 0 ? -d : d;
            dsw = d > dsw ? d : dsw;
        }
        printf("synth10:   MODE change: largest 2nd difference %d (steady %d)\n", dsw, dmax);
        ok(dsw < 3 * dmax + 200, "DELAY: MONO to PING under a sounding line: no click");
    }
    song.g[G_DMODE] = DM_MONO, song.g[G_DMIX] = 0, song.g[G_DFDBK] = 0;
    fx_run(3u * FS / CTL, 0, 0, 0);
}

/* (1.0, a fix) the JUNO chorus on a loud chord: its products wrapped round past 2^16 (a crack at a chord change) */
static void t_juno_loud(void)
{
    static const uint8_t c1[8] = {45, 52, 57, 60, 64, 67, 69, 72}, c2[8] = {41, 48, 53, 57, 60, 65, 69, 72};
    int32_t worst[2], pk = 0;
    uint32_t ch, i;
    for (ch = 0; ch < 2u; ch++) {
        track_t *t = &trk[0];
        int32_t w = 0;
        quiet();
        for (i = 0; i < P_E0; i++)
            t->p[i] = TP[i].def;
        host_preset(t, ENGI_JUNO, 0);                  /* (JUNO 106 PAD: CHOR I) */
        t->p[P_REV] = 0, t->p[P_DLY] = 0, t->p[P_ATK] = 0, t->p[P_SUS] = 127;
        if (!ch)
            t->p[P_E7] = 0;
        for (i = 0; i < 8u; i++)
            trk_note_on(t, c1[i], 127);
        run(1400);
        for (i = 0; i < 8u; i++)
            trk_note_off(t, c1[i]), trk_note_on(t, c2[i], 127);
        run(2800);
        for (i = 2; i < ncap; i++) {
            int32_t d = abs(cl[i] - 2 * cl[i - 1] + cl[i - 2]);
            w = d > w ? d : w;
            pk = abs(cl[i]) > pk ? abs(cl[i]) : pk;
        }
        worst[ch] = w;
        trk_all_off(t);
    }
    printf("synth10:   JUNO 106 PAD, 8 voices at 127, a chord change: largest 2nd difference %d (CHOR OFF: %d), peak %d\n",
           worst[1], worst[0], pk);
    ok(worst[1] < 2 * worst[0] + 400, "JUNO chorus: a loud chord changed: no crack (its products in 64 bits)");
}

static project_t pj;
static void t_store(void)
{
    track_t *a = &trk[0], *b = &trk[1];
    int16_t va[P_COUNT], vb[P_COUNT];
    uint32_t i, k, same = 1, dflt = 1;
    quiet();
    for (i = 0; i < NSTEP; i++)
        trk[2].step[i].n = 0;
    for (i = P_SPREAD; i < P_E0; i++) {
        const param_desc_t *d = &TP[i];
        a->p[i] = va[i] = (int16_t)(d->min + (int32_t)(i * 7u + 3u) % (d->max - d->min + 1));
        b->p[i] = vb[i] = (int16_t)(d->min + (int32_t)(i * 5u + 1u) % (d->max - d->min + 1));
    }
    song.g[G_DMODE] = DM_TAPE;
    proj_capture(&pj);
    for (i = P_SPREAD; i < P_E0; i++)
        a->p[i] = b->p[i] = TP[i].def;
    song.g[G_DMODE] = DM_MONO;
    proj_apply(&pj, 1);
    for (i = P_SPREAD; i < P_E0; i++)
        same &= a->p[i] == va[i] && b->p[i] == vb[i];
    for (i = 0; i < NSTEP; i++)
        same &= trk[2].micro[i] == 0;
    ok(same && song.g[G_DMODE] == DM_TAPE && sizeof pj == 3840u,
       "project: every 1.0 value of both sounds and the delay's MODE kept (track 3's nudges: none), the size unchanged");
    trk[2].step[5].n = 1, trk[2].step[5].note[0] = 60, trk[2].micro[5] = -7;
    proj_capture(&pj);
    {   /* (after 1.2) track 3 with steps (a SLOOP project's): its nudges stay its own in the project, the 1.0 values
         * beside it (project.c proj_x10_t: the side record in flash, object 14 in a backup) */
        uint32_t own = pj.t[2].micro[5] == -7;
        for (i = 0; i < NSTEP; i++)
            own &= i == 5u || pj.t[2].micro[i] == 0;
        for (i = P_SPREAD; i < P_E0; i++)
            a->p[i] = b->p[i] = TP[i].def;
        song.g[G_DMODE] = DM_MONO;
        proj_apply(&pj, 1);
        same = own;
        for (i = P_SPREAD; i < P_E0; i++)
            same &= a->p[i] == va[i] && b->p[i] == vb[i];
        ok(same && trk[2].micro[5] == -7 && trk[2].step[5].n == 1 && song.g[G_DMODE] == DM_TAPE,
           "project: track 3 with steps: its nudges kept as they are, every 1.0 value and the MODE kept (beside the project)");
        memset(&proj_x10s[proj_x10_at(&pj)], 0, sizeof proj_x10s[0]);   /* (a project of 1.0 .. 1.2: nothing beside it) */
        for (i = P_SPREAD; i < P_E0; i++)
            a->p[i] = va[i];
        proj_apply(&pj, 1);
        for (i = P_SPREAD; i < P_E0; i++)
            dflt &= a->p[i] == TP[i].def && b->p[i] == TP[i].def;
        ok(dflt && trk[2].micro[5] == -7 && song.g[G_DMODE] == DM_MONO,
           "project: track 3 with steps, saved by 1.0 .. 1.2 (nothing beside it): its nudges kept, the 1.0 values at their defaults");
    }
    trk[2].step[5].n = 0, trk[2].micro[5] = 0;
    proj_capture(&pj);
    memset(pj.t[2].micro, 0, sizeof pj.t[2].micro);    /* (as 0.9 wrote it) */
    memset(&proj_x10s[proj_x10_at(&pj)], 0, sizeof proj_x10s[0]);
    for (i = P_SPREAD; i < P_E0; i++)
        a->p[i] = va[i];
    proj_apply(&pj, 1);
    dflt = 1;
    for (i = P_SPREAD; i < P_E0; i++)
        dflt &= a->p[i] == TP[i].def;
    ok(dflt, "project: a 0.9 project: the 1.0 values at their defaults");
    {   /* user presets */
        up_rec_t r, r2;
        int16_t v[P_COUNT], v2[P_COUNT], out[P_COUNT], def[P_COUNT];
        uint32_t good = 1;
        for (i = 0; i < P_COUNT; i++)
            def[i] = i >= P_E0 ? 0 : TP[i].def;
        for (i = 0; i < P_COUNT; i++)
            v[i] = (int16_t)(i >= P_E0 ? 40 + i : i < UP_E0V ? i * 3 : TP[i].min + (int32_t)(i * 7u) % (TP[i].max - TP[i].min + 1));
        for (k = 0; k < MX_SLOTS; k++)                  /* (the new sources, in slots 1..6 and 7..12) */
            v[mx_pid(k)] = (int16_t)(k % 2u ? MS_LFO3 + k % 5u : k % 3u);
        memset(&r, 0, sizeof r);
        r.used = UP_USED, r.ver = UP_VER, r.name[0] = 'T';
        up_rec_set(&r, v);
        up_params(&r, out, def);
        for (i = 0; i < P_COUNT; i++)
            good &= out[i] == v[i];
        ok(good && r.np == 61u && sizeof r == 192u, "user presets: every value back, LFO 3 and the macros as sources in all 12 slots");
        memcpy(v2, v, sizeof v2);
        for (i = P_SPREAD; i < P_E0; i++)
            v2[i] = TP[i].def;
        for (k = 0; k < MX_SLOTS6; k++)
            v2[mx_pid(k)] &= 15;
        memset(&r2, 0, sizeof r2);
        r2.used = UP_USED, r2.ver = UP_VER, r2.name[0] = 'T';
        up_rec_set(&r2, v2);
        ok(!memcmp(&r, &r2, sizeof r), "user presets: the record SLOOP reads the same with or without the 1.0 values");
        up_rec_set(&r, v);
        up_xt[0] = 0;                                   /* (no tail: an older save) */
        up_params(&r, out, def);
        good = 1;
        for (i = P_SPREAD; i < P_E0; i++)
            good &= out[i] == def[i];
        for (k = 0; k < MX_SLOTS6; k++)
            good &= out[mx_pid(k)] == (v[mx_pid(k)] & 15);
        ok(good, "user presets: no tail: the 1.0 values at their defaults, the sources' four bits");
    }
}

static int has_page(const char *t, uint32_t id)
{
    uint32_t i, k;
    for (i = 0; i < NELEM(PAGES); i++)
        if (!strcmp(PAGES[i].title, t)) {
            for (k = 0; k < 4u; k++)
                if (PAGES[i].id[k] == id)
                    return 1;
        }
    return 0;
}
static void t_pages(void)
{
    ok(has_page("VOICE 3", P_SPREAD) && has_page("LFO 3", P_L3RATE) && has_page("LFO 3", P_L3MODE) &&
       has_page("ENV 2 LOOP", P_E2LOOP) && has_page("MACROS", P_MAC1) && has_page("MACROS", P_MAC4) &&
       has_page("DLY 2", G_DMODE),
       "PAGES: VOICE 3 (SPREAD), LFO 3, ENV 2 LOOP, MACROS, DLY 2 (MODE)");
    ok(!strcmp(TP[P_MAC1].label, "BRITE") && !strcmp(TP[P_MAC4].label, "SPACE") && !strcmp(GP[G_DMODE].label, "MODE") &&
       !strcmp(TP[P_SPREAD].label, "SPRED"), "PAGES: the names (BRITE .. SPACE, SPRED, MODE)");
}

int main(void)
{
    uint32_t i;
    host_tracks_init();
    for (i = 0; i < NPART; i++)
        host_preset(&trk[i], TRK_DEF[i][0], TRK_DEF[i][1]);
    clipper_proj_neutral();
    trk[1].p[P_LYMODE] = LY_OFF;
    t_spread();
    t_matrix();
    t_lfo3_env2();
    t_macros();
    t_reverb();
    t_delay();
    t_juno_loud();
    t_store();
    t_pages();
    printf(fails ? "synth10: %d FAILED\n" : "synth10: all passed\n", fails);
    return fails;
}
