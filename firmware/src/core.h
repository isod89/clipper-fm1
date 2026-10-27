/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca core types: tracks, voices, engines, parameters.
 * Four tracks: tracks 1..3 are synth parts (each its own engine, preset, parameters,
 * voices and 64-step pattern), track 4 is the GM drum part (drums.c; its own voices,
 * pattern and the pattern parameters of its track_t). The parts share one budget of
 * NVOICE sounding voices (voice.c). */
#include <stdint.h>
/* Clipper: a synthesizer, one instrument (track 1: song.sel is always 0). Tracks 2..4 and the drum machine stay in
 * the data (the project formats are SLOOP's) but are silent, never selected, their sequencers stopped; MIDI plays the
 * instrument on every channel. 0 = SLOOP's four-track groovebox (the host tests of the shared code build it). */
#ifndef CLIPPER_SYNTH
#define CLIPPER_SYNTH 1
#endif
/* Clipper 0.5.1 (the crackle hunt): written by the audio interrupt (audio.c), shown in GLO > SYSTEM's CPU column
 * (ui_draw.c) */
static volatile uint32_t diag_late;      /* halves the DMA had already moved past: since power-on */
static volatile uint32_t diag_peak;      /* the worst half (render + TIMER5 in it), % of its deadline, since shown */
#ifndef NVOICE
#define NVOICE 8                 /* voices per part, and the budget shared by all parts (Clipper: -DNVOICE=12 / 16) */
#endif
#define NPOLY NVOICE
#define NPART 3                  /* synth parts: tracks 1..3 */
#define NTRK 4                   /* + the drum track */
#define TRK_DRUM 3
enum { V_POLY, V_MONO, V_LEGATO, V_UNISON, V_PUNI2, V_PUNI4 };   /* P_VOICE (Clipper 0.3: UNI2 / UNI4, poly unison) */
static inline int voice_poly(int m) { return m == V_POLY || m >= V_PUNI2; }   /* a note per key, its own voices */
#define NSTEP 64
#define HALF_FRAMES 256          /* I2S half buffer: 5.8 ms at 44.1 kHz */
#ifndef FELUCCA_SLICE
#define FELUCCA_SLICE 0          /* the SLICE engine (eng_slice.c): kept in the tree, not built by default */
#endif
/* SLOOP 2.4: FM6 (eng_fm6.c) is engine 9 in every build; SLICE, when built, comes last (10). Projects and user
 * presets store the engine index as a byte, and no shipped SLOOP was built with SLICE, so its number is stored
 * nowhere: FM6 may take 9 and SLICE moves up (engines.c ENGINES[], params.c N_ENGNAME follow this order) */
#if CLIPPER_SYNTH                /* Clipper: its own engines after HYBRID (SLICE is the groovebox's) */
#if FELUCCA_SLICE
#error "SLICE is a groovebox engine (CLIPPER_SYNTH 0)"
#endif
#define CLIPPER_NENG 10          /* WAVE ACID STRINGS MACRO VPS TERRAIN FMX WSEQ JUNO VECTOR */
#define NENGINES (13 + CLIPPER_NENG)
#define ENGI_WAVE 13u            /* Clipper 0.2: wavetables (eng_wave.c) */
#define ENGI_ACID 14u            /*   a 303-style bass line (eng_acid.c) */
#define ENGI_STRINGS 15u         /*   a string machine (eng_strings.c) */
#define ENGI_MACRO 16u           /*   macro oscillators after Plaits (eng_macro.c) */
#define ENGI_VPS 17u             /* Clipper 0.5: vector phaseshaping (eng_vps.c, Felucca discussion #145) */
#define ENGI_TERRAIN 18u         /*   wave terrain, lite (eng_terrain.c, Felucca discussion #146) */
#define ENGI_FMX 19u             /* Clipper 0.6: six operators, waves, PM / FM / RING, any routing (eng_fmx.c) */
#define ENGI_WSEQ 20u            /*   wave sequencing (eng_wseq.c, after the Wavestation) */
#define ENGI_JUNO 21u            /* Clipper 0.8: the Juno poly and its chorus (eng_juno.c, fx.c juno_chorus) */
#define ENGI_VECTOR 22u          /*   vector synthesis: four waves, a joystick (eng_vector.c, after the Prophet VS) */
#else
#define NENGINES (13 + FELUCCA_SLICE)
#endif
#define ENGI_GRAIN 8u            /* GRAIN, PHYS, NOISE (SLOOP 2.5, appended after FM6: the stores keep the numbers) */
#define ENGI_PHYS 10u
#define ENGI_NOISE 11u
#define ENGI_HYBRID 12u          /* Clipper: the HYBRID voice (eng_hybrid.c), after NOISE */
#define ENGI_SLICE 13u           /* with FELUCCA_SLICE: after HYBRID */
#define ENGI_FM6 9u              /* the FM6 engine's index (eng_fm6.c, the stores: append-only) */
#define UP_SLOTS 32u             /* user presets (upreset.c) */
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------- parameters --- */
enum {
    F_INT, F_PCT, F_BIPCT, F_TIME, F_LFOHZ, F_CUTOFF, F_DB, F_SEMI, F_ENUM, F_BPM, F_NOTE,
    F_ONOFF, F_OCT, F_STEPS,
    F_SWING,                    /* 0..100: MPC swing, 50 % (straight) .. 75 % */
    F_FILT,                     /* -64..63: the DJ filter, LP <- OFF -> HP */
    F_CDB,                      /* Clipper: dB around the middle of the range (the master EQ) */
    F_SPLIT                     /* Clipper: the layer's split, a MIDI note stored + 4 (68 = C4) */
};

