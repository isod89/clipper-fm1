/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Projects: four slots in .noinit RAM (the song sections A..D), so they survive resets and UBOOT
 * entry. With FELUCCA_FLASH every save also goes to flash through storage.c, and an empty RAM slot
 * is filled from flash on load. The working project is also kept in flash by itself (autosave, when
 * the transport is stopped and nothing sounds) and comes back at power-on: SLOOP starts where you
 * left it.
 *
 * Formats: 5 ("FUN5", written, SLOOP 2.4): format 4 plus, per track, the nudge of each step (micro), NLOCK
 * parameter locks, the steps' fill conditions (2 bits each) and P_TFLT (one parameter more, just before P_E0).
 * Read and converted: 4 ("FUN4", SLOOP 2.0 .. 2.3: PROJ_NP_V4 parameters, today's G_COUNT, 10-byte
 * steps with levels and ratchets, the drum track's 16 lanes; no nudge, no lock), 3 ("FUN3", SLOOP 1.x: 8-byte
 * steps, the drum track's notes become its lanes, the swings x 0.8 for the MPC scale), 2 ("FUN2") and 1
 * ("FUN1"), which held PROJ_NP_V2 parameters per track, mapped by count as user presets are (the first
 * PROJ_NP_V2 - 8 are P_LEVEL.. in order, the last 8 P_E0..P_E7; the parameters added since take their
 * defaults). Their engine bytes are kept: formats 1 and 2 had engines 0..7 (ANALOG .. WHEEL), and the
 * engines added since were appended, no index moved; the drum track's byte (it has no engine) becomes 0.
 *
 * Built on the host too (tests/project_test.c, -DPROJ_HOST): the part above the #ifndef
 * PROJ_HOST needs core.h, params.c (TP), drums.c (the lanes), the engines and trk_def_engine (ui.c). */
#ifndef UI_SMALL
#define UI_SMALL                                   /* (gfx.c: minsize on the device; a test alone: nothing) */
#endif
#define PROJ_MAGIC 0x46554E35u                 /* "FUN5": format 4 + per-step nudge, parameter locks, fill conditions (SLOOP 2.4) */
#define PROJ_MAGIC_V4 0x46554E34u              /* "FUN4": four tracks, P_COUNT parameters each, 10-byte steps; read only */
#define PROJ_MAGIC_V3 0x46554E33u              /* "FUN3": SLOOP 1.x; read only */
#define PROJ_MAGIC_V2 0x46554E32u              /* "FUN2": four tracks, PROJ_NP_V2 parameters; read only */
#define PROJ_MAGIC_V1 0x46554E31u              /* "FUN1": one instrument; loads into track 1 */
#define PROJ_NP_V4 58u                         /* P_COUNT of format 4 (P_E0 was 50) */
#define PROJ_NP_V3 57u                         /* P_COUNT of format 3 (P_E0 was 49) */
#define PROJ_NG_V3 27u                         /* G_COUNT of formats 1..3 */
#define PROJ_NP_V2 53u                         /* P_COUNT of formats 1 and 2 (P_E0 was 45) */
#define PROJ_NG_V2 27u                         /* G_COUNT of formats 1 and 2 */
#define PROJ_NG 32u                            /* the globals of formats 4 and 5 (G_COUNT until 2.4). SLOOP 2.5's
                                                * G_DRDLY rides in the byte after sel (0 in older projects: off),
                                                * so the format and its size do not change */
_Static_assert(G_DRDLY == PROJ_NG && G_COUNT == PROJ_NG + 1u + (CLIPPER_SYNTH ? 1u : 0u),
               "the globals a project holds, then G_DRDLY (Clipper 1.0: then G_DMODE, with the 1.0 values)");
/* the parameters a track holds: P_COUNT of SLOOP 2.4 / 2.5 (P_E0 was 53). Clipper 0.4 added PROJ_NX before P_E0
 * (LFO 2, ENV 2, the matrix: core.h); the synth never uses tracks 3 and 4, so tracks 1 and 2's are kept in track 3's
 * lock slots: free ones (step LOCK_FREE), which SLOOP (and any firmware) passes over, a byte a value in their param
 * and val bytes (marked CLIPPER_PROJ_MARK: proj_capture, proj_apply). The format and its size do not change: SLOOP
 * reads a Clipper project as before, tracks 3 and 4 as they are. (Clipper 0.4's first build, CLIPPER_PROJ_MARK_04,
 * put them in tracks 3 and 4's p[], where SLOOP took them for their sounds: read too) */
#define PROJ_NP 61u
#define PROJ_E0 53u
#define PROJ_NX (P_E0 - PROJ_E0)
_Static_assert(P_COUNT == PROJ_NP || (CLIPPER_SYNTH && P_COUNT == PROJ_NP + PROJ_NX && P_COUNT < 128 && NTRK == 4),
               "a track's parameters as stored (Clipper: track 3's lock slots hold the rest: PROJ_XM3 below)");
#if CLIPPER_SYNTH
/* tracks 1 and 2's LFO 2, ENV 2 and matrix (0.4: PROJ_XA values each) and INSERT (0.6: PROJ_XI each) <-> bytes in the
 * free lock slots of track 3 (lk): 3 bytes a slot (param, val low, val high), value - its minimum (every range fits a
 * byte). The 0.4 part first, as 0.4 / 0.5 wrote it (2 x PROJ_XA bytes, then CLIPPER_PROJ_MARK), then 0.6's (2 x PROJ_XI
 * bytes, then CLIPPER_PROJ_MARK_06): a project of 0.4 / 0.5 loads its matrix, its INSERT off */
#define PROJ_XA ((uint32_t)(P_ITYPE - PROJ_E0))       /* P_L2RATE .. P_M6AMT */
#define PROJ_XI ((uint32_t)(P_IMIX + 1 - P_ITYPE))     /* P_ITYPE .. P_IMIX */
#define PROJ_XM1 (2u * PROJ_XA)                       /* the byte of CLIPPER_PROJ_MARK */
#define PROJ_XM2 (PROJ_XM1 + 1u + 2u * PROJ_XI)       /* the byte of CLIPPER_PROJ_MARK_06 */
/* 0.8: the MOD SEQ's RATE LEN SMTH START, SPICE, DICE (PROJ_XS) of both, 4 bytes each (proj_xs_word), after 0.6's,
 * then CLIPPER_PROJ_MARK_08; their steps in track 4's free lock slots (two a byte, + 8; then the mark) */
#define PROJ_XS ((uint32_t)(P_VINT - P_MSRATE))
/* 0.9: VINTAGE and LOOP of both (PROJ_X9 each, value - its minimum) in track 4, after the steps' mark, then
 * CLIPPER_PROJ_MARK_09 */
#define PROJ_X9 ((uint32_t)(P_SPREAD - P_VINT))
#define PROJ_X9_AT (PROJ_MS_MARK + 1u)
#define PROJ_X9_MARK (PROJ_X9_AT + 2u * PROJ_X9)
#define CLIPPER_PROJ_MARK_09 0xC9u
#define PROJ_XM3 (PROJ_XM2 + 1u + 8u)                 /* the byte of CLIPPER_PROJ_MARK_08 */
#define CLIPPER_PROJ_MARK_08 0xC8u
#define PROJ_MS_MARK 64u                              /* track 4: the byte after the steps */
_Static_assert(PROJ_XA == 25u && PROJ_XI == 5u && PROJ_XS == 6u && PROJ_X9 == 2u && P_MSRATE == P_IMIX + 1 &&
               PROJ_XM3 < 3u * NLOCK && PROJ_X9_MARK < 3u * NLOCK, "the project's Clipper bytes in tracks 3 and 4's lock slots");
UI_SMALL static uint32_t proj_xs_word(const int16_t *p)        /* RATE (4 bits) START (1) LEN - 1 (4) SMTH (7) SPICE (7) DICE (7) */
{
    return (uint32_t)clamp(p[P_MSRATE], 0, 15) | (uint32_t)clamp(p[P_MSKEY], 0, 1) << 4 |
           (uint32_t)clamp(p[P_MSLEN] - 1, 0, 15) << 5 | (uint32_t)clamp(p[P_MSSMTH], 0, 127) << 9 |
           (uint32_t)clamp(p[P_SPICE], 0, 127) << 16 | (uint32_t)clamp(p[P_DICE], 0, 127) << 23;
}
UI_SMALL static int32_t proj_xs_val(uint32_t w, uint32_t i)   /* value P_MSRATE + i of a word */
{
    switch (i) {
    case 0: return (int32_t)(w & 15u);
    case 1: return (int32_t)((w >> 5) & 15u) + 1;
    case 2: return (int32_t)((w >> 9) & 127u);
    case 3: return (int32_t)((w >> 4) & 1u);
    case 4: return (int32_t)((w >> 16) & 127u);
    default: return (int32_t)((w >> 23) & 127u);
    }
}
_Static_assert(P_MSLEN == P_MSRATE + 1 && P_MSSMTH == P_MSRATE + 2 && P_MSKEY == P_MSRATE + 3 && P_SPICE == P_MSRATE + 4 &&
               P_DICE == P_MSRATE + 5, "proj_xs_val's order");
UI_SMALL static void proj_bytes_put(plock_t *lk, const uint8_t *b)
{
    uint32_t k;
    for (k = 0; k < NLOCK; k++) {
        lk[k].step = LOCK_FREE;
        lk[k].param = b[3u * k];
        lk[k].val = (int16_t)(b[3u * k + 1u] | b[3u * k + 2u] << 8);
    }
}
UI_SMALL static void proj_ms_put(plock_t *lk)                  /* track 4's lock slots: both sounds' steps */
{
    uint8_t b[3u * NLOCK];
    uint32_t j, n;
    memset(b, 0, sizeof b);
    for (j = 0; j < 2u; j++)
        for (n = 0; n < 32u; n++) {
            uint32_t a = 2u * n, c = a + 1u;
            b[32u * j + n] = (uint8_t)((uint32_t)clamp(trk[j].ms[a / MSEQ_STEPS][a % MSEQ_STEPS] + 8, 0, 15) |
                                       (uint32_t)clamp(trk[j].ms[c / MSEQ_STEPS][c % MSEQ_STEPS] + 8, 0, 15) << 4);
        }
    b[PROJ_MS_MARK] = CLIPPER_PROJ_MARK_08;
    for (j = 0; j < 2u; j++)                          /* (0.9) VINTAGE, LOOP */
        for (n = 0; n < PROJ_X9; n++)
            b[PROJ_X9_AT + PROJ_X9 * j + n] = (uint8_t)clamp(trk[j].p[P_VINT + n] - TP[P_VINT + n].min, 0, 255);
    b[PROJ_X9_MARK] = CLIPPER_PROJ_MARK_09;
    proj_bytes_put(lk, b);
}
UI_SMALL static uint32_t proj_ext_at(uint32_t j, uint32_t k)   /* the byte of track j's value k (P_E0 - PROJ_E0 of them) */
{
    return k < PROJ_XA ? j * PROJ_XA + k : PROJ_XM1 + 1u + j * PROJ_XI + (k - PROJ_XA);
}
UI_SMALL static void proj_ext_put(plock_t *lk)
{
    uint8_t b[3u * NLOCK];
    uint32_t j, k;
    memset(b, 0, sizeof b);
    for (j = 0; j < 2u; j++) {
        uint32_t w = proj_xs_word(trk[j].p);
        for (k = 0; k < PROJ_XA + PROJ_XI; k++)
            b[proj_ext_at(j, k)] = (uint8_t)clamp(trk[j].p[PROJ_E0 + k] - TP[PROJ_E0 + k].min, 0, 255);
        for (k = 0; k < 4u; k++)
            b[PROJ_XM2 + 1u + 4u * j + k] = (uint8_t)(w >> (8u * k));
    }
    b[PROJ_XM1] = CLIPPER_PROJ_MARK;
    b[PROJ_XM2] = CLIPPER_PROJ_MARK_06;
    b[PROJ_XM3] = CLIPPER_PROJ_MARK_08;
    proj_bytes_put(lk, b);
}
UI_SMALL static uint32_t proj_ext_byte(const plock_t *lk, uint32_t n)
{
    return n % 3u ? ((uint16_t)lk[n / 3u].val >> (8u * (n % 3u - 1u))) & 255u : lk[n / 3u].param;
}
UI_SMALL static int proj_ext_get(const plock_t *lk, uint32_t j, uint32_t k)   /* track j's value k, or -1 (none) */
{
    uint32_t i;
    for (i = 0; i < NLOCK; i++)
        if (lk[i].step != LOCK_FREE)
            return -1;
    if (proj_ext_byte(lk, PROJ_XM1) != CLIPPER_PROJ_MARK)
        return -1;
    if (k >= PROJ_XA && proj_ext_byte(lk, PROJ_XM2) != CLIPPER_PROJ_MARK_06)
        return -1;                                     /* (0.4 / 0.5: no INSERT) */
    if (k >= PROJ_XA + PROJ_XI) {                      /* (0.8) the MOD SEQ's, SPICE's: none before 0.8 */
        uint32_t w = 0, i;
        if (proj_ext_byte(lk, PROJ_XM3) != CLIPPER_PROJ_MARK_08)
            return -1;
        for (i = 0; i < 4u; i++)
            w |= proj_ext_byte(lk, PROJ_XM2 + 1u + 4u * j + i) << (8u * i);
        return (int)(proj_xs_val(w, k - PROJ_XA - PROJ_XI) - TP[P_MSRATE + k - PROJ_XA - PROJ_XI].min);
    }
    return (int)proj_ext_byte(lk, proj_ext_at(j, k));
}
UI_SMALL static int proj_x9_get(const plock_t *lk, uint32_t j, uint32_t k)   /* (0.9) track j's VINTAGE / LOOP, or -1 */
{
    uint32_t i;
    for (i = 0; i < NLOCK; i++)
        if (lk[i].step != LOCK_FREE)
            return -1;
    if (proj_ext_byte(lk, PROJ_X9_MARK) != CLIPPER_PROJ_MARK_09)
        return -1;
    return (int)proj_ext_byte(lk, PROJ_X9_AT + PROJ_X9 * j + k);
}
UI_SMALL static int proj_ms_get(const plock_t *lk, uint32_t j, track_t *t)   /* track j's steps from track 4's: 0, -1 none */
{
    uint32_t i, n;
    for (i = 0; i < NLOCK; i++)
        if (lk[i].step != LOCK_FREE)
            return -1;
    if (proj_ext_byte(lk, PROJ_MS_MARK) != CLIPPER_PROJ_MARK_08)
        return -1;
    for (n = 0; n < 32u; n++) {
        uint32_t b = proj_ext_byte(lk, 32u * j + n), a = 2u * n, c = a + 1u;
        t->ms[a / MSEQ_STEPS][a % MSEQ_STEPS] = (int8_t)((int32_t)(b & 15u) - 8);
        t->ms[c / MSEQ_STEPS][c % MSEQ_STEPS] = (int8_t)((int32_t)(b >> 4) - 8);
    }
    return 0;
}
#endif
typedef struct {                               /* one track; the drum track ignores engine / preset */
    int16_t p[PROJ_NP];
    uint8_t engine, preset;
    union {
        step_t step[NSTEP];
        dstep_t dstep[NSTEP];                  /* (the drum track: 16 lanes, the same size) */
    };
    int8_t micro[NSTEP];                       /* SLOOP 2.4: each step's nudge (core.h) */
    plock_t lock[NLOCK];                       /* and its parameter locks (step LOCK_FREE = none) */
    uint8_t fill[NSTEP / 4];                   /* and its fill condition, 2 bits a step (FC_*) */
} proj_trk_t;
#if CLIPPER_SYNTH
/* 1.0: SPREAD .. the matrix's slot 12 of both (PROJ_X10 each, value - its minimum) and G_DMODE, in track 3's nudges
 * (micro: SLOOP plays them on track 3's steps; Clipper writes them only while track 3 has none, so they nudge nothing
 * there): byte 0 CLIPPER_PROJ_MARK_10, then track 1's, track 2's, G_DMODE, a check byte. After Clipper 1.2 the same
 * block also goes beside every project (proj_x10_t below), so it is kept when track 3 has steps too */
#define PROJ_X10 ((uint32_t)(P_E0 - P_SPREAD))
#define PROJ_X10_DM (1u + 2u * PROJ_X10)
#define PROJ_X10_CK (PROJ_X10_DM + 1u)
#define CLIPPER_PROJ_MARK_10 0xCAu
_Static_assert(PROJ_X10 == 27u && PROJ_X10_CK < NSTEP, "1.0's values in track 3's nudges");
UI_SMALL static uint32_t proj_x10_ck(const int8_t *m)
{
    uint32_t i, c = 0x5Au;
    for (i = 0; i < PROJ_X10_CK; i++)
        c = ((c << 1) | (c >> 7)) ^ (uint8_t)m[i];
    return c & 255u;
}
UI_SMALL static void proj_x10_fill(int8_t *m)          /* the block of the working sounds into m[0 .. PROJ_X10_CK] */
{
    uint32_t j, n;
    m[0] = (int8_t)CLIPPER_PROJ_MARK_10;
    for (j = 0; j < 2u; j++)
        for (n = 0; n < PROJ_X10; n++)
            m[1u + PROJ_X10 * j + n] = (int8_t)(uint8_t)clamp(trk[j].p[P_SPREAD + n] - TP[P_SPREAD + n].min, 0, 255);
    m[PROJ_X10_DM] = (int8_t)(uint8_t)clamp(song.g[G_DMODE], GP[G_DMODE].min, GP[G_DMODE].max);
    m[PROJ_X10_CK] = (int8_t)proj_x10_ck(m);
}
UI_SMALL static void proj_x10_put(proj_trk_t *d, const step_t *st)   /* (track 3's own steps: none, or nothing put) */
{
    uint32_t n;
    for (n = 0; n < NSTEP; n++)
        if (st[n].n)
            return;
    memset(d->micro, 0, NSTEP);
    proj_x10_fill(d->micro);
}
UI_SMALL static int proj_x10_ok(const int8_t *m)
{
    return (uint8_t)m[0] == CLIPPER_PROJ_MARK_10 && (uint8_t)m[PROJ_X10_CK] == proj_x10_ck(m);
}
#endif
typedef struct {
    uint32_t magic, size;
    int16_t g[PROJ_NG];
    uint8_t sel, drdly, rsv[2];                /* the selected track; 2.5: G_DRDLY */
    proj_trk_t t[NTRK];
    uint32_t sum;
} project_t;
typedef struct {                               /* a track of format 4 (SLOOP 2.0 .. 2.3), read only */
    int16_t p[PROJ_NP_V4];
    uint8_t engine, preset;
    union {
        step_t step[NSTEP];
        dstep_t dstep[NSTEP];
    };
} proj_trk_v4_t;
typedef struct {                               /* format 4, read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG];
    uint8_t sel, rsv[3];
    proj_trk_v4_t t[NTRK];
    uint32_t sum;
} project_v4_t;
typedef struct { uint8_t note[4], n, time, flags, vel; } step8_t;   /* the steps of formats 1..3 */
typedef struct {                               /* a track of format 3, read only */
    int16_t p[PROJ_NP_V3];
    uint8_t engine, preset;
    step8_t step[NSTEP];
} proj_trk_v3_t;
typedef struct {                               /* format 3 (SLOOP 1.x), read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG_V3];
    uint8_t sel, rsv[3];
    proj_trk_v3_t t[NTRK];
    uint32_t sum;
} project_v3_t;
typedef struct {                               /* a track of formats 1 and 2, read only */
    int16_t p[PROJ_NP_V2];
    uint8_t engine, preset;
    step8_t step[NSTEP];
} proj_trk_v2_t;
typedef struct {                               /* format 2 (until 0.9), read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG_V2];
    uint8_t sel, rsv[3];
    proj_trk_v2_t t[NTRK];
    uint32_t sum;
} project_v2_t;
typedef struct {                               /* format 1 (until 0.5 beta), read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG_V2];
    proj_trk_v2_t t;
    uint32_t sum;
} project_v1_t;
/* format 6 ("FUN6", SLOOP 2.6), read only: format 5 with SLOOP 2.6's ten parameters (P_ITYPE .. P_LPOL, its INSERT,
 * ANALOG filter TYPE, ENV SYNC, LFO 2 SYNC / TRIG / POL: none of them Clipper's) in a byte each before P_E0, and each
 * step's nudge and fill condition in one byte. Read so a project of SLOOP 2.6 on this FM-1 loads here (and is not shown
 * as an empty slot to be saved over): its own ten values are left out */
#define PROJ_MAGIC_V6 0x46554E36u
#define PROJ_NX_V6 10u
typedef struct {
    int16_t p[PROJ_E0];                        /* P_LEVEL .. P_VLEAD */
    int16_t e[8];                              /* P_E0 .. P_E7 */
    uint8_t x[PROJ_NX_V6];                     /* SLOOP 2.6's (P_ITYPE .. P_LPOL there: ids 53..62, P_E0 63) */
    uint8_t engine, preset;
    union {
        step_t step[NSTEP];
        dstep_t dstep[NSTEP];
    };
    uint8_t mf[NSTEP];                         /* each step's nudge (6 bits, signed) | its fill condition << 6 */
    plock_t lock[NLOCK];
} proj_trk_v6_t;
typedef struct {
    uint32_t magic, size;
    int16_t g[PROJ_NG];
    uint8_t sel, drdly, rsv[2];
    proj_trk_v6_t t[NTRK];
    uint32_t sum;
} project_v6_t;
_Static_assert(sizeof(project_v6_t) == 3816u, "format 6 as SLOOP 2.6 stores it");
_Static_assert(sizeof(project_v2_t) == 2552u && sizeof(project_v1_t) == 688u && sizeof(project_v3_t) == 2584u &&
               sizeof(project_v4_t) == 3112u, "formats 1 / 2 / 3 / 4 as they were stored");
_Static_assert(sizeof(project_t) <= 3840u, "format 5 fits one flash object (storage.c ST_PAYLOAD_MAX)");
project_t proj_slot[4] __attribute__((section(".noinit")));

UI_SMALL static uint32_t proj_hash(const void *p, uint32_t n)   /* FNV-1a over n bytes */
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i, s = 0x811C9DC5u;
    for (i = 0; i < n; i++)
        s = (s ^ b[i]) * 16777619u;
    return s;
}
UI_SMALL static uint32_t proj_sum(const project_t *p) { return proj_hash(p, sizeof *p - 4u); }
UI_SMALL static int proj_ok(const project_t *q) { return q->magic == PROJ_MAGIC && q->size == sizeof *q && q->sum == proj_sum(q); }

/* ---- old formats -> format 5 */
/* an old step into a synth step (no level, no ratchet) */
UI_SMALL static void step_from8(step_t *d, const step8_t *s)
{
    memcpy(d->note, s->note, 4);
    d->n = s->n;
    d->time = s->time;
    d->flags = s->flags;
    d->vel = s->vel;
    d->lvl = d->rat = 0;
}
/* an old drum step (GM notes) into the drum track's lanes; its velocity / accent -> their level */
UI_SMALL static void dstep_from8(dstep_t *d, const step8_t *s)
{
    uint32_t i, lvl = (s->flags & SF_ACCENT) || s->vel > 115u ? LV_HARD : !s->vel ? LV_NORM : vel_lvl(s->vel);
    memset(d, 0, sizeof *d);
    if (s->time != ST_NOTE)
        return;
    for (i = 0; i < s->n && i < 4u; i++)
        dstep_set(d, lane_of_note(s->note[i] & 127u), lvl, 0);
}
UI_SMALL static int16_t swing_from_v3(int32_t v) { return (int16_t)clamp((v * 4 + 2) / 5, 0, 100); }   /* /250 -> /200 */

/* the globals of formats 1..3 (G_* unchanged since; any added later: their defaults) */
UI_SMALL static void proj_g_from_old(int16_t *g, const int16_t *g2)
{
    uint32_t i;
    for (i = 0; i < PROJ_NG; i++)
        g[i] = i < PROJ_NG_V3 ? g2[i] : GP[i].def;
    g[G_SWING] = swing_from_v3(g[G_SWING]);
}

/* no nudge, no lock, no condition (formats 1..4) */
UI_SMALL static void proj_trk_plain(proj_trk_t *d)
{
    uint32_t k;
    memset(d->micro, 0, sizeof d->micro);
    memset(d->fill, 0, sizeof d->fill);
    for (k = 0; k < NLOCK; k++) {
        d->lock[k].step = LOCK_FREE;
        d->lock[k].param = 0;
        d->lock[k].val = 0;
    }
}

/* a track of format 3 -> today's (by id up to P_SLDEPTH; P_E0.. moved) */
UI_SMALL static void proj_trk_from_v3(proj_trk_t *d, const proj_trk_v3_t *s, int drum)
{
    uint32_t k, nc = PROJ_NP_V3 - 8u;
    proj_trk_plain(d);
    for (k = 0; k < PROJ_E0; k++)
        d->p[k] = k < nc ? s->p[k] : TP[k].def;
    for (k = 0; k < 8u; k++)
        d->p[PROJ_E0 + k] = s->p[nc + k];
    d->p[P_SSWING] = swing_from_v3(d->p[P_SSWING]);
    d->p[P_ASWING] = swing_from_v3(d->p[P_ASWING]);
    d->engine = drum ? 0u : s->engine;
    d->preset = drum ? 0u : s->preset;
    for (k = 0; k < NSTEP; k++) {
        if (drum)
            dstep_from8(&d->dstep[k], &s->step[k]);
        else
            step_from8(&d->step[k], &s->step[k]);
    }
}

/* a track of formats 1 and 2 -> format 3 (mapped by count, see the top) */
UI_SMALL static void proj_trk_v2_to_v3(proj_trk_v3_t *d, const proj_trk_v2_t *s, int drum)
{
    uint32_t k, nc = PROJ_NP_V2 - 8u;
    for (k = 0; k < PROJ_NP_V3 - 8u; k++)
        d->p[k] = k < nc ? s->p[k] : TP[k].def;
    for (k = 0; k < 8u; k++)
        d->p[PROJ_NP_V3 - 8u + k] = s->p[nc + k];
    d->engine = drum ? 0u : s->engine;          /* (indices 0..7 as they were) */
    d->preset = drum ? 0u : s->preset;
    memcpy(d->step, s->step, sizeof d->step);
}

/* a format 4 project (n bytes in *v4) -> slot q as format 5: the same, no nudge, no lock, no condition, no FILTER */
UI_SMALL static int proj_from_v4(project_t *q, const project_v4_t *v4, int n)
{
    uint32_t i;
    if (n != (int)sizeof *v4 || v4->magic != PROJ_MAGIC_V4 || v4->size != sizeof *v4 ||
        v4->sum != proj_hash(v4, sizeof *v4 - 4u))
        return 0;
    memset(q, 0, sizeof *q);
    q->magic = PROJ_MAGIC;
    q->size = sizeof *q;
    memcpy(q->g, v4->g, sizeof q->g);
    q->sel = v4->sel;
    for (i = 0; i < NTRK; i++) {
        uint32_t k;
        for (k = 0; k < PROJ_E0; k++)                  /* by id up to P_CHORD, the parameters added since: */
            q->t[i].p[k] = k < PROJ_NP_V4 - 8u ? v4->t[i].p[k] : TP[k].def;   /* their defaults; P_E0.. moved */
        for (k = 0; k < 8u; k++)
            q->t[i].p[PROJ_E0 + k] = v4->t[i].p[PROJ_NP_V4 - 8u + k];
        q->t[i].engine = v4->t[i].engine;
        q->t[i].preset = v4->t[i].preset;
        memcpy(q->t[i].step, v4->t[i].step, sizeof q->t[i].step);
        proj_trk_plain(&q->t[i]);
    }
    q->sum = proj_sum(q);
    return 1;
}

/* a format 6 project (SLOOP 2.6; n bytes in *v6) -> slot q as format 5 (no Clipper mark: proj_apply sets what it held
 * as neutral); a lock on one of 2.6's own ten parameters is dropped, one on P_E0.. moves with it */
/* Clipper 0.6: a SLOOP 2.6 project read (proj_from_v6): its track 1's INSERT, kept beside the project_t it became (that
 * has no room for it: SLOOP's tracks 3 and 4 are its own), per slot (0..3: proj_slot, 4: any other), for proj_apply */
static int16_t proj_v6_ins[5][5];
static uint8_t proj_v6_ins_ok[5];
UI_SMALL static uint32_t proj_v6_at(const project_t *q)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (q == &proj_slot[i])
            return i;
    return 4u;
}
UI_SMALL static int proj_from_v6(project_t *q, const project_v6_t *v6, int n)
{
    uint32_t i, k;
    if (n != (int)sizeof *v6 || v6->magic != PROJ_MAGIC_V6 || v6->size != sizeof *v6 ||
        v6->sum != proj_hash(v6, sizeof *v6 - 4u))
        return 0;
    for (k = 0; k < 5u; k++)                           /* (SLOOP 2.6: x[0..4] = P_ITYPE .. P_IMIX there) */
        proj_v6_ins[proj_v6_at(q)][k] = (int16_t)v6->t[0].x[k];
    proj_v6_ins_ok[proj_v6_at(q)] = 1;
    memset(q, 0, sizeof *q);
    q->magic = PROJ_MAGIC;
    q->size = sizeof *q;
    memcpy(q->g, v6->g, sizeof q->g);
    q->sel = v6->sel;
    q->drdly = v6->drdly;
    for (i = 0; i < NTRK; i++) {
        const proj_trk_v6_t *s = &v6->t[i];
        proj_trk_t *d = &q->t[i];
        for (k = 0; k < PROJ_E0; k++)
            d->p[k] = s->p[k];
        for (k = 0; k < 8u; k++)
            d->p[PROJ_E0 + k] = s->e[k];
        d->engine = s->engine;
        d->preset = s->preset;
        memcpy(d->step, s->step, sizeof d->step);
        memset(d->fill, 0, sizeof d->fill);
        for (k = 0; k < NSTEP; k++) {
            int32_t m = s->mf[k] & 63;
            d->micro[k] = (int8_t)(m >= 32 ? m - 64 : m);
            d->fill[k >> 2] |= (uint8_t)(((uint32_t)s->mf[k] >> 6) << (2u * (k & 3u)));
        }
        for (k = 0; k < NLOCK; k++) {
            uint32_t id = s->lock[k].param;
            d->lock[k] = s->lock[k];
            if (s->lock[k].step == LOCK_FREE || (id >= PROJ_E0 && id < PROJ_E0 + PROJ_NX_V6) || id >= PROJ_E0 + PROJ_NX_V6 + 8u) {
                d->lock[k].step = LOCK_FREE;
                d->lock[k].param = 0;
                d->lock[k].val = 0;
            } else if (id >= PROJ_E0 + PROJ_NX_V6) {
                d->lock[k].param = (uint8_t)(id - PROJ_NX_V6);   /* P_E0.. as stored here */
            }
        }
    }
    q->sum = proj_sum(q);
    return 1;
}

/* a format 3 project -> slot q as format 5 */
UI_SMALL static void proj_from_v3_ok(project_t *q, const project_v3_t *v3)
{
    uint32_t i;
    memset(q, 0, sizeof *q);
    q->magic = PROJ_MAGIC;
    q->size = sizeof *q;
    proj_g_from_old(q->g, v3->g);
    q->sel = v3->sel;
    for (i = 0; i < NTRK; i++)
        proj_trk_from_v3(&q->t[i], &v3->t[i], i == TRK_DRUM);
    q->sum = proj_sum(q);
}
UI_SMALL static int proj_from_v3(project_t *q, const project_v3_t *v3, int n)
{
    if (n != (int)sizeof *v3 || v3->magic != PROJ_MAGIC_V3 || v3->size != sizeof *v3 ||
        v3->sum != proj_hash(v3, sizeof *v3 - 4u))
        return 0;
    proj_from_v3_ok(q, v3);
    return 1;
}

static project_v3_t proj_v3_tmp;               /* (formats 1, 2: through format 3) */
/* a format 2 project (n bytes in *v2) -> slot q as format 5 */
UI_SMALL static int proj_from_v2(project_t *q, const project_v2_t *v2, int n)
{
    project_v3_t *v3 = &proj_v3_tmp;
    uint32_t i;
    if (n != (int)sizeof *v2 || v2->magic != PROJ_MAGIC_V2 || v2->size != sizeof *v2 ||
        v2->sum != proj_hash(v2, sizeof *v2 - 4u))
        return 0;
    memset(v3, 0, sizeof *v3);
    memcpy(v3->g, v2->g, sizeof v3->g);
    v3->sel = v2->sel;
    for (i = 0; i < NTRK; i++)
        proj_trk_v2_to_v3(&v3->t[i], &v2->t[i], i == TRK_DRUM);
    proj_from_v3_ok(q, v3);
    return 1;
}

/* a format 1 project (n bytes in *v1) -> slot q as format 5: the instrument becomes track 1,
 * tracks 2..4 start empty (their sounds as at power-on) */
UI_SMALL static int proj_from_v1(project_t *q, const project_v1_t *v1, int n)
{
    project_v3_t *v3 = &proj_v3_tmp;
    uint32_t i;
    if (n != (int)sizeof *v1 || v1->magic != PROJ_MAGIC_V1 || v1->size != sizeof *v1 ||
        v1->sum != proj_hash(v1, sizeof *v1 - 4u))
        return 0;
    memset(v3, 0, sizeof *v3);
    memcpy(v3->g, v1->g, sizeof v3->g);
    proj_trk_v2_to_v3(&v3->t[0], &v1->t, 0);
    proj_from_v3_ok(q, v3);
    for (i = 1; i < NTRK; i++) {               /* the other tracks: their defaults, no steps */
        uint32_t k;
        for (k = 0; k < PROJ_NP; k++)
            q->t[i].p[k] = k >= PROJ_E0 ? ENGINES[trk_def_engine(i)]->edit[k - PROJ_E0].def : TP[k].def;
        q->t[i].engine = (uint8_t)trk_def_engine(i);
        q->t[i].preset = 0xFF;                 /* 0xFF: its default preset (project_load) */
        memset(q->t[i].step, 0, sizeof q->t[i].step);
        if (i != TRK_DRUM)
            for (k = 0; k < NSTEP; k++)
                q->t[i].step[k].time = ST_REST;
        proj_trk_plain(&q->t[i]);
    }
    q->sum = proj_sum(q);
    return 1;
}

/* n bytes of a stored project (any format) -> slot q as format 5; 0 = not a project */
UI_SMALL static int proj_import(project_t *q, const void *b, int n)
{
    proj_v6_ins_ok[proj_v6_at(q)] = 0;                 /* (proj_from_v6 sets it again) */
    if (n == (int)sizeof *q && proj_ok((const project_t *)b)) {
        memcpy(q, b, sizeof *q);
        return 1;
    }
    return proj_from_v6(q, (const project_v6_t *)b, n) || proj_from_v4(q, (const project_v4_t *)b, n) ||
           proj_from_v3(q, (const project_v3_t *)b, n) ||
           proj_from_v2(q, (const project_v2_t *)b, n) || proj_from_v1(q, (const project_v1_t *)b, n);
}

#if CLIPPER_SYNTH
/* (after Clipper 1.2) the 1.0 block of every project, out of the project: with steps on track 3 (a SLOOP project loaded)
 * its nudges are its own, and the block had nowhere to go (its values came back at their defaults). Kept beside each
 * project in RAM (proj_x10s, bound to its project by the project's sum), in flash in the project object's side record
 * (storage.c st_side: SLOOP never reads a project's, its own save drops it), and in the editor's backup as object 14
 * (editor.c). Track 3's nudges still hold it when it has no steps, as 1.0 .. 1.2 wrote it (they read it only there) */
#define PROJ_X10_MAGIC 0x30315850u                   /* "PX10" */
typedef struct {
    uint32_t magic, sum;                             /* the project it belongs to: its sum */
    int8_t m[PROJ_X10_CK + 1u];                      /* as in track 3's nudges */
    uint8_t rsv[3];
    uint32_t chk;                                    /* proj_hash of the bytes before */
} proj_x10_t;
#define PROJ_X10_N 7u                                /* 0..3 the slots, 4 any other (the backup's snapshot), 5 the
                                                      * autosave, 6 the loop song mode put aside (proj_x10_at) */
_Static_assert(sizeof(proj_x10_t) == 72u, "the 1.0 block beside a project");
/* (in .pool, zeroed at boot: .noinit, which keeps proj_slot over a warm reset, has not 504 B left. So a live section
 * stored while playing and not yet written to flash comes back from a warm reset without its block: with steps on
 * track 3, its 1.0 values at their defaults) */
static proj_x10_t proj_x10s[PROJ_X10_N] __attribute__((section(".pool")));
static uint32_t proj_x10_at(const project_t *p);
UI_SMALL static int proj_x10_valid(const proj_x10_t *x)
{
    return x->magic == PROJ_X10_MAGIC && x->chk == proj_hash(x, sizeof *x - 4u) && proj_x10_ok(x->m);
}
UI_SMALL static void proj_x10_seal(proj_x10_t *x, uint32_t sum)
{
    x->magic = PROJ_X10_MAGIC;
    x->sum = sum;
    x->rsv[0] = x->rsv[1] = x->rsv[2] = 0;
    x->chk = proj_hash(x, sizeof *x - 4u);
}
UI_SMALL static const int8_t *proj_x10_of(const project_t *p)   /* p's 1.0 block: in track 3's nudges, else beside it, else 0 */
{
    const proj_x10_t *x = &proj_x10s[proj_x10_at(p)];
    if (p->rsv[0] != CLIPPER_PROJ_MARK)
        return 0;
    if (proj_x10_ok(p->t[2].micro))
        return p->t[2].micro;
    return proj_x10_valid(x) && x->sum == p->sum ? x->m : 0;
}
#ifdef PROJ_HOST
static uint32_t proj_x10_at(const project_t *p)      /* (host tests: the slots, else 4) */
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (p == &proj_slot[i])
            return i;
    return 4u;
}
#endif
#endif

