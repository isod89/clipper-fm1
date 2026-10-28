/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* User presets (editor protocol v2, cmds 16-21; the SAVE > USER page): 32
 * slots in two storage.c objects (OBJ_UPRESET0/1, 0xDC000..0xDFFFF), 16
 * records each, mirrored in RAM so browsing never reads flash. A record:
 * engine, name, the instrument parameters, a 16-step pattern.
 *
 * A bank whose magic, record size or slot count differ reads as empty (Clipper: but its own 0.4 bank, converted); so
 * does a record with another UP_VER. A record keeps np = the P_COUNT it was
 * stored with; when that differs it is mapped by count: its last 8 values
 * are P_E0..P_E7, the first np - 8 are P_LEVEL.. in order, missing ones take
 * their defaults. So common parameters may only be added just before P_E0
 * (else bump UP_VER).
 *
 * With -DUP_HOST (host test) only the part above #ifndef UP_HOST is built;
 * it needs nothing but core.h. */
#ifndef UI_SMALL
#define UI_SMALL                                   /* (gfx.c: minsize on the device; a test alone: nothing) */
#endif
#define UP_PER_BANK 16u
#define UP_PMAX 72u                              /* room for P_COUNT to grow */
#define UP_USED 0xA5u
#define UP_VER 1u
#define UP_BANK_MAGIC 0x31425055u                /* "UPB1" */
typedef struct {
    uint8_t used, ver, engine, np;               /* UP_USED, UP_VER, engine, P_COUNT when stored */
    char name[12];                               /* ASCII 32..126, 0-padded (no 0 when 12 long) */
    int16_t p[UP_PMAX];
    uint8_t note[16], flags[16];                 /* note 0 = rest; flags 1 accent, 2 slide, 4 tie */
} up_rec_t;
typedef struct {
    uint32_t magic;
    uint16_t rsize, nslot;
    up_rec_t r[UP_PER_BANK];
} up_bank_t;
_Static_assert(sizeof(up_rec_t) == 192, "user preset record layout (SLOOP's and Clipper's: one bank for both)");
_Static_assert(P_COUNT < 128, "user preset record: P_COUNT");
#if CLIPPER_SYNTH
/* Clipper: the record SLOOP reads (the banks are shared when the FM-1 goes from one firmware to the other: a bank of
 * another shape reads as empty there, and a save there writes over it). A Clipper record holds SLOOP 2.5's layout
 * (np = UP_NP: the 53 common parameters, then P_E0..P_E7), and its 25 parameters SLOOP has not (LFO 2, ENV 2, the
 * matrix: P_L2RATE .. P_M6AMT) packed in the record's spare values p[UP_NP .. UP_NP + 9] (129 bits), marked
 * UP_XMAGIC in p[71] (SLOOP leaves them 0: up_store clears the record). SLOOP maps it by count (its own new values
 * at their defaults); a SLOOP record (np 61, or 71 from 2.6) here: the common ones by place, the engine's 8 last, the
 * rest (2.6's INSERT and the like, which Clipper has not) dropped, LFO 2 / ENV 2 / the matrix at their defaults.
 * Clipper 0.4's own bank (232-byte records, np 86) is read and made this one (up_bank_check) */
#define UP_E0V 53u                               /* P_E0 of SLOOP 2.4 / 2.5 and of the stored layout */
#define UP_NP (UP_E0V + 8u)
#define UP_NX ((uint32_t)P_IMIX + 1u - UP_E0V)    /* the Clipper parameters packed (0.8's MOD SEQ and SPICE: up_side) */
#define UP_NX4 25u                               /* .. by 0.4 / 0.5 (LFO 2, ENV 2, the matrix) */
#define UP_XMAGIC 0x434C                         /* "CL": a 0.4 / 0.5 record (p[71]) */
#define UP_XMAGIC6 0x4Du                         /* 0.6: p[71]'s low byte; its high byte the stream's bits 160..167 */
#define UP_RSIZE_04 232u                         /* Clipper 0.4's record (92 values) */
#define UP_NP_V26 71u
#define UP_P04 86u                               /* Clipper 0.4's P_COUNT, and its P_E0 */
#define UP_E004 78u                            /* SLOOP 2.6's records: its INSERT at p[53..57] */
static const uint8_t UP_XW[UP_NX] = {7, 3, 1, 7, 7, 7, 7, 4, 4, 7, 4, 4, 7, 4, 4, 7, 4, 4, 7, 4, 4, 7, 4, 4, 7,
                                     4, 7, 7, 7, 7};   /* bits (0.6: + the INSERT's TYPE, A, B, C, MIX) */