typedef struct {
    const char *label;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;   /* F_ENUM */
    const char *unit;           /* F_INT / F_ENUM optional unit */
} param_desc_t;

enum {                          /* per-track parameters */
    P_LEVEL,
    P_ATK, P_DEC, P_SUS, P_REL,
    P_ED_FLT, P_ED_PIT, P_ED_SHP, P_ED_FX,     /* P_ED_FX: the sound's level trim (1/2 dB), set by the presets so
                                                * every factory sound comes out as loud as the others (fx.c) */
    P_LRATE, P_LWAVE, P_LPHASE, P_LFADE,
    P_LD_PIT, P_LD_FLT, P_LD_SHP, P_LD_AMP,
    P_AMODE, P_ARATE, P_AOCT, P_AGATE,
    P_ASWING, P_APROB, P_AHOLD, P_AORDER,
    P_ROOT, P_SCALE, P_QUANT, P_TRANS,
    P_SLEN, P_SDIV, P_SSWING, P_SGATE,
    P_DIST, P_CHOR, P_DLY, P_REV,
    P_VOICE, P_GLIDE, P_PAN, P_MUTE,
    P_GLMODE, P_PRIO, P_ALLOC, P_DETUNE,
    P_SLCR, P_SLPAT, P_SLRATE, P_SLDEPTH,      /* SLICER insert (slicer.c); new common parameters go just
                                                * before P_E0 (user presets and projects map by count) */
    P_CHORD,                                   /* chord mode: one key plays a chord of the scale (seq.c) */
    P_TFLT,                                    /* SLOOP 2.4: the track's filter, < 0 low-pass, > 0 high-pass (fx.c) */
    P_STRUM,                                   /* SLOOP 2.4: a chord's notes one after the other, ms each (> 0 down, < 0 up) */
    P_VLEAD,                                   /* SLOOP 2.4: chord mode, each chord voiced nearest the last (seq.c) */
#if CLIPPER_SYNTH                              /* Clipper 0.4: per sound, every engine (voice.c mod_matrix) */
    P_L2RATE, P_L2WAVE, P_L2MODE,              /* LFO 2: rate, wave, FREE (one per part) / KEY (per note, restarted) */
    P_E2ATK, P_E2DEC, P_E2SUS, P_E2REL,        /* ENV 2: a second ADSR, a modulation source only */
    P_M1SRC, P_M1DST, P_M1AMT,                 /* the matrix: 6 slots of source -> destination x amount (MS_*, MT_*) */
    P_M2SRC, P_M2DST, P_M2AMT,
    P_M3SRC, P_M3DST, P_M3AMT,
    P_M4SRC, P_M4DST, P_M4AMT,
    P_M5SRC, P_M5DST, P_M5AMT,
    P_M6SRC, P_M6DST, P_M6AMT,
    P_ITYPE, P_IA, P_IB, P_IC, P_IMIX,         /* Clipper 0.6: the sound's INSERT (SLOOP 2.6's, fx.c): TYPE, A B C, MIX */
    P_MSRATE, P_MSLEN, P_MSSMTH, P_MSKEY,      /* Clipper 0.8: the MOD SEQ (voice.c mseq_*): a step's length, steps, glide,
                                                * SYNC (the song's grid) / KEY (from each first key) */
    P_SPICE, P_DICE,                           /* .. SPICE / DICE (seq.c spice_*): how much the arp's and the sequencer's
                                                * notes vary, and which variation */
    P_VINT,                                    /* Clipper 0.9: VINTAGE, each voice a little its own (voice.c vintage) */
    P_SLOOP,                                   /* .. LOOP: SPICE's variation comes round every LOOP notes; the arp's
                                                * TURNG mode: its register's length (seq.c turing_step) */
    P_SPREAD,                                  /* Clipper 1.0: the voices across the stereo field (voice.c vspread) */
    P_L3RATE, P_L3WAVE, P_L3MODE,              /* .. LFO 3 (as LFO 2), a matrix source */
    P_E2LOOP,                                  /* .. ENV 2: ONE (an ADSR) / LOOP (attack and decay over and over) */
    P_MAC1, P_MAC2, P_MAC3, P_MAC4,            /* .. the sound's four macros: BRITE TIMBR (+-) MOTN SPACE (0..127), each
                                                * a matrix source too (voice.c, fx.c) */
    P_M7SRC, P_M7DST, P_M7AMT,                 /* .. the matrix's slots 7 to 12 (mx_pid) */
    P_M8SRC, P_M8DST, P_M8AMT,
    P_M9SRC, P_M9DST, P_M9AMT,
    P_M10SRC, P_M10DST, P_M10AMT,
    P_M11SRC, P_M11DST, P_M11AMT,
    P_M12SRC, P_M12DST, P_M12AMT,
#endif
    P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7,
    P_COUNT
};
/* Clipper: the synth has no slicer, so its pattern's slot holds the filter MODEL of the engines with a low-pass of
 * their own (ANALOG): 1 SVF (12 dB, as before), 2 MOOG (the 24 dB ladder, dsp.c). The same slot: projects and user
 * presets keep it without a new format */