/* ---- the working project <-> a project_t */
UI_SMALL static void proj_capture(project_t *p)        /* what is playing now, as a project */
{
    uint32_t i;
    memset(p, 0, sizeof *p);
    p->magic = PROJ_MAGIC;
    p->size = sizeof *p;
    for (i = 0; i < PROJ_NG; i++)
        p->g[i] = song.g[i];
    p->sel = song.sel;
    p->drdly = (uint8_t)song.g[G_DRDLY];
    p->rsv[0] = CLIPPER_SYNTH ? CLIPPER_PROJ_MARK : 0u;   /* (Clipper 0.3: its bus, layer and modulation are set) */
    for (i = 0; i < NTRK; i++) {
        uint32_t k;
        for (k = 0; k < PROJ_E0; k++)
            p->t[i].p[k] = trk[i].p[k];
        for (k = 0; k < 8u; k++)
            p->t[i].p[PROJ_E0 + k] = trk[i].p[P_E0 + k];
        memcpy(p->t[i].lock, trk[i].lock, sizeof trk[i].lock);
#if CLIPPER_SYNTH
        for (k = 0; k < NLOCK; k++)                    /* (the locks' EDIT ids as stored) */
            if (trk[i].lock[k].param >= P_E0)
                p->t[i].lock[k].param = (uint8_t)(trk[i].lock[k].param - PROJ_NX);
        if (i == 2u)                                   /* (Clipper 0.4: tracks 1 and 2's LFO 2, ENV 2, matrix) */
            proj_ext_put(p->t[2].lock);
        if (i == 3u)                                   /* (0.8: their MOD SEQ's steps) */
            proj_ms_put(p->t[3].lock);
#endif
        p->t[i].engine = trk[i].eng_req;
        p->t[i].preset = trk[i].preset;
        memcpy(p->t[i].step, trk[i].step, sizeof trk[i].step);
        memcpy(p->t[i].micro, trk[i].micro, sizeof trk[i].micro);
        memcpy(p->t[i].fill, trk[i].fill, sizeof trk[i].fill);
#if CLIPPER_SYNTH
        if (i == 2u)                                   /* (1.0: tracks 1 and 2's SPREAD .. slot 12, G_DMODE) */
            proj_x10_put(&p->t[2], trk[2].step);
#endif
    }
    p->sum = proj_sum(p);
#if CLIPPER_SYNTH
    {   /* (after 1.2) and beside it, whatever track 3 holds */
        proj_x10_t *x = &proj_x10s[proj_x10_at(p)];
        proj_x10_fill(x->m);
        proj_x10_seal(x, p->sum);
    }
#endif
}