_Static_assert(UP_NX == 30u && P_L2RATE == UP_E0V && P_ITYPE == UP_E0V + UP_NX4 && P_IMIX + 1 == P_MSRATE &&
               P_SLOOP == P_SPREAD - 1, "the packed parameters, in order");
#define UP_XMS 161u                              /* (0.8) a bit of the stream: note / flags hold the MOD SEQ's steps */
/* bit k of the packed stream: the record's p[UP_NP + k / 16] for k < 160, then p[71]'s high byte */
UI_SMALL static int16_t *up_xword(int16_t *p, uint32_t k, uint32_t *sh)
{
    if (k < 160u) {
        *sh = k & 15u;
        return &p[UP_NP + (k >> 4)];
    }
    *sh = 8u + (k - 160u);
    return &p[UP_PMAX - 1u];
}
UI_SMALL static void up_xpack(int16_t *p, const int16_t *x)   /* x: P_L2RATE .. P_IMIX -> the record's p */
{
    uint32_t i, j, bit = 0, sh;
    for (i = 0; i < 10u; i++)
        p[UP_NP + i] = 0;
    p[UP_PMAX - 1u] = UP_XMAGIC6;
    for (i = 0; i < UP_NX; i++) {
        uint32_t v = (uint32_t)clamp(x[i] - TP[UP_E0V + i].min, 0, (1 << UP_XW[i]) - 1);
        if (UP_E0V + i >= P_M1SRC && UP_E0V + i <= P_M6AMT && (UP_E0V + i - P_M1SRC) % 3u == 0u)
            v = (uint32_t)clamp(x[i], 0, MS_NSRC - 1) & 15u;   /* (1.0: a source's fifth bit in the tail) */
        for (j = 0; j < UP_XW[i]; j++, bit++)
            if ((v >> j) & 1u) {
                int16_t *w = up_xword(p, bit, &sh);
                *w = (int16_t)((uint16_t)*w | (uint16_t)(1u << sh));
            }
    }
}
UI_SMALL static void up_xunpack(const int16_t *p, int16_t *x, uint32_t nx)   /* the first nx of them */
{
    uint32_t i, j, bit = 0, sh;
    for (i = 0; i < nx; i++) {
        uint32_t v = 0;
        for (j = 0; j < UP_XW[i]; j++, bit++) {
            const int16_t *w = up_xword((int16_t *)p, bit, &sh);
            v |= (((uint32_t)(uint16_t)*w >> sh) & 1u) << j;
        }
        x[i] = (int16_t)((int32_t)v + TP[UP_E0V + i].min);
    }
}
_Static_assert(160u + 8u >= 129u + 32u, "the packed parameters fit the record's spare values");
/* Clipper's own record: 0 none, 4 of 0.4 / 0.5 (25 packed), 6 of 0.6 (30) */
UI_SMALL static int up_xrec(const up_rec_t *r)
{
    if (r->np != UP_NP)
        return 0;
    if (r->p[UP_PMAX - 1u] == UP_XMAGIC)
        return 4;
    return ((uint16_t)r->p[UP_PMAX - 1u] & 0xFFu) == UP_XMAGIC6 ? 6 : 0;
}
static uint8_t up_ld[8u + UP_PER_BANK * UP_RSIZE_04] __attribute__((section(".pool")));   /* (a 0.4 bank, loading) */
static up_bank_t up_bank[UP_SLOTS / UP_PER_BANK] __attribute__((section(".pool")));
/* 0.8: what the record has no room for (the MOD SEQ's RATE LEN SMTH START, SPICE, DICE: UP_NS values) rides in the
 * bank's side record (storage.c st_side: SLOOP never sees it), a slot's bytes v[0] = 1 (set), then each value - its
 * minimum; a slot without them (an older save, SLOOP's) loads their defaults. The steps: the record's note / flags
 * (16 + 16 bytes, two steps a byte, + 8: SLOOP stores its pattern there and loads none of it), marked by UP_XMS */