#define P_FMODEL P_SLPAT
enum { FM_SVF = 1, FM_MOOG = 2, FM_MS20, FM_DIODE, FM_SEM, FM_NOTCH, FM_BP, FM_HP, FM_LAST = FM_HP };   /* (0.9: dsp.c fmod_*) */
/* Clipper: two globals of the groovebox are free in the synth (no kick to duck to, no note repeat): the reverb's
 * TYPE (ROOM, PLATE: fx.c) and the plate's PRE-delay (ms) */
#define G_RTYPE G_DUCK
#define G_RPRE G_ROLL
enum { RT_ROOM, RT_PLATE, RT_HALL, RT_SHIMMER };   /* (1.0: HALL and SHIMMER: the plate's tank, fx.c) */
enum { DM_MONO, DM_PING, DM_TAPE };   /* (1.0) G_DMODE */
/* Clipper 0.3: the drum globals are the master bus (bus.c): EQ LOW / MID / HIGH, dB around their centre (so a SLOOP
 * project, its drum level 100, reverb 16, channel 10, loads flat), and COMP (the drums' delay send: 0 = off) */
#define G_EQLOW G_DRLVL
#define G_EQMID G_DRCH
#define G_EQHIGH G_DRREV
#define G_COMP G_DRDLY
#define EQ_LOW_C 100
#define EQ_MID_C 10
#define EQ_HIGH_C 16
/* Clipper 0.3: the per-sound modulation (voice.c track_render) in the slicer's slots (the synth has none):
 * VEL>F velocity to the cutoff (bipolar), the mod wheel's and the aftertouch's destination (MD_*) */
#define P_VELF P_SLCR
#define P_MWDST P_SLRATE
#define P_ATDST P_SLDEPTH
enum { MD_OFF, MD_VIB, MD_CUT, MD_SHP, MD_WAH, MD_TREM };
/* Clipper 0.3: the layer (track 2's sound under or beside track 1's: voice.c layer_*) keeps its settings in track 2's
 * pattern parameters (its sequencer never runs in the synth): MODE (OFF, LAYER, SPLIT), SPLIT (the lowest note of
 * the main sound, stored + 4: a SLOOP default of 64 is C4), OCT (-2..2, stored + 2); its level is track 2's LEVEL */
#define P_LYMODE P_SSWING
#define P_LYSPLIT P_SGATE
#define P_LYOCT P_SDIV
#define P_LYCH P_SLEN            /* Clipper 0.6, MULTI: the MIDI channel (1..16) the layer's sound answers on its own */
enum { LY_OFF, LY_LAYER, LY_SPLIT, LY_MULTI };   /* MULTI (0.6): two parts, the layer on its own channel (bi-timbral) */
/* Clipper 0.4: the matrix's sources and destinations (P_MnSRC, P_MnDST; voice.c mod_matrix) */
#define MX_SLOTS 12u             /* (1.0: 12; slots 1..6 at P_M1SRC, 7..12 at P_M7SRC: mx_pid) */
#define MX_SLOTS6 6u
enum { MS_OFF, MS_LFO1, MS_LFO2, MS_ENV1, MS_ENV2, MS_VEL, MS_KEY, MS_MW, MS_AT, MS_RAND, MS_SLIDE,
       MS_SEQ1, MS_SEQ2, MS_SEQ3, MS_SEQ4,      /* (0.8: the MOD SEQ's four lanes) */
       MS_LFO3, MS_MAC1, MS_MAC2, MS_MAC3, MS_MAC4, MS_NSRC };   /* (1.0: LFO 3, the macros; 5 bits: the user presets'
                                                * record keeps 4 of slots 1..6's, the fifth in the bank's tail) */
#if CLIPPER_SYNTH
/* a matrix slot's first parameter (its source; + 1 destination, + 2 amount) */
#define mx_pid(k) ((uint32_t)((k) < MX_SLOTS6 ? P_M1SRC + 3u * (k) : P_M7SRC + 3u * ((k) - MX_SLOTS6)))
/* the matrix slot (0..11) of a parameter, -1: none; *f its field (0 source, 1 destination, 2 amount) */
static inline int mx_slot_of(uint32_t i, uint32_t *f)
{
    if (i >= P_M1SRC && i <= P_M6AMT)
        return *f = (i - P_M1SRC) % 3u, (int)((i - P_M1SRC) / 3u);
    if (i >= P_M7SRC && i <= P_M12AMT)
        return *f = (i - P_M7SRC) % 3u, (int)(MX_SLOTS6 + (i - P_M7SRC) / 3u);
    return -1;
}
#endif
#define MSEQ_LANES 4u            /* Clipper 0.8: the MOD SEQ, per sound: 4 lanes of 16 steps, -8..7 each */
#define MSEQ_STEPS 16u
enum { MT_OFF, MT_PITCH, MT_CUT, MT_SHAPE, MT_AMP, MT_PAN, MT_L2RATE, MT_E1, MT_COUNT = MT_E1 + 8 };
enum { L2_FREE, L2_KEY };
#define CLIPPER_PROJ_MARK 0xC5u  /* project_t.rsv[0]: written by Clipper (project.c: + LFO 2, ENV 2, the matrix in track 3's
                                  * free lock slots, SLOOP sees nothing of them) */