/* a project's tracks (and its globals, all: a load; or only the drum level / reverb / delay: a song
 * section) into the working one, every value back inside its range. The audio ISR must not run
 * meanwhile (the song sections: called from it; a load: IRQ off) */
UI_SMALL static void proj_apply(const project_t *p, int all)
{
    uint32_t i, k;
#if CLIPPER_SYNTH
    const int8_t *xm = proj_x10_of(p);                  /* (1.0's values: track 3's nudges, or beside the project) */
#endif
    for (i = 0; i < PROJ_NG; i++)
        if (all ? i != G_SLOT && i != G_LOAD && i != G_SAVE && i != G_SYNC && i != G_MIDI && i != G_ROUTE :
                  !CLIPPER_SYNTH && (i == G_DRLVL || i == G_DRREV))   /* (Clipper: the master EQ, not a section's) */
            song.g[i] = (int16_t)clamp(p->g[i], GP[i].min, GP[i].max);
    if (all || !CLIPPER_SYNTH)
        song.g[G_DRDLY] = (int16_t)clamp(p->drdly, GP[G_DRDLY].min, GP[G_DRDLY].max);   /* (a load and a section, as REV) */
    for (k = 0; k < NTRK; k++) {
        track_t *t = &trk[k];
        const proj_trk_t *s = &p->t[k];
        uint32_t e = k < NPART ? s->engine % NENGINES : 0u;
        t->eng_req = (uint8_t)e;
        t->user = 0;                                    /* (no user preset slot is saved) */
        t->lk_n = 0;                                    /* (the locks in force: the values come from the project) */
        for (i = 0; i < P_COUNT; i++) {                 /* every value back inside its range */
            const param_desc_t *d = k == TRK_DRUM && i == P_E0 ? &DRUM_KIT_DESC :   /* the drum kit */
                                    i >= P_E0 && i <= P_E7 ? &ENGINES[e]->edit[i - P_E0] : &TP[i];
            int32_t v = i < PROJ_E0 ? s->p[i] : i >= P_E0 ? s->p[PROJ_E0 + i - P_E0] : d->def;
#if CLIPPER_SYNTH
            if (i >= P_SPREAD && i < P_E0 && k < 2u) {    /* (1.0: in track 3's nudges, or beside the project) */
                v = xm ? (uint8_t)xm[1u + PROJ_X10 * k + (i - P_SPREAD)] + d->min : d->def;
            } else if (i >= P_VINT && i < P_E0 && k < 2u && p->rsv[0] == CLIPPER_PROJ_MARK) {
                int x = proj_x9_get(p->t[3].lock, k, i - P_VINT);   /* (0.9: in track 4) */
                v = x < 0 ? d->def : x + d->min;
            } else if (i >= PROJ_E0 && i < P_E0 && k < 2u && p->rsv[0] == CLIPPER_PROJ_MARK) {
                int x = proj_ext_get(p->t[2].lock, k, i - PROJ_E0);   /* Clipper: tracks 1 and 2's, in track 3 */
                v = x < 0 ? d->def : x + d->min;
            } else if (i >= PROJ_E0 && i < P_ITYPE && k < 2u && p->rsv[0] == CLIPPER_PROJ_MARK_04) {
                v = p->t[k + 2u].p[i - PROJ_E0];        /* (0.4's first build: in tracks 3 and 4's p[]) */
            } else if (i >= P_ITYPE && i <= P_IMIX && k == 0u && p->rsv[0] != CLIPPER_PROJ_MARK && proj_v6_ins_ok[proj_v6_at(p)]) {
                v = proj_v6_ins[proj_v6_at(p)][i - P_ITYPE];   /* (a SLOOP 2.6 project: its track 1's INSERT) */
            } else if (i < PROJ_XA + PROJ_XI && k >= 2u && p->rsv[0] == CLIPPER_PROJ_MARK_04) {
                v = d->def;                             /* (there, theirs were not stored) */
            }
#endif
            t->p[i] = (int16_t)clamp(v, d->min, d->max);
        }
        t->preset = (uint8_t)(ENGINES[e]->npresets ? (s->preset == 0xFFu ? 0u : s->preset) % ENGINES[e]->npresets : 0u);
        arp_snd[k % NTRK] = (uint8_t)t->p[P_AMODE];     /* (1.0) a loaded project's arp is its sounds': the next sound
                                                         * loaded turns it off (seq.c arp_snd) */
        memcpy(t->step, s->step, sizeof t->step);
        for (i = 0; i < NSTEP; i++)                     /* the nudges and locks, each inside its range */
            t->micro[i] = (int8_t)clamp(s->micro[i], MICRO_MIN, MICRO_MAX);
#if CLIPPER_SYNTH
        if (k == 2u && p->rsv[0] == CLIPPER_PROJ_MARK && proj_x10_ok(s->micro))
            memset(t->micro, 0, sizeof t->micro);       /* (1.0's values, not nudges) */
        if (k == 2u && all)
            song.g[G_DMODE] = xm ? (int16_t)clamp((uint8_t)xm[PROJ_X10_DM], GP[G_DMODE].min, GP[G_DMODE].max) : GP[G_DMODE].def;
#endif
        for (i = 0; i < NSTEP / 4u; i++) {              /* the conditions: 3 means normal (0) */
            uint32_t b = s->fill[i], j;
            for (j = 0; j < 4u; j++)
                if (((b >> (2u * j)) & 3u) == 3u)
                    b &= ~(3u << (2u * j));
            t->fill[i] = (uint8_t)b;
        }
        for (i = 0; i < NLOCK; i++) {
            const plock_t *l = &s->lock[i];
            uint32_t id = l->param >= PROJ_E0 ? l->param - PROJ_E0 + P_E0 : l->param;   /* (EDIT ids as stored) */
            if (l->step < NSTEP && id < P_COUNT && p_lockable(id)) {
                const param_desc_t *d = k == TRK_DRUM && id == P_E0 ? &DRUM_KIT_DESC :
                                        id >= P_E0 && id <= P_E7 ? &ENGINES[e]->edit[id - P_E0] : &TP[id];
                t->lock[i].step = l->step;
                t->lock[i].param = (uint8_t)id;
                t->lock[i].val = (int16_t)clamp(l->val, d->min, d->max);
            } else {
                t->lock[i].step = LOCK_FREE;            /* no such step or parameter: the slot is free */
                t->lock[i].param = 0;
                t->lock[i].val = 0;
            }
        }
        if (k != TRK_DRUM)
            for (i = 0; i < NSTEP; i++) {
                step_t *st = &t->step[i];
                uint32_t j;
                if (st->n > 4u)
                    st->n = 4;
                if (st->time > ST_REST)
                    st->time = ST_REST;
                st->vel &= 127u;                    /* (after Clipper 1.2: past 127 from a file, up to twice as loud) */
                for (j = 0; j < 4u; j++)
                    st->note[j] &= 127u;
            }
#if CLIPPER_SYNTH
        if (k < 2u && (p->rsv[0] != CLIPPER_PROJ_MARK || proj_ms_get(p->t[3].lock, k, t)))
            mseq_init(t);                               /* (0.8) its MOD SEQ, or the shapes to start from */
#endif
        fm6_track_loaded(t);                            /* FM6: the project keeps PTCH, not the patch: its slot's */
    }
#if CLIPPER_SYNTH
    if (all && p->rsv[0] != CLIPPER_PROJ_MARK && p->rsv[0] != CLIPPER_PROJ_MARK_04 && p->rsv[0] != CLIPPER_PROJ_MARK_03)   /* a SLOOP or Clipper 0.1 / 0.2 project: what its drum */
        clipper_proj_neutral();                         /* globals and slicer slots held means nothing here */
#endif
}