/* 0.9: + VINTAGE and LOOP (UP_NS 8): a slot's v[0] 2; 0.8's side (UPS8, 8 bytes a slot, v[0] 1, 6 values) still read */
#define UP_NS ((uint32_t)P_SPREAD - P_MSRATE)
#define UP_NS8 6u                                /* (0.8's) */
#define UP_SIDE_MAGIC 0x39535055u                /* "UPS9" */
#define UP_SIDE_MAGIC8 0x38535055u               /* "UPS8" (0.8) */
typedef struct {
    uint32_t magic;
    uint8_t v[UP_PER_BANK][10];
    uint32_t crc;
} up_side_t;
typedef struct {
    uint32_t magic;
    uint8_t v[UP_PER_BANK][8];
    uint32_t crc;
} up_side8_t;
_Static_assert(UP_NS == 8u && sizeof(up_side_t) == 168u && sizeof(up_side8_t) == 136u, "the user presets' side record");
static up_side_t up_side[UP_SLOTS / UP_PER_BANK];
static uint8_t up_xs[10];                        /* the side bytes of the record up_rec_set made last (up_put's) */
/* 1.0: SPREAD .. the matrix's slot 12 (UP_NT values) ride in the bank's tail (storage.c st_tail: after the payload, in
 * the rest of its sector: SLOOP and older versions never see it, their save erases it), with the fifth bit of slots
 * 1..6's sources (the record keeps four: LFO 3 and the macros are 15..19). A slot's bytes: v[0] 1 (set), v[1] the
 * fifth bits (slot k: bit k), then each value - its minimum; a slot without them loads their defaults */
#define UP_NT ((uint32_t)P_E0 - P_SPREAD)
#define UP_TAIL_MAGIC 0x30315055u                /* "UP10" */
typedef struct {
    uint32_t magic;
    uint8_t v[UP_PER_BANK][UP_NT + 2u];
    uint32_t crc;
} up_tail_t;
_Static_assert(UP_NT == 27u && sizeof(up_tail_t) + sizeof(up_bank_t) <= 3840u, "the user presets' tail fits the sector");
static up_tail_t up_tail[UP_SLOTS / UP_PER_BANK] __attribute__((section(".pool")));
static uint8_t up_xt[UP_NT + 2u];                /* the tail bytes of the record up_rec_set made last */
UI_SMALL static const uint8_t *up_tail_of(const up_rec_t *r)   /* a record's tail bytes, 0 = none */
{
    uint32_t b;
    for (b = 0; b < UP_SLOTS / UP_PER_BANK; b++)
        if (r >= up_bank[b].r && r < up_bank[b].r + UP_PER_BANK)
            return up_tail[b].magic == UP_TAIL_MAGIC && up_tail[b].v[r - up_bank[b].r][0] == 1u ? up_tail[b].v[r - up_bank[b].r] : 0;
    return up_xt[0] == 1u ? up_xt : 0;
}
UI_SMALL static const uint8_t *up_side_of(const up_rec_t *r)   /* a record's side bytes, 0 = none */
{
    uint32_t b;
    for (b = 0; b < UP_SLOTS / UP_PER_BANK; b++)
        if (r >= up_bank[b].r && r < up_bank[b].r + UP_PER_BANK)
            return up_side[b].magic == UP_SIDE_MAGIC ? up_side[b].v[r - up_bank[b].r] : 0;
    return up_xs;
}
UI_SMALL static int up_has_ms(const up_rec_t *r)          /* note / flags hold the MOD SEQ */
{
    uint32_t sh;
    const int16_t *w;
    if (up_xrec(r) != 6)
        return 0;
    w = up_xword((int16_t *)r->p, UP_XMS, &sh);
    return ((uint16_t)*w >> sh) & 1u;
}
UI_SMALL static void up_ms_put(up_rec_t *r, int8_t (*ms)[MSEQ_STEPS])   /* (after up_rec_set) */
{
    uint32_t n, sh;
    int16_t *w = up_xword(r->p, UP_XMS, &sh);
    for (n = 0; n < 32u; n++) {
        uint32_t lo = (uint32_t)clamp(ms[(2u * n) / MSEQ_STEPS][(2u * n) % MSEQ_STEPS] + 8, 0, 15);
        uint32_t hi = (uint32_t)clamp(ms[(2u * n + 1u) / MSEQ_STEPS][(2u * n + 1u) % MSEQ_STEPS] + 8, 0, 15);
        (n < 16u ? r->note : r->flags)[n % 16u] = (uint8_t)(lo | hi << 4);
    }
    *w = (int16_t)((uint16_t)*w | (uint16_t)(1u << sh));
}
UI_SMALL static void up_ms_get(const up_rec_t *r, int8_t (*ms)[MSEQ_STEPS])
{
    uint32_t n;
    for (n = 0; n < 32u; n++) {
        uint32_t b = (n < 16u ? r->note : r->flags)[n % 16u];
        ms[(2u * n) / MSEQ_STEPS][(2u * n) % MSEQ_STEPS] = (int8_t)((int32_t)(b & 15u) - 8);
        ms[(2u * n + 1u) / MSEQ_STEPS][(2u * n + 1u) % MSEQ_STEPS] = (int8_t)((int32_t)(b >> 4) - 8);
    }
}
#else
#define UP_E0V ((uint32_t)P_E0)
static up_bank_t up_bank[UP_SLOTS / UP_PER_BANK];
#endif

