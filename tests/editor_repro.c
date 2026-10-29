/* SPDX-License-Identifier: GPL-3.0-only */
/* Reproducers of what tests/editor_fuzz.c found in Clipper (CLIPPER_SYNTH=1), on tests/editor_host.c. Each case
 * returns 0 (passes) once fixed, 1 on the code as it was:
 *   editor_repro 1   a user wavetable (WAVE's USR1..4, an FWTB header in a sample slot) is left out of the backup: BK_LIST
 *                    lists the slot with 0 bytes, so the editor's restore of that backup erases the slot (SMP_ERASE)
 *   editor_repro 2   TRACK_MIX of track 4 (the groovebox's drum level) writes 0..127 into G_DRLVL, which Clipper uses as the
 *                    master EQ's LOW (88..112): the value goes past its range (the EQ reads up to +-12 dB, the page shows it)
 *   editor_repro 4   the backup of the user preset banks (objects 6, 7) holds the records only: each preset's Clipper 0.8+ values
 *                    (MOD SEQ RATE LEN SMTH START, SPICE, DICE, VINTAGE, LOOP: the bank's side record) and 1.0 values
 *                    (SPREAD .. matrix slot 12, the macros: its tail) are not in it, and a restore keeps the side / tail of
 *                    what the bank held before in RAM: a preset comes back with another preset's values (and its defaults
 *                    after the next power-on)
 *   editor_repro 3   with any step on track 3 (an old SLOOP project loaded, the editor's TRACK_STEP), a project did not keep
 *                    Clipper 1.0's values (SPREAD, LFO 3, ENV 2 LOOP, the MACROS, matrix slots 7..12, the delay MODE): a
 *                    section saved and loaded (RAM, flash), a power cycle, the autosave, a backup restored all brought them
 *                    back at their defaults; track 3's own nudges must stay its own
 * Build as tests/editor_fuzz.sh builds editor_fuzz.c (CLIPPER_SYNTH=1). */
#include "editor_host.c"

static void ed(uint32_t cmd, const uint8_t *a, uint32_t na)
{
    uint8_t f[700];
    f[0] = ED_HDR0; f[1] = ED_HDR1; f[2] = ED_HDR2; f[3] = (uint8_t)cmd;
    memcpy(f + 4, a, na);
    usb_sysex(f, 4 + na);                               /* through usb.c's SysEx assembly, as the editor sends it */
}
static uint32_t pack7(uint8_t *o, const uint8_t *p, uint32_t n)
{
    uint32_t w = 0;
    while (n) {
        uint32_t k = n > 7u ? 7u : n, m = 0, i;
        for (i = 0; i < k; i++) m |= (uint32_t)(p[i] >> 7) << i;
        o[w++] = (uint8_t)m;
        for (i = 0; i < k; i++) o[w++] = p[i] & 127u;
        p += k; n -= k;
    }
    return w;
}
static uint32_t v14(uint8_t *o, int32_t v) { uint32_t u = (uint32_t)(v + 8192); o[0] = u & 127u; o[1] = (u >> 7) & 127u; return 2; }