#ifndef PROJ_HOST
#if FELUCCA_ARRANGER
#include "arranger_scene.c"
#endif
static uint8_t sec_dirty, song_dirty;           /* live sections / the song: in RAM, not yet in flash */
static int proj_st_save(uint32_t obj, const project_t *p);
static void proj_x10_load(uint32_t obj, const project_t *q);
#if FELUCCA_FLASH
/* slot from flash into RAM (format 5, or an old one converted) */
static union {
    project_t v5;
    project_v6_t v6;
    project_v4_t v4;
    project_v3_t v3;
    project_v2_t v2;
    project_v1_t v1;
} proj_tmp;
UI_SMALL static void proj_fetch(uint32_t slot)
{
    project_t *q = &proj_slot[slot & 3u];
    int n = st_load(OBJ_PROJECT0 + (slot & 3u), &proj_tmp, sizeof proj_tmp);
    if (!proj_import(q, &proj_tmp, n))
        q->magic = 0;
    else
        proj_x10_load(OBJ_PROJECT0 + (slot & 3u), q);   /* (after Clipper 1.2) its 1.0 block */
}
#include "fm6_bank.c"                           /* the FM6 patch bank (eng_fm6.c PTCH B1..B27): staged in proj_tmp */
#endif

UI_SMALL static void project_save(uint32_t slot)
{
    project_t *p = &proj_slot[slot & 3u];
#if FELUCCA_ARRANGER
    if (song.playing || transport_req) { ui_message("STOP BEFORE SAVE"); return; }
#endif
    proj_capture(p);
#if FELUCCA_FLASH
    if (flash_ok) {
        ui_message(proj_st_save(OBJ_PROJECT0 + (slot & 3u), p) ? "SAVE ERROR" : "SAVED");
        return;
    }
#endif
    ui_message("SAVED (RAM)");
}