#define CLIPPER_PROJ_MARK_06 0xC6u   /* (0.6: the INSERT of both sounds after them, project.c proj_ext_put) */
#define CLIPPER_PROJ_MARK_04 0xC4u   /* by Clipper 0.4's first build (the same, in tracks 3 and 4's p[]) */
#define CLIPPER_PROJ_MARK_03 0xC3u   /* by Clipper 0.3 (else the bus, layer, mod reset) */

enum {                          /* global parameters */
    G_BPM, G_SWING, G_CLOCK, G_TUNE,
    G_DTIME, G_DFDBK, G_DCOLOR, G_DMIX,
    G_RSIZE, G_RDAMP, G_CRATE, G_CDEPTH,
    G_MIDI, G_SYNC, G_ROUTE, G_INFO,
    G_SLOT, G_NAME, G_LOAD, G_SAVE,
    G_ENGSEL, G_ENGGO,          /* no page (the ENGINE page is gone); a SET of G_ENGSEL switches the engine (editor) */
    G_CLRSEQ, G_INITSND,
    G_DRCH, G_DRLVL, G_DRREV,
    G_DUST, G_DUCK, G_FILT,     /* the master bus: lo-fi / vinyl, the kick ducking the parts, the DJ filter (fx.c) */
    G_ROLL,                     /* note repeat rate (ARP + key, seq.c) */
    G_NEWPRJ,                   /* TOOLS > NEW: a new project (GO) */
    G_DRDLY,                    /* SLOOP 2.5: the drums' delay send (GLO > DRUMS; a project keeps it in its own byte) */
#if CLIPPER_SYNTH
    G_DMODE,                    /* Clipper 1.0: the delay MONO / PING (ping-pong) / TAPE (wow, flutter, saturation)
                                 * (a project keeps it with 1.0's values, project.c) */
#endif
    G_COUNT
};

/* ----------------------------------------------------------- voices --- */
typedef struct {
    uint8_t note, vel, gate, active;
    uint8_t stage;               /* env: 0 off, 1 attack, 2 decay/sustain, 3 release, 4 fading out (given up) */
    uint8_t kill;                /* stage 4: blocks of the fade left */
    uint8_t pitch_frac;          /* the glide between 1/16 semitones: pitch_cur + pitch_frac / 256 */
    uint8_t pend;                /* (SLOOP 2.8.3) VP_*: a sample's note waiting for its voice's fade (voice.c voice_start) */
    int16_t penv;                /* the pitch envelope (ENV > PIT): Q15, a fast fall from the note's start */
    uint8_t pend_note, pend_vel; /* (SLOOP 2.8.3) that note (in the padding: voice_t keeps its size) */
    int32_t env;                 /* Q24 */
    int32_t env_out;             /* last control-rate amplitude, Q15 */
    int32_t pitch16, pitch_cur;  /* 1/16 semitone, with glide */
    int32_t gstep;               /* glide: 1/4096 semitone per control tick, 0 = RATE (GLIDE: the time an octave takes) */
    int32_t fine;                /* unison detune: phase increment * (1 + fine / 4096) */
    uint8_t pat;                 /* Clipper: polyphonic aftertouch of the note, 0..127 (MIDI An) */
    uint8_t sus;                 /* Clipper: released while the sustain pedal is down: held until it comes up */
    uint32_t ph[3];
    int32_t s[8];                /* engine state (filters, envs) */
    uint32_t age;
#if CLIPPER_SYNTH                /* Clipper 0.4: the voice's own modulation (voice.c) */
    uint32_t l2ph;               /* LFO 2 in KEY mode: its phase, from the note's start */
    int32_t env2;                /* ENV 2, Q24 */
    int16_t rnd, l2rnd;          /* a random value per note; LFO 2's S&H value */
    uint8_t e2st;                /* ENV 2: 0 off, 1 attack, 2 decay / sustain, 3 release */
    uint32_t l3ph;               /* (1.0) LFO 3 in KEY mode */
    int16_t l3rnd;               /* .. its S&H value */
    int16_t vdrift, vdtgt;       /* (0.9) VINTAGE: the voice's slow pitch drift now, and where it goes (Q15) */
    uint8_t ch;                  /* the MIDI channel its note came on (MPE: its own bend, pressure, slide) */
#endif
} voice_t;

typedef struct {                 /* per-voice control-rate modulation, computed in voice.c */
    uint32_t inc;                /* phase increment of the base pitch */
    int32_t pitch16;
    int32_t amp0, amp1;          /* Q15 ramp over the block */
    int32_t cutoff;              /* 0..127 << 8 */
    int32_t shape;               /* 0..127 << 8 */
    int32_t envq15;              /* env value (for engines that use it as a mod source) */
    int32_t fine;                /* the residual below 1/16 semitone in inc: unison detune, TUNE, the glide's
                                  * fraction (1/4096); FM6 reads it (the others take inc) */
} vmod_t;

