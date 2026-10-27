/* SPDX-License-Identifier: GPL-3.0-only */
/* Every engine's EDIT values, as the screen shows them (params.c param_format), over each one's whole range: an F_INT
 * value with a name list reads that list up to its 0 (the names split the range), so a list without its 0 read past
 * its end. A Clipper report: the FM KICK preset (FMX), its ALG turned past 32: a FELUCCA CRASH and a glitched screen
 * (N_FMX_ALG and N_FMX_PATCH had no 0 at their end; the screen took a pointer from what came after them). Run under
 * ASan (tests/run_tests.sh): an overrun stops it. Exit status: the number of failed checks. */
#define main hostsim_main
#include "hostsim.c"
#undef main
int main(void)
{
    uint32_t e, k, n = 0, fails = 0;
    char val[32];
    const char *unit;
    for (e = 0; e < NENGINES; e++) {
        const engine_t *eng = ENGINES[e];
        for (k = 0; k < 8u; k++) {
            const param_desc_t *d = &eng->edit[k];
            int32_t v;
            if (!d->label)
                continue;
            if (d->fmt == F_INT && d->names) {            /* the list: its 0 within reach (a sane length) */
                uint32_t c = 0;
                while (c < 512u && d->names[c])
                    c++;
                if (c >= 512u) {
                    printf("names: %s %s: no 0 at the end of its names FAIL\n", eng->name, d->label);
                    fails++;
                    continue;
                }
            }
            for (v = d->min; v <= d->max; v++, n++)
                param_format(d, v, val, &unit);
        }
    }
    printf("names: %u values of %u engines formatted%s\n", n, (uint32_t)NENGINES, fails ? "" : " ok");
    printf("names: %u failed\n", fails);
    return (int)fails;
}