/* a project into the working one: the transport stops, everything sounding is released */
UI_SMALL static void project_apply(const project_t *p)
{
    uint32_t k;
    transport_req = 2;
    panic_req = (1u << NTRK) - 1u;
    fm1_irq_off();                                      /* the audio ISR must not see half a project */
    proj_apply(p, 1);
    part_ctl_reset();                                   /* (the wheels back to the middle, the pedal up) */
    song.sel = (uint8_t)(p->sel < NTRK && !CLIPPER_SYNTH ? p->sel : 0u);
    fm1_irq_on();
    for (k = 0; k < NPART; k++)                         /* a format 1 project: the default sounds of tracks 2, 3 */
        if (p->t[k].preset == 0xFFu) {
            apply_preset_to(&trk[k], TRK_DEF[k][1]);
            steps_clear(&trk[k]);
        }
    sync_reload = 1;
    ui.force = 1;
}

UI_SMALL static void project_load(uint32_t slot)
{
    project_t *p = &proj_slot[slot & 3u];
#if FELUCCA_ARRANGER
    if (song.playing || transport_req) { ui_message("STOP BEFORE LOAD"); return; }
#endif
#if FELUCCA_FLASH
    if (flash_ok && !proj_ok(p))
        proj_fetch(slot);
#endif
    if (!proj_ok(p)) {
        ui_message("EMPTY SLOT");
        return;
    }
    project_apply(p);
    ui_message("LOADED");
}