static int repro_wt_backup(void)
{
    uint8_t a[700], hdr[sizeof(smp_user_hdr_t)];
    wt_user_hdr_t *h = (wt_user_hdr_t *)hdr;
    uint32_t n, off, i, len = 99, pos;
    a[0] = 0; ed(ED_SMP_BEGIN, a, 1);                   /* a wavetable into USR1, as the editor's Samples page sends it */
    for (off = 0; off < WT_USER_LEN; off += 256) {
        uint8_t d[256];
        uint32_t o = SMP_USER_DATA + off;
        for (i = 0; i < 256; i++) d[i] = (uint8_t)(off / 256 + i * 7);
        n = 0; a[n++] = 0; a[n++] = o & 127u; a[n++] = (o >> 7) & 127u; a[n++] = (o >> 14) & 127u;
        n += pack7(a + n, d, 256);
        ed(ED_SMP_WRITE, a, n);
    }
    memset(hdr, 0xFF, sizeof hdr);
    memset(h, 0, sizeof *h);
    h->magic = WT_USER_MAGIC; h->version = 1; h->nf = WT_NF; h->nh = WT_NH; memcpy(h->name, "MYTABLE", 7);
    h->data_len = WT_USER_LEN; h->crc = st_crc32(nor + SMP_USER_OFF(0) + SMP_USER_DATA, WT_USER_LEN);
    n = 0; a[n++] = 0; n += pack7(a + n, hdr, sizeof hdr);
    ed(ED_SMP_END, a, n);
    printf("SMP_END rc %u; wt_user_ok[0] %u\n", rx_n > 6 ? rx_last[6] : 99u, wt_user_ok[0]);
    ed(ED_SMP_INFO, a, 0);
    printf("SMP_INFO: USR1 zones %u (127 = a wavetable)\n", rx_n > 7 ? rx_last[7] : 99u);
    ed(ED_BK_LIST, a, 0);                               /* -> rc, count, per object: id, len (5), crc (5) */
    for (pos = 7; pos + 11 <= rx_n; pos += 11)
        if (rx_last[pos] == 32u) {
            len = 0;
            for (i = 0; i < 5; i++) len |= (uint32_t)rx_last[pos + 1 + i] << (7 * i);
        }
    printf("BK_LIST: object 32 (USR1) %u bytes (the slot holds %u)\n", len, SMP_USER_DATA + WT_USER_LEN);
    if (len != SMP_USER_DATA + WT_USER_LEN)
        return 1;                                       /* (the editor's restore then sends SMP_ERASE: the table is gone) */
    {   /* read it as the editor's backup does, erase the slot, write it back as its restore does: the same table */
        static uint8_t img[SMP_USER_DATA + WT_USER_LEN], was[SMP_USER_DATA + WT_USER_LEN];
        memcpy(was, nor + SMP_USER_OFF(0) + SMP_USER_DATA, WT_USER_LEN);
        for (off = 0; off < len; off += 256) {
            uint32_t k, w;
            n = 0; a[n++] = 32; for (i = 0; i < 5; i++) a[n++] = (off >> (7 * i)) & 127u; a[n++] = 0; a[n++] = 2;
            ed(ED_BK_GET, a, n);
            if (rx_n < 13 || rx_last[6]) { printf("BK_GET rc %u\n", rx_n > 6 ? rx_last[6] : 99u); return 1; }
            for (k = 14, w = 0; w < 256 && k < rx_n - 1; ) {
                uint32_t m = rx_last[k++], j;
                for (j = 0; j < 7 && w < 256 && k < rx_n - 1; j++) img[off + w++] = (uint8_t)(rx_last[k++] | ((m >> j) & 1u) << 7);
            }
        }
        a[0] = 0; ed(ED_SMP_ERASE, a, 1);
        a[0] = 0; ed(ED_SMP_BEGIN, a, 1);
        for (off = SMP_USER_DATA; off < len; off += 256) {
            n = 0; a[n++] = 0; a[n++] = off & 127u; a[n++] = (off >> 7) & 127u; a[n++] = (off >> 14) & 127u;
            n += pack7(a + n, img + off, 256);
            ed(ED_SMP_WRITE, a, n);
        }
        n = 0; a[n++] = 0; n += pack7(a + n, img, sizeof(smp_user_hdr_t));
        ed(ED_SMP_END, a, n);
        printf("restored: SMP_END rc %u, wt_user_ok[0] %u, the spectra %s\n", rx_n > 6 ? rx_last[6] : 99u, wt_user_ok[0],
               memcmp(was, nor + SMP_USER_OFF(0) + SMP_USER_DATA, WT_USER_LEN) ? "differ" : "the same");
        return !wt_user_ok[0] || memcmp(was, nor + SMP_USER_OFF(0) + SMP_USER_DATA, WT_USER_LEN) != 0;
    }
}

static int repro_eq_low(void)
{
    uint8_t a[8];
    a[0] = TRK_DRUM; v14(a + 1, 127); a[3] = 0;
    ed(ED_TRACK_MIX, a, 4);
    printf("TRACK_MIX track 4 level 127: g[G_DRLVL] (%s) = %d, its range %d..%d\n", GP[G_DRLVL].label, song.g[G_DRLVL],
           GP[G_DRLVL].min, GP[G_DRLVL].max);
    return song.g[G_DRLVL] < GP[G_DRLVL].min || song.g[G_DRLVL] > GP[G_DRLVL].max;
}

