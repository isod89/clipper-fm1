/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Voice allocation, envelopes, LFO and per-track rendering. Runs in the
 * audio ISR.
 *
 * Each synth part has its own NVOICE voices (so MONO / LEGATO / UNISON keep using
 * v[0..]), but only NVOICE of all the parts' voices sound at once: a part that starts
 * a voice while the budget is full takes one from any part (voice_victim): the oldest
 * released voice, else the oldest extra UNISON voice, else the oldest held voice of a
 * POLY part that is not its lowest note. A voice taken from another part fades out
 * over one block (stage 4: the envelope goes to 0, the block's amplitude ramp
 * declicks it); one of the part's own is restarted in place. Extra UNISON
 * voices only start when there is room. The drum track has its own voices (drums.c). */
static uint32_t vage;                                   /* voice ages: one clock for every part */
/* engines that play recorded material (a position, not a phase): no phases kept or spread */
#if FELUCCA_SLICE
static int eng_sampled(const engine_t *e) { return e == &ENG_SAMPLE || e == &ENG_SLICE; }
#else
static int eng_sampled(const engine_t *e) { return e == &ENG_SAMPLE; }
#endif
/* an LFO's wave (N_LWAVE) at phase ph; rnd: its S&H value, Q15 */
static int32_t lfo_wave_n(int32_t wave, uint32_t ph, int32_t rnd)
{
    switch (wave) {
    case 1:
        return osc_tri(ph);
    case 2:
        return (int32_t)(ph >> 16) - 32768;
    case 3:
        return ph < 0x80000000u ? 32767 : -32767;
    case 4:
        return rnd;
    default:
        return osc_sine(ph);
    }
}
static int32_t lfo_wave(track_t *t, uint32_t ph)
{
    return lfo_wave_n(t->p[P_LWAVE], ph, (int32_t)(t->lfo_rnd >> 16) - 32768);
}

static uint8_t midi_breath;                             /* (1.0) GLO > MIDI 2 > EWI (seq.c): 0 OFF, 1 VOL, 2 V+AT */
/* (1.1, the EWI player again: "a soft note adds a second one, at full level") an EWI is a monophonic instrument and
 * its breath is the level: with EWI on, the instrument (and its LAYER) plays one note at a time, legato, whatever
 * the sound's VOICE (a note fingered over the last one moves it, no second voice); the notes' velocity is fixed
 * (seq.c), the breath alone shapes the level */
static inline uint32_t voice_mode(const track_t *t)
{
    return CLIPPER_SYNTH && midi_breath && (uint32_t)(t - trk) < NPART && ctl_part(t) == 0u ? (uint32_t)V_LEGATO
                                                                                   : (uint32_t)t->p[P_VOICE];
}
static uint8_t midi_bend;                               /* (1.0) GLO > MIDI > BEND, or RPN 0 (seq.c): the pitch bend's
                                                         * range in semitones, 0 AUTO (the HYBRID patch's, else 2) */
#if CLIPPER_SYNTH
/* Clipper 0.4: MPE (menu SYSTEM > MPE; seq.c MIDI in). Channel 1 is the master channel (the part's bend, pressure,
 * CCs, as ever); a note on a member channel (2..16) is that channel's own: its pitch bend (over the range chosen
 * here, 48 semitones by the MPE standard), its pressure (as a polyphonic aftertouch: AT) and its CC 74 (SLIDE, a
 * matrix source; with no slot taking it, the cutoff) follow it alone (voice.c track_render, v->ch) */
#define MPE_MODES 6u
static uint8_t mpe_mode;                                /* 0 off, else the member channels' bend range (MPE_RANGE) */
static const char *const MPE_NAME[MPE_MODES] = {"OFF", "48 ST", "2 ST", "12 ST", "24 ST", "96 ST"};
static const uint8_t MPE_RANGE[MPE_MODES] = {0, 48, 2, 12, 24, 96};
static int16_t mpe_bend[16];                            /* per channel: Q15, as part_ctl's */
static uint8_t mpe_press[16];                           /* 0..127 */
static int8_t mpe_slide[16];                            /* CC 74 - 64: 0 at rest */
static uint8_t mpe_cur_ch;                              /* the channel of the note being started (seq.c), 0 = none */
static void mpe_reset(void)
{
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        mpe_bend[i] = 0;
        mpe_press[i] = 0;
        mpe_slide[i] = 0;
    }
    mpe_cur_ch = 0;
}
#endif

static void track_lfo_tick(track_t *t)
{
    uint32_t old = t->lfo_ph;
    t->lfo_ph += LFO_INC[t->p[P_LRATE] & 127];
    if (t->lfo_ph < old)
        t->lfo_rnd = rng();
    t->lfo_val = lfo_wave(t, t->lfo_ph);
    if (t->lfo_fade < 32767) {
        int32_t step = (int32_t)(ENV_LIN[t->p[P_LFADE] & 127] >> 9);
        t->lfo_fade = t->p[P_LFADE] ? clamp(t->lfo_fade + (step ? step : 1), 0, 32767) : 32767;
    }
}

/* voices the engine may use (POLY and UNISON): its cap, else all of them */
static uint32_t trk_nvoice(const track_t *t)
{
    uint32_t c = ENGINES[t->engine] == &ENG_PHYS ? phys_poly(t) : ENGINES[t->engine]->poly;   /* (PHYS: by MODEL) */
    return c && c < NPOLY ? c : NPOLY;
}

/* ------------------------------------------------- the shared voice budget --- */
static uint32_t voices_busy(void)                       /* sounding voices of all parts (not the fading ones) */
{
    uint32_t p, i, n = 0;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            n += trk[p].v[i].active && trk[p].v[i].stage != 4u;
    return n;
}

static uint32_t lowest_held(const track_t *t)           /* index of the lowest held note (the bass), NVOICE = none */
{
    uint32_t i, low = NVOICE;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].active && t->v[i].gate && (low == NVOICE || t->v[i].note < t->v[low].note))
            low = i;
    return low;
}

/* the voice to give up for a new one (see the top); soft: only released voices and
 * extra UNISON voices of other parts. Returns its index, *pp its part; NVOICE = none */
static uint32_t voice_victim(const track_t *self, int soft, track_t **pp)
{
    uint32_t p, i, best = NVOICE, cat = 4;
    for (p = 0; p < NPART; p++) {
        track_t *t = &trk[p];
        uint32_t mode = voice_mode(t), nu = mode == V_UNISON ? trk_nvoice(t) : 1u;
        uint32_t low = voice_poly((int)mode) ? lowest_held(t) : NVOICE;
        if (soft && t == self)
            continue;
        for (i = 0; i < NVOICE; i++) {
            const voice_t *v = &t->v[i];
            uint32_t c;
            if (!v->active || v->stage == 4u)
                continue;
            if (!v->gate)
                c = 1;                                  /* released */
            else if (mode == V_UNISON && i > 0 && i < nu)
                c = 2;                                  /* an extra UNISON voice */
            else if (!soft && voice_poly((int)mode) && i != low)
                c = 3;                                  /* held, not the bass */
            else
                continue;
            if (c < cat || (c == cat && v->age < (*pp)->v[best].age)) {
                cat = c;
                best = i;
                *pp = t;
            }
        }
    }
    return best;
}

static uint32_t voice_kills;                            /* voices given up (budget, overload): console, hostsim */
#define KILL_BLOCKS 2u                                  /* 1.5 ms: no click on a bass, little extra load */
static void voice_kill(voice_t *v)                      /* fade out over the next blocks (env_tick stage 4) */
{
    v->gate = 0;
    if (v->stage != 4u)
        v->kill = KILL_BLOCKS;
    v->stage = 4;
    voice_kills++;
}

/* overload (after Felucca 1.0; audio.c asks for it when two halves in a row took > 85 % of their time):
 * fade one voice out over the next blocks (voice_kill: no click), one more each half while it lasts. A
 * single late half (a USB burst, a flash write) sheds nothing. First the quietest releasing voice; else
 * the oldest held one that is not a part's lowest POLY note nor its MONO / LEGATO / UNISON lead, as the
 * voice budget keeps them: a dense chord on a heavy engine thins out from the top, bass and lead stay. */
static uint32_t shed_count;

static void shed_voice(void)
{
    uint32_t p, i;
    voice_t *best = 0;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++) {
            voice_t *v = &trk[p].v[i];
            if (v->active && !v->gate && v->stage != 4u && (!best || v->env < best->env))
                best = v;
        }
    if (best) {
        voice_kill(best);
        shed_count++;
        return;
    }
    for (p = 0; p < NPART; p++) {
        const track_t *t = &trk[p];
        uint32_t mode = voice_mode(t), low = voice_poly((int)mode) ? lowest_held(t) : 0u;
        for (i = 0; i < NVOICE; i++) {
            voice_t *v = &trk[p].v[i];
            if (v->active && v->gate && v->stage != 4u &&
                ((voice_poly((int)mode) && i != low) || (!voice_poly((int)mode) && i > 0u)) && (!best || v->age < best->age))
                best = v;
        }
    }
    if (best) {
        voice_kill(best);
        shed_count++;
    }
}

/* the budget is full: free a voice for part t. 0 = nothing to take (soft) */
static int voice_room(track_t *t, int soft)
{
    track_t *vp = 0;
    uint32_t k;
    if (voices_busy() < NVOICE)
        return 1;
    k = voice_victim(t, soft, &vp);
    if (k == NVOICE)
        return !soft;                                   /* hard: over the budget (cannot happen with 3 parts) */
    voice_kill(&vp->v[k]);
    return 1;
}

/* POLY allocation (within the engine's voice cap). Same note: reuse its voice. Else a free voice: ROTATE takes
 * the next one round-robin (release tails ring out), REUSE (or any GLIDE) takes
 * the free voice whose last pitch is closest, so poly portamento moves each
 * voice the shortest way. Steal: the oldest released voice, else the oldest
 * held one, never the lowest held note (the bass). A free voice needs room in the
 * shared budget: when the voice to give up is one of this part's, it is restarted
 * in place instead. */