/* ---- the working project, kept in flash by itself: saved when it changed, the transport is stopped,
 * nothing sounds and the panel was not touched for AUTOSAVE_IDLE (a flash erase stops the audio for
 * ~50 ms: never while something plays); loaded at power-on (autosave_resume) */
#define AUTOSAVE_IDLE 2500u                    /* ms without input */
#define AUTOSAVE_GAP 20000u                    /* ms between two saves at least */
static project_t autosave_buf __attribute__((section(".pool")));
static uint32_t autosave_hash, autosave_ms, autosave_checked;
#if CLIPPER_SYNTH
_Static_assert(sizeof(proj_x10_t) <= ST_SIDE_MAX, "the 1.0 block in a project object's side record");
static uint32_t proj_x10_at(const project_t *p)      /* the 1.0 block beside project p (PROJ_X10_N) */
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (p == &proj_slot[i])
            return i;
    return p == &autosave_buf ? 5u : p == &song_keep ? 6u : 4u;
}
#endif
/* what the autosave compares: the project's sum and (after Clipper 1.2) its 1.0 block's, which is not in the project
 * while track 3 has steps (a change of SPREAD then changes no byte of the project) */
UI_SMALL static uint32_t proj_sig(const project_t *p)
{
#if CLIPPER_SYNTH
    const proj_x10_t *x = &proj_x10s[proj_x10_at(p)];
    if (proj_x10_valid(x) && x->sum == p->sum)
        return p->sum ^ x->chk;
#endif
    return p->sum;
}
/* a project into flash (OBJ_PROJECT0 + k, OBJ_AUTOSAVE): (after Clipper 1.2) its 1.0 block in the side record */
UI_SMALL static int proj_st_save(uint32_t obj, const project_t *p)
{
    int rc;
#if CLIPPER_SYNTH
    const proj_x10_t *x = &proj_x10s[proj_x10_at(p)];
    st_side = proj_x10_valid(x) && x->sum == p->sum ? x : 0;
    st_side_len = sizeof *x;
#endif
    rc = st_save(obj, p, sizeof *p);
    st_side = 0;
    return rc;
}
#if CLIPPER_SYNTH
static uint32_t proj_restored_sum;                  /* the working project a backup restored last (object 0): its sum */
UI_SMALL static void proj_x10_set(const int8_t *m)   /* a 1.0 block's values into the working sounds (backup object 14) */
{
    uint32_t k, i;
    fm1_irq_off();
    for (k = 0; k < 2u; k++)
        for (i = 0; i < PROJ_X10; i++)
            trk[k].p[P_SPREAD + i] = (int16_t)clamp((uint8_t)m[1u + PROJ_X10 * k + i] + TP[P_SPREAD + i].min,
                                                    TP[P_SPREAD + i].min, TP[P_SPREAD + i].max);
    song.g[G_DMODE] = (int16_t)clamp((uint8_t)m[PROJ_X10_DM], GP[G_DMODE].min, GP[G_DMODE].max);
    fm1_irq_on();
}
#endif
/* q just read from obj: its 1.0 block from the side record, when it is q's */
UI_SMALL static void proj_x10_load(uint32_t obj, const project_t *q)
{
#if CLIPPER_SYNTH
    proj_x10_t x, *y = &proj_x10s[proj_x10_at(q)];
    if (st_side_load(obj, &x, sizeof x) == 0 && proj_x10_valid(&x) && x.sum == q->sum)
        *y = x;
    else if (!proj_x10_valid(y) || y->sum != q->sum)
        memset(y, 0, sizeof *y);
#else
    (void)obj, (void)q;
#endif
}

UI_SMALL static int audio_quiet(void)
{
    uint32_t p, i;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            if (trk[p].v[i].active)
                return 0;
    for (i = 0; i < NDRUM; i++)
        if (drums.v[i].active)
            return 0;
    return 1;
}

UI_SMALL static void autosave_tick(void)                /* main loop */
{
#if FELUCCA_FLASH
    uint32_t h, now = fm1_ms;
    if (!flash_ok || song.playing || transport_req || rec_wait || ft_on || ui.menu ||
        now - ui_input_ms < AUTOSAVE_IDLE || now - autosave_ms < AUTOSAVE_GAP || now - autosave_checked < 1000u)
        return;
    autosave_checked = now;
    proj_capture(&autosave_buf);
    h = proj_sig(&autosave_buf);
    if (h == autosave_hash || !audio_quiet())
        return;
    if (proj_st_save(OBJ_AUTOSAVE, &autosave_buf) == 0)
        autosave_hash = h;
    autosave_ms = fm1_ms;
#endif
}

UI_SMALL static void autosave_resume(void)              /* power-on: the project as it was left (felucca_init) */
{
#if FELUCCA_FLASH
    project_t *q = &autosave_buf;
    int n;
    if (!flash_ok)
        return;
    n = st_load(OBJ_AUTOSAVE, &proj_tmp, sizeof proj_tmp);
    if (!proj_import(q, &proj_tmp, n))
        return;
    proj_x10_load(OBJ_AUTOSAVE, q);                    /* (after Clipper 1.2) its 1.0 block */
    autosave_hash = proj_sig(q);
    proj_apply(q, 1);
    song.sel = (uint8_t)(q->sel < NTRK && !CLIPPER_SYNTH ? q->sel : 0u);
    for (n = 0; n < NPART; n++)
        trk[n].engine = trk[n].eng_req;        /* (nothing sounds yet: no fade) */
#endif
}

/* 0.7: Clipper's side record of the settings (storage.c st_side): what has no place in the settings word (whose free
 * bits SLOOP uses) nor in the payload (whose length older versions check): the favourites (ui.c fav), USB RATE and
 * CLOCK OUT (seq.c clk_out),
 * with a magic and a CRC. SLOOP 2.7 writes its own (its theme first): each firmware takes only its own back */
#define CLIP_SIDE_MAGIC 0x37504C43u                  /* "CLP7" */
typedef struct {
    uint32_t magic;
    uint8_t usb_rate, clk_out, flags, mch;           /* (1.0) flags: bit 0 STEP ADVANCE OFF (0 before 1.0: ON), bits
                                                      * 1..4 GLO > MIDI > OUT CH - 1; mch: IN CH, 0 OMNI (0 before 1.0) */
    uint16_t fav[FAV_N];
    uint32_t crc;
} clip_side_t;
/* 0.9: then the TUNING (voice.c tune_cfg), its own magic and CRC (0.7 / 0.8 read their part and pass over it; their
 * saves leave it erased: EQUAL) */