UI_SMALL static up_rec_t *up_rec(uint32_t k) { return &up_bank[k / UP_PER_BANK].r[k % UP_PER_BANK]; }

UI_SMALL static int up_valid(const up_rec_t *r)
{
    return r->used == UP_USED && r->ver == UP_VER && r->engine < NENGINES && r->np >= 8u && r->np <= UP_PMAX &&
           r->name[0];
}

UI_SMALL static int up_used(uint32_t k) { return k < UP_SLOTS && up_valid(up_rec(k)); }

/* today's values v (P_COUNT) into record r (np, p[]) */
UI_SMALL static void up_rec_set(up_rec_t *r, const int16_t *v)
{
    uint32_t i;
#if CLIPPER_SYNTH
    r->np = UP_NP;
    for (i = 0; i < UP_E0V; i++)
        r->p[i] = v[i];
    for (i = 0; i < 8u; i++)
        r->p[UP_E0V + i] = v[P_E0 + i];
    up_xpack(r->p, &v[UP_E0V]);
    up_xs[0] = 2;
    for (i = 0; i < UP_NS; i++)
        up_xs[1u + i] = (uint8_t)clamp(v[P_MSRATE + i] - TP[P_MSRATE + i].min, 0, 255);
    up_xt[0] = 1;                                       /* (1.0) the tail's */
    up_xt[1] = 0;
    for (i = 0; i < MX_SLOTS6; i++)
        if (clamp(v[mx_pid(i)], 0, MS_NSRC - 1) & 16)
            up_xt[1] |= (uint8_t)(1u << i);
    for (i = 0; i < UP_NT; i++)
        up_xt[2u + i] = (uint8_t)clamp(v[P_SPREAD + i] - TP[P_SPREAD + i].min, 0, 255);
#else
    r->np = P_COUNT;
    for (i = 0; i < P_COUNT; i++)
        r->p[i] = v[i];
#endif
}

/* the record's values in today's P_* order (mapped by count, see the top); def = the defaults */
UI_SMALL static void up_params(const up_rec_t *r, int16_t *out, const int16_t *def)
{
    uint32_t i, nc = r->np - 8u, ncm = nc < UP_E0V ? nc : UP_E0V;   /* (the common ones it has, by place) */
    for (i = 0; i < P_E0; i++)
        out[i] = i < ncm ? r->p[i] : def[i];
    for (i = 0; i < 8u; i++)
        out[P_E0 + i] = r->p[nc + i];
#if CLIPPER_SYNTH
    if (up_xrec(r))                                     /* Clipper's own: LFO 2, ENV 2, the matrix (0.6: the INSERT) */
        up_xunpack(r->p, &out[UP_E0V], up_xrec(r) == 6 ? UP_NX : UP_NX4);
    else if (r->np == UP_NP_V26)                        /* SLOOP 2.6's: its INSERT, the same types and values */
        for (i = 0; i < 5u; i++)
            out[P_ITYPE + i] = r->p[UP_E0V + i];
    {
        const uint8_t *sd = up_xrec(r) ? up_side_of(r) : 0;   /* (0.8) the MOD SEQ's and SPICE's: the side's */
        uint32_t nv = !sd ? 0u : sd[0] == 2u ? UP_NS : sd[0] == 1u ? UP_NS8 : 0u;   /* (0.8's: 6 of them) */
        for (i = 0; i < UP_NS; i++)
            out[P_MSRATE + i] = i < nv ? (int16_t)(sd[1u + i] + TP[P_MSRATE + i].min) : def[P_MSRATE + i];
    }
    {
        const uint8_t *tl = up_xrec(r) ? up_tail_of(r) : 0;   /* (1.0) SPREAD .. slot 12, the sources' fifth bits */
        for (i = 0; i < UP_NT; i++)
            out[P_SPREAD + i] = tl ? (int16_t)(tl[2u + i] + TP[P_SPREAD + i].min) : def[P_SPREAD + i];
        if (tl)
            for (i = 0; i < MX_SLOTS6; i++)
                if ((tl[1] >> i) & 1u)
                    out[mx_pid(i)] = (int16_t)(out[mx_pid(i)] | 16);
    }
#endif
}