/* read backup object id (n bytes) as the editor's backup does */
static int bk_read(uint32_t id, uint8_t *img, uint32_t n)
{
    uint8_t a[16];
    uint32_t off, i;
    ed(ED_BK_LIST, a, 0);
    for (off = 0; off < n; off += 256) {
        uint32_t c = n - off > 256 ? 256 : n - off, k, w, m;
        m = 0; a[m++] = (uint8_t)id; for (i = 0; i < 5; i++) a[m++] = (off >> (7 * i)) & 127u; a[m++] = c & 127u; a[m++] = (uint8_t)(c >> 7);
        ed(ED_BK_GET, a, m);
        if (rx_n < 13 || rx_last[6]) return 1;
        for (k = 14, w = 0; w < c && k < rx_n - 1; ) {
            uint32_t mm = rx_last[k++], j;
            for (j = 0; j < 7 && w < c && k < rx_n - 1; j++) img[off + w++] = (uint8_t)(rx_last[k++] | ((mm >> j) & 1u) << 7);
        }
    }
    return 0;
}
static int bk_write(uint32_t id, const uint8_t *img, uint32_t n)
{
    uint8_t a[700];
    uint32_t off, i, m, crc = st_crc32(img, n);
    m = 0; a[m++] = 0; a[m++] = (uint8_t)id; for (i = 0; i < 5; i++) a[m++] = (n >> (7 * i)) & 127u; for (i = 0; i < 5; i++) a[m++] = (crc >> (7 * i)) & 127u;
    ed(ED_BK_PUT, a, m);
    for (off = 0; off < n; off += 256) {
        uint32_t c = n - off > 256 ? 256 : n - off;
        m = 0; a[m++] = 1; a[m++] = (uint8_t)id; for (i = 0; i < 5; i++) a[m++] = (off >> (7 * i)) & 127u;
        m += pack7(a + m, img + off, c);
        ed(ED_BK_PUT, a, m);
    }
    a[0] = 2; a[1] = (uint8_t)id; ed(ED_BK_PUT, a, 2);
    return rx_n > 7 ? rx_last[7] : 99;
}
static int bk_listed(uint32_t id)                     /* the length BK_LIST gives object id, -1: not listed */
{
    uint8_t a[2];
    uint32_t pos, i, len;
    ed(ED_BK_LIST, a, 0);
    for (pos = 7; pos + 11 <= rx_n; pos += 11)
        if (rx_last[pos] == id) {
            for (len = 0, i = 0; i < 5; i++) len |= (uint32_t)rx_last[pos + 1 + i] << (7 * i);
            return (int)len;
        }
    return -1;
}
static int repro_up_backup(void)
{
    static uint8_t img[4][4096];
    static const uint8_t OBJ[4] = {6, 7, 12, 13};      /* (12, 13: the presets' side records and tails, once fixed) */
    int len[4];
    uint8_t a[16];
    static const uint8_t ID[3] = {P_SPREAD, P_MSRATE, P_VINT};
    int16_t want[3];
    uint32_t i, o;
    int bad = 0, rc;
    for (i = 0; i < 3; i++) {                           /* the sound: these three away from their defaults */
        const param_desc_t *d = &TP[ID[i]];
        want[i] = (int16_t)(d->def == d->max ? d->min : d->max);
        a[0] = 0; a[1] = ID[i]; v14(a + 2, want[i]); ed(ED_SET, a, 4);
    }
    a[0] = 0; memcpy(a + 1, "KEEP ME", 8); ed(ED_UP_STORE, a, 9);   /* -> user preset 1 */
    printf("UP_STORE rc %u\n", rx_n > 6 ? rx_last[6] : 99u);
    for (o = 0; o < 4; o++) {                           /* the editor's backup: every user preset object LIST gives */
        len[o] = bk_listed(OBJ[o]);
        if (len[o] > 0 && bk_read(OBJ[o], img[o], (uint32_t)len[o])) { printf("BK_GET %u failed\n", OBJ[o]); return 1; }
        printf("backup: object %u, %d bytes\n", OBJ[o], len[o]);
    }
    for (i = 0; i < 3; i++) { a[0] = 0; a[1] = ID[i]; v14(a + 2, TP[ID[i]].def); ed(ED_SET, a, 4); }
    a[0] = 0; memcpy(a + 1, "OTHER", 6); ed(ED_UP_STORE, a, 7);     /* another sound into slot 1 meanwhile */
    for (o = 0; o < 4; o++)                             /* the restore, in the editor's order (BK.RESTORE) */
        if (len[o] >= 0) {
            rc = bk_write(OBJ[o], img[o], (uint32_t)len[o]);
            printf("restore of object %u: rc %d\n", OBJ[o], rc);
        }
    a[0] = 0; ed(ED_UP_LOAD, a, 1);
    for (i = 0; i < 3; i++) {
        printf("%-6s stored %d, after the backup's restore %d (%s)\n", TP[ID[i]].label, want[i], trk[0].p[ID[i]], up_rec(0)->name);
        bad |= trk[0].p[ID[i]] != want[i];
    }
    up_boot();                                          /* the next power-on: the banks, their side records, tails from flash */
    a[0] = 0; ed(ED_UP_LOAD, a, 1);
    for (i = 0; i < 3; i++) {
        printf("%-6s after a power-on %d\n", TP[ID[i]].label, trk[0].p[ID[i]]);
        bad |= trk[0].p[ID[i]] != want[i];
    }
    return bad;
}