static voice_t *voice_alloc(track_t *t, uint32_t note)
{
    uint32_t i, np = trk_nvoice(t), best = np, low = np, nfree = 0;
    int32_t bd = 0x7FFFFFFF;
    for (i = 0; i < np; i++) {
        if (t->v[i].active && t->v[i].note == note) {
            if (t->v[i].stage == 4u && voices_busy() >= NVOICE)
                voice_room(t, 0);                       /* (SLOOP 2.8.3) a voice fading out (given up) sounds again: within the
                                                         * budget of NVOICE, as a new one (after Melodee's voice_reuse) */
            return &t->v[i];
        }
        nfree += !t->v[i].active;
    }
    if (nfree && voices_busy() >= NVOICE) {
        track_t *vp = 0;
        uint32_t k = voice_victim(t, 0, &vp);
        if (k < np && vp == t)
            return &t->v[k];                            /* our own: restart it in place (no click) */
        if (k < NVOICE)
            voice_kill(&vp->v[k]);
    }
    if (t->p[P_ALLOC] || t->p[P_GLIDE]) {
        for (i = 0; i < np; i++) {
            int32_t d = t->v[i].pitch_cur - (int32_t)note * 16;
            if (t->v[i].active)
                continue;
            d = d < 0 ? -d : d;
            if (!t->v[i].pitch_cur)
                d = 0x7FFFFFF0;                          /* never played: last choice */
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
    } else {
        for (i = 0; i < np && best == np; i++) {
            uint32_t k = (t->rr + i) % np;
            if (!t->v[k].active)
                best = k;
        }
        t->rr = (uint8_t)((best == np ? t->rr : best) + 1u) % np;
    }
    if (best < np)
        return &t->v[best];
    for (i = 0; i < np; i++)                             /* the lowest held note is protected */
        if (t->v[i].gate && (low == np || t->v[i].note < t->v[low].note))
            low = i;
    for (i = 0; i < np; i++)                             /* oldest released (not one fading out for another part) */
        if (!t->v[i].gate && t->v[i].stage != 4u && (best == np || t->v[i].age < t->v[best].age))
            best = i;
    if (best == np)
        for (i = 0; i < np; i++)                         /* oldest held, not the bass */
            if (i != low && (best == np || t->v[i].age < t->v[best].age))
                best = i;
    return &t->v[best == np ? 0 : best];
}

/* glide, in 1/4096 semitone a control tick (pitch_cur . pitch_frac): RATE = GLIDE is the time an octave
 * takes (any interval at that speed), TIME = GLIDE is the time of any glide. A sequencer SLIDE with
 * GLIDE 0 glides in SLIDE_T (TIME), as an acid slide */
#define SLIDE_T 54                                       /* TIME_MS index: ~50 ms */
static int32_t glide_ticks(int32_t g)                    /* control ticks of TIME_MS index g */
{
    int32_t k = (int32_t)ENV_LIN[g & 127];
    return k ? (int32_t)((1u << 24) / (uint32_t)k) : 1;
}
static void glide_set(track_t *t, voice_t *v, int glide)
{
    int32_t d, ticks;
    if (!glide || !v->pitch_cur) {
        v->pitch_cur = v->pitch16;
        v->pitch_frac = 0;
        v->gstep = 0;
        return;
    }
    if (t->p[P_GLMODE] || !t->p[P_GLIDE]) {
        d = (v->pitch16 << 8) - ((v->pitch_cur << 8) + v->pitch_frac);
        d = d < 0 ? -d : d;
        ticks = glide_ticks(t->p[P_GLIDE] ? t->p[P_GLIDE] : SLIDE_T);
        v->gstep = d / ticks > 0 ? d / ticks : 1;
    } else {
        v->gstep = 0;
    }
}

/* (SLOOP 2.8.3) a sample (SAMPLE, SLICE) played again on a voice still sounding: it starts over at its first frame, so
 * its attack must start from silence too. The envelope went on from the level it had: the sample jumped from where it
 * was to its start at that level (a click), and ATK did nothing. That was every note in MONO / LEGATO / UNISON (one
 * voice) and the same note played again in POLY (its voice reused); different notes in POLY took fresh voices and
 * faded in (a SLOOP user's chopped breaks, ATK set to soften the chops' starts). Now the voice fades out over
 * KILL_BLOCKS (~1.5 ms: env_tick stage 4), then the note starts on it from silence with its ATK (track_render:
 * VP_ON). A note-off within the fade is kept for it (VP_OFF: released as it starts, or held by the pedal) */
#define VP_ON 1u
#define VP_OFF 2u
#define VP_GLIDE 4u
#define VP_UNI 8u   /* (after Clipper 1.2) an extra UNISON voice: its start spread (unison_spread) */
static void voice_start(track_t *t, voice_t *v, uint32_t note, uint32_t vel, int glide)
{
    const engine_t *e = ENGINES[t->engine];
    int sounding = v->active && v->stage != 0;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    int32_t s0 = v->s[0], s1 = v->s[1], s3 = v->s[3], s4 = v->s[4], s5 = v->s[5], s6 = v->s[6], s7 = v->s[7];
    if (sounding && eng_sampled(e)) {                   /* (SLOOP 2.8.3) fade it out first, then this note from silence */
        v->note = (uint8_t)note;                        /* (the same note again finds it: voice_alloc) */
        v->pend_note = (uint8_t)note;
        v->pend_vel = (uint8_t)vel;
        v->pend = (uint8_t)(VP_ON | (glide ? VP_GLIDE : 0u));
        v->gate = 0;
        v->sus = 0;
        if (v->stage != 4u)
            v->kill = KILL_BLOCKS;
        v->stage = 4;
        v->age = ++vage;
        return;
    }
    v->pend = 0;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->gate = 1;
    v->sus = 0;
    v->pat = 0;
    v->active = 1;
    v->stage = 1;
    v->age = ++vage;
    v->pitch16 = (int32_t)note * 16;
    glide_set(t, v, glide);
    v->penv = 32767;                                    /* the pitch envelope starts over (not on legato) */
    if (!sounding) {
        v->env = 0;
        v->env_out = 0;
    }                                                   /* sounding: the attack starts from the current level */
#if CLIPPER_SYNTH
    {   /* Clipper 0.4: ENV 2 starts over (from its level when sounding), LFO 2 (KEY) from 0, a new random value */
        static uint32_t seed = 0x5EED1234u;             /* (its own: rng()'s other users unchanged) */
        if (!sounding)
            v->env2 = 0;
        v->e2st = 1;
        v->l2ph = 0;
        v->l3ph = 0;                                    /* (1.0) LFO 3 KEY: from the note's start */
        seed = seed * 1664525u + 1013904223u;
        v->rnd = (int16_t)(seed >> 16);
        seed = seed * 1664525u + 1013904223u;
        v->l2rnd = (int16_t)(seed >> 16);
        v->l3rnd = v->l2rnd;
        v->ch = mpe_cur_ch;
    }
#endif
    e->note_on(t, v);
    if (sounding && !eng_sampled(e)) {                 /* retrigger / steal: keep phases and filter states */
        v->ph[0] = ph0;                                 /* (resetting them clicks) */
        v->ph[1] = ph1;
        v->ph[2] = ph2;
        if (e == &ENG_ANALOG) {
            v->s[0] = s0;
            v->s[1] = s1;
        } else if (e == &ENG_DIGITAL) {
            v->s[5] = s5;
            v->s[6] = s6;
            v->s[7] = s7;                               /* op 4 phase; the modulator envelope restarts */
            v->s[3] = s3;                               /* (0.9) the index glides there from where it was */
        } else if (e == &ENG_LOFI) {
            v->s[0] = s0;
            v->s[4] = s4;
        } else if (e == &ENG_TRIO) {
            v->s[0] = s0;                               /* filter */
            v->s[1] = s1;
            v->s[4] = s4;                               /* the sample waiting for its step corrections */
        } else if (e == &ENG_PHASE) {
            v->s[0] = s0;                               /* the WAVE / WAVE2 toggles go with the phases kept */
            v->s[1] = s1;
        } else if (e == &ENG_NOISE) {
            v->s[0] = s0;                               /* the filter and the colour poles (2.5) */
            v->s[1] = s1;
            v->s[4] = s4;
            v->s[5] = s5;
            v->s[6] = s6;
            v->s[7] = s7;
        }
    }
}

/* a UNISON voice's start phases moved by r (Q32: a turn of its note), as if it had started that much of a period
 * sooner: in phase the voices stack, spread they sum as noise. Each engine moves its own oscillators (after Clipper
 * 1.2: FMX and MACRO took nothing here, every voice in phase: FMX +12 dB over POLY, peaks of 4 full scales; MACRO's
 * chords +7 dB peaks) */
static void unison_spread(track_t *t, voice_t *v, uint32_t r)
{
    const engine_t *e = ENGINES[t->engine];
    /* (after Clipper 1.2) each engine moves all its oscillators (and reseeds its noise): ph[0] alone left the others in
     * phase (ANALOG's osc 2, TRIO's 2 and 3, DIGITAL's and FM6's operators, the subs, SAMPLE's start...) */
    if (e == &ENG_ANALOG)
        analog_spread(t, v, r);
    else if (e == &ENG_DIGITAL)
        digital_spread(t, v, r);
    else if (e == &ENG_PHASE)
        phase_spread(t, v, r);
    else if (e == &ENG_LOFI)
        lofi_spread(t, v, r);
    else if (e == &ENG_TRIO)
        trio_spread(t, v, r);
    else if (e == &ENG_NOISE)
        noise_spread(t, v, r);
    else if (e == &ENG_FM6)
        fm6_spread(t, v, r);
    else if (e == &ENG_SAMPLE)
        sample_spread(t, v, r);
    else if (e == &ENG_HYBRID)
        hybrid_spread(t, v, r);
#if CLIPPER_SYNTH
    else if (e == &ENG_FMX)
        fmx_spread(t, v, r);
    else if (e == &ENG_MACRO)
        macro_spread(t, v, r);
    else if (e == &ENG_STRINGS)
        strings_spread(t, v, r);
    else if (e == &ENG_WAVE)
        wave_spread(t, v, r);
    else if (e == &ENG_VPS)
        vps_spread(t, v, r);
    else if (e == &ENG_JUNO)
        juno_spread(t, v, r);
    else if (e == &ENG_WSEQ)
        wseq_spread(t, v, r);
#endif
#if FELUCCA_SLICE
    else if (e == &ENG_SLICE)
        ;                                            /* (slices: as played) */
#endif
    else
        v->ph[0] += r;
}

/* MONO / LEGATO / UNISON: one note on one voice (eight for UNISON, or the
 * engine's voice cap, spread by DETUNE over the same width) */
static void mono_play(track_t *t, uint32_t note, uint32_t vel, int retrig, int glide)
{
    uint32_t mode = voice_mode(t), nv = mode == V_UNISON ? trk_nvoice(t) : 1u, i;
    for (i = 0; i < nv; i++) {
        voice_t *v = &t->v[i];
        int32_t k = 2 * (int32_t)i - (int32_t)(nv - 1u);   /* -7 .. 7 */
        if (nv > 1u && nv < NVOICE)
            k = k * (NVOICE - 1) / (int32_t)(nv - 1u);      /* fewer voices: the outer ones as wide */
        v->fine = nv > 1u ? k * t->p[P_DETUNE] * 56 / 889 : 0;   /* up to ~±40 cents */
        if (retrig || !v->active || !v->gate) {
            int was_sounding = v->active && v->stage != 0;
            if ((!v->active || v->stage == 4u) && !voice_room(t, i > 0))
                continue;                                   /* no room for this extra UNISON voice */
            /* level: about the same sum for 8 or 4 voices at random phases */
            voice_start(t, v, note, nv >= NVOICE ? vel * 36u / 100u : nv > 1u ? vel / 2u : vel, glide);
            if (nv > 1u && i && !was_sounding) {   /* random start phases (SAMPLE: its start; not SLICE): */
                static uint32_t seed = 0x1234567u;          /* in phase they stack, evenly spread they cancel */
                seed = seed * 1664525u + 1013904223u;
                unison_spread(t, v, seed);
            } else if (nv > 1u && i && (v->pend & VP_ON))
                v->pend |= VP_UNI;                          /* (a sample's, after its fade: spread as it starts) */
        } else {                                            /* legato: new pitch, same envelope */
            v->note = (uint8_t)note;
#if CLIPPER_SYNTH
            v->ch = mpe_cur_ch;                             /* (MPE: the new note's channel) */
#endif
            v->pitch16 = (int32_t)note * 16;
            if (vel > v->vel && nv == 1u)
                v->vel = (uint8_t)vel;
            glide_set(t, v, glide);
        }
    }
    t->mono_note = (uint8_t)note;
}

/* the note the mono stack asks for (PRIO: LAST / LOW / HIGH), 0 = none */
static uint32_t mono_pick(const track_t *t)
{
    uint32_t i, n;
    if (!t->nmono)
        return 0;
    n = t->mono_stack[t->nmono - 1u];
    for (i = 0; i < t->nmono; i++) {
        if (t->p[P_PRIO] == 1 && t->mono_stack[i] < n)
            n = t->mono_stack[i];
        if (t->p[P_PRIO] == 2 && t->mono_stack[i] > n)
            n = t->mono_stack[i];
    }
    return n;
}

static void mono_remove(track_t *t, uint32_t note)
{
    uint32_t j, k = 0;
    for (j = 0; j < t->nmono; j++)
        if (t->mono_stack[j] != note)
            t->mono_stack[k++] = t->mono_stack[j];
    t->nmono = (uint8_t)k;
}

#if CLIPPER_SYNTH
/* Clipper 0.3: poly unison (VOICE UNI2 / UNI4): each note on 2 or 4 voices spread by DETUNE as UNISON's (the
 * outer ones as wide), random start phases. The voice for one more: a free one while the budget has room, else
 * this part's oldest released, else a released or extra voice of another part, else this part's oldest held that is
 * not its lowest note (the bass). The same note again restarts its own voices in place */
static int puni_taken(const uint32_t *idx, uint32_t n, uint32_t i)
{
    uint32_t j;
    for (j = 0; j < n; j++)
        if (idx[j] == i)
            return 1;
    return 0;
}
static uint32_t puni_pick(track_t *t, const uint32_t *idx, uint32_t n, uint32_t np)
{
    uint32_t i, best = np, low = lowest_held(t);
    if (voices_busy() < NVOICE)
        for (i = 0; i < np; i++)
            if (!t->v[i].active && !puni_taken(idx, n, i))
                return i;
    for (i = 0; i < np; i++)
        if (t->v[i].active && !t->v[i].gate && t->v[i].stage != 4u && !puni_taken(idx, n, i) &&
            (best == np || t->v[i].age < t->v[best].age))
            best = i;
    if (best < np)
        return best;
    if (voice_room(t, 1))                                /* another part gives one up (soft) */
        for (i = 0; i < np; i++)
            if (!t->v[i].active && !puni_taken(idx, n, i))
                return i;
    for (i = 0; i < np; i++)
        if (t->v[i].gate && !puni_taken(idx, n, i) && (low >= NVOICE || t->v[i].note != t->v[low].note) &&
            (best == np || t->v[i].age < t->v[best].age))
            best = i;
    return best;
}
static void puni_on(track_t *t, uint32_t note, uint32_t vel)
{
    static uint32_t seed = 0x2468ACEu;
    uint32_t np = trk_nvoice(t), k = t->p[P_VOICE] == V_PUNI2 ? 2u : 4u, idx[4], n = 0, i, j;
    if (k > np)
        k = np;
    for (i = 0; i < np && n < k; i++)                  /* the same note: its voices again */
        if (t->v[i].active && t->v[i].note == note && t->v[i].stage != 4u)
            idx[n++] = i;
    while (n < k) {
        i = puni_pick(t, idx, n, np);
        if (i >= np)
            break;
        idx[n++] = i;
    }
    for (j = 0; j < n; j++) {
        voice_t *v = &t->v[idx[j]];
        int was = v->active && v->stage != 0;
        int32_t s = n > 1u ? (2 * (int32_t)j - (int32_t)(n - 1u)) * 7 : 0;   /* -7 .. 7 (x (n - 1)) */
        v->fine = n > 1u ? s * t->p[P_DETUNE] * 56 / (889 * (int32_t)(n - 1u)) : 0;
        voice_start(t, v, note, n >= 4u ? vel / 2u : n > 1u ? vel * 70u / 100u : vel, t->p[P_GLIDE] != 0);
        if (j && !was) {                                /* random start phases (in phase they stack) */
            seed = seed * 1664525u + 1013904223u;
            unison_spread(t, v, seed);
        } else if (j && (v->pend & VP_ON))
            v->pend |= VP_UNI;                          /* (a sample's, after its fade: spread as it starts) */
    }
}
#endif

/* the notes each synth part started since the UI last looked (menu NOTES: a short note lights its key
 * for a few frames, as a drum hit does; ui_studio.c pads_tick). Set here (mostly the audio ISR), taken
 * by the UI with the IRQ off */
static volatile uint32_t note_hits[NPART][4];
/* the visualiser (ui_vis.c): the tracks a note started on (bit per track), each one's last note, a kick */
static volatile uint8_t vis_hit, vis_kick_hit, vis_note[NTRK];

static void trk_note_on1(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t any = 0, i, mode = voice_mode(t);
    if (trk_silent(t))
        return;                                         /* MUTE, or another track soloed */
    vis_hit |= (uint8_t)(1u << ((uint32_t)(t - trk) & 3u));
    vis_note[(uint32_t)(t - trk) & 3u] = (uint8_t)note;
    if (is_drum(t)) {                                   /* the drum track: GM drums (drums.c) */
        if (note == 35u || note == 36u)
            vis_kick_hit = 1;
#if !CLIPPER_SYNTH || BOARD_SMK37                                      /* (Clipper 1.2: no drum machine, its sounds out of the image) */
        drum_on(note, vel);
#endif
        return;
    }
    if ((uint32_t)(t - trk) < NPART && note < 128u)
        note_hits[t - trk][note >> 5] |= 1u << (note & 31u);
    if (t->xf_on || t->eng_req != t->engine) {          /* engine switch under way: after the fade */
        for (i = 0; i < t->xp_n && t->xp_note[i] != note; i++)
            ;
        if (i == t->xp_n && t->xp_n < 4u)
            t->xp_n++;
        i = i < t->xp_n ? i : t->xp_n - 1u;             /* (full: the last one is replaced) */
        t->xp_note[i] = (uint8_t)note;
        t->xp_vel[i] = (uint8_t)vel;
        return;
    }
    for (i = 0; i < NVOICE; i++)
        any |= t->v[i].gate;
    if (!any) {                                        /* fresh phrase: LFO retrigger and fade */
        t->lfo_ph = (uint32_t)t->p[P_LPHASE] << 25;
        t->lfo_fade = 0;
    }
    if (voice_poly((int)mode)) {
        voice_t *v;
        t->nmono = 0;                                   /* no stale mono stack after a mode change */
        t->mono_note = 0;
#if CLIPPER_SYNTH
        if (mode >= V_PUNI2) {                          /* Clipper 0.3: poly unison */
            puni_on(t, note, vel);
            return;
        }
#endif
        v = voice_alloc(t, note);
        v->fine = 0;
        voice_start(t, v, note, vel, t->p[P_GLIDE] != 0);
        return;
    }
    {
        uint32_t sounding = t->mono_note && t->v[0].active && t->v[0].gate, want;
        mono_remove(t, note);
        if (t->nmono >= 8u)
            mono_remove(t, t->mono_stack[0]);
        t->mono_stack[t->nmono++] = (uint8_t)note;
        want = mono_pick(t);
        if (sounding && want == t->mono_note)
            return;                                     /* LOW / HIGH priority: this key does not win */
        if (mode == V_MONO)                             /* MONO: always retrigger, glide when GLIDE is set */
            mono_play(t, want, vel, 1, t->p[P_GLIDE] != 0 || t->slide_glide);
        else                                            /* LEGATO / UNISON: glide only between held notes */
            mono_play(t, want, vel, !sounding, sounding && (t->p[P_GLIDE] != 0 || t->slide_glide));
    }
}

/* STRUM (2.4, P_STRUM): a chord's notes start one after the other, |P_STRUM| ms apart (> 0 from the lowest up, as
 * a guitar's down stroke; < 0 from the highest down). The later ones wait here, in samples; a note-off before its
 * start cancels it, a track's all-off (STOP) clears them */
#define STQ 16u
static struct { uint8_t on, trk, note, vel; uint32_t left; } stq[STQ];
static void strum_cancel(const track_t *t, uint32_t note, int all)
{
    uint32_t i;
    for (i = 0; i < STQ; i++)
        if (stq[i].on && stq[i].trk == (uint8_t)(t - trk) && (all || stq[i].note == note))
            stq[i].on = 0;
}
static void trk_note_off1(track_t *t, uint32_t note)
{
    uint32_t i, k = 0, mode = voice_mode(t);
    if (is_drum(t))
        return;                                         /* one-shots */
    strum_cancel(t, note, 0);
    for (i = 0; i < t->xp_n; i++)                       /* not sounding yet (engine switch): forget it */
        if (t->xp_note[i] != note) {
            t->xp_note[k] = t->xp_note[i];
            t->xp_vel[k++] = t->xp_vel[i];
        }
    t->xp_n = (uint8_t)k;
    mono_remove(t, note);                               /* in any mode: a key let go after a VOICE change must
                                                         * not stay in the MONO stack (after Felucca 1.0) */
    if (!voice_poly((int)mode)) {
        uint32_t nv = mode == V_UNISON ? trk_nvoice(t) : 1u;
        if (t->mono_note == note) {
            uint32_t next = mono_pick(t);
            if (next) {                                 /* fall back to a held note, legato */
                mono_play(t, next, t->v[0].vel, 0, t->p[P_GLIDE] != 0);
            } else {
                for (i = 0; i < nv; i++)
                    if (t->v[i].gate) {
                        t->v[i].gate = 0;
                        t->v[i].stage = 3;
                    }
                t->mono_note = 0;
            }
        }
    }
    for (i = 0; i < NVOICE; i++)                        /* POLY voices (also ones left from another mode: */
        if (t->v[i].note == note && t->v[i].gate &&     /* VOICE changed while they were held) */
            (voice_poly((int)mode) || i >= (mode == V_UNISON ? trk_nvoice(t) : 1u) || t->mono_note != note)) {
            if (voice_poly((int)mode) && part_ctl[CLIPPER_SYNTH ? ctl_part(t) : (uint32_t)(t - trk) % NPART].sus) {
                t->v[i].sus = 1;                        /* Clipper: the sustain pedal holds it */
                continue;
            }
            t->v[i].gate = 0;
            t->v[i].stage = 3;
        }
    for (i = 0; i < NVOICE; i++)                        /* (SLOOP 2.8.3) waiting for its fade: it starts, then releases */
        if ((t->v[i].pend & VP_ON) && t->v[i].pend_note == note && t->mono_note != note)
            t->v[i].pend |= VP_OFF;
}

/* Clipper: the sustain pedal comes up: the notes it held are released (a key still down keeps its note:
 * pressed again, its voice started over and lost the mark) */
static void trk_sustain_up1(track_t *t)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].sus) {
            t->v[i].sus = 0;
            if (t->v[i].gate) {
                t->v[i].gate = 0;
                t->v[i].stage = 3;
            }
        }
}

