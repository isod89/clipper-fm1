/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* Clipper 0.2 plate reverb (firmware/src/fx.c plate_*: G_RTYPE = PLATE), through fx_buses (the reverb bus alone):
 *   build/host/reverb_test [WAV]
 * 1. an impulse: the tail decays (RT60 measured), longer with SIZE, shorter with DAMP's highs gone first
 * 2. stable: 30 s of full-scale noise at SIZE 127, DAMP 0: bounded, and silent again after (no runaway)
 * 3. wide: left and right decorrelated (|r| < 0.5 over the tail)
 * 4. the level of ROOM's at the default settings (within 4 dB, the same send)
 * 5. PRE: the first output comes PRE ms after the input
 * 6. ROOM -> PLATE -> ROOM with a tail running: no burst (the other one starts from silence)
 * 7. the delay line (Clipper: at half rate): an echo at the delay time, the repeats decaying
 * WAV: the impulse response of ROOM and PLATE, one after the other (to listen). Exit status: failed checks. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("reverb: %-92s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

#define SEC 10
static int32_t L[FS * SEC], R[FS * SEC];

static void setup(int type, int size, int damp, int pre)
{
    memset(&fx, 0, sizeof fx);
    memset(rev_line, 0, sizeof rev_line);
    memset(rev_ap, 0, sizeof rev_ap);
    pl.ready = 0;
    song.g[G_RTYPE] = (int16_t)type;
    song.g[G_RSIZE] = (int16_t)size;
    song.g[G_RDAMP] = (int16_t)damp;
    song.g[G_RPRE] = (int16_t)pre;
    song.g[G_DMIX] = 0;
    song.g[G_DFDBK] = 0;
    song.g[G_CDEPTH] = 0;
}

/* in(i): the send at sample i; secs of output into L, R */
static void run(int32_t (*in)(uint32_t), uint32_t samples)
{
    static int32_t zero[CTL], rin[CTL], wl[CTL], wr[CTL];
    uint32_t i, k;
    for (i = 0; i < samples; i += CTL) {
        for (k = 0; k < CTL; k++)
            rin[k] = in(i + k);
        fx_buses(zero, zero, rin, wl, wr, CTL);
        for (k = 0; k < CTL && i + k < samples; k++)
            L[i + k] = wl[k], R[i + k] = wr[k];
    }
}
static int32_t impulse(uint32_t i) { return i < 4 ? 60000 : 0; }
static int32_t nz_st = 99;
static int32_t noise(uint32_t i) { return i < FS * 30u ? (int32_t)(noise32(&nz_st) % 131072u) - 65536 : 0; }

/* RT60 from the energy decay (Schroeder integration), -5 .. -35 dB, extrapolated */
static double rt60(uint32_t n)
{
    static double e[FS * SEC];
    double tot = 0, t5 = -1, t35 = -1;
    int32_t i;
    for (i = (int32_t)n - 1; i >= 0; i--) {
        tot += (double)L[i] * L[i] + (double)R[i] * R[i];
        e[i] = tot;
    }
    for (i = 0; i < (int32_t)n; i++) {
        double d = 10 * log10(e[i] / e[0] + 1e-30);
        if (t5 < 0 && d <= -5)
            t5 = (double)i / FS;
        if (t35 < 0 && d <= -35)
            t35 = (double)i / FS;
    }
    return t5 >= 0 && t35 > t5 ? (t35 - t5) * 2 : 99;
}
static double bright(uint32_t a, uint32_t b)       /* the first difference's energy / the energy */
{
    double s = 0, d = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        s += (double)L[i] * L[i], d += (double)(L[i] - L[i - 1]) * (L[i] - L[i - 1]);
    return sqrt(d / s);
}
static double rms(uint32_t a, uint32_t b)
{
    double s = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        s += (double)L[i] * L[i] + (double)R[i] * R[i];
    return sqrt(s / (2.0 * (b - a)));
}

int main(int argc, char **argv)
{
    char w[140];
    double t_small, t_big, t_damp, r_room, r_plate;
    host_tracks_init();
    /* 1. decay */
    setup(RT_PLATE, 40, 60, 0), run(impulse, FS * SEC), t_small = rt60(FS * SEC);
    setup(RT_PLATE, 110, 60, 0), run(impulse, FS * SEC), t_big = rt60(FS * SEC);
    setup(RT_PLATE, 110, 120, 0), run(impulse, FS * SEC), t_damp = rt60(FS * SEC);
    snprintf(w, sizeof w, "PLATE RT60: SIZE 40 %.2f s, SIZE 110 %.2f s (longer, < 8 s)", t_small, t_big);
    check(w, t_small > 0.5 && t_big > t_small * 1.8 && t_big < 8);
    {   /* DAMP: the highs die first (a darker tail: the first difference against the level, 0.3 .. 1 s) */
        double b_open, b_damp;
        setup(RT_PLATE, 110, 0, 0), run(impulse, FS * 2), b_open = bright(FS / 3, FS);
        setup(RT_PLATE, 110, 120, 0), run(impulse, FS * 2), b_damp = bright(FS / 3, FS);
        snprintf(w, sizeof w, "PLATE DAMP 120: a darker tail than DAMP 0 (brightness %.3f vs %.3f)", b_damp, b_open);
        check(w, b_damp < 0.5 * b_open);
    }
    (void)t_damp;
    /* 3. wide */
    {
        double sl = 0, sr = 0, slr = 0;
        uint32_t i;
        setup(RT_PLATE, 90, 60, 0), run(impulse, FS * 3);
        for (i = FS / 10; i < FS * 2u; i++)
            sl += (double)L[i] * L[i], sr += (double)R[i] * R[i], slr += (double)L[i] * R[i];
        snprintf(w, sizeof w, "PLATE stereo: L / R correlation over the tail %.2f (< 0.5)", slr / sqrt(sl * sr));
        check(w, fabs(slr / sqrt(sl * sr)) < 0.5);
    }
    /* 4. level against ROOM (defaults: SIZE 90, DAMP 60), a burst of noise */
    nz_st = 5, setup(RT_ROOM, 90, 60, 0), run(noise, FS * 2), r_room = rms(FS / 2, FS * 2);
    nz_st = 5, setup(RT_PLATE, 90, 60, 0), run(noise, FS * 2), r_plate = rms(FS / 2, FS * 2);
    snprintf(w, sizeof w, "PLATE as loud as ROOM (the same send): %.1f dB", 20 * log10(r_plate / r_room));
    check(w, fabs(20 * log10(r_plate / r_room)) < 4.0);
    /* 5. pre-delay */
    {
        uint32_t i, first = 0;
        setup(RT_PLATE, 90, 60, 50), run(impulse, FS);
        for (i = 0; i < FS; i++)
            if (abs(L[i]) > 50 || abs(R[i]) > 50) {
                first = i;
                break;
            }
        snprintf(w, sizeof w, "PRE 50 ms: the first output at %.1f ms (50 .. 60: + the diffusers)", first * 1000.0 / FS);
        check(w, first * 1000.0 / FS >= 50 && first * 1000.0 / FS < 60);
    }
    /* 2. stable (last: it uses the 30 s) */
    {
        static int32_t big[CTL];
        uint32_t i, k;
        int32_t mx = 0, tail = 0;
        static int32_t zero[CTL], wl[CTL], wr[CTL];
        nz_st = 77;
        setup(RT_PLATE, 127, 0, 0);
        for (i = 0; i < FS * 50u; i += CTL) {
            for (k = 0; k < CTL; k++)
                big[k] = noise(i + k);
            fx_buses(zero, zero, big, wl, wr, CTL);
            for (k = 0; k < CTL; k++) {
                if (abs(wl[k]) > mx)
                    mx = abs(wl[k]);
                if (i > FS * 48u && abs(wl[k]) > tail)
                    tail = abs(wl[k]);
            }
        }
        snprintf(w, sizeof w, "SIZE 127 DAMP 0: 30 s of full-scale noise bounded (peak %d), 20 s later quiet (%d)", mx, tail);
        check(w, mx < 200000 && tail < mx / 20);
    }
    /* 6. switching under a tail */
    {
        static int32_t zero[CTL], rin[CTL], wl[CTL], wr[CTL];
        uint32_t i, k;
        int32_t before = 0, after = 0;
        nz_st = 3;
        setup(RT_ROOM, 110, 40, 0);
        for (i = 0; i < FS * 3u; i += CTL) {
            if (i == FS)
                song.g[G_RTYPE] = RT_PLATE;
            if (i == FS * 2u)
                song.g[G_RTYPE] = RT_ROOM;
            for (k = 0; k < CTL; k++)
                rin[k] = i < FS / 2 ? noise(i + k) / 2 : 0;
            fx_buses(zero, zero, rin, wl, wr, CTL);
            for (k = 0; k < CTL; k++) {
                if (i < FS && abs(wl[k]) > before)
                    before = abs(wl[k]);
                if (i >= FS && abs(wl[k]) > after)
                    after = abs(wl[k]);
            }
        }
        snprintf(w, sizeof w, "ROOM -> PLATE -> ROOM under a tail: nothing louder after (%d) than before (%d)", after, before);
        check(w, after <= before);
    }
    {   /* 7. the delay line at half rate (Clipper): the echo at the delay time, the repeats decaying */
        static int32_t zero[CTL], din[CTL], wl[CTL], wr[CTL];
        static int32_t y[FS * 3 + CTL];                  /* (+ CTL: the last block) */
        uint32_t i, k, dl, first = 0, second = 0;
        int32_t p1 = 0, p2 = 0;
        setup(RT_ROOM, 90, 60, 0);
        song.g[G_DMIX] = 127, song.g[G_DFDBK] = 70, song.g[G_DCOLOR] = 127, song.g[G_BPM] = 100;
        dl = delay_samples();
        memset(dly_buf, 0, sizeof dly_buf);
        for (i = 0; i < FS * 3u; i += CTL) {
            for (k = 0; k < CTL; k++)
                din[k] = i + k < 8 ? 40000 : 0;
            fx_buses(zero, din, zero, wl, wr, CTL);
            for (k = 0; k < CTL; k++)
                y[i + k] = wl[k];
        }
        for (i = 1; i < FS * 3u; i++) {
            if (!first && abs(y[i]) > 2000)
                first = i;
            if (first && i > first + dl / 2 && !second && abs(y[i]) > 300)
                second = i;
        }
        for (i = first; i < first + 32 && i < FS * 3u; i++)
            p1 = abs(y[i]) > p1 ? abs(y[i]) : p1;
        for (i = second; i < second + 32 && i < FS * 3u; i++)
            p2 = abs(y[i]) > p2 ? abs(y[i]) : p2;
        snprintf(w, sizeof w, "DLY at half rate: the echo after %u samples (the time: %u), the next %u later, quieter (%d -> %d)",
                 first, dl, second - first, p1, p2);
        check(w, abs((int)first - (int)dl) <= 2 && abs((int)(second - first) - (int)dl) <= 2 && p2 < p1 && p2 > p1 / 10);
    }
    if (argc > 1) {                                  /* the two impulse responses, to listen */
        static int16_t pcm[FS * 8 * 2];
        uint32_t i, k = 0;
        FILE *f;
        setup(RT_ROOM, 90, 60, 0), run(impulse, FS * 4);
        for (i = 0; i < FS * 4u; i++)
            pcm[k++] = (int16_t)clamp(L[i] / 2, -32768, 32767), pcm[k++] = (int16_t)clamp(R[i] / 2, -32768, 32767);
        setup(RT_PLATE, 90, 60, 20), run(impulse, FS * 4);
        for (i = 0; i < FS * 4u; i++)
            pcm[k++] = (int16_t)clamp(L[i] / 2, -32768, 32767), pcm[k++] = (int16_t)clamp(R[i] / 2, -32768, 32767);
        f = fopen(argv[1], "wb");
        if (f) {
            uint32_t bytes = k * 2u, v;
            fwrite("RIFF", 1, 4, f), v = 36 + bytes, fwrite(&v, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f);
            v = 16, fwrite(&v, 4, 1, f);
            {
                uint16_t h[2] = {1, 2};
                fwrite(h, 2, 2, f);
            }
            v = FS, fwrite(&v, 4, 1, f), v = FS * 4u, fwrite(&v, 4, 1, f);
            {
                uint16_t h[2] = {4, 16};
                fwrite(h, 2, 2, f);
            }
            fwrite("data", 1, 4, f), fwrite(&bytes, 4, 1, f), fwrite(pcm, 2, k, f);
            fclose(f);
        }
    }
    printf(bad ? "reverb: %d FAILED\n" : "reverb test passed\n", bad);
    return bad;
}