typedef struct {
    const char *name;
    int8_t e[8];                 /* P_E0..P_E7 (signed: an interval below the note; every value fits) */
    uint8_t env[4];              /* ATK DEC SUS REL */
    int8_t fenv;                 /* ENV -> FILTER amount (-64..63) */
    uint8_t mono;                /* 1 = MONO (bass / lead), 0 = POLY */
    /* the rest of the patch; each value is stored + 1, 0 = the default */
    uint8_t fx[4];               /* DIST, CHORUS, DELAY, REVERB sends */
    uint8_t arp[4];              /* MODE, RATE, OCT, GATE */
    uint8_t pat;                 /* sequence pattern (PATTERNS[pat - 1]), loaded only into an empty sequencer */
    int8_t x[8];                 /* more of the patch: up to 4 (track parameter id + 1, value) pairs, 0 = none
                                  * (glide, pitch / LFO modulation, voice mode...: preset_extras) */
} preset_t;
#define FX(d, c, dl, r) .fx = {(d) + 1, (c) + 1, (dl) + 1, (r) + 1}
#define ARP(m, rt, o, g) .arp = {(m) + 1, (rt) + 1, (o) + 1, (g) + 1}
#define PAT(n) .pat = (n)
#define XP(...) .x = {__VA_ARGS__}  /* XP(P_GLIDE + 1, 30, P_ED_PIT + 1, 8): parameter id + 1, value */

struct track;
typedef struct {
    const char *name;            /* "VA" */
    const char *page_title[2];
    param_desc_t edit[8];        /* P_E0..P_E7 */
    const preset_t *presets;
    uint8_t npresets;
    int8_t fil_page;             /* EDIT page that holds the filter, -1 = none */
    void (*note_on)(struct track *t, voice_t *v);
    void (*render)(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);
    uint16_t color;              /* accent colour of the engine (RGB565) */
    uint8_t macro[4];            /* HOME: the four parameters on KNOB 1..4 */
    uint8_t poly;                /* voice cap for POLY and UNISON, 0 = NVOICE */
    /* optional (0 = none): the voice amplitude instead of the ADSR curve, once per control tick;
     * gets the ADSR value (Q15, env_tick already ran: it still gates the voice), returns Q15 */
    int32_t (*amp)(struct track *t, voice_t *v, int32_t adsr);
    /* optional: a mode-dependent descriptor of EDIT k (the same range and default as edit[k],
     * another label / value names), 0 = edit[k] */
    const param_desc_t *(*desc)(const struct track *t, uint32_t k);
    /* optional: once per block and part, before its voices (also with no voice sounding) */
    void (*block)(struct track *t);
    /* 1 = the engine's own envelopes are the voice's amplitude (FM6): voice.c renders it at 1.0 (no ADSR, no
     * velocity; LFO -> AMP and the fades still apply; ENV is 0) and the voice ends when done() says so (once
     * per control tick, before the render), not at the end of the ADSR's release */
    uint8_t ownenv;
    int (*done)(struct track *t, voice_t *v);
} engine_t;

/* ------------------------------------------------------------ track --- */
enum { ST_NOTE, ST_TIE, ST_REST };
#define SF_ACCENT 1u
#define SF_SLIDE 2u
/* (0.7, after SLOOP 2.7) bits 3..6 of a NOTE step's flags: note k goes on from the step before (not triggered, not
 * released), so notes held in the pattern carry on under a note recorded over them (seq.c rec_note, seq_step). An
 * older version ignores them (the notes play again there). Bit 2 stays free (the user presets' patterns use 4 for a tie) */
#define SF_CONT0 8u
#define SF_CONT(k) (SF_CONT0 << (k))
#define SF_CONT_ALL (15u * SF_CONT0)
/* SLOOP 2.7.2: bit 7 of a NOTE step's flags (MONO / LEGATO / UNISON parts): played legato, recorded while the note
 * before was still held: the note before is held to it and this one takes over as a held key does (LEGATO: the
 * envelope goes on; a glide only with GLIDE or a SLIDE), not released and played again (seq.c rec_note, seq_step).
 * An older version ignores it (the note plays again there) */
#define SF_LEG 0x80u
/* the dynamics of a note or a drum hit (2 bits each in the steps): played without velocity
 * (OCT- / OCT+ held on the drum track, the step page) or from a MIDI velocity */
