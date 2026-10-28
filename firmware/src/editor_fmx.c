/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* v12 (Clipper 0.6): the FMX engine's patches (eng_fmx.c), EDITOR_PROTOCOL.md "FMX patches".
 *   FMX_GET    target, index        -> target, index, rc, [pack7 of the 128-byte patch]
 *              target 0: the part `index` plays now (0..2), 1: factory patch `index`, 2: bank slot `index` (0..26:
 *              an FMX record as it is, a DX7 voice converted, an empty slot rc 2)
 *   FMX_PUT    0, part, pack7 (128) -> 0, part, rc     the part's patch from its next block (fmx_sanitize), until a
 *              sound loads or PTCH turns (or STORE)
 *   FMX_STORE  slot, pack7 (128)    -> slot, rc        into bank slot `slot` (0..26), marked FMX's (fm6_bank.c); the
 *              song stopped (rc 3 while it plays: the flash write silences the audio a moment)
 * rc: 0 ok, 1 bad request, 2 empty slot / flash error, 3 the song plays. Every byte of the patch 0..127. */
#ifndef UI_SMALL
#define UI_SMALL                                   /* (gfx.c: minsize on the device; a test alone: nothing) */
#endif
enum { ED_FMX_GET = 90, ED_FMX_PUT, ED_FMX_STORE };
enum { ED_FMX_TRACK, ED_FMX_FACTORY, ED_FMX_BANK };

UI_SMALL static int ed_fmx_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    fmx_patch_t p;
    uint32_t rc = 0;
    switch (cmd) {
    case ED_FMX_GET:
        if (n != 2u)
            rc = 1;
        else if (a[0] == ED_FMX_TRACK && a[1] < NPART)
            p = fmx_cur[a[1]];
        else if (a[0] == ED_FMX_FACTORY && a[1] < FMX_NFACTORY)
            p = FMX_FACTORY[a[1]];
        else if (a[0] == ED_FMX_BANK && a[1] < FM6_BANK_N) {
            uint8_t pk[FM6_PACKED];
            if (!fm6_bank_read || fm6_bank_read(a[1], pk))
                rc = 2;
            else
                fmx_slot_patch(FMX_NFACTORY + a[1], &p);
        } else
            rc = 1;
        ed_b(n ? a[0] : 127u);
        ed_b(n > 1u ? a[1] : 127u);
        ed_b(rc);
        if (!rc)
            ed_pack7((const uint8_t *)&p, sizeof p);
        return 1;
    case ED_FMX_PUT:
        rc = n < 2u || a[0] != ED_FMX_TRACK || a[1] >= NPART ||
             ed_unpack7(a + 2, n - 2u, (uint8_t *)&p, sizeof p) != sizeof p;
        if (!rc) {
            fmx_sanitize(&p);
            fm1_irq_off();
            fmx_cur[a[1]] = p;
            fmx_dirty[a[1]] = 1;
            fm1_irq_on();
            ui.force = 1;
        }
        ed_b(n ? a[0] : 127u);
        ed_b(n > 1u ? a[1] : 127u);
        ed_b(rc);
        return 1;
    case ED_FMX_STORE:
        rc = n < 1u || a[0] >= FM6_BANK_N || ed_unpack7(a + 1, n - 1u, (uint8_t *)&p, sizeof p) != sizeof p;
        if (!rc && (song.playing || transport_req))
            rc = 3;
        if (!rc) {
            fmx_sanitize(&p);
            rc = !fmx_bank_store || fmx_bank_store(a[0], (const uint8_t *)&p) ? 2u : 0u;
        }
        ed_b(n ? a[0] : 127u);
        ed_b(rc);
        return 1;
    }
    return 0;
}

/* v13 (Clipper 0.8): the MOD SEQ's steps of a part (its RATE LEN SMTH START, SPICE, DICE: parameters as any other)
 *   MSEQ_GET  part              -> part, rc, 64 bytes (lane 1's 16 steps, then lane 2's ..: each value + 8, 0..15)
 *   MSEQ_PUT  part, 64 bytes    -> part, rc        the part's steps at once (each clamped to -8..7)
 * rc: 0 ok, 1 bad request */
