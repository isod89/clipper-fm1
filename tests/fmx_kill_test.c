/* SPDX-License-Identifier: GPL-3.0-only */
/* FMX: a voice given up (voice.c voice_kill: another part needed it, or the overload shed it) ends at the end of its
 * fade without fmx_done, so its fmx_voice_t stayed "live": the next note on it was taken for a retrigger and kept
 * the old note's operator phases, envelopes and feedback samples. The operators' relative phases then differ from
 * note to note: FMX BRASS's carrier took a DC offset of up to half its level (+-7000 on an rms of 13000) and its
 * timbre changed with every such note. FM6 starts such a voice from rest (its note_on: !v->env && !v->env_out).
 * The check: a note on each voice given up sounds as the same note on a fresh voice (their difference under -12 dB), and its DC
 * stays as small. Exit status: failed checks. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define NS 8192u
static int fails;
static void ok(int c, const char *what)
{
    printf("fmx_kill: %-90s %s\n", what, c ? "ok" : "FAIL");
    fails += !c;
}
static void load(uint32_t e, uint32_t pi)
{
    uint32_t i;
    for (i = 0; i < P_E0; i++)                       /* as ui.c apply_preset_to */
        trk[0].p[i] = TP[i].def;
    host_preset(&trk[0], e, pi);
    trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
    trk[0].p[P_SPREAD] = 0;                          /* (each voice the same: in the middle, no VINTAGE) */
    trk[0].p[P_VINT] = 0;
}
/* note 60 for 46 ms + NS samples: the samples after the first 46 ms into x, returns the DC */
static double play(int32_t *x)
{
    static int32_t o[CTL * 2];
    uint32_t b, i;
    double s = 0;
    trk_note_on(&trk[0], 60, 100);
    for (b = 0; b < 64; b++)
        mix_block(o, CTL);
    for (b = 0; b < NS / CTL; b++) {
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++)
            s += x[b * CTL + i] = mix_l[i] + mix_r[i];
    }
    trk_note_off(&trk[0], 60);
    for (b = 0; b < 3000; b++)                       /* (released, rung out: fmx_done ends the voice) */
        mix_block(o, CTL);
    return s / NS / 2;
}
int main(void)
{
    static int32_t o[CTL * 2], ref[NS], x[NS];
    uint32_t pi = 0, k, b, same = 0, dcok = 0;
    double dc0, dc, worst = 0;
    char msg[128];
    const engine_t *en = ENGINES[ENGI_FMX];
    host_tracks_init();
    for (k = 0; k < en->npresets; k++)
        if (!strcmp(en->presets[k].name, "FMX BRASS"))
            pi = k;
    ok(!strcmp(en->presets[pi].name, "FMX BRASS"), "FMX BRASS found");
    load(ENGI_FMX, pi);
    for (b = 0; b < 32; b++)
        mix_block(o, CTL);
    dc0 = play(ref);
    load(ENGI_FMX, 0);                               /* FMX EP: eight notes, every voice given up while it sounds */
    for (k = 0; k < NVOICE; k++)
        trk_note_on(&trk[0], 48 + k * 3, 100);
    for (b = 0; b < 200; b++)
        mix_block(o, CTL);
    for (k = 0; k < NVOICE; k++)
        if (trk[0].v[k].active)
            voice_kill(&trk[0].v[k]);
    for (b = 0; b < 16; b++)
        mix_block(o, CTL);
    trk_all_off(&trk[0]);
    load(ENGI_FMX, pi);
    for (b = 0; b < 32; b++)
        mix_block(o, CTL);
    for (k = 0; k < NVOICE; k++) {                   /* (ROTATE: each note on the next voice) */
        dc = play(x);
        {
            uint32_t i;
            double d = 0, r = 0;
            for (i = 0; i < NS; i++)
                d += (double)(x[i] - ref[i]) * (x[i] - ref[i]), r += (double)ref[i] * ref[i];
            same += d < r * 0.063;                   /* (under -12 dB: the same sound; fresh notes on other voices differ by
                                                     * -16 .. -36 dB, the old retriggered ones by -1.4 .. +6 dB) */
            if (getenv("V")) printf("voice %u: difference %.1f dB, dc %.0f\n", k, 10 * log10(d / r + 1e-12), dc);
        }
        dcok += fabs(dc) < fabs(dc0) + 500;
        if (fabs(dc) > worst)
            worst = fabs(dc);
    }
    snprintf(msg, sizeof msg, "FMX BRASS C4 on a voice given up = on a fresh voice, within -12 dB (%u of %u)", same, NVOICE);
    ok(same == NVOICE, msg);
    snprintf(msg, sizeof msg, "  .. its DC as a fresh note's (%.0f; worst %.0f, of %u voices %u)", dc0, worst, NVOICE, dcok);
    ok(dcok == NVOICE, msg);
    printf("fmx_kill: %d failed\n", fails);
    return fails;
}