static void trk_all_off1(track_t *t)
{
    uint32_t i;
    strum_cancel(t, 0, 1);
    for (i = 0; i < NVOICE; i++) {
        t->v[i].gate = 0;
        t->v[i].sus = 0;
        t->v[i].stage = t->v[i].active ? 3 : 0;
        t->v[i].pend = 0;                               /* (SLOOP 2.8.3: a note waiting for its voice's fade: gone too) */
    }
    t->nmono = 0;
    t->mono_note = 0;
    t->xp_n = 0;
}

/* Clipper 0.3: the layer. Track 2's sound with track 1's (LAYER: every note on both; SPLIT: below SPLIT on track
 * 2, from it up on track 1), its octave OCT; what a note-on did is kept per note, so its note-off goes where it
 * went whatever changed meanwhile (the mode, the split, the octave). Track 2 has no other way in: its sequencer and
 * arpeggiator never run, the keys, MIDI and track 1's sequencer and arpeggiator all come through here */
#if CLIPPER_SYNTH
static volatile int16_t syn_morph_req = -1;      /* MIDI CC 20 (seq.c): MORPH, applied by the UI (ui_synth.c) */
static uint8_t ly_route[128], ly_note[128];           /* per note: bit 0 track 1, bit 1 track 2; track 2's note */
static uint32_t layer_mode(void)
{
    int32_t m = trk[1].p[P_LYMODE];
    return m == LY_LAYER || m == LY_SPLIT || m == LY_MULTI ? (uint32_t)m : LY_OFF;
}
static void trk_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t m, r = 1;
    if (t != &trk[0] || note > 127u) {
        trk_note_on1(t, note, vel);
        return;
    }
    m = layer_mode();                                   /* (MULTI: the layer comes by its own channel only) */
    if (m == LY_LAYER)
        r = 3;
    else if (m == LY_SPLIT)
        r = (int32_t)note >= trk[1].p[P_LYSPLIT] - 4 ? 1u : 2u;
    if (ly_route[note] & 2u)                            /* (the same note again: its layer note first let go) */
        trk_note_off1(&trk[1], ly_note[note]);
    ly_route[note] = (uint8_t)r;
    if (r & 2u) {
        ly_note[note] = (uint8_t)clamp((int32_t)note + (trk[1].p[P_LYOCT] - 2) * 12, 0, 127);
        trk_note_on1(&trk[1], ly_note[note], vel);
    }
    if (r & 1u)
        trk_note_on1(t, note, vel);
}
static void trk_note_off(track_t *t, uint32_t note)
{
    uint32_t r;
    if (t != &trk[0] || note > 127u) {
        trk_note_off1(t, note);
        return;
    }
    r = ly_route[note] ? ly_route[note] : 1u;
    ly_route[note] = 0;
    if (r & 2u)
        trk_note_off1(&trk[1], ly_note[note]);
    if (r & 1u)
        trk_note_off1(t, note);
}
static void trk_sustain_up(track_t *t)
{
    trk_sustain_up1(t);
    if (t == &trk[0] && !layer_multi())                /* (MULTI: the layer has its own pedal) */
        trk_sustain_up1(&trk[1]);
}
static void trk_all_off(track_t *t)
{
    trk_all_off1(t);
    if (t == &trk[0]) {
        trk_all_off1(&trk[1]);
        memset(ly_route, 0, sizeof ly_route);
    }
}
#else
#define trk_note_on trk_note_on1
#define trk_note_off trk_note_off1
#define trk_sustain_up trk_sustain_up1
#define trk_all_off trk_all_off1
#endif