enum { ED_MSEQ_GET = 93, ED_MSEQ_PUT, ED_TUNE_GET, ED_TUNE_PUT };
/* v14 (Clipper 0.9): the TUNING (voice.c tune_cfg), kept with the settings
 *   TUNE_GET                                   -> rc, system, root, n, period (3 x 7 bits), n - 1 degrees (3 x 7 bits),
 *                                                 (1.0) A4 - 400 Hz (0..80)
 *   TUNE_PUT  system, root, n, period, degrees [, A4 - 400] -> rc   (the USER scale: n 1..48 degrees of a period, 1/16
 *             cent, each above the one before; from a Scala .scl: the editor converts it. 1.0: the A4 byte optional,
 *             without it A4 stays as it is; 0.9 took no such byte, so the editor sends it only when GET gave one)
 * rc: 0 ok, 1 bad request */
UI_SMALL static uint32_t ed_u21(const uint8_t *a) { return (uint32_t)(a[0] & 127u) | (uint32_t)(a[1] & 127u) << 7 | (uint32_t)(a[2] & 127u) << 14; }
UI_SMALL static int ed_mseq_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    uint32_t i, rc;
    switch (cmd) {
    case ED_MSEQ_GET:
        rc = n != 1u || a[0] >= NPART;
        ed_b(n ? a[0] : 127u);
        ed_b(rc);
        if (!rc)
            for (i = 0; i < MSEQ_LANES * MSEQ_STEPS; i++)
                ed_b((uint32_t)(trk[a[0]].ms[i / MSEQ_STEPS][i % MSEQ_STEPS] + 8));
        return 1;
    case ED_TUNE_GET:
        ed_b(0);
        ed_b(tune_cfg.sys);
        ed_b(tune_cfg.root);
        ed_b(tune_cfg.n);
        for (i = 0; i < 3u; i++)
            ed_b((tune_cfg.period >> (7u * i)) & 127u);
        for (i = 1; i < tune_cfg.n && i < TU_MAXDEG; i++) {
            uint32_t k;
            for (k = 0; k < 3u; k++)
                ed_b((tune_cfg.deg[i] >> (7u * k)) & 127u);
        }
        ed_b((uint32_t)(clamp(tune_cfg.a4, -40, 40) + 40));   /* (1.0) A4 - 400 Hz */
        return 1;
    case ED_TUNE_PUT: {
        tune_t c = tune_cfg;
        uint32_t nd = n >= 6u ? a[2] : 0u, prev = 0;
        rc = n < 6u || a[0] >= TU_N || a[1] > 11u || nd < 1u || nd > TU_MAXDEG ||
             (n != 6u + 3u * (nd - 1u) && (n != 7u + 3u * (nd - 1u) || a[n - 1u] > 80u));   /* (1.0: + A4) */
        if (!rc) {
            c.sys = a[0], c.root = a[1], c.n = (uint8_t)nd;
            c.period = (uint16_t)clamp((int32_t)ed_u21(a + 3), 16, 65535);
            if (n == 7u + 3u * (nd - 1u))
                c.a4 = (int8_t)((int32_t)a[n - 1u] - 40);
            memset(c.deg, 0, sizeof c.deg);
            for (i = 1; i < nd && !rc; i++) {
                uint32_t d = ed_u21(a + 6u + 3u * (i - 1u));
                rc = d <= prev || d >= c.period;        /* (each above the one before, inside the period) */
                c.deg[i] = (uint16_t)d, prev = d;
            }
        }
        if (!rc) {
            tune_cfg = c;
            tune_build();
            settings_later = 1;
            ui.force = 1;
        }
        ed_b(rc);
        return 1;
    }
    case ED_MSEQ_PUT:
        rc = n != 1u + MSEQ_LANES * MSEQ_STEPS || a[0] >= NPART;
        if (!rc) {
            for (i = 0; i < MSEQ_LANES * MSEQ_STEPS; i++)
                trk[a[0]].ms[i / MSEQ_STEPS][i % MSEQ_STEPS] = (int8_t)clamp((int32_t)a[1u + i] - 8, -8, 7);
            ui.force = 1;
        }
        ed_b(n ? a[0] : 127u);
        ed_b(rc);
        return 1;
    }
    return 0;
}
