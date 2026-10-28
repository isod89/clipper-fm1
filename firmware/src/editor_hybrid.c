/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* v11 (Clipper): the HYBRID engine's patches (eng_hybrid.c), EDITOR_PROTOCOL.md "HYBRID patches".
 *   HY_GET  target, index        -> target, index, rc, [pack7 of the 128-byte patch]
 *           target 0: the part `index` plays now (0..2), 1: factory patch `index`
 *   HY_PUT  0, part, pack7 (128) -> 0, part, rc     the part's own patch from its next block (eng_hybrid.c
 *           hy_set_patch: sanitised), until PTCH turns or a sound loads
 * rc: 0 ok, 1 bad request. The patch is the C layout of hy_patch_t (signed bytes as two's complement): the
 * editor packs and unpacks it field by field (editor.html HY). */
#ifndef UI_SMALL
#define UI_SMALL                                   /* (gfx.c: minsize on the device; a test alone: nothing) */
#endif
enum { ED_HY_GET = 77, ED_HY_PUT };
enum { ED_HY_TRACK, ED_HY_FACTORY };

UI_SMALL static int ed_hy_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    hy_patch_t p;
    uint32_t rc;
    switch (cmd) {
    case ED_HY_GET:
        rc = n != 2u ? 1u : 0u;
        if (!rc && a[0] == ED_HY_TRACK && a[1] < NPART)
            p = hy_gen[a[1]] != hy_gen_seen[a[1]] ? hy_in[a[1]] : hy_cur[a[1]];   /* (sent, not played yet: it) */
        else if (!rc && a[0] == ED_HY_FACTORY && a[1] < HY_NFACTORY)
            p = HY_FACTORY[a[1]];
        else
            rc = 1;
        ed_b(n ? a[0] : 127u);
        ed_b(n > 1u ? a[1] : 127u);
        ed_b(rc);
        if (!rc)
            ed_pack7((const uint8_t *)&p, HY_PATCH_SIZE);
        return 1;
    case ED_HY_PUT:
        rc = n < 2u || a[0] != ED_HY_TRACK || a[1] >= NPART ||
             ed_unpack7(a + 2, n - 2u, (uint8_t *)&p, HY_PATCH_SIZE) != HY_PATCH_SIZE;
        if (!rc) {
            hy_set_patch(a[1], &p);
            ui.force = 1;
        }
        ed_b(n ? a[0] : 127u);
        ed_b(n > 1u ? a[1] : 127u);
        ed_b(rc);
        return 1;
    }
    return 0;
}