#if CLIPPER_SYNTH
#define TUNE_SIDE_MAGIC 0x394E5554u                  /* "TUN9" */
typedef struct {
    uint32_t magic;
    tune_t t;
    uint32_t crc;
} tune_side_t;
#endif
typedef struct {
    clip_side_t a;
#if CLIPPER_SYNTH
    tune_side_t tu;
#endif
} side2_t;
_Static_assert(sizeof(clip_side_t) == 44u && sizeof(side2_t) <= ST_SIDE_MAX, "the side record");
static side2_t side_out, side_saved;
UI_SMALL static void side_fill(side2_t *s)                    /* what the side record holds now */
{
    clip_side_t *r = &s->a;
    memset(s, 0, sizeof *s);
    r->magic = CLIP_SIDE_MAGIC;
    r->usb_rate = (uint8_t)(usb_rate % 3u);
    r->clk_out = (uint8_t)((clk_out != 0u) | (midi_bend % 25u) << 1 | (midi_breath % 3u) << 6);   /* (1.0: bits 1..5
                                                                                            * the bend range, 6..7 EWI) */
    r->flags = (uint8_t)((step_adv_off != 0u) | (midi_out_ch & 15u) << 1 | (midi_local_off != 0u) << 5 |
                         (midi_cc_out != 0u) << 6 | (midi_pc_off != 0u) << 7);
    r->mch = (uint8_t)(midi_in_ch % 17u);
    memcpy(r->fav, fav, sizeof fav);
    r->crc = st_crc32(r, sizeof *r - 4u);
#if CLIPPER_SYNTH
    s->tu.magic = TUNE_SIDE_MAGIC;
    s->tu.t = tune_cfg;
    s->tu.crc = st_crc32(&s->tu, sizeof s->tu - 4u);
#endif
}
UI_SMALL static void side_take(const side2_t *s)              /* a side record read back: each part when it checks out */
{
    const clip_side_t *r = &s->a;
    if (r->magic == CLIP_SIDE_MAGIC && r->crc == st_crc32(r, sizeof *r - 4u)) {
        usb_rate = (uint8_t)(r->usb_rate % 3u);
        clk_out = (uint8_t)(r->clk_out & 1u);
        midi_bend = (uint8_t)(((r->clk_out >> 1) & 31u) % 25u);
        midi_breath = (uint8_t)((r->clk_out >> 6) % 3u);
        step_adv_off = (uint8_t)(r->flags & 1u);
        midi_local_off = (uint8_t)((r->flags >> 5) & 1u);
        midi_cc_out = (uint8_t)((r->flags >> 6) & 1u);
        midi_pc_off = (uint8_t)(r->flags >> 7);
        midi_out_ch = (uint8_t)((r->flags >> 1) & 15u);
        midi_in_ch = (uint8_t)(r->mch <= 16u ? r->mch : 0u);
        memcpy(fav, r->fav, sizeof fav);
    }
#if CLIPPER_SYNTH
    if (s->tu.magic == TUNE_SIDE_MAGIC && s->tu.crc == st_crc32(&s->tu, sizeof s->tu - 4u)) {
        tune_cfg = s->tu.t;
        tune_build();
    }
#endif
}
/* settings + learned panel table: one flash object. The flash copy wins at
 * boot (the .noinit copies are garbage after a power-off). */
typedef struct {
    uint32_t magic, palette, lowcut, zoom;
    panel_t panel;
#if FELUCCA_ARRANGER
    arr_config_t arrangement;
#endif
    uint32_t lights;                               /* SLOOP 2.3: the backlight (panel.c lights_word); appended,
                                                    * so 2.2 still reads its part (st_load cuts at its size) */
} persist_t;
#define PERSIST_SIZE_V22 __builtin_offsetof(persist_t, lights)   /* the settings as 2.2 wrote them (no lights) */
_Static_assert(sizeof(persist_t) == PERSIST_SIZE_V22 + 4u, "lights: the last word, no padding before it");
#if FELUCCA_ARRANGER
_Static_assert(__builtin_offsetof(persist_t, arrangement) == 48u && sizeof(arr_config_t) == 36u,
               "the web editor's Song page edits bytes 48..83 of the settings (web/editor.html ARR)");
#endif
#if FELUCCA_ARRANGER
#define PERSIST_MAGIC 0x50455233u                  /* "PER3": includes the song order */
#else
#define PERSIST_MAGIC 0x50455232u
#endif
#if FELUCCA_FLASH
static persist_t persist_saved;
static void dsu_boot(void);
#endif

UI_SMALL static void persist_boot(void)                    /* before settings_init / panel_init */
{
#if FELUCCA_ARRANGER
    arr_defaults(&arrangement);
#endif
#if FELUCCA_FLASH
    persist_t p;
    uint32_t f = irq_save();
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;       /* the expected 1 MiB part, else stay RAM-only */
    irq_restore(f);
    if (!flash_ok)
        return;
    fl_plain_window_init();                        /* flash above 0x93000 reads as plaintext through XIP
                                                    * (user sample sets are played from there) */
    {
        uint32_t k;
        for (k = 0; k < SMP_USER_SLOTS; k++)
            smp_user_scan(k);
    }
    {
        int n = st_load(OBJ_SETTINGS, &p, sizeof p);
        if (n == (int)PERSIST_SIZE_V22 && p.magic == PERSIST_MAGIC)
            p.lights = 0;                          /* from 2.2: backlight off */
        if (((n == (int)sizeof p || n == (int)PERSIST_SIZE_V22) && p.magic == PERSIST_MAGIC)
#if FELUCCA_ARRANGER
            || (n == (int)(16u + sizeof(panel_t)) && p.magic == 0x50455232u)
#endif
            ) {
            settings.magic = SETTINGS_MAGIC;
            settings.palette = p.palette;
            settings.lowcut = p.lowcut;
            settings.zoom = p.zoom;
            if (p.panel.magic == PANEL_MAGIC)
                panel = p.panel;
            if (p.magic == PERSIST_MAGIC)
                lights_from_word(p.lights);
            else
                p.lights = 0;
#if FELUCCA_ARRANGER
            if (p.magic == PERSIST_MAGIC && arr_valid(&p.arrangement, 15u))
                arrangement = p.arrangement;
            else
                p.arrangement = arrangement;
#endif
            persist_saved = p;
            {   /* 0.7: the favourites and USB RATE, in the side record of the same copy */
                side2_t r;
                if (st_side_load(OBJ_SETTINGS, &r, sizeof r) == 0)
                    side_take(&r);
                side_fill(&side_saved);
            }
        } else if (n == (int)(8u + sizeof(panel_t)) && p.magic == 0x50455231u) {   /* "PER1": palette, panel */
            const uint32_t *w = (const uint32_t *)&p;
            panel_t old;
            memcpy(&old, w + 2, sizeof old);
            settings.magic = SETTINGS_MAGIC;
            settings.palette = w[1];
            settings.lowcut = 0;
            settings.zoom = 0;
            if (old.magic == PANEL_MAGIC)
                panel = old;
        }
    }
    {   /* projects: fill empty RAM slots from flash, so the slot list is right after power-on. A slot
         * still valid in RAM (a warm reset: an update, UPDATE MODE, a crash) may never have reached
         * flash (a live section stored while playing): marked to be written when quiet */
        uint32_t i;
        for (i = 0; i < 4u; i++)
            if (!proj_ok(&proj_slot[i])) {
                proj_fetch(i);
            } else {
                int n = st_load(OBJ_PROJECT0 + i, &proj_tmp, sizeof proj_tmp);
                if (n != (int)sizeof proj_slot[i] || memcmp(&proj_tmp.v5, &proj_slot[i], sizeof proj_slot[i]))
                    sec_dirty |= (uint8_t)(1u << i);
                else
                    proj_x10_load(OBJ_PROJECT0 + i, &proj_slot[i]);   /* (Clipper: its 1.0 block, from flash) */
            }
    }
    dsu_boot();                                    /* SLOOP 2.5: the SYN kits (after the settings) */
    up_boot();                                     /* user presets */
    fm6_bank_boot();                               /* the FM6 patch bank (fm6_bank.c) */
#endif
}

UI_SMALL static int project_used(uint32_t slot) { return proj_ok(&proj_slot[slot & 3u]); }

#if FELUCCA_FLASH
/* SLOOP 2.5: the settings object holds the SYN kits after persist_t (drum_synth.c dsu): firmware before 2.5 reads
 * its persist_t and leaves the rest (st_load cuts at its size); its own next save drops them */
/* what another firmware keeps in the settings object after the SYN kits (SLOOP 2.6: its MIDI LEARN map and GLO > MIDI
 * channels): read at power-on, written back as it was with every save, so going back to it finds them (it was dropped:
 * there they came back at their defaults, and a SLOOP 2.6 object without them reads as 2.5's) */
#define SET_TAIL_MAX 64u
static uint8_t set_tail[SET_TAIL_MAX];
static uint32_t set_tail_n;
_Static_assert(sizeof(persist_t) + sizeof(dsu_bank_t) + SET_TAIL_MAX <= ST_PAYLOAD_MAX, "the settings object");
UI_SMALL static int settings_write(const persist_t *p)
{
    int rc;
    if (!dsu_valid(&dsu))
        dsu_defaults();
    side_fill(&side_out);
    st_side = &side_out;
    st_side_len = sizeof side_out;
    rc = st_save3(OBJ_SETTINGS, p, sizeof *p, &dsu, sizeof dsu, set_tail, set_tail_n);
    st_side = 0;
    if (rc)
        return -1;
    side_saved = side_out;
    dsu_dirty = 0;
    return 0;
}
UI_SMALL static void dsu_boot(void)                          /* the SYN kits from the settings object, else the defaults */
{
    st_hdr_t h;
    dsu_defaults();
    set_tail_n = 0;
    if (st_current(OBJ_SETTINGS, &h) >= 0 && h.len >= sizeof(persist_t) + sizeof(dsu_bank_t) &&
        h.len <= sizeof(persist_t) + sizeof(dsu_bank_t) + SET_TAIL_MAX &&
        dsu_valid((const dsu_bank_t *)(st_buf + sizeof(persist_t)))) {
        uint32_t k;
        memcpy(&dsu, st_buf + sizeof(persist_t), sizeof dsu);
        for (k = 0; k < DSU_N; k++)
            dsu_fix_kit(&dsu.k[k]);
        set_tail_n = h.len - sizeof(persist_t) - sizeof(dsu_bank_t);   /* (SLOOP 2.6's MIDI part: kept) */
        memcpy(set_tail, st_buf + sizeof(persist_t) + sizeof(dsu_bank_t), set_tail_n);
    }
    dsu_dirty = 0;
}
#endif