/* note i of a chord of n (notes[]): at once, or strummed (rank: its place from the lowest, or the highest) */
static void trk_note_chord(track_t *t, const uint8_t *notes, uint32_t n, uint32_t i, uint32_t vel)
{
    int32_t s = t->p[P_STRUM];
    uint32_t rank = 0, j, q;
    if (!s || n < 2u || is_drum(t)) {
        trk_note_on(t, notes[i], vel);
        return;
    }
    for (j = 0; j < n; j++)                             /* its place: lowest first (down), highest first (up) */
        if (j != i && (s > 0 ? notes[j] < notes[i] || (notes[j] == notes[i] && j < i)
                             : notes[j] > notes[i] || (notes[j] == notes[i] && j < i)))
            rank++;
    if (!rank) {
        trk_note_on(t, notes[i], vel);
        return;
    }
    strum_cancel(t, notes[i], 0);
    for (q = 0; q < STQ && stq[q].on; q++)
        ;
    if (q == STQ) {                                     /* (full: at once) */
        trk_note_on(t, notes[i], vel);
        return;
    }
    stq[q].trk = (uint8_t)(t - trk);
    stq[q].note = (uint8_t)notes[i];
    stq[q].vel = (uint8_t)vel;
    stq[q].left = rank * (uint32_t)(s < 0 ? -s : s) * (uint32_t)FS / 1000u;
    stq[q].on = 1;
}
/* each audio block: the strummed notes due */
static void strum_block(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < STQ; i++)
        if (stq[i].on) {
            if (stq[i].left <= n) {
                stq[i].on = 0;
                trk_note_on(&trk[stq[i].trk % NTRK], stq[i].note, stq[i].vel);
            } else {
                stq[i].left -= n;
            }
        }
}

/* engine switch, at each block start (events_block), before any note of the block. The UI writes
 * eng_req and the new engine's P_E0..P_E7 together (IRQ off). The part's sounding voices (released by
 * the preset change) then fade out over XF_BLOCKS blocks on the old engine, rendered with its own
 * parameters (pe_old: an engine never reads another engine's values, which index its tables); only
 * then the engine switches and the notes that came during the fade start on it. A part with nothing
 * sounding switches at once. */
#if CLIPPER_SYNTH
#define XF_BLOCKS 8u                                    /* Clipper: 8 x 32 samples, 5.8 ms (a bass fades without a thump) */
#else
#define XF_BLOCKS 4u                                    /* 4 x 32 samples: 2.9 ms */
#endif
static void engine_block(track_t *t)
{
    uint32_t i, any = 0;
    if (t->xf_on || t->eng_req != t->engine) {
        if (!t->xf_on) {
            for (i = 0; i < NVOICE; i++)
                any |= t->v[i].active;
            if (any) {
                t->xf_on = 1;
                t->xf = XF_BLOCKS;
                return;
            }
        } else if (t->xf) {
            return;                                     /* still fading (track_render counts down) */
        }
        for (i = 0; i < NVOICE; i++) {                  /* faded to 0: gone */
            voice_t *v = &t->v[i];
            v->active = v->gate = 0;
            v->stage = 0;
            v->env = v->env_out = 0;
        }
        if (t->eng_req == ENGI_FM6 && ((fm6_pend >> ((uint32_t)(t - trk) % NPART)) & 1u))
            return;                                     /* (SLOOP 2.6) FM6: the new notes wait for the new sound's patch
                                                         * (eng_fm6.c fm6_poll, main loop): not a note of the old one */
        t->engine = t->eng_req % NENGINES;
        t->xf_on = 0;
#if CLIPPER_SYNTH
        t->xf_snd = 0;
#endif
        t->nmono = 0;
        t->mono_note = 0;
        {
            uint32_t n = t->xp_n;
            t->xp_n = 0;
            for (i = 0; i < n; i++)
                trk_note_on1(t, t->xp_note[i], t->xp_vel[i]);   /* (the layer had it then) */
        }
    }
    for (i = 0; i < 8u; i++)                            /* the engine's own values, for a later fade */
        t->pe_old[i] = t->p[P_E0 + i];
}

#if CLIPPER_SYNTH
/* Clipper 0.3: before a sound changes under sounding voices (a preset, an engine, a user preset, SWAP, BACK...):
 * they fade out over XF_BLOCKS with all their own values (p_old: fx.c mix_part renders the part with them; pe_old the
 * engine's), then go; notes played meanwhile wait for the new sound (engine_block). Without it the voices ringing
 * on took the new values at once (a new trim, filter, envelope, table) and clicked. The main loop / UI calls it */
static void sound_fade_begin(track_t *t)
{
    uint32_t i, any = 0;
    if ((uint32_t)(t - trk) >= NPART || t->xf_on)
        return;
    for (i = 0; i < NVOICE; i++)
        any |= t->v[i].active && t->v[i].stage != 0;
    if (!any)
        return;
    memcpy(t->p_old, t->p, sizeof t->p_old);
    t->xf_snd = 1;
    t->xf = XF_BLOCKS;
    RING_PUBLISH();
    t->xf_on = 1;                                       /* (last: the ISR uses p_old from then on) */
}
#else
#define sound_fade_begin(t) ((void)0)
#endif

/* one control tick (CTL samples) of the amplitude envelope; returns Q15 */
static int32_t env_tick(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    int32_t sus = (int32_t)p[P_SUS] << 17;              /* Q24 */
    switch (v->stage) {
    case 1:
        v->env += (int32_t)ENV_LIN[p[P_ATK] & 127];
        if (v->env >= (1 << 24)) {
            v->env = 1 << 24;
            v->stage = 2;
        }
        break;
    case 2:
        v->env += mulq16(sus - v->env, ENV_EXP[p[P_DEC] & 127]);
        break;
    case 3:
        v->env -= mulq16(v->env, ENV_EXP[p[P_REL] & 127]);
        if (v->env < (1 << 12)) {
            v->env = 0;
            v->stage = 0;
            v->active = 0;
        }
        break;
    case 4:                                             /* given up for another part: faded out over KILL_BLOCKS */
        if (v->kill > 1u) {
            v->env -= v->env / v->kill;
            v->kill--;
        } else {
            v->env = 0;
            v->stage = 0;
            v->active = 0;
            v->kill = 0;
        }
        break;
    default:
        v->env = 0;
        break;
    }
    return v->env >> 9;
}