enum { LV_NORM, LV_GHOST, LV_SOFT, LV_HARD };
typedef struct {                 /* a step of a synth part (10 bytes): up to 4 notes (POLY), time, accent, slide */
    uint8_t note[4];
    uint8_t n;                   /* notes in use, 0 = empty */
    uint8_t time;                /* ST_NOTE / ST_TIE / ST_REST */
    uint8_t flags;               /* SF_ACCENT | SF_SLIDE */
    uint8_t vel;
    uint8_t lvl;                 /* 2 bits per note (note k: bits 2k..2k+1): LV_* */
    uint8_t rat;                 /* 2 bits per note: the hits it plays in its step - 1 (ratchet x1..x4) */
} step_t;
#define DRUM_LANES 16            /* the drum track: 16 sounds, one per white key (drums.c LANE_*) */
typedef struct {                 /* a step of the drum track (10 bytes, the size of a step_t) */
    uint8_t on[2];               /* bit per lane */
    uint8_t lvl[4];              /* 2 bits per lane (lane k: byte k / 4, bits 2 (k % 4)..): LV_* */
    uint8_t rat[4];              /* 2 bits per lane: ratchet */
} dstep_t;
_Static_assert(sizeof(step_t) == 10 && sizeof(dstep_t) == 10, "a step is 10 bytes on every track");
/* SLOOP 2.4: per-step nudge (micro timing) and parameter locks (seq.c seq_tick / lock_step) */
#define MICRO_MIN (-32)          /* a step's nudge in 1/64 of its length: half a step early .. */
#define MICRO_MAX 31             /* .. just under half a step late (0 = on the grid) */
#define NLOCK 24                 /* parameter locks per track (several may share a step: other params) */
typedef struct {                 /* a lock: on step `step` the track's p[param] is `val` (4 bytes) */
    uint8_t step;                /* 0xFF = a free slot */
    uint8_t param;               /* P_* (a lockable one: p_lockable) */
    int16_t val;
} plock_t;
#define LOCK_FREE 0xFFu
/* a step's condition (SLOOP 2.4 fill, GLO + key 9 / 10): plays always, only during a fill, never during one */
enum { FC_NORM, FC_FILL, FC_NOFILL };     /* (3: as FC_NORM) */