UI_SMALL static void persist_fill(persist_t *p)              /* the settings as they are now */
{
    memset(p, 0, sizeof *p);
    p->magic = PERSIST_MAGIC;
    p->palette = settings.palette;
    p->lowcut = settings.lowcut;
    p->zoom = settings.zoom;
    p->panel = panel;
    p->lights = lights_word();
#if FELUCCA_ARRANGER
    p->arrangement = arrangement;
#endif
}

UI_SMALL static void settings_save(void)
{
#if FELUCCA_FLASH
    persist_t p;
    if (!flash_ok)
        return;
    persist_fill(&p);
    side_fill(&side_out);
    if (!memcmp(&p, &persist_saved, sizeof p) && !memcmp(&side_out, &side_saved, sizeof side_out))
        return;                                    /* unchanged: no erase cycle (0.7: the side record too) */
    if (settings_write(&p) == 0)
        persist_saved = p;
#endif
}

#if FELUCCA_FLASH
_Static_assert(sizeof(project_t) <= ST_PAYLOAD_MAX, "project does not fit one flash sector");

/* ---- backup restore (editor.c BK_PUT): each object checked as a load checks it, then written through the
 * same A/B commit as a save. rc: 0 ok, 2 not a valid object, 3 stop the song first, 4 flash */
UI_SMALL static int panel_valid(const panel_t *q)           /* a permutation of the buttons and of the knobs */
{
    uint32_t i, b = 0, e = 0;
    if (q->magic != PANEL_MAGIC)
        return 0;
    for (i = 0; i < NB; i++) {
        if (q->btn[i] >= 14u || (b >> q->btn[i]) & 1u)
            return 0;
        b |= 1u << q->btn[i];
    }
    for (i = 0; i < NE; i++) {
        if (q->enc[i] >= 7u || (e >> q->enc[i]) & 1u || (q->dir[i] != 1 && q->dir[i] != -1))
            return 0;
        e |= 1u << q->enc[i];
    }
    return 1;
}

UI_SMALL static uint32_t settings_restore(const void *raw, uint32_t n)
{
    persist_t p;
    if (n != sizeof p && n != PERSIST_SIZE_V22)
        return 2;
    memset(&p, 0, sizeof p);
    memcpy(&p, raw, n);
    if (p.magic != PERSIST_MAGIC || p.palette >= NPALETTES || p.lowcut > 1u || p.zoom > 1u || !panel_valid(&p.panel))
        return 2;
#if FELUCCA_ARRANGER
    if (!arr_valid(&p.arrangement, 15u))
        return 2;
    if ((song.playing || transport_req) && memcmp(&p.arrangement, &arrangement, sizeof arrangement))
        return 3;                                    /* SLOOP 2.5: a new song order only when stopped (as on the SONG screen) */
#endif
    if (!flash_ok || settings_write(&p))
        return 4;
    persist_saved = p;
    settings.palette = p.palette;
    settings.lowcut = p.lowcut;
    settings.zoom = p.zoom;
    panel = p.panel;
#if FELUCCA_ARRANGER
    arrangement = p.arrangement;
#endif
    lights_from_word(p.lights);
    song.g[G_SYNC] = (int16_t)lights_sync;
    song.g[G_MIDI] = (int16_t)lights_mout;
    song.g[G_ROUTE] = (int16_t)lights_min;
    palette_set(settings.palette);
    fx_lowcut = (uint8_t)(settings.lowcut != 0);
    ui.force = 1;
    return 0;
}

/* slot 0..3 (n 0: empty), or 4: the working project (loaded now) */
UI_SMALL static uint32_t project_restore(uint32_t slot, const void *raw, uint32_t n)
{
    if (song.playing || transport_req)
        return 3;
    if (slot < 4u && !n) {
        if (!flash_ok || st_save(OBJ_PROJECT0 + slot, raw, 0))
            return 4;
        memset(&proj_slot[slot], 0, sizeof proj_slot[slot]);
        sec_dirty &= (uint8_t)~(1u << slot);
        return 0;
    }
    if (!proj_import(&autosave_buf, raw, (int)n))
        return 2;
#if CLIPPER_SYNTH
    {   /* (its 1.0 block until object 14 brings it: the backup snapshot's, LIST's, when it is that project) */
        proj_x10_t *x = &proj_x10s[proj_x10_at(&autosave_buf)];
        if (proj_x10_valid(&proj_x10s[4]) && proj_x10s[4].sum == autosave_buf.sum)
            *x = proj_x10s[4];
        else
            memset(x, 0, sizeof *x);
    }
#endif
    if (slot == 4u) {
        project_apply(&autosave_buf);
#if CLIPPER_SYNTH
        proj_restored_sum = autosave_buf.sum;           /* (object 14 may bring its 1.0 block next) */
#endif
        return 0;
    }
    if (!flash_ok || proj_st_save(OBJ_PROJECT0 + slot, &autosave_buf))
        return 4;
    memcpy(&proj_slot[slot], &autosave_buf, sizeof proj_slot[slot]);
#if CLIPPER_SYNTH
    proj_x10s[slot] = proj_x10s[proj_x10_at(&autosave_buf)];   /* (its 1.0 block, when it has one: object 14 brings it) */
#endif
    proj_v6_ins_ok[slot] = proj_v6_ins_ok[4];           /* (a SLOOP 2.6 project's INSERT goes with it) */
    memcpy(proj_v6_ins[slot], proj_v6_ins[4], sizeof proj_v6_ins[slot]);
    sec_dirty &= (uint8_t)~(1u << slot);
    return 0;
}
#endif
#if FELUCCA_ARRANGER
UI_SMALL static void arrangement_save(void)
{
    if (song.playing || transport_req) { ui_message("STOP BEFORE SAVE"); return; }
    settings_save();
#if FELUCCA_FLASH
    if (flash_ok) {
        ui_message(memcmp(&persist_saved.arrangement, &arrangement, sizeof arrangement) ? "SAVE ERROR" : "SONG SAVED");
        return;
    }
#endif
    ui_message("SONG IN RAM ONLY");
}

/* ---- live sections (SAVE + key, ui_layers.c). A section is a project slot (A..D = 1..4): stored into RAM
 * at once (playing too), written to flash once the transport is stopped and nothing sounds (an erase
 * stops the audio for ~50 ms); a song recorded with SONG REC is saved the same way. */
UI_SMALL static void section_store(uint32_t s)
{
    s &= 3u;
    fm1_irq_off();                                      /* (the audio ISR may be applying a section) */
    proj_capture(&proj_slot[s]);
    live_sec = (int8_t)s;
    fm1_irq_on();
    sec_dirty |= (uint8_t)(1u << s);
}
UI_SMALL static void section_load(uint32_t s)                    /* stopped: the section is the loop now */
{
    s &= 3u;
    project_apply(&proj_slot[s]);
    live_sec = (int8_t)s;
}
UI_SMALL static void sections_write(void)                        /* the dirty sections and song into flash */
{
    uint32_t i;
#if FELUCCA_FLASH
    if (flash_ok)
        for (i = 0; i < 4u; i++)
            if (((sec_dirty >> i) & 1u) && proj_st_save(OBJ_PROJECT0 + i, &proj_slot[i]) == 0)
                sec_dirty &= (uint8_t)~(1u << i);       /* (a failed write stays dirty: tried again later) */
    if (!flash_ok)
#endif
        sec_dirty = 0;
    (void)i;
    if (song_dirty) {
        song_dirty = 0;
        settings_save();
    }
}
/* before an intentional reset (an update, UPDATE MODE, UBOOT from the host): the audio is stopped, so
 * whatever is only in RAM goes to flash now: the live sections, the song, the working project */
UI_SMALL static void persist_flush_now(void)
{
    sections_write();
#if FELUCCA_FLASH
    if (flash_ok && !arrangement_clock.running) {     /* (a song playing: the tracks hold a section) */
        proj_capture(&autosave_buf);
        if (proj_sig(&autosave_buf) != autosave_hash && proj_st_save(OBJ_AUTOSAVE, &autosave_buf) == 0)
            autosave_hash = proj_sig(&autosave_buf);
    }
#endif
}
UI_SMALL static void sections_flush(void)                        /* main loop */
{
    static uint32_t tried;
    if (srec_done) {
        song_dirty = srec_done != 0xFFu;
        if (song_dirty) {
            char b[8];
            fmt_int(b, srec_done);
            ui_say("SONG PARTS ", b);
        } else {
            ui_message("NO SONG");
        }
        srec_done = 0;
    }
    if ((uint32_t)song.g[G_SYNC] != lights_sync) {      /* GLO > SYSTEM > SYNC: kept with the settings */
        lights_sync = (uint8_t)song.g[G_SYNC];
        settings_later = 1;
    }
    if ((uint32_t)(song.g[G_MIDI] != 0) != lights_mout) {   /* GLO > SYSTEM > MIDI: the same */
        lights_mout = (uint8_t)(song.g[G_MIDI] != 0);
        settings_later = 1;
    }
    if ((uint32_t)(song.g[G_ROUTE] != 0) != lights_min) {   /* GLO > SYSTEM > IN: the same */
        lights_min = (uint8_t)(song.g[G_ROUTE] != 0);
        settings_later = 1;
    }
    if (settings_later) {                               /* the menu closed while playing */
        settings_later = 0;
        song_dirty = 1;                                 /* (settings_save when quiet, with the song) */
    }
    if ((!sec_dirty && !song_dirty) || song.playing || transport_req || !audio_quiet() || fm1_ms - ui_input_ms < 1500u ||
        fm1_ms - tried < 5000u)
        return;
    tried = fm1_ms;                                     /* (a failed write: again in 5 s, not every frame) */
    sections_write();
    if (sec_dirty)
        ui_message("SAVE ERROR: RETRYING");
}
#endif
#endif /* PROJ_HOST */