#if CLIPPER_SYNTH
/* Clipper 0.4: ENV 2, a second ADSR (P_E2ATK..P_E2REL) for the matrix; it releases with the voice. Q15 */
static int32_t env2_tick(const int16_t *p, voice_t *v)
{
    if (v->stage >= 3u && v->e2st && v->e2st < 3u)
        v->e2st = 3;
    switch (v->e2st) {
    case 1:
        v->env2 += (int32_t)ENV_LIN[p[P_E2ATK] & 127];
        if (v->env2 >= (1 << 24)) {
            v->env2 = 1 << 24;
            v->e2st = 2;
        }
        break;
    case 2:
        v->env2 += mulq16(((int32_t)p[P_E2SUS] << 17) - v->env2, ENV_EXP[p[P_E2DEC] & 127]);
        if (p[P_E2LOOP] && v->env2 - ((int32_t)p[P_E2SUS] << 17) < (1 << 18))
            v->e2st = 1;                                /* (1.0) LOOP: close to the sustain, the attack again (from
                                                         * there: no jump), until the note is let go */
        break;
    case 3:
        v->env2 -= mulq16(v->env2, ENV_EXP[p[P_E2REL] & 127]);
        if (v->env2 < (1 << 12)) {
            v->env2 = 0;
            v->e2st = 0;
        }
        break;
    default:
        v->env2 = 0;
        break;
    }
    return v->env2 >> 9;
}

/* Clipper 0.4: the modulation matrix, on every engine. Six slots (P_M1SRC..P_M6AMT) of a source (MS_*: LFO 1 / 2,
 * ENV 1 / 2, velocity, key, mod wheel, aftertouch, a random value per note, MPE slide), all Q15, times AMT
 * (-64..63), to a destination (MT_*): the pitch (+-24 semitones, finer near 0), the cutoff (+-4 octaves), the shape,
 * the level, the pan (the part's, from its newest note), LFO 2's rate, or any of the engine's eight EDIT values
 * (over their whole range, per note). The slots in use, read once a block */
typedef struct {
    uint8_t src[MX_SLOTS], dst[MX_SLOTS];
    int8_t amt[MX_SLOTS];
    uint32_t n, srcs, dsts;                             /* slots in use; bit per source, per destination */
} mx_t;
static void mx_load(const int16_t *p, mx_t *x)
{
    uint32_t k;
    x->n = x->srcs = x->dsts = 0;
    for (k = 0; k < MX_SLOTS; k++) {
        uint32_t id = mx_pid(k);                        /* (1.0: 12 slots, 7..12 at P_M7SRC) */
        int32_t sr = p[id], ds = p[id + 1u], a = p[id + 2u];
        if (sr <= MS_OFF || sr >= MS_NSRC || ds <= MT_OFF || ds >= MT_COUNT || !a)
            continue;
        if (ds == MT_L2RATE && sr == MS_LFO2)
            continue;                                   /* (LFO 2 on its own rate: none) */
        x->src[x->n] = (uint8_t)sr;
        x->dst[x->n] = (uint8_t)ds;
        x->amt[x->n] = (int8_t)clamp(a, -64, 63);
        x->srcs |= 1u << sr;
        x->dsts |= 1u << ds;
        x->n++;
    }
}
/* Clipper 0.8: the MOD SEQ. Four lanes of 16 steps (-8..7) per sound, stepping at RATE (the song's grid while it
 * plays and START is SYNC; from each first key with KEY, and while stopped), LEN steps, each lane's value gliding
 * (SMTH) to its step's: the matrix's sources SEQ1..SEQ4 (seq.c mseq_tick moves them, once a block). A sound loaded
 * (a factory one) brings four shapes to start from: a ramp, a triangle, a rhythm, a random line */
static const int8_t MSEQ_INIT[MSEQ_LANES][MSEQ_STEPS] = {
    {-8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7},
    {0, 2, 4, 6, 7, 5, 3, 1, -1, -3, -5, -7, -8, -6, -4, -2},
    {7, 7, -8, -8, 7, -8, 7, 7, -8, 7, -8, -8, 7, -8, 7, -8},
    {3, -5, 6, -2, 0, 7, -7, 2, -4, 5, -1, -8, 4, 1, -6, 6},
};
static void mseq_init(track_t *t)
{
    memcpy(t->ms, MSEQ_INIT, sizeof t->ms);
}
static int32_t mseq_q15(int32_t v) { return clamp(v * 4369, -32767, 32767); }   /* a step: -8 full scale, 7 just under */
/* Clipper 0.9, the TUNING (GLO > TUNING; the editor sends a Scala scale): every note's pitch off equal temperament by
 * tun_tab (1/4096 semitone), between notes interpolated (a glide goes through the scale). Two kinds of systems:
 *   12-note tables, each pitch class's deviation from equal temperament around ROOT (JUST, PYTHAG, MEANTONE, WERCK3,
 *   KIRNB3, the maqams RAST and BAYATI with their neutral thirds);
 *   scales of n degrees in a period (SLENDRO, PELOG, 19 / 24 / 31-EDO, the USER scale from a .scl file): every key
 *   the next degree, C4 + ROOT the scale's 1/1 at its equal-tempered pitch.
 * G_TUNE (A4 in cents) still moves it all. EQUAL: tun_on 0, nothing touched, bit for bit.
 * (1.0) A4: the reference pitch in Hz (400 .. 480: 432, 415 baroque, 442 / 443 orchestras), every system on top of it */
enum { TU_EQUAL, TU_JUST, TU_PYTH, TU_MEAN, TU_WERCK, TU_KIRN, TU_RAST, TU_BAYATI, TU_SLENDRO, TU_PELOG, TU_19, TU_24,
       TU_31, TU_USER, TU_N };
static const char *const N_TUNE[TU_N] = {"EQUAL", "JUST", "PYTHA", "MEANT", "WERCK", "KIRNB", "RAST", "BAYAT", "SLEND",
                                         "PELOG", "19EDO", "24EDO", "31EDO", "USER"};
#define TU_MAXDEG 48u
typedef struct {
    uint8_t sys, root, n;                               /* system, ROOT (0..11), USER: degrees (1..TU_MAXDEG) */
    int8_t a4;                                          /* (1.0) A4 - 440 Hz (-40..40: 400..480 Hz); 0.9 stored 0 */
    uint16_t period;                                    /* USER: the period, 1/16 cent (1200 cents: 19200) */
    uint16_t deg[TU_MAXDEG];                            /* USER: degrees 1..n - 1 above 1/1 (deg[0] = 0), 1/16 cent */
} tune_t;
static tune_t tune_cfg = {TU_EQUAL, 0, 12, 0, 19200,      /* (USER until a scale comes: equal temperament) */
                          {0, 1600, 3200, 4800, 6400, 8000, 9600, 11200, 12800, 14400, 16000, 17600}};
static int32_t tun_tab[129];
/* (1.0) the reference A4, 400 .. 480 Hz: 12 x log2(A4 / 440) in 1/4096 semitone (432: -1301, 415: -4148) */
static const int16_t TU_A4[81] = {-6759, -6582, -6405, -6229, -6053, -5878, -5703, -5528, -5354, -5181, -5008, -4835, -4663,
    -4491, -4319, -4148, -3977, -3807, -3637, -3468, -3299, -3130, -2962, -2794, -2627, -2460, -2293, -2127, -1961, -1795,
    -1630, -1465, -1301, -1137, -974, -810, -648, -485, -323, -161, 0, 161, 322, 482, 642, 801, 960, 1119, 1278, 1436, 1594,
    1751, 1908, 2065, 2221, 2377, 2533, 2688, 2843, 2998, 3152, 3306, 3460, 3613, 3766, 3919, 4071, 4223, 4375, 4526, 4677,
    4828, 4978, 5128, 5278, 5428, 5577, 5726, 5874, 6022, 6170};
static volatile uint8_t tun_on;
static const int16_t TU_TAB12[6][12] = {               /* 1/10 cent off equal temperament, from ROOT */
    {0, 117, 39, 156, -137, -20, -98, 20, 137, -156, 176, -117},       /* JUST (5-limit) */
    {0, -98, 39, -59, 78, -20, 117, 20, -78, 59, -39, 98},             /* PYTHAG */
    {0, -240, -68, 103, -137, 34, -205, -34, -274, -103, 68, -171},    /* MEANTONE (1/4 comma) */
    {0, -98, -78, -59, -98, -20, -117, -39, -78, -117, -39, -78},      /* WERCKMEISTER III */
    {0, -98, -68, -59, -137, -20, -98, -34, -78, -103, -39, -117},     /* KIRNBERGER III */
    {0, 0, 0, 0, -500, 0, 0, 0, 0, 0, 0, -500},                        /* RAST: E and B half flat */
};
static int32_t tun_deg_cents16(uint32_t sys, uint32_t k, uint32_t *n, uint32_t *period)   /* degree k, 1/16 cent */
{
    static const uint16_t SL[5] = {0, 231, 474, 717, 955}, PE[7] = {0, 120, 258, 539, 675, 785, 943};
    switch (sys) {
    case TU_SLENDRO: *n = 5, *period = 19200; return SL[k % 5u] * 16;
    case TU_PELOG: *n = 7, *period = 19200; return PE[k % 7u] * 16;
    case TU_19: *n = 19, *period = 19200; return (int32_t)(k % 19u) * 19200 / 19;
    case TU_24: *n = 24, *period = 19200; return (int32_t)(k % 24u) * 800;
    case TU_31: *n = 31, *period = 19200; return (int32_t)(k % 31u) * 19200 / 31;
    default:
        *n = clamp(tune_cfg.n, 1, TU_MAXDEG), *period = clamp(tune_cfg.period, 16, 65535);
        return k % *n ? tune_cfg.deg[k % *n] : 0;
    }
}
/* the table from tune_cfg (the main loop: a change on GLO > TUNING, the editor, power-on) */
static void tune_build(void)
{
    static int32_t tab[129];
    uint32_t sys = tune_cfg.sys < TU_N ? tune_cfg.sys : TU_EQUAL, key, root = tune_cfg.root % 12u;
    int32_t a4 = TU_A4[clamp(tune_cfg.a4, -40, 40) + 40];   /* (1.0) the reference pitch, on every key */
    for (key = 0; key < 128u; key++) {
        int32_t off = 0;
        if (sys >= TU_JUST && sys <= TU_RAST) {
            off = TU_TAB12[sys - TU_JUST][(key + 12u - root) % 12u] * 4096 / 1000;
        } else if (sys == TU_BAYATI) {                  /* (on ROOT: its 2nd degree half flat) */
            off = (key + 12u - root) % 12u == 2u ? -50 * 4096 / 100 : 0;
        } else if (sys != TU_EQUAL) {
            int32_t d = (int32_t)key - (60 + (int32_t)root), oct, k;
            uint32_t n, per;
            tun_deg_cents16(sys, 0, &n, &per);
            oct = d >= 0 ? d / (int32_t)n : -((-d + (int32_t)n - 1) / (int32_t)n);
            k = d - oct * (int32_t)n;
            off = (oct * (int32_t)per + tun_deg_cents16(sys, (uint32_t)k, &n, &per)) * 64 / 25 -
                  d * 4096;                             /* (1/16 cent x 4096 / 1600; fits 32 bits: +-8 octaves) */
            off = clamp(off, -100 * 4096, 100 * 4096);
        }
        tab[key] = off + a4;
    }
    tab[128] = tab[127];
    fm1_irq_off();
    memcpy(tun_tab, tab, sizeof tun_tab);
    tun_on = (uint8_t)(sys != TU_EQUAL || a4 != 0);
    fm1_irq_on();
}
static inline int32_t tun_at(int32_t p16)               /* the offset at a pitch in 1/16 semitone */
{
    int32_t n = clamp(p16, 0, 127 * 16) >> 4, f = clamp(p16, 0, 127 * 16) & 15;
    return tun_tab[n] + (((tun_tab[n + 1] - tun_tab[n]) * f) >> 4);
}