typedef struct track {
    int16_t p[P_COUNT];
    uint8_t engine, preset;      /* engine: what the audio ISR renders */
    uint8_t eng_req;             /* engine the UI asked for (the ISR switches at a block start) */
    uint8_t user;                /* user preset slot + 1 the sound came from (UI), 0 = none */
    voice_t v[NVOICE];
    /* LFO */
    uint32_t lfo_ph;
    int32_t lfo_val;             /* Q15 */
    int32_t lfo_fade;            /* Q15 ramp after note-on */
    uint32_t lfo_rnd;
    /* keyboard / arp input: held notes in press order */
    uint8_t held[16];
    uint8_t nheld;
    uint8_t arp_phys;            /* keys physically held for the arp */
    uint8_t latched;             /* HOLD: keep notes after release */
    /* arp runtime (clock units: samples x BPM, fx.c BEAT_U) */
    uint32_t arp_pos;            /* stopped: units into the current arp step */
    uint32_t arp_abs;            /* playing: the arp step of the transport grid last played */
    uint32_t arp_idx;
    uint8_t arp_note;            /* sounding arp note, 0 = none */
    uint8_t arp_new;             /* a chord just started: its first note now */
    uint8_t arp_ch[3];           /* (0.7, after SLOOP 2.7) MODE CHORD: the other notes sounding with arp_note, 0 = none */
    uint8_t arp_walk;            /* (0.7) MODE WALK: the place in the note list it stands on */
    uint8_t arp_odd;             /* (0.7) stopped: the next arp step is an odd one (its SWG: late) */
    uint32_t arp_off;            /* units to its note-off */
    uint16_t tm_reg;             /* (0.9) the arp's TURNG: its shift register */
    int16_t tm_seed;             /* .. the DICE it was seeded with (+ 1; 0: not yet) */
#if CLIPPER_SYNTH                /* Clipper 0.8: the MOD SEQ (voice.c mseq_*), part of the sound */
    int8_t ms[MSEQ_LANES][MSEQ_STEPS];   /* the steps, -8..7 */
    uint8_t ms_step, ms_held;    /* the step playing; keys held last block (KEY: the first one restarts it) */
    uint32_t ms_pos;             /* units into the step (free running: stopped, or KEY) */
    int32_t ms_cur[MSEQ_LANES];  /* each lane's value now (Q15, glided: SMTH) */
    uint8_t fm_eff;              /* (0.9) the filter model the engines play (P_FMODEL after the duck: fx.c part_duck) */
    uint8_t dk_phase, dk_pos;    /* .. the part's duck: 0 none, 1 out, 2 held silent, 3 in; samples into its half */
    uint8_t dk_free, dk_hold;    /* .. a change may land now (silent, or held); blocks held */
#endif
    /* sequencer: synth parts step[], the drum track dstep[] (16 lanes) */
    union {
        step_t step[NSTEP];
        dstep_t dstep[NSTEP];
    };
    int8_t micro[NSTEP];         /* each step's nudge, MICRO_MIN..MICRO_MAX (1/64 of a step; - early, + late) */
    plock_t lock[NLOCK];         /* parameter locks, unsorted (step LOCK_FREE = a free slot) */
    uint8_t fill[NSTEP / 4];     /* each step's condition, 2 bits (seq.c step_fill): FC_* */
    /* the locks in force (seq.c lock_step): the params overridden now, what they were, what the lock set */
    uint8_t lk_n;
    uint8_t lk_param[NLOCK];
    int16_t lk_base[NLOCK];
    int16_t lk_set[NLOCK];
    uint32_t seq_abs;            /* the step of the transport grid last played (seq.c trk_grid), SEQ_NONE */
    uint16_t seq_idx;            /* its index in the pattern */
    uint8_t seq_den;             /* the DIV it was played on (a DIV change waits for the next step) */
    uint8_t arp_den;             /* the same for the arp's RATE */
    uint8_t seq_notes[4];        /* sounding seq notes */
    uint8_t seq_n;
    uint8_t seq_hold;            /* last step slides: keep the notes until the next step */
    uint8_t seq_skip;            /* the step playing failed its fill condition: nothing of it sounds */
    uint8_t slide_glide;         /* next legato note glides (slide) */
    uint32_t seq_off;            /* units to the note-off of the step's notes */
    uint8_t seq_active;          /* any step programmed */
    uint8_t rat_done[4];         /* ratchet hits played in this step: per note (synth) */
    uint32_t rat_lanes;          /* .. per lane (drums): 2 bits each */
    uint32_t rskip_abs;          /* live recording put notes into the step about to play: */
    uint8_t rskip_n, rskip[4];   /* do not trigger them again there (they sound already) */
    uint16_t rskip_lanes;        /* (the drum track: lanes) */
    /* live recording of held notes (seq.c rec_hold): the steps they are held into become TIEs */
    uint8_t rh_n, rh_note[4];    /* recorded notes still held, 0 = none */
    uint8_t rh_start;            /* the step they were recorded into */
    uint8_t rh_ties;             /* TIE steps written after it */
    uint8_t rh_last;             /* the last of them; rh_bak: what it held (an early release puts it back) */
    uint32_t rh_last_abs;        /* (its step of the transport grid) */
    step_t rh_bak;
    step_t rh_put;               /* (SLOOP 2.8.3) what the hold wrote into rh_last (an early release puts rh_bak back only over it) */
    uint8_t rh_end;              /* (SLOOP 2.7.1) one of them let go, others still held: the next step, only those go on */
    uint8_t rh_rs;               /* (2.7.1) rh_start is such a step (rh_last, rh_last_abs: it) */
    uint8_t rh_offn, rh_off[4];  /* (2.7.1) the ones let go since the hold's last NOTE step (they end there) */
    uint32_t pass;               /* loops played since PLAY (a recording pass: one undo) */
    /* mono */
    uint8_t mono_stack[8];
    uint8_t nmono;
    uint8_t mono_note;           /* note the MONO / LEGATO / UNISON voice(s) play, 0 = none */
    uint8_t rr;                  /* POLY ROTATE: next voice to try */
    /* mix runtime */
    int32_t lvl;                 /* the LEVEL gain (Q12) of the last block: a change is ramped (fx.c) */
    int32_t gpl, gpr;            /* (0.7) the PAN gains (Q12) of the last block: a change glides too */
    uint8_t mix_on;              /* (0.7) lvl / gpl / gpr hold a block's values (0: the first block starts at its own) */
    int32_t peak;
    int32_t dist_hp, dist_lp1, dist_lp2;   /* DIST insert state (fx.c) */
    int32_t att;                 /* mute / solo fade: attenuation, Q15 (0 = heard; fx.c mix_part, drums_mix) */
    uint8_t dist_on;             /* DIST was on in the last block (its states restart when it comes on) */
    int16_t dist_last;           /* (Clipper 0.9) DIST's amount while it fades out (turned to 0 under a sound) */
    int32_t dist_w;              /* .. its wet share, Q15: DIST turned on or off under a sound fades (no click) */
    int32_t snd_c, snd_d, snd_r; /* .. the sends of the last block: a change glides as LEVEL's does */
    uint8_t tail;                /* blocks to mix after the last voice (the DIST tail) */
    int16_t armp, aholdp;        /* P_AMODE / P_AHOLD as last seen by the ISR */
    /* engine switch (voice.c engine_block): the old engine's voices fade out, then it switches */
    uint8_t xf_on, xf;           /* fading; blocks of the fade still to render */
    int16_t pe_old[8];           /* P_E0..P_E7 of the sounding engine: the fade renders with these */
    uint8_t xp_n, xp_note[4], xp_vel[4];   /* note-ons during the fade, played on the new engine */
#if CLIPPER_SYNTH
    uint8_t xf_snd;              /* Clipper 0.3: the fade is a sound change (a preset, a SWAP...): the old sound */
    int16_t p_old[P_E0];         /* fades with all its own values (voice.c sound_fade_begin, fx.c mix_part) */
    uint32_t l2ph;               /* Clipper 0.4: LFO 2 in FREE mode (one for the part) */
    int16_t l2rnd;               /* its S&H value */
    int16_t panmod;              /* the matrix's PAN, from the part's sources (fx.c mix_part) */
    uint32_t l3ph;               /* (1.0) LFO 3 in FREE mode */
    int16_t l3rnd;
    int16_t spr;                 /* (1.0) SPREAD as the last block had it (a change glides through the block) */
    int32_t sd_g;                /* (1.0) the side's gain after DIST / the INSERT (Q15, 0: none yet) */
    uint8_t ins_run;             /* Clipper 0.6: the INSERT ran (or faded out) in the last block (fx.c track_insert) */
    uint8_t ins_xt;              /* .. blocks it ran on after the last voice (its tail: a flanger's feedback) */
    uint16_t ins_res;            /* .. |its last sample| (a tail still ringing) */
#endif
} track_t;