UI_SMALL static void up_bank_check(uint32_t b, int len)  /* after loading bank b (len bytes, -1 = none): wrong shape -> empty */
{
    up_bank_t *bk = &up_bank[b];
    if (len != (int)sizeof *bk || bk->magic != UP_BANK_MAGIC || bk->rsize != sizeof(up_rec_t) ||
        bk->nslot != UP_PER_BANK)
        memset(bk, 0, sizeof *bk);
}
#if CLIPPER_SYNTH
/* a stored bank (len bytes of src, -1 = none) into bank b: today's as it is, Clipper 0.4's (232-byte records, np 86,
 * P_E0 at 78) made today's record by record. 1: it was 0.4's (to be written back, so SLOOP reads it too) */
UI_SMALL static int up_bank_import(uint32_t b, const uint8_t *src, int len)
{
    up_bank_t *bk = &up_bank[b];
    const up_bank_t *h = (const up_bank_t *)src;
    memset(&up_tail[b], 0, sizeof up_tail[b]);          /* (1.0: the tail is the sector's; up_boot loads it after) */
    if (len == (int)(8u + UP_PER_BANK * UP_RSIZE_04) && h->magic == UP_BANK_MAGIC && h->rsize == UP_RSIZE_04 &&
        h->nslot == UP_PER_BANK) {
        uint32_t k, i;
        memset(bk, 0, sizeof *bk);
        bk->magic = UP_BANK_MAGIC;
        bk->rsize = sizeof(up_rec_t);
        bk->nslot = UP_PER_BANK;
        for (k = 0; k < UP_PER_BANK; k++) {
            const uint8_t *o = src + 8u + k * UP_RSIZE_04;   /* used ver engine np, name[12], p[92], note, flags */
            up_rec_t *r = &bk->r[k];
            int16_t v[P_COUNT], op[92];
            memcpy(op, o + 16, sizeof op);
            if (o[0] != UP_USED || o[1] != UP_VER || o[3] != UP_P04)
                continue;                                /* (0.4 wrote np = its P_COUNT: anything else stays empty) */
            r->used = o[0], r->ver = o[1], r->engine = o[2];
            memcpy(r->name, o + 4, sizeof r->name);
            for (i = 0; i < P_COUNT; i++)                /* (0.4's layout: today's but the INSERT) */
                v[i] = i < P_ITYPE ? op[i] : i < P_E0 ? TP[i].def : op[i - P_E0 + UP_E004];
            up_rec_set(r, v);
            memcpy(r->note, o + 16 + sizeof op, 16);
            memcpy(r->flags, o + 32 + sizeof op, 16);
        }
        return 1;
    }
    memset(bk, 0, sizeof *bk);
    if (len > 0 && len <= (int)sizeof *bk)
        memcpy(bk, src, (uint32_t)len);
    up_bank_check(b, len);
    return 0;
}
#endif

UI_SMALL static int up_name_ok(const uint8_t *s, uint32_t n)   /* 1..12 printable ASCII */
{
    uint32_t i;
    if (!n || n > 12u)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < 32u || s[i] > 126u)
            return 0;
    return 1;
}

UI_SMALL static void up_name(uint32_t k, char *b)       /* upper case, 0-terminated: b holds 13 */
{
    const up_rec_t *r = up_rec(k);
    uint32_t i;
    for (i = 0; i < 12u && r->name[i]; i++)
        b[i] = r->name[i] >= 'a' && r->name[i] <= 'z' ? (char)(r->name[i] - 32) : r->name[i];
    b[i] = 0;
}

UI_SMALL static void up_pat_norm(uint8_t *note, uint8_t *flags)   /* tie: no note; rest: no flags */
{
    *note &= 127u;
    if (*flags & 4u) {
        *note = 0;
        *flags = 4;
    } else {
        *flags = *note ? (uint8_t)(*flags & (SF_ACCENT | SF_SLIDE)) : 0u;
    }
}

UI_SMALL static void up_pat_from(up_rec_t *r, const step_t *st)   /* the first 16 steps -> the pattern */
{
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        r->note[i] = st[i].time == ST_NOTE && st[i].n ? st[i].note[0] : 0u;
        r->flags[i] = st[i].time == ST_TIE ? 4u : st[i].flags;
        up_pat_norm(&r->note[i], &r->flags[i]);
    }
}

UI_SMALL static int up_pat_empty(const up_rec_t *r)
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (r->note[i])
            return 0;
    return 1;
}

/* UP_PUT arguments: slot, engine, name, P_COUNT x v14, 16 x (note, flags) -> *r (values not yet
 * clamped); 0 ok, 1 bad arguments. *slot gets the slot byte when there is one. */