/* Clipper 0.9, VINTAGE (VOICE 2 > VINTG): the voices of an analog polysynth are never quite alike (the Prophet-5's
 * "slop"): each voice here has its own small tuning error and filter offset (its "card", fixed), a slow drift of its
 * pitch (a random walk, a new target every second or so) and each note a little of its own. At 127: the card +-6 cents,
 * the drift +-5, the note +-2, the filter +-1.5 steps; 0: nothing, bit for bit */
static int32_t vcard(uint32_t k, uint32_t salt)         /* the voice card's value, Q15 */
{
    uint32_t h = (k + 1u) * 0x9E3779B1u ^ (salt + 7u) * 0x85EBCA77u;
    h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12;
    return (int32_t)(h >> 16) - 32768;
}
static int32_t vintage_pitch(voice_t *v, uint32_t k, int32_t vint)   /* 1/4096 semitone; once a block */
{
    if ((rng() & 1023u) < 2u)                           /* (~1.4 s between targets) */
        v->vdtgt = (int16_t)((int32_t)(rng() >> 16) - 32768);
    v->vdrift = (int16_t)(v->vdrift + ((v->vdtgt - v->vdrift) >> 9));
    return ((((vcard(k, 0u) * 246) >> 15) + ((v->vdrift * 205) >> 15) + ((v->rnd * 82) >> 15)) * vint) >> 7;
}
/* LFO 2's rate index, + the slots to MT_L2RATE (sv: the sources) */
static uint32_t mx_l2rate(const int16_t *p, const mx_t *x, const int32_t *sv)
{
    int32_t r = 0;
    uint32_t k;
    if (x->dsts & (1u << MT_L2RATE))
        for (k = 0; k < x->n; k++)
            if (x->dst[k] == MT_L2RATE)
                r += sv[x->src[k]] * x->amt[k] >> 6;
    return (uint32_t)clamp(p[P_L2RATE] + (r >> 9), 0, 127);   /* (+-64 steps) */
}
#endif

/* render one block of a part into out (cleared here); returns the voices rendered */
#if CLIPPER_SYNTH
/* 0.7, the voices on both cores. The second core of the FM-1 (cpu1.c, after SLOOP 2.7 and X0X) takes some of a part's
 * voices: track_render works out every voice's modulation here as before, keeps the render calls of an engine that
 * may split (vs_ok: its render touches its voice, its output and what it only reads), hands the second half to core
 * 1 (into vs_buf), renders the first half here, waits, adds. A voice whose EDIT values the matrix moves renders after
 * the wait (it writes the part's values for its call). Integer sums: the same output, bit for bit, whoever renders
 * what (tests: CORES=1 / 2, the golden renders). Core 1 late (8 ms): it is held and its voices render here, once */
static voice_t *vs_v[NVOICE];
static vmod_t vs_m[NVOICE];
static uint32_t vs_n;
static voice_t *vsl_v[NVOICE];                       /* the late ones and their EDIT values */
static vmod_t vsl_m[NVOICE];
static int16_t vsl_p[NVOICE][8];
static uint32_t vsl_n;
/* (1.0) SPREAD: each voice's place in the stereo field. The part renders its voices' sum (the mid, as before: bit for
 * bit without SPREAD) and, when SPREAD is up, part_side: each voice times its gain (its place x SPREAD, gliding
 * through the block), which fx.c mix_part1 adds to the left and takes from the right. A voice alone into a scratch
 * first (vs_tmp, one per core: engines.c C1_IX) */
static int32_t part_side[CTL], vs_side1[CTL];
static uint8_t part_side_on, vs_side1_on;
static int32_t vs_g0[NVOICE], vs_g1[NVOICE], vsl_g0[NVOICE], vsl_g1[NVOICE];
/* the places, Q15: neighbours (the voices one note after the other take) left and right, the outer ones first */
static const int16_t VS_POS[16] = {-32767, 32767, -16384, 16384, -24576, 24576, -8192, 8192,
                                   -29491, 29491, -19661, 19661, -11469, 11469, -3277, 3277};
static void vrender(track_t *t, const engine_t *e, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, int32_t g0,
                    int32_t g1, int32_t *side, int32_t *tmp)
{
    uint32_t i;
    if (!g0 && !g1) {
        e->render(t, v, out, n, m);
        return;
    }
    for (i = 0; i < n; i++)
        tmp[i] = 0;
    e->render(t, v, tmp, n, m);
    for (i = 0; i < n; i++) {
        int32_t g = g0 + (((g1 - g0) * (int32_t)i) >> CTL_LOG2);
        out[i] += tmp[i];
        side[i] += ((tmp[i] >> 4) * g) >> 11;          /* (>> 4 first: 32 bits) */
    }
}
static struct { track_t *t; const engine_t *e; uint32_t n, a, b; } vsj;
static uint32_t vs_jobs, vs_split;                  /* (console, tests: blocks handed over, voices rendered there) */
static void vs_job(uint32_t arg)                    /* on core 1 */
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < vsj.n; i++)
        vs_buf[i] = vs_side1[i] = 0;
    vs_side1_on = 0;
    for (i = vsj.a; i < vsj.b; i++) {
        vs_side1_on |= vs_g0[i] || vs_g1[i];
        vrender(vsj.t, vsj.e, vs_v[i], vs_buf, vsj.n, &vs_m[i], vs_g0[i], vs_g1[i], vs_side1, vs_tmp[1]);
    }
}
static int vs_ok(const engine_t *e)                 /* its render may run on two cores at once, for two voices */
{
    return e == &ENG_HYBRID || e == &ENG_FMX || e == &ENG_WAVE || e == &ENG_ANALOG || e == &ENG_FM6 || e == &ENG_VPS ||
           e == &ENG_TERRAIN || e == &ENG_WSEQ || e == &ENG_MACRO || e == &ENG_STRINGS || e == &ENG_TRIO ||
           e == &ENG_PHASE || e == &ENG_DIGITAL || e == &ENG_LOFI || e == &ENG_ACID || e == &ENG_JUNO || e == &ENG_VECTOR;
}
static void vs_finish(track_t *t, const engine_t *e, int32_t *out, uint32_t n)
{
    uint32_t k, i, h = vs_n, handed = 0;
    if (vs_n >= 2u && n <= CTL && cpu1_units != 0) {
        h = vs_n / 2u;                              /* core 1 the second half (the bigger one when odd: this core
                                                     * also works out the modulation, the mix and the effects) */
        vsj.t = t, vsj.e = e, vsj.n = n, vsj.a = h, vsj.b = vs_n;
        handed = (uint32_t)cpu1_run(vs_job, 0);
        if (!handed)
            h = vs_n;
    }
    for (k = 0; k < h; k++)
        vrender(t, e, vs_v[k], out, n, &vs_m[k], vs_g0[k], vs_g1[k], part_side, vs_tmp[0]);
    if (handed) {
        if (cpu1_wait() < 0) {                      /* late: held from now on (cpu1.c); its voices here */
            for (k = h; k < vs_n; k++)
                vrender(t, e, vs_v[k], out, n, &vs_m[k], vs_g0[k], vs_g1[k], part_side, vs_tmp[0]);
        } else {
            for (i = 0; i < n; i++)
                out[i] += vs_buf[i];
            if (vs_side1_on)
                for (i = 0; i < n; i++)
                    part_side[i] += vs_side1[i];
            vs_jobs++;
            vs_split += vs_n - h;
        }
    }
    for (k = 0; k < vsl_n; k++) {                    /* the matrix's EDIT values: this voice's own, for its call */
        int16_t keep[8];
        uint32_t j;
        for (j = 0; j < 8u; j++) {
            keep[j] = t->p[P_E0 + j];
            t->p[P_E0 + j] = vsl_p[k][j];
        }
        vrender(t, e, vsl_v[k], out, n, &vsl_m[k], vsl_g0[k], vsl_g1[k], part_side, vs_tmp[0]);
        for (j = 0; j < 8u; j++)
            t->p[P_E0 + j] = keep[j];
    }
    vs_n = vsl_n = 0;
}
#endif

#if CLIPPER_SYNTH
/* (after Clipper 1.2) the level of a voice of UNISON (UNI2, UNI4) against POLY's, Q15, as mono_play and puni_on set it
 * through the velocity (about the same sum at random phases): for FMX, whose envelopes are the voice's level (no
 * velocity there: its unison came out +6 dB over POLY with the phases spread, +12 before) */
