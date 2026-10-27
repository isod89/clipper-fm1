/* SLOOP 2.8.2: a note held while others are played over it (reddit "SLOOP 2.8.1 chopping notes"). Clipper: on the
 * instrument (track 1, the only one the sequencer and MIDI play), its sound set to POLY or MONO.
 *   live      POLY: hold C1, play C2 D2 E2 over it while the pattern plays and records: C1 is never
 *             restarted nor released while its key is down
 *   playback  the take plays back: C1 sounds from its start to its end in one go, the scale over it
 *   len       SEQ > NOTES, LEN: a note's length runs on under the notes after it (POLY)
 * Exit status: the number of failed checks. */
#define FELUCCA_ARRANGER 1
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i) { return i < NPART ? i : 0; }
#include "../firmware/src/project.c"
static struct { uint8_t force; } ui;
static uint8_t sync_reload;
#include "../firmware/src/arranger_scene.c"

static int fails;
static void check(int ok, const char *what) { printf("hold: %-74s %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }
static uint64_t blk;
static void block(void) { int32_t out[CTL * 2]; mix_block(out, CTL); blk++; }
/* C1's voice on track t: its age (a restart makes a new one), gated or not; 0 = no gated voice */
static uint32_t voice_of(track_t *t, uint32_t note, int *gated)
{
    uint32_t i;
    *gated = 0;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].active && t->v[i].note == note && t->v[i].gate) { *gated = 1; return t->v[i].age; }
    return 0;
}
static uint32_t SB;   /* blocks a step (1/16 at 120 BPM) */
static void steps(uint32_t n) { uint32_t i; for (i = 0; i < n * SB; i++) block(); }
static void reset(uint32_t ti, int voice)
{
    uint32_t i;
    host_tracks_init();
    for (i = 0; i < NTRK; i++) { steps_clear(&trk[i]); trk_all_off(&trk[i]); }
    memset(&drums, 0, sizeof drums);
    drums.set = -2;
    song.g[G_BPM] = 120;
    host_preset(&trk[ti], ti == 0 ? 0 : 1, 0);
    if (voice >= 0) trk[ti].p[P_VOICE] = (int16_t)voice;
    trk[ti].p[P_SLEN] = 16;
    song.sel = (uint8_t)ti;
    blk = 0;
}

/* live: transport on, record armed; C1 down at step 0, C2 D2 E2 (two steps each) from step 2, C1 up at step 10.
 * Returns the restarts / releases of C1 seen while its key was down */
static void live_take(uint32_t ti, int voice, const char *what, int pass2)
{
    track_t *t = &trk[ti];
    const uint32_t C1 = 36, SC[3] = {48, 50, 52};
    uint32_t age0, restarts = 0, cuts = 0, k, b, i;
    int g;
    reset(ti, voice);
    SB = div_samples(2) / CTL;
    song.rec = (uint8_t)(1u << ti);
    rec_wait = 0;
    transport_req = 1;
    block();
    /* (a pass already recorded under it, for the overdub case) */
    input_on(t, C1, 100);
    for (i = 0; i < 4; i++) block();
    age0 = voice_of(t, C1, &g);
    for (k = 0; k < 12u * SB; k++) {
        uint32_t st = k / SB, at = k % SB;
        if (at == 0 && st >= 2 && st < 8 && st % 2 == 0) input_on(t, SC[(st - 2) / 2], 100);
        if (at == SB - 2 && st >= 2 && st < 8 && st % 2 == 1) input_off(t, SC[(st - 3) / 2]);
        if (at == 0 && st == 10) input_off(t, C1);
        block();
        if (st < 10) {
            uint32_t a = voice_of(t, C1, &g);
            if (!g) { if (!cuts && getenv("DBG")) printf("  cut at step %u +%u blocks\n", st, at); cuts++; }
            else if (a != age0) { if (getenv("DBG")) printf("  restart at step %u +%u\n", st, at); restarts++; age0 = a; }
        }
    }
    {
        char b2[160];
        snprintf(b2, sizeof b2, "%s, live: C1 held under the scale (restarts %u, cut %u blocks)", what, restarts, cuts);
        check(!restarts && !cuts, b2);
        voice_of(t, C1, &g);
        snprintf(b2, sizeof b2, "%s, live: C1 ends when its key is let go", what);
        check(!g, b2);
    }
    if (!pass2) return;
    /* playback: the rest of the bar, then the take plays on its own (record off) */
    song.rec = 0;
    while (t->seq_idx != 0 || (clk_pos != 0 && 0)) block();
    {
        uint32_t first = 0, gated = 0, restarts2 = 0, a0 = 0;
        for (k = 0; k < 10u * SB; k++) {
            uint32_t a;
            block();
            a = voice_of(t, C1, &g);
            if (k < SB) continue;
            if (g) { gated++; if (!a0) a0 = a; else if (a != a0) { restarts2++; a0 = a; } }
        }
        {
            char b2[160];
            snprintf(b2, sizeof b2, "%s, playback: C1 sounds under the scale in one go (gated %u/%u, restarts %u)", what, gated, 9u * SB, restarts2);
            check(gated >= 9u * SB - 2u && !restarts2, b2);
        }
    }
    transport_req = 2;
    block();
}