UI_SMALL static int up_parse(const uint8_t *a, uint32_t na, up_rec_t *r, uint32_t *slot)
{
    uint32_t i, n, k;
    if (na < 3u)
        return 1;
    *slot = a[0];
    for (n = 0; 2u + n < na && a[2 + n]; n++)
        ;
    k = 3u + n;                                  /* after the name's 0 */
    if (a[0] >= UP_SLOTS || a[1] >= NENGINES || 2u + n >= na || !up_name_ok(a + 2, n) ||
        na < k + 2u * P_COUNT + 32u)
        return 1;
    memset(r, 0, sizeof *r);
    r->used = UP_USED;
    r->ver = UP_VER;
    r->engine = a[1];
    for (i = 0; i < n; i++)
        r->name[i] = (char)a[2 + i];
    {
        int16_t v[P_COUNT];
        for (i = 0; i < P_COUNT; i++, k += 2u)
            v[i] = (int16_t)((int32_t)((a[k] & 127u) | (a[k + 1] & 127u) << 7) - 8192);
        up_rec_set(r, v);
    }
    for (i = 0; i < 16u; i++, k += 2u) {
        r->note[i] = a[k];
        r->flags[i] = a[k + 1];
        up_pat_norm(&r->note[i], &r->flags[i]);
    }
    return 0;
}

#ifndef UP_HOST
UI_SMALL static const param_desc_t *up_desc(uint32_t e, uint32_t i)
{
    return i >= P_E0 ? &ENGINES[e]->edit[i - P_E0] : &TP[i];
}

UI_SMALL static void up_values(const up_rec_t *r, int16_t *v)   /* mapped and clamped for its engine */
{
    int16_t def[P_COUNT];
    uint32_t i;
    for (i = 0; i < P_COUNT; i++)
        def[i] = up_desc(r->engine, i)->def;
    up_params(r, v, def);
    for (i = 0; i < P_COUNT; i++)
        v[i] = (int16_t)clamp(v[i], up_desc(r->engine, i)->min, up_desc(r->engine, i)->max);
}

UI_SMALL static void up_boot(void)                      /* persist_boot: the banks from flash */
{
#if FELUCCA_FLASH
    uint32_t b;
    for (b = 0; b < UP_SLOTS / UP_PER_BANK; b++)
#if CLIPPER_SYNTH
    {
        memset(&up_side[b], 0, sizeof up_side[b]);
        if (up_bank_import(b, up_ld, flash_ok ? st_load(OBJ_UPRESET0 + b, up_ld, sizeof up_ld) : -1))
            st_save(OBJ_UPRESET0 + b, &up_bank[b], sizeof up_bank[b]);   /* (a 0.4 bank: written back as SLOOP reads it) */
        else if (flash_ok && (st_side_load(OBJ_UPRESET0 + b, &up_side[b], sizeof up_side[b]) ||
                              up_side[b].magic != UP_SIDE_MAGIC ||
                              up_side[b].crc != st_crc32(&up_side[b], sizeof up_side[b] - 4u))) {
            up_side8_t o;                               /* (none, another firmware's, or 0.8's: made 0.9's) */
            uint32_t k;
            memset(&up_side[b], 0, sizeof up_side[b]);
            if (!st_side_load(OBJ_UPRESET0 + b, &o, sizeof o) && o.magic == UP_SIDE_MAGIC8 &&
                o.crc == st_crc32(&o, sizeof o - 4u)) {
                up_side[b].magic = UP_SIDE_MAGIC;
                for (k = 0; k < UP_PER_BANK; k++)
                    memcpy(up_side[b].v[k], o.v[k], sizeof o.v[k]);   /* (v[0] 1: its 6 values) */
                up_side[b].crc = st_crc32(&up_side[b], sizeof up_side[b] - 4u);
            }
        }
        memset(&up_tail[b], 0, sizeof up_tail[b]);       /* (1.0) the tail, when it is ours and whole (inside the loop:
                                                         * each bank's; 1.0's first build ran it once, after the loop, on
                                                         * up_tail[2], past the array, and never read the tails back) */
        if (flash_ok && (st_tail_load(OBJ_UPRESET0 + b, &up_tail[b], sizeof up_tail[b]) ||
                         up_tail[b].magic != UP_TAIL_MAGIC || up_tail[b].crc != st_crc32(&up_tail[b], sizeof up_tail[b] - 4u)))
            memset(&up_tail[b], 0, sizeof up_tail[b]);
    }
#else
        up_bank_check(b, flash_ok ? st_load(OBJ_UPRESET0 + b, &up_bank[b], sizeof up_bank[b]) : -1);
#endif
#endif
}