typedef struct {
    int16_t g[G_COUNT];
    uint8_t playing, seq_mode;
    uint8_t rec;                 /* live recording armed: bit per track */
    uint8_t sel;                 /* selected track 0..NTRK-1: keys, pages, editor */
    int8_t octave;
    volatile uint8_t solo;       /* bit per track soloed (GLO + key), 0 = none: the others are silent */
    uint32_t tick;               /* sub-blocks since play */
    uint32_t cpu_q8;             /* audio ISR load, 1/256 */
    uint32_t master_q12;
    int32_t batt_raw;            /* smoothed ADC ch3 (battery divider), 0 = not read yet */
} song_t;

static track_t trk[NTRK];        /* the instrument: three parts and the drum track */
static song_t song;
#if CLIPPER_SYNTH
/* Clipper 0.6: the layer in MULTI (the two sounds two parts: the layer on its own MIDI channel, its own controls) */
static inline int layer_multi(void) { return trk[1].p[P_LYMODE] == LY_MULTI; }
/* the part whose performance controls (bend, mod wheel, pressure, pedal: part_ctl) a track follows: the instrument's,
 * the layer's own in MULTI */
static inline uint32_t ctl_part(const track_t *t) { return layer_multi() ? (uint32_t)(t - trk) % NPART : 0u; }
static inline uint32_t layer_ch(void) { return (uint32_t)(trk[1].p[P_LYCH] < 1 ? 1 : trk[1].p[P_LYCH] > 16 ? 16 : trk[1].p[P_LYCH]); }
#else
static inline uint32_t ctl_part(const track_t *t) { return (uint32_t)(t - trk) % NPART; }
#endif

/* The transport clock (seq.c runs it). One unit = one sample at 1 BPM: a beat is BEAT_U units at any
 * tempo, and every division of it (1/4 .. 1/64, the triplets) is a whole number of units. The steps
 * of every track, the arp, the rolls, the click, the SLICER and the song arranger (which counts the
 * same units) all follow it: no rounding, no drift between them, a tempo change moves them together.
 * clk_beat: beats since PLAY, clk_pos: units into that beat, at the start of the block being rendered
 * (events_block advances it by CTL x BPM after the block's events). */
#ifndef FS
#define FS 44100                 /* (as build/gen/felucca_tables.h: tools/gen_tables.py) */
#endif
#define BEAT_U ((uint32_t)FS * 60u)
static volatile uint32_t clk_beat, clk_pos;
static const uint8_t DIV_DEN[6] = {1, 2, 4, 8, 3, 6};    /* N_DIV 0..5: beats = 1 / DEN */
#define NDIV_SHORT 6u            /* the divisions inside a beat (N_DIV, the arp's RATE) */
#define NDIV_STEP 9u             /* N_SDIV: + 1/2 note, a bar, two bars (whole beats: DIV_BEATS) */
#define NDIV_DLY 8u              /* N_DLY: + 1/8 and 1/16 dotted */
static const uint8_t DIV_BEATS[3] = {2, 4, 8};
/* a step's length in clock units (N_SDIV order; the arp's RATE uses 0..5) */
static uint32_t div_units(uint32_t div)
{
    return div < NDIV_SHORT ? BEAT_U / DIV_DEN[div] : BEAT_U * DIV_BEATS[(div - NDIV_SHORT) % 3u];
}
/* the delay's TIME in clock units (N_DLY order) */
static uint32_t dly_units(uint32_t d)
{
    return d < NDIV_SHORT ? BEAT_U / DIV_DEN[d] : d == NDIV_SHORT ? BEAT_U * 3u / 4u : BEAT_U * 3u / 8u;
}
/* length of one step (N_SDIV order) in samples at the song tempo (rounded down) */
static uint32_t div_samples(uint32_t div) { return div_units(div) / (uint32_t)song.g[G_BPM]; }
/* length of the delay's TIME (N_DLY order) in samples at the song tempo (rounded down) */
static uint32_t dly_samples(uint32_t d) { return dly_units(d) / (uint32_t)song.g[G_BPM]; }
#define TSEL (&trk[song.sel])    /* the selected track */
#define TDRUM (&trk[TRK_DRUM])
static int is_drum(const track_t *t) { return t == TDRUM; }
/* silent: MUTE, or another track is soloed */
static int trk_silent(const track_t *t)
{
    uint32_t i = (uint32_t)(t - trk);
    if (CLIPPER_SYNTH && i)      /* Clipper: only the instrument (track 1) sounds, and its layer (track 2) when on */
        return i != 1u || (trk[1].p[P_LYMODE] != LY_LAYER && trk[1].p[P_LYMODE] != LY_SPLIT && trk[1].p[P_LYMODE] != LY_MULTI);
    return t->p[P_MUTE] || (song.solo && !((song.solo >> i) & 1u));
}
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */
/* Two early failed boots -> USB recovery; recovery reset -> mask-ROM UBOOT. */
#include "bootguard.h"
bootguard_t bootguard __attribute__((section(".noinit")));