static void dump(track_t *t)
{
    uint32_t i, k;
    for (i = 0; i < 12; i++) {
        step_t *x = &t->step[i];
        printf("  %2u %s", i, x->time == ST_TIE ? "TIE " : x->time == ST_REST ? "REST" : "NOTE");
        for (k = 0; k < x->n; k++) printf(" %u%s", x->note[k], (x->flags & SF_CONT(k)) ? "c" : "");
        printf("\n");
    }
}
/* overdub: a scale recorded first (pass 1), then C1 held over it from step 0 to step 10 (pass 2): it plays back
 * in one go under the scale (2.8.1 ended the hold at the first step with notes) */
static void overdub(uint32_t ti, int voice, const char *what, uint32_t rel)
{
    track_t *t = &trk[ti];
    const uint32_t C1 = 36, SC[3] = {48, 50, 52};
    uint32_t k, gated = 0, restarts = 0, a0 = 0, a;
    int g;
    char b2[160];
    reset(ti, voice);
    SB = div_samples(2) / CTL;
    song.rec = (uint8_t)(1u << ti);
    rec_wait = 0;
    transport_req = 1;
    block();
    for (k = 0; k < 16u * SB; k++) {                     /* pass 1: the scale, steps 2..7 */
        uint32_t st = k / SB, at = k % SB;
        if (at == 0 && st >= 2 && st < 8 && st % 2 == 0) input_on(t, SC[(st - 2) / 2], 100);
        if (at == SB - 2 && st >= 2 && st < 8 && st % 2 == 1) input_off(t, SC[(st - 3) / 2]);
        block();
    }
    for (k = 0; k < 16u * SB; k++) {                     /* pass 2: C1 held from step 0 to step 10 */
        uint32_t st = k / SB, at = k % SB;
        if (at == 0 && st == 0 && !getenv("NOC1")) input_on(t, C1, 100);
        if (at == 0 && st == rel && !getenv("NOC1")) input_off(t, C1);
        block();
    }
    song.rec = 0;
    if (getenv("DUMP")) dump(t);
    uint32_t late = 0, scale_on = 0, scale_late = 0;
    for (k = 0; k < 16u * SB; k++) {                     /* pass 3: plays back */
        block();
        { int g2; uint32_t q; for (q = 0; q < 3; q++) { voice_of(t, SC[q], &g2); if (g2) { if (k / SB >= 2 && k / SB < 8) scale_on++; if (k / SB >= 9) scale_late++; } } }
        a = voice_of(t, C1, &g);
        if (g && k / SB >= rel + 1u) late++;
        if (k / SB >= 1 && k / SB < rel - 1u) { if (g) { gated++; if (!a0) a0 = a; else if (a != a0) { restarts++; a0 = a; } } }
    }
    snprintf(b2, sizeof b2, "%s, overdub to step %u: C1 held over a recorded scale plays back whole (%u/%u, restarts %u)", what, rel, gated, (rel - 2u) * SB, restarts);
    check(gated >= (rel - 2u) * SB - 2u && !restarts, b2);
    snprintf(b2, sizeof b2, "%s, overdub: C1 ends where it was let go (%u late blocks)", what, late);
    check(!late, b2);
    snprintf(b2, sizeof b2, "%s, overdub: the scale plays as recorded (%u blocks), and ends (%u late)", what, scale_on, scale_late);
    check(scale_on >= 5u * SB && !scale_late, b2);
    transport_req = 2;
    block();
}

/* MIDI in overflow: a burst of 80 note-ons then their note-offs, faster than the audio reads them: the ring drops some
 * note-offs; nothing stays held (2.8.1: the notes whose note-off was lost hung) */
static void midi_burst(void)
{
    track_t *t = &trk[0];
    uint32_t i, k, hung = 0;
    reset(0, V_POLY);
    song.g[G_ROUTE] = 0;
    for (k = 0; k < 4; k++) block();
    for (i = 0; i < 40u; i++)                           /* (USB packets: cable 0, CIN 9 / 8, channel 1 = track 1? no: CH of track 2) */
        midi_in_event(0x09u | 0x90u << 8 | (36u + i) << 16 | 100u << 24);   /* (Clipper: OMNI, the instrument) */
    for (i = 0; i < 40u; i++)
        midi_in_event(0x08u | 0x80u << 8 | (36u + i) << 16);
    for (k = 0; k < 200; k++) block();
    for (i = 0; i < NVOICE; i++)
        hung += t->v[i].active && t->v[i].gate;
    check(!hung, "MIDI in: a burst over the ring's 64: no note left hanging");
}

static void mono_note(void)
{
    track_t *t = &trk[0];
    reset(0, V_MONO);                                   /* (Clipper: the sound set to MONO) */
    song.rec = 1u;
    rec_wait = 0;
    transport_req = 1;
    block();
    mono_hint = 0;
    input_on(t, 36, 100);
    block();
    check(!mono_hint, "MONO sound, recording: one key: nothing said");
    input_on(t, 48, 100);
    block();
    check(mono_hint == 1u, "MONO sound, recording: a key over a held one: MONO: SET VOICE POLY");
    input_off(t, 48); input_off(t, 36);
    transport_req = 2;
    block();
}

int main(void)
{
    live_take(0, V_POLY, "the sound (POLY)", 1);
    midi_burst();
    mono_note();
    overdub(0, V_POLY, "the sound (POLY)", 10);
    overdub(0, V_POLY, "the sound (POLY)", 5);
    overdub(0, V_POLY, "the sound (POLY)", 7);
    printf("hold: %d failed\n", fails);
    return fails;
}