/* record k = *r (0: erase), then the bank to flash: 0 ok, 2 flash error, 3 no flash (kept in RAM) */
UI_SMALL static int up_put(uint32_t k, const up_rec_t *r)
{
    up_bank_t *bk = &up_bank[k / UP_PER_BANK];
    bk->magic = UP_BANK_MAGIC;
    bk->rsize = sizeof(up_rec_t);
    bk->nslot = UP_PER_BANK;
    if (r)
        *up_rec(k) = *r;
    else
        memset(up_rec(k), 0, sizeof(up_rec_t));
#if CLIPPER_SYNTH
    {   /* (0.8) its side bytes: up_rec_set's (made just before), none when erased */
        up_side_t *sd = &up_side[k / UP_PER_BANK];
        if (sd->magic != UP_SIDE_MAGIC)
            memset(sd, 0, sizeof *sd);
        sd->magic = UP_SIDE_MAGIC;
        if (r)
            memcpy(sd->v[k % UP_PER_BANK], up_xs, sizeof up_xs);
        else
            memset(sd->v[k % UP_PER_BANK], 0, sizeof sd->v[0]);
        sd->crc = st_crc32(sd, sizeof *sd - 4u);
    }
    {   /* (1.0) its tail bytes the same */
        up_tail_t *tl = &up_tail[k / UP_PER_BANK];
        if (tl->magic != UP_TAIL_MAGIC)
            memset(tl, 0, sizeof *tl);
        tl->magic = UP_TAIL_MAGIC;
        if (r)
            memcpy(tl->v[k % UP_PER_BANK], up_xt, sizeof up_xt);
        else
            memset(tl->v[k % UP_PER_BANK], 0, sizeof tl->v[0]);
        tl->crc = st_crc32(tl, sizeof *tl - 4u);
    }
#endif
    if (!r) {
        uint32_t i;
        for (i = 0; i < NTRK; i++)
            if (trk[i].user == k + 1u)
                trk[i].user = 0;
    }
    up_gen++;
#if FELUCCA_FLASH
    if (flash_ok) {
        int rc;
#if CLIPPER_SYNTH
        st_side = &up_side[k / UP_PER_BANK];
        st_side_len = sizeof(up_side_t);
        st_tail = &up_tail[k / UP_PER_BANK];
        st_tail_len = sizeof(up_tail_t);
#endif
        rc = st_save(OBJ_UPRESET0 + k / UP_PER_BANK, bk, sizeof *bk);
        st_side = 0;
        st_tail = 0;
        return rc ? 2 : 0;
    }
#endif
    return 3;
}

UI_SMALL static void up_slot_label(char *b, uint32_t k)  /* "U07" */
{
    b[0] = 'U';
    b[1] = (char)('0' + (k + 1u) / 10u);
    b[2] = (char)('0' + (k + 1u) % 10u);
    b[3] = 0;
}

/* the selected part's sound -> slot k; name 0 or "": engine name + slot number ("ANALOG 07").
 * 1 = the drum track is selected (it has no sound to store) */
UI_SMALL static int up_store(uint32_t k, const char *name)
{
    up_rec_t r;
    uint32_t i;
    if (is_drum(TSEL))
        return 1;
    memset(&r, 0, sizeof r);
    r.used = UP_USED;
    r.ver = UP_VER;
    r.engine = TSEL->eng_req;
    if (name && name[0]) {
        for (i = 0; i < 12u && name[i]; i++)
            r.name[i] = name[i];
    } else {
        char b[16], l[4];
        str_cpy(b, ENGINES[TSEL->eng_req]->name, 9);
        up_slot_label(l, k);
        str_cpy(b + str_len(b), " ", 2);
        str_cpy(b + str_len(b), l + 1, 3);
        for (i = 0; i < 12u && b[i]; i++)
            r.name[i] = b[i];
    }
    up_rec_set(&r, TSEL->p);
#if CLIPPER_SYNTH
    up_ms_put(&r, TSEL->ms);                            /* (0.8) the MOD SEQ's steps where the pattern was */
#else
    up_pat_from(&r, TSEL->step);
#endif
    return up_put(k, &r);
}

/* slot k -> the selected part's sound: engine and every parameter except its mix (LEVEL,
 * PAN, MUTE: the TRACKS faders) and its pattern parameters (param_kept). LIVE: the pattern
 * stored in the record is not loaded: changing the sound never changes the sequence.
 * 0 ok, 1 empty (or the drum track is selected) */