/* ---- 3: Clipper 1.0's values with steps on track 3 (an old SLOOP project loaded): a save, its load from RAM and from
 * flash, a power cycle, the autosave and its resume, the editor's backup and restore must all give them back, and track
 * 3's own nudges must stay its own (SLOOP plays them) */
static const uint8_t X10_ID[4] = {P_SPREAD, P_MAC3, P_E2LOOP, P_M7AMT};
static int16_t x10_want[4];
static void x10_set(int want)                           /* the four values and the delay MODE: wanted, or the defaults */
{
    uint8_t a[8];
    uint32_t i;
    for (i = 0; i < 4; i++) { a[0] = 0; a[1] = X10_ID[i]; v14(a + 2, want ? x10_want[i] : TP[X10_ID[i]].def); ed(ED_SET, a, 4); }
    a[0] = 1; a[1] = G_DMODE; v14(a + 2, want ? GP[G_DMODE].max : GP[G_DMODE].def); ed(ED_SET, a, 4);
}
static int x10_check(const char *when)
{
    uint32_t i;
    int bad = 0;
    printf("%-34s", when);
    for (i = 0; i < 4; i++) {
        printf(" %s %d/%d", TP[X10_ID[i]].label, trk[0].p[X10_ID[i]], x10_want[i]);
        bad |= trk[0].p[X10_ID[i]] != x10_want[i];
    }
    printf(" MODE %d/%d, track 3: step %u nudge %d", song.g[G_DMODE], GP[G_DMODE].max, trk[2].step[1].n, trk[2].micro[1]);
    bad |= song.g[G_DMODE] != GP[G_DMODE].max || trk[2].step[1].n != 1u || trk[2].micro[1] != -7;
    printf("  %s\n", bad ? "LOST" : "ok");
    return bad;
}
static void proj_op_on(uint32_t op, uint32_t slot)      /* ED_PROJECT op, then the audio a moment (its transport_req done) */
{
    uint8_t a[2];
    a[0] = (uint8_t)op; a[1] = (uint8_t)slot; ed(ED_PROJECT, a, 2);
    audio_blocks(8);
}
static void proj_op(uint32_t op) { proj_op_on(op, 0); }   /* .. on A */
static void power_cycle(void)                           /* a cold start: RAM's projects gone, the boot as main.c runs it */
{
    uint32_t i;
    memset(proj_slot, 0, sizeof proj_slot);
#ifdef PROJ_X10_MAGIC
    memset(proj_x10s, 0, sizeof proj_x10s);             /* (.pool: zeroed at boot) */
#endif
    for (i = 0; i < NTRK; i++) {                        /* the sounds as before the autosave resumes */
        uint32_t k;
        for (k = P_SPREAD; k < P_E0; k++) trk[i].p[k] = TP[k].def;
        memset(trk[i].step, 0, sizeof trk[i].step);
        memset(trk[i].micro, 0, sizeof trk[i].micro);
    }
    song.g[G_DMODE] = GP[G_DMODE].def;
    persist_boot();
    audio_blocks(8);
}
static int repro_x10(void)
{
    static uint8_t img[6][4096];
    static const uint8_t OBJ[6] = {2, 3, 4, 5, 0, 14};  /* (the editor's restore order; 14 once fixed) */
    int len[6], bad = 0;
    uint8_t a[16];
    uint32_t i;
    a[0] = 2; a[1] = 1; a[2] = 1; a[3] = 60; a[4] = a[5] = a[6] = 0; a[7] = ST_NOTE; a[8] = 0; a[9] = 100;
    ed(ED_TRACK_STEP, a, 10);                           /* track 3, step 2: a note (as an old SLOOP project has them) */
    a[0] = 2; a[1] = 1; a[2] = 64 - 7; ed(ED_MICRO_SET, a, 3);   /* .. nudged early */
    for (i = 0; i < 4; i++) {
        const param_desc_t *d = &TP[X10_ID[i]];
        x10_want[i] = (int16_t)(d->def == d->max ? d->min : d->max);
    }
    x10_set(1);
    proj_op(1);           /* save into A */
    x10_set(0);
    proj_op(0);           /* load A (from RAM) */
    bad |= x10_check("section A saved, loaded");
    x10_set(0);
    proj_slot[0].magic ^= 1u;                           /* (RAM's copy gone: from flash) */
    proj_op(0);
    bad |= x10_check("loaded from flash");
    power_cycle();
    proj_op(0);
    bad |= x10_check("after a power cycle, loaded");
    x10_set(0);                                         /* live section B (SAVE + B), written when quiet */
    section_store(1);
    sections_write();
    x10_set(1);                                         /* stored again with the values (the project's bytes the */
    section_store(1);                                   /* same: they are only beside it), written when quiet */
    sections_write();
#ifdef PROJ_X10_MAGIC
    memset(proj_x10s, 0, sizeof proj_x10s);             /* a warm reset: the sections kept in RAM (.noinit), the */
#endif                                                  /* blocks not (.pool) */
    sec_dirty = 0;
    persist_boot();
    audio_blocks(8);
    x10_set(0);
    proj_op_on(0, 1);
    bad |= x10_check("live section B, warm reset, B");
    power_cycle();
    x10_set(0);
    proj_op_on(0, 1);
    bad |= x10_check("live section B, power cycle, B");
    proj_op(0);
    fm1_ms += 60000u;                                   /* idle long enough: the autosave */
    ui_input_ms = fm1_ms - 10000u;
    autosave_tick();
    power_cycle();
    autosave_resume();
    audio_blocks(8);
    bad |= x10_check("after a power cycle, autosave resumed");
    for (i = 0; i < 6; i++) {                           /* the editor's backup: every listed project object (and 14) */
        len[i] = bk_listed(OBJ[i]);
        if (len[i] > 0 && bk_read(OBJ[i], img[i], (uint32_t)len[i])) { printf("BK_GET %u failed\n", OBJ[i]); return 1; }
    }
    for (i = 0; i < 5; i++) {                           /* restored into another unit: no section, no autosave in it */
        uint32_t c, obj = i < 4 ? OBJ_PROJECT0 + i : OBJ_AUTOSAVE;
        for (c = 0; c < 2; c++) st_erase(st_sector(obj, c));
    }
    power_cycle();
    x10_set(0);
    for (i = 0; i < 6; i++)
        if (len[i] >= 0)
        {
            printf("restore of object %u (%d bytes): rc %d\n", OBJ[i], len[i], bk_write(OBJ[i], img[i], (uint32_t)len[i]));
            audio_blocks(8);
        }
    bad |= x10_check("backup restored: the working sound");
    x10_set(0);
    proj_op(0);
    bad |= x10_check("backup restored: A loaded");
    power_cycle();
    proj_op(0);
    bad |= x10_check("backup restored, power cycle, A");
    return bad;
}

int main(int argc, char **argv)
{
    int which = argc > 1 ? atoi(argv[1]) : 1, r;
    harness_init();
    r = which == 1 ? repro_wt_backup() : which == 2 ? repro_eq_low() : which == 3 ? repro_x10() : repro_up_backup();
    printf("editor_repro %d: %s\n", which, r ? "FAIL (the bug is there)" : "ok");
    return r;
}
