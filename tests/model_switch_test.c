/* SPDX-License-Identifier: GPL-3.0-only */
/* (after Clipper 1.2) the filter MODEL changed under a held chord, every pair of models, on ANALOG, WAVE and VECTOR: the
 * click mark (the largest second difference) and the peak right after the change against the steady sound before and
 * after, as synth9_test does it (synth9 changes the model on ANALOG only). VECTOR keeps one set of filter states for
 * every model: a ladder's states (x 4096) read by an SVF burst at full scale (DIODE -> SEM: 17994, steady ~1800) until
 * fx.c part_apply cleared them while the part is ducked. Cutoffs 20 and 40, levels 40 and 100. Exit status: clicks. */
#define main hostsim_main
#include <stddef.h>
#include "hostsim.c"
#undef main
#define NCAP (4096u * 24u)
static int32_t cl[NCAP];
static uint32_t ncap;
static void run(uint32_t nb)
{
    static int32_t o[CTL * 2];
    uint32_t b, i;
    ncap = 0;
    for (b = 0; b < nb; b++) {
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++)
            if (ncap < NCAP) cl[ncap++] = o[2 * i];
    }
}
static int32_t d2max(void)
{
    int32_t mx = 0, d;
    uint32_t i;
    for (i = 2; i < ncap; i++) {
        d = cl[i] - 2 * cl[i - 1] + cl[i - 2];
        d = d < 0 ? -d : d;
        mx = d > mx ? d : mx;
    }
    return mx;
}
static int32_t peak(void)
{
    int32_t mx = 0;
    uint32_t i;
    for (i = 0; i < ncap; i++) mx = abs(cl[i]) > mx ? abs(cl[i]) : mx;
    return mx;
}
static const char *const MN[9] = {"", "SVF", "MOOG", "MS20", "DIODE", "SEM", "NOTCH", "BP", "HP"};
static uint32_t sweep(int32_t cut, int32_t lvl)
{
    static const uint32_t engs[3] = {0, ENGI_WAVE, ENGI_VECTOR};
    static const uint8_t ch[3] = {48, 55, 64};
    track_t *t = &trk[0];
    uint32_t e, from, to, i, bad = 0;
    for (e = 0; e < 3u; e++)
        for (from = FM_SVF; from <= FM_LAST; from++)
            for (to = FM_SVF; to <= FM_LAST; to++) {
                int32_t a, b, c, pk, steady_pk;
                if (to == from) continue;
                for (i = 0; i < NPART; i++) trk_all_off(&trk[i]);
                run(600);
                host_preset(t, engs[e], 0);
                t->p[P_DIST] = t->p[P_CHOR] = t->p[P_DLY] = t->p[P_REV] = 0;
                t->p[P_ATK] = 0, t->p[P_SUS] = 127, t->p[P_REL] = 10, t->p[P_VOICE] = V_POLY, t->p[P_GLIDE] = 0;
                t->p[P_SPREAD] = 0, t->p[P_ITYPE] = 0, t->p[P_AMODE] = 0, t->p[P_VINT] = 0;
                t->p[P_FMODEL] = (int16_t)from;
                if (engs[e] == ENGI_VECTOR) t->p[P_E7] = (int16_t)cut; else t->p[P_E4] = (int16_t)cut;
                t->p[P_LEVEL] = (int16_t)lvl;
                for (i = 0; i < 3u; i++) trk_note_on(t, ch[i], 100);
                run(800);                                  /* (the duck of the preset's change, the sound steady) */
                run(200); a = d2max(); { int32_t pa = peak();
                t->p[P_FMODEL] = (int16_t)to;
                run(16); b = d2max(); pk = peak();
                run(200); c = d2max(); pa = pa > peak() ? pa : peak(); steady_pk = pa; }
                a = a > c ? a : c;
                if (b > a * 3 / 2 + 64 || pk > steady_pk * 3 / 2 + 64) {
                    bad++;
                    printf("model_switch: %-6s %-5s -> %-5s: click mark %6d (steady %5d), peak %6d after the change (steady %6d)\n",
                           ENGINES[engs[e]]->name, MN[from], MN[to], b, a, pk, steady_pk);
                }
            }
    printf("model_switch: cutoff %3d level %3d: %u of %u changes click\n", cut, lvl, bad, 3u * (FM_LAST - FM_SVF + 1u) * (FM_LAST - FM_SVF));
    return bad;
}
int main(void)
{
    uint32_t bad = 0;
    host_tracks_init();
    bad += sweep(20, 40);
    bad += sweep(40, 40);
    bad += sweep(20, 100);
    bad += sweep(40, 100);
    printf("model_switch: %s\n", bad ? "FAIL" : "no click on any model change (ANALOG, WAVE, VECTOR)");
    return bad != 0;
}