static int32_t uni_level(const track_t *t)
{
    uint32_t vm = voice_mode(t);
    return vm == V_UNISON ? (trk_nvoice(t) >= NVOICE ? 11796 : 16384) : vm == V_PUNI4 ? 16384 : vm == V_PUNI2 ? 22938 : 32767;
}
#endif
static uint32_t track_render(track_t *t, int32_t *out, uint32_t n)
{
    const engine_t *e = ENGINES[t->engine];
    const int16_t *p = t->p;
    uint32_t i;
    int32_t lfo = mulq15(t->lfo_val, t->lfo_fade);
    /* Clipper: the pitch bend (MIDI En), in 1/4096 semitone: the HYBRID patch's range, else +-2 semitones */
    uint32_t pc = CLIPPER_SYNTH ? ctl_part(t) : (uint32_t)(t - trk) % NPART;   /* (Clipper: the layer plays part 1's
                                                         * controls; in MULTI its own) */
    int32_t bend = (uint32_t)(t - trk) < NPART ?
                   (part_ctl[pc].bend * (midi_bend ? (int32_t)midi_bend :
                                         e == &ENG_HYBRID ? (int32_t)hy_cur[t - trk].bend : 2)) >> 3 : 0;
#if CLIPPER_SYNTH
    /* Clipper 0.3: the mod wheel's and the aftertouch's destinations (P_MWDST, P_ATDST), velocity to the cutoff
     * (P_VELF); VIB and TREM ride a vibrato LFO of their own (~5.5 Hz sine), WAH the sound's LFO */
    static uint32_t vib_ph[NPART];
    int32_t vib, mw = part_ctl[pc].mw, cat = part_ctl[pc].at;
    if (e == &ENG_HYBRID)                               /* (HYBRID: its matrix takes MW and AT) */
        mw = cat = 0;
    vib_ph[(uint32_t)(t - trk) % NPART] += 535642u * n;
    vib = osc_sine(vib_ph[(uint32_t)(t - trk) % NPART]);
    /* Clipper 0.4: the matrix (mx_load); sv: its sources (the part's here, the voice's in the loop) */
    mx_t mx;
    int32_t sv[MS_NSRC], l2free, l3free, pan_new = 0, vint = clamp(t->p[P_VINT], 0, 127);
    uint32_t pan_age = 0, catr = part_ctl[pc].at;
    /* (1.0) the macros: BRITE (the cutoff, +-) and TIMBR (the shape, +-), MOTN (a vibrato and LFO 3 on the cutoff),
     * SPACE (fx.c: the reverb and delay sends, and SPREAD); SPREAD: the voices' gains from the last block's to this
     * one's */
    int32_t mac1 = clamp(t->p[P_MAC1], -64, 63), mac2 = clamp(t->p[P_MAC2], -64, 63), mac3 = clamp(t->p[P_MAC3], 0, 127);
    int32_t spr = clamp(t->p[P_SPREAD] + clamp(t->p[P_MAC4], 0, 127) / 2, 0, 127), spr0 = t->spr;
    int sided = (uint32_t)(t - trk) < NPART && (spr || spr0);
#endif
    /* TUNE in cents: whole 1/16 semitones in the pitch, the rest as a fine factor (no dead zone) */
    int32_t tune = song.g[G_TUNE] >= 0 ? song.g[G_TUNE] * 16 / 100 : -((-song.g[G_TUNE] * 16 + 99) / 100);
    int32_t tune_fine = (song.g[G_TUNE] * 16 - tune * 100) * 2367 / 16000;   /* rest, in 1/4096 (1 ct = 2.367) */
    uint32_t nr = 0, fade = t->xf_on && t->xf;
    int16_t pe_new[8];
#if CLIPPER_SYNTH
    int split = vs_ok(e) && cpu1_units != 0 && cpu1_on();   /* (0.7) the voices on both cores */
    vs_n = vsl_n = 0;
    part_side_on = (uint8_t)sided;
    if (sided)
        for (i = 0; i < n; i++)
            part_side[i] = 0;
    t->spr = (int16_t)spr;
#endif
    for (i = 0; i < n; i++)
        out[i] = 0;
    if (fade)                                           /* engine switch: the old engine, its own values */
        for (i = 0; i < 8u; i++) {
            pe_new[i] = t->p[P_E0 + i];
            t->p[P_E0 + i] = t->pe_old[i];
        }
    track_lfo_tick(t);
#if CLIPPER_SYNTH
    mx_load(p, &mx);
    for (i = 0; i < MS_NSRC; i++)
        sv[i] = 0;
    sv[MS_LFO1] = lfo;                                  /* (the part's sources; with FADE, as the LFO's routes) */
    sv[MS_MW] = part_ctl[pc].mw * 258;                  /* (on HYBRID too: its own matrix has them as well) */
    sv[MS_AT] = (int32_t)catr * 258;
    for (i = 0; i < MSEQ_LANES; i++)                    /* (0.8) the MOD SEQ's lanes: the part's */
        sv[MS_SEQ1 + i] = t->ms_cur[i];
    {   /* LFO 2 FREE: one for the part, its rate moved by the part's sources only */
        uint32_t old = t->l2ph;
        t->l2ph += LFO_INC[mx_l2rate(p, &mx, sv)];
        if (t->l2ph < old)
            t->l2rnd = (int16_t)(rng() >> 16);
        l2free = lfo_wave_n(p[P_L2WAVE], t->l2ph, t->l2rnd);
    }
    {   /* (1.0) LFO 3 FREE, and the macros as sources */
        uint32_t old = t->l3ph;
        t->l3ph += LFO_INC[clamp(p[P_L3RATE], 0, 127)];
        if (t->l3ph < old)
            t->l3rnd = (int16_t)(rng() >> 16);
        l3free = lfo_wave_n(p[P_L3WAVE], t->l3ph, t->l3rnd);
        sv[MS_LFO3] = l3free;
        sv[MS_MAC1] = mac1 * 512, sv[MS_MAC2] = mac2 * 512;
        sv[MS_MAC3] = mac3 * 258, sv[MS_MAC4] = clamp(p[P_MAC4], 0, 127) * 258;
    }
#endif
    if (e->block)                                       /* the engine's per-part work (DRAWBAR: bars, rotor) */
        e->block(t);
    for (i = 0; i < NVOICE; i++) {
        voice_t *v = &t->v[i];
        vmod_t m;
        int32_t env, pitch;
#if CLIPPER_SYNTH
        int32_t mpit = 0, mcut = 0, mshp = 0, mamp = 0, med[8];
        uint32_t medm = 0;                              /* bit per EDIT value the matrix moves */
        int mpe;
#endif
        if ((v->pend & VP_ON) && !v->active && !fade) {   /* (SLOOP 2.8.3) faded out: the sample's note, from silence */
            uint32_t pf = v->pend;
            v->pend = 0;
            if (voice_room(t, 0)) {                     /* (it was not counted while fading: within the budget) */
                voice_start(t, v, v->pend_note, v->pend_vel, (pf & VP_GLIDE) != 0);
                if (pf & VP_UNI) {                      /* (after Clipper 1.2) UNISON: not in phase with the others */
                    static uint32_t seed = 0x13579BDu;
                    seed = seed * 1664525u + 1013904223u;
                    unison_spread(t, v, seed);
                }
                if (pf & VP_OFF) {                      /* let go within the fade (~1.5 ms): released as it starts, */
                    if (voice_poly((int)voice_mode(t)) &&     /* or held by the sustain pedal */
                        part_ctl[CLIPPER_SYNTH ? ctl_part(t) : (uint32_t)(t - trk) % NPART].sus)
                        v->sus = 1;
                    else {
                        v->gate = 0;
                        v->stage = 3;
                    }
                }
            }
        }
        if (!v->active)
            continue;
#if CLIPPER_SYNTH
        mpe = mpe_mode && v->ch;                        /* MPE: the note's own channel */
        if (mpe)
            v->pat = mpe_press[v->ch & 15u];            /* (its pressure: the voice's aftertouch) */
#endif
        {
            env = env_tick(t, v);
            if (e->ownenv && v->active) {               /* the engine's envelopes (FM6): they end the voice */
                if (e->done(t, v)) {
                    v->active = v->gate = 0;
                    v->stage = 0;
                    v->kill = 0;
                    v->env = v->env_out = 0;
                    continue;
                }
                if (v->stage != 4u) {                   /* (the ADSR's release never ends it; a voice given up for
                                                         * another part still fades out: env_tick stage 4) */
                    v->env = 1 << 24;
                    env = 32767;
                }
            }
#if CLIPPER_SYNTH
            if (mx.n) {                                 /* the matrix: the voice's sources, then every slot */
                uint32_t k;
                int32_t pan = 0;
                sv[MS_ENV1] = env;
                sv[MS_ENV2] = mx.srcs & (1u << MS_ENV2) ? env2_tick(p, v) : 0;
                sv[MS_VEL] = v->vel * 258;
                sv[MS_KEY] = clamp(((int32_t)v->note - 60) * 512, -32768, 32767);
                sv[MS_AT] = (int32_t)(v->pat > catr ? v->pat : catr) * 258;
                sv[MS_RAND] = v->rnd;
                sv[MS_SLIDE] = mpe ? mpe_slide[v->ch & 15u] * 512 : 0;
                if (p[P_L2MODE] == L2_KEY) {            /* LFO 2 KEY: the note's own, from its start */
                    uint32_t old = v->l2ph;
                    sv[MS_LFO2] = 0;
                    v->l2ph += LFO_INC[mx_l2rate(p, &mx, sv)];
                    if (v->l2ph < old)
                        v->l2rnd = (int16_t)(rng() >> 16);
                    sv[MS_LFO2] = lfo_wave_n(p[P_L2WAVE], v->l2ph, v->l2rnd);
                } else {
                    sv[MS_LFO2] = l2free;
                }
                if (p[P_L3MODE] == L2_KEY && (mx.srcs & (1u << MS_LFO3))) {   /* (1.0) LFO 3 KEY: the note's own */
                    uint32_t old = v->l3ph;
                    v->l3ph += LFO_INC[clamp(p[P_L3RATE], 0, 127)];
                    if (v->l3ph < old)
                        v->l3rnd = (int16_t)(rng() >> 16);
                    sv[MS_LFO3] = lfo_wave_n(p[P_L3WAVE], v->l3ph, v->l3rnd);
                } else {
                    sv[MS_LFO3] = l3free;
                }
                for (k = 0; k < mx.n; k++) {
                    int32_t x = sv[mx.src[k]] * mx.amt[k] >> 6;   /* Q15 x AMT / 64 */
                    uint32_t d = mx.dst[k];
                    if (d == MT_PITCH)                  /* +-24 semitones, AMT squared: fine near 0 */
                        mpit += (x * (mx.amt[k] < 0 ? -mx.amt[k] : mx.amt[k]) * 24) >> 9;
                    else if (d == MT_CUT)
                        mcut += x >> 1;
                    else if (d == MT_SHAPE)
                        mshp += x >> 1;
                    else if (d == MT_AMP)
                        mamp += x;
                    else if (d == MT_PAN)
                        pan += x;
                    else if (d >= MT_E1) {
                        if (!(medm & (1u << (d - MT_E1))))
                            med[d - MT_E1] = 0;
                        medm |= 1u << (d - MT_E1);
                        med[d - MT_E1] += x;
                    }
                }
                if ((mx.dsts & (1u << MT_PAN)) && v->age >= pan_age) {   /* the pan: the newest note's */
                    pan_age = v->age;
                    pan_new = pan;
                }
            } else if (mx.srcs & (1u << MS_ENV2)) {
                env2_tick(p, v);
            }
#endif
            if (e->amp)                                 /* the engine's own amplitude curve */
                env = e->amp(t, v, env);
            m.envq15 = e->ownenv ? 0 : env;
            m.amp1 = e->ownenv ? env : mulq15(env, v->vel * 258);
#if CLIPPER_SYNTH
            if (e == &ENG_FMX && t->p[P_VOICE] >= V_UNISON)   /* (after Clipper 1.2) FMX: UNISON's level per voice */
                m.amp1 = mulq15(m.amp1, uni_level(t));
#endif
            if (p[P_LD_AMP])
                m.amp1 = mulq15(m.amp1, 32767 - mulq15((lfo + 32768) >> 1, p[P_LD_AMP] * 258));
#if CLIPPER_SYNTH
            {   /* TREM: up to 60 % of the level away, at the vibrato's rate */
                int32_t tr = (p[P_MWDST] == MD_TREM ? mw : 0) + (p[P_ATDST] == MD_TREM ? (e == &ENG_HYBRID ? 0 : v->pat > cat ? v->pat : cat) : 0);
                if (tr)
                    m.amp1 = mulq15(m.amp1, 32767 - mulq15((vib + 32768) >> 1, (tr > 127 ? 127 : tr) * 155));
            }
            if (mamp)                                   /* the matrix's AMP: 0 .. x1.5 */
                m.amp1 = m.amp1 * clamp(32768 + mamp, 0, 49151) >> 15;
#endif
            if (fade)                                   /* linear to 0 over the fade */
                m.amp1 = m.amp1 * (int32_t)(t->xf - 1u) / (int32_t)XF_BLOCKS;
            m.amp0 = v->env_out;
            v->env_out = m.amp1;
        }
        if (v->pitch_cur != v->pitch16 || v->pitch_frac) {   /* glide, in 1/4096 semitone */
            int32_t q = (v->pitch_cur << 8) + v->pitch_frac, d = (v->pitch16 << 8) - q;
            int32_t st = v->gstep ? v->gstep : (int32_t)((ENV_LIN[p[P_GLIDE] & 127] * 3u) >> 10);   /* RATE: an octave in GLIDE */
            if (st < 1)
                st = 1;
            q += d > 0 ? (d < st ? d : st) : (-d < st ? d : -st);
            v->pitch_cur = q >> 8;
            v->pitch_frac = (uint8_t)(q & 255);
        }
        if (v->penv)                                    /* the pitch envelope: ~40 ms */
            v->penv = (int16_t)(v->penv - ((v->penv * 590 + 32767) >> 15));
        if (!env && !m.amp0 && v->stage == 2 && !eng_sampled(e))
            continue;                                   /* held at a silent sustain (SUS 0): nothing to render */
        {   /* the pitch in 1/4096 semitone: glide, LFO, the pitch envelope; the fraction goes into the increment */
            int32_t q = (v->pitch_cur << 8) + v->pitch_frac + ((lfo * p[P_LD_PIT] * 3) >> 7) +
                        ((v->penv * p[P_ED_PIT] * 3) >> 7) + bend;
#if CLIPPER_SYNTH
            {   /* VIB: up to +-half a semitone */
                int32_t vd = (p[P_MWDST] == MD_VIB ? mw : 0) + (p[P_ATDST] == MD_VIB ? (e == &ENG_HYBRID ? 0 : v->pat > cat ? v->pat : cat) : 0);
                if (vd)
                    q += (vib * (vd > 127 ? 127 : vd)) >> 11;
                q += clamp(mpit, -24 * 4096, 24 * 4096);   /* the matrix */
                if (mac3)                               /* (1.0) MOTN: a vibrato up to +-35 cents */
                    q += (vib * mac3) >> 12;
                if (mpe)                                /* MPE: the note's own bend */
                    q += (mpe_bend[v->ch & 15u] * (int32_t)MPE_RANGE[mpe_mode % MPE_MODES]) >> 3;
                if (tun_on)                             /* (0.9) the TUNING: the note's own pitch (glides too) */
                    q += tun_at(v->pitch_cur);
                if (vint)                               /* (0.9) VINTAGE: the voice's own tuning and drift */
                    q += vintage_pitch(v, (uint32_t)(t - trk) * NVOICE + i, vint);
            }
#endif
            pitch = (q >> 8) + tune;
            m.pitch16 = clamp(pitch, 0, 2047);
            m.inc = pitch_inc(m.pitch16);
            q = (q & 255) * 3792 >> 16;                 /* the fraction, as fine (1/16 st = 14.8) */
            m.fine = v->fine + tune_fine + q;
            if (m.fine)                                 /* unison detune, fine tune and the fraction */
                m.inc += (uint32_t)((int32_t)(m.inc >> 12) * m.fine);
        }
        m.cutoff = ((lfo * p[P_LD_FLT]) >> 7) + ((m.envq15 * p[P_ED_FLT]) >> 7);
        if (v->vel > 110)                               /* accent opens the filter with the env */
            m.cutoff += (m.envq15 * 24) >> 7;
        m.shape = (64 << 8) + ((lfo * p[P_LD_SHP]) >> 7) + ((m.envq15 * p[P_ED_SHP]) >> 7);
#if CLIPPER_SYNTH
        {   /* VEL>F: around velocity 96, up to ~2 octaves up (127) and ~4.5 down (32); CUT: up to ~4 octaves,
             * WAH: the sound's LFO on the cutoff, up to ~+-2 octaves; SHP: the shape up to its top */
            int32_t a = e == &ENG_HYBRID ? 0 : v->pat > cat ? v->pat : cat;
            if (p[P_VELF])
                m.cutoff += ((int32_t)v->vel - 96) * p[P_VELF] * 4;
            if (p[P_MWDST] == MD_CUT)
                m.cutoff += mw * 120;
            if (p[P_ATDST] == MD_CUT)
                m.cutoff += a * 120;
            if (p[P_MWDST] == MD_WAH)
                m.cutoff += (lfo * mw) >> 9;
            if (p[P_ATDST] == MD_WAH)
                m.cutoff += (lfo * a) >> 9;
            if (p[P_MWDST] == MD_SHP)
                m.shape += mw * 128;
            if (p[P_ATDST] == MD_SHP)
                m.shape += a * 128;
            m.cutoff += clamp(mcut, -32767, 32767);    /* the matrix */
            m.cutoff += mac1 * 160;                     /* (1.0) BRITE: +-40 steps (about +-2.5 octaves) */
            if (mac3)                                   /* .. MOTN: LFO 3 on the cutoff, up to +-24 steps */
                m.cutoff += (l3free * mac3) >> 10;
            m.shape = clamp(m.shape + mac2 * 128, 0, 127 << 8);   /* .. TIMBR: +-32 steps of the shape */
            if (vint)                                   /* (0.9) VINTAGE: each voice's filter a little off */
                m.cutoff += ((vcard((uint32_t)(t - trk) * NVOICE + i, 1u) * 400) >> 15) * vint >> 7;
            if (mshp)
                m.shape = clamp(m.shape + clamp(mshp, -32767, 32767), 0, 127 << 8);
            if (mpe && !(mx.srcs & (1u << MS_SLIDE)))  /* MPE: SLIDE to the cutoff when no slot takes it */
                m.cutoff += mpe_slide[v->ch & 15u] * 200;   /* (+-64: about +-3 octaves) */
        }
        if (medm && split) {                            /* (0.7) after the wait: they write the part's values */
            uint32_t k;
            for (k = 0; k < 8u; k++) {
                const param_desc_t *d = &e->edit[k];
                vsl_p[vsl_n][k] = t->p[P_E0 + k];
                if ((medm >> k) & 1u && d->fmt != F_ENUM && d->max > d->min)
                    vsl_p[vsl_n][k] = (int16_t)clamp(t->p[P_E0 + k] + ((clamp(med[k], -32767, 32767) * (d->max - d->min)) >> 15),
                                                   d->min, d->max);
            }
            vsl_v[vsl_n] = v, vsl_m[vsl_n] = m;
            vsl_g0[vsl_n] = sided ? (VS_POS[i % 16u] * spr0 * 23) >> 12 : 0;
            vsl_g1[vsl_n] = sided ? (VS_POS[i % 16u] * spr * 23) >> 12 : 0;
            vsl_n++;
            nr++;
            continue;
        }
        if (medm) {                                     /* the EDIT values the matrix moves: this voice's own */
            int16_t keep[8];
            uint32_t k;
            for (k = 0; k < 8u; k++) {
                const param_desc_t *d = &e->edit[k];
                keep[k] = t->p[P_E0 + k];
                if ((medm >> k) & 1u && d->fmt != F_ENUM && d->max > d->min)
                    t->p[P_E0 + k] = (int16_t)clamp(keep[k] + ((clamp(med[k], -32767, 32767) * (d->max - d->min)) >> 15),
                                                     d->min, d->max);
            }
            vrender(t, e, v, out, n, &m, sided ? (VS_POS[i % 16u] * spr0 * 23) >> 12 : 0,
                    sided ? (VS_POS[i % 16u] * spr * 23) >> 12 : 0, part_side, vs_tmp[0]);
            for (k = 0; k < 8u; k++)
                t->p[P_E0 + k] = keep[k];
            nr++;
            continue;
        }
        if (split) {                                    /* (0.7) rendered after the loop, on both cores */
            vs_v[vs_n] = v, vs_m[vs_n] = m;
            vs_g0[vs_n] = sided ? (VS_POS[i % 16u] * spr0 * 23) >> 12 : 0;
            vs_g1[vs_n] = sided ? (VS_POS[i % 16u] * spr * 23) >> 12 : 0;
            vs_n++;
            nr++;
            continue;
        }
        vrender(t, e, v, out, n, &m, sided ? (VS_POS[i % 16u] * spr0 * 23) >> 12 : 0,
                sided ? (VS_POS[i % 16u] * spr * 23) >> 12 : 0, part_side, vs_tmp[0]);
        nr++;
        continue;
#endif
        e->render(t, v, out, n, &m);
        nr++;
    }
#if CLIPPER_SYNTH
    if (split)
        vs_finish(t, e, out, n);
#endif
#if CLIPPER_SYNTH
    {   /* the matrix's PAN (fx.c mix_part): to the newest note's, 4 steps a block at most (no click) */
        int32_t tgt = (mx.dsts & (1u << MT_PAN)) ? clamp(pan_new >> 9, -64, 64) : 0;
        if (pan_age || !(mx.dsts & (1u << MT_PAN)))
            t->panmod = (int16_t)(t->panmod + clamp(tgt - t->panmod, -4, 4));
    }
#endif
    if (fade) {
        for (i = 0; i < 8u; i++)
            t->p[P_E0 + i] = pe_new[i];
        t->xf--;
    }
    return nr;
}