UI_SMALL static int up_load_to(track_t *t, uint32_t k)
{
    const up_rec_t *r;
    int16_t v[P_COUNT];
    uint32_t i;
    int keep_arp = arp_user_on(t);                      /* (1.0: the player's arp only, seq.c arp_snd) */
    if (!up_used(k) || is_drum(t))
        return 1;
    r = up_rec(k);
    up_values(r, v);
    for (i = 0; i < P_COUNT; i++)                       /* (LEN etc. of a kept pattern changed too, and */
        if (param_kept(i) || (keep_arp && i >= P_AMODE && i <= P_AORDER) ||   /* a preset pattern then counted as */
            (i == P_AHOLD && !t->p[P_AMODE] && t->p[P_AHOLD]))   /* edited; 0.7: the arp that runs goes on, KBD HOLD */
            v[i] = t->p[i];                             /* stays on, ui.c apply_preset_to) */
    if (!v[P_AMODE] && !t->p[P_AMODE])
        v[P_AHOLD] = t->p[P_AHOLD];                     /* (0.7) KBD HOLD (HOLD with the arp OFF) is the part's, not the
                                                         * preset's (an older preset's HOLD meant nothing there) */
#if CLIPPER_SYNTH
    if (t == &trk[0])
        hist_before_load();                             /* (0.7: the edits a step of their own, ui_synth.c) */
#endif
    sound_fade_begin(t);
    if (keep_arp)
        arp_keep_req |= (uint8_t)(1u << trk_index(t));  /* (0.7: its notes too) */
    arp_snd[trk_index(t) % NTRK] = (uint8_t)(keep_arp ? 0 : v[P_AMODE]);
    panic_req |= (uint8_t)(1u << trk_index(t));
    fm1_irq_off();                                      /* the audio ISR must not see half a sound */
    t->eng_req = r->engine;
    for (i = 0; i < P_COUNT; i++)
        t->p[i] = v[i];
#if CLIPPER_SYNTH
    if (up_has_ms(r))                                   /* (0.8) its MOD SEQ, or the shapes to start from */
        up_ms_get(r, t->ms);
    else
        mseq_init(t);
#endif
    t->preset = 0;
    fm1_irq_on();
    fm6_track_loaded(t);                                /* FM6: a user preset holds the PTCH and the macros */
    t->user = (uint8_t)(k + 1u);
    sync_reload = 1;
    ui.force = 1;
    return 0;
}
UI_SMALL static int up_load(uint32_t k) { return up_load_to(TSEL, k); }

UI_SMALL static uint32_t up_count(void)                 /* used slots */
{
    uint32_t k, n = 0;
    for (k = 0; k < UP_SLOTS; k++)
        n += (uint32_t)up_used(k);
    return n;
}

UI_SMALL static uint32_t up_nth(uint32_t n)             /* slot of the n-th used one (n < up_count()) */
{
    uint32_t k;
    for (k = 0; k < UP_SLOTS; k++)
        if (up_used(k) && !n--)
            return k;
    return 0;
}

UI_SMALL static uint32_t up_rank(uint32_t slot)         /* used slots before it */
{
    uint32_t k, n = 0;
    for (k = 0; k < slot && k < UP_SLOTS; k++)
        n += (uint32_t)up_used(k);
    return n;
}

/* SAVE > USER page actions, with the message in the top bar */
UI_SMALL static void up_ui(uint32_t op, uint32_t k)     /* 0 load, 1 erase, 2 save */
{
    char l[4];
    int rc;
    up_slot_label(l, k);
    if (op != 1u && is_drum(TSEL)) {
        ui_message("DRUM TRACK: NO SOUND");
        return;
    }
    if (op < 2u && !up_used(k)) {
        ui_message("EMPTY SLOT");
        return;
    }
    if (op == 0u) {
        up_load(k);
        ui_say("LOADED ", l);
        return;
    }
    if (song.playing || transport_req) {               /* a flash erase stops the audio ~50 ms */
        ui_message("STOP BEFORE SAVE");
        return;
    }
    rc = op == 1u ? up_put(k, 0) : up_store(k, 0);
    if (rc == 3)
        ui_message(op == 1u ? "ERASED (RAM)" : "SAVED (RAM)");
    else if (rc)
        ui_message(op == 1u ? "ERASE ERROR" : "SAVE ERROR");
    else
        ui_say(op == 1u ? "ERASED " : "SAVED ", l);
    ui.force = 1;
}
#endif
