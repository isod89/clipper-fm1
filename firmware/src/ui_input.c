/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca UI input: LEDs, knobs and buttons, the layers (ui_layers.c), holds, SEQ step entry, panel setup. */
/* ----------------------------------------------------------- LEDs --- */
/* The LED picture is built off-line and copied one byte per column: clearing
 * and relighting would let the 10 kHz scan catch the dark gap and flicker. */
#ifndef UI_SMALL
#define UI_SMALL                                   /* (gfx.c: minsize on the device; a test alone: nothing) */
#endif
static uint8_t led_pos[41];                        /* (col << 3) | row bit, 0xFF = none */

UI_SMALL static void led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < 41u; id++) {
        led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    led_pos[id] = (uint8_t)((p << 3) | r);
    }
}

UI_SMALL static void led_put(uint8_t *nl, uint32_t id, int on)
{
    uint8_t q = led_pos[id];
    if (q != 0xFF && on)
        nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
}

static const uint8_t FAM_BTN[FAM_COUNT] = {B_HOME, B_ENV, B_LFO, B_FX, B_SCL, B_EDIT, B_GLO, B_SAVE,
                                           B_ARP, B_SEQ, B_HOME};   /* button of each page family (TRACKS: HOME) */

UI_SMALL static uint32_t cur_fam(void) { return ui.home ? FAM_HOME : cur_page()->fam; }
UI_SMALL static uint32_t cur_btn(void)                      /* the button of the screen shown */
{
    if (!ui.home && cur_page()->scope == SC_SONG)
        return B_SAVE;
    if (!ui.home && cur_page()->scope == SC_DRUM)
        return B_SEQ;
    return FAM_BTN[cur_fam()];
}

/* PLAY flashes on every beat, as heard (the sound leaves ~9 ms after it is rendered) */
UI_SMALL static int play_led(void)
{
    uint32_t p = clk_pos, lat = HALF_FRAMES * 3u / 2u * (uint32_t)song.g[G_BPM];
    if (!song.playing)
        return 0;
    p = p >= lat ? p - lat : p + BEAT_U - lat;
    return p < BEAT_U / 4u;
}

/* the keys' lights: what the layer held does, else the keys down and the drum hits */
/* what sounds on track t, as keys: the drum hits (each lights its key a few frames) or, on a synth
 * track, the voices still held, mapped back through the keyboard (octave, scale, chords; by
 * @renebohne, PR #11). Shown at once with menu NOTES on, on every screen and layer (keys_notes_dim) */
UI_SMALL static uint32_t keys_sounding(const track_t *t)
{
    uint32_t i, m = 0;
    if (is_drum(t)) {
        for (i = 0; i < DRUM_LANES; i++)
            if (pad_lit[i])
                m |= 1u << key_of_white(i);
        return m;
    }
    for (i = 0; i < NVOICE; i++) {
        const voice_t *v = &t->v[i];
        if (v->active && v->gate && v->stage <= 2) {
            uint32_t k, note = v->note;
            for (k = 0; k < 27u; k++)
                if (kb_map(t, k) == note)
                    m |= 1u << k;
        }
    }
    if (t == TSEL)                                 /* + the notes just started, a few frames (pads_tick) */
        for (i = 0; i < 27u; i++)
            if (key_lit[i])
                m |= 1u << i;
    return m;
}

UI_SMALL static uint32_t scale_keys(uint32_t root_only)   /* SEL: the keys in the scale (or its roots only) */
{
    uint32_t i, m = 0, root = (uint32_t)trk[0].p[P_ROOT] % 12u, mask = SCALE_MASK[clamp(trk[0].p[P_SCALE], 0, NSCALES - 1)];
    for (i = 0; i < 27u; i++) {
        uint32_t d = (53u + i - root + 120u) % 12u;
        if ((mask >> d) & 1u && (!root_only || !d))
            m |= 1u << i;
    }
    return m;
}

UI_SMALL static uint32_t erase_lanes(const track_t *t)   /* EDIT erase, drum track: the sounds the pattern holds */
{
    uint32_t i, m = 0;
    if (is_drum(t))
        for (i = 0; i < trk_len(t); i++) {
            uint32_t l, d = dstep_mask(&t->dstep[i]);
            for (l = 0; d; l++, d >>= 1)
                if (d & 1u)
                    m |= 1u << key_of_white(l);
        }
    return m;
}

UI_SMALL static uint32_t keys_lit(void)
{
    uint32_t m = 0, i, blink = (fm1_ms / 125u) & 1u;
    track_t *t = TSEL;
    switch (ui.layer) {
    case LY_FX:
        return punch.req >= 0 ? 1u << key_of_white((uint32_t)punch.req) : 0u;
    case LY_STEP: {                                /* the steps that play; the playhead blinks */
        uint32_t len = trk_len(t);
        for (i = 0; i < 16u; i++) {
            uint32_t idx = ui.step_page * 16u + i, on;
            if (idx >= len)
                continue;
            on = is_drum(t) ? dstep_has(&t->dstep[idx], pen_lane) : step_on(&t->step[idx]);
            if (song.playing && idx == t->seq_idx)
                on = !on || blink;
            if (on)
                m |= 1u << key_of_white(i);
        }
        return m | fm1_in.notes;
    }
    case LY_SCALE:                                 /* the keys in the scale; the root blinks */
        if (lights_notes)                          /* NOTES: the scale goes dim (keys_notes_dim), what sounds lit */
            return (blink ? scale_keys(1) : 0u) | keys_sounding(t) | fm1_in.notes;
        return scale_keys(0) & ~(blink ? 0u : scale_keys(1));
    case LY_MIX:                                   /* tracks heard: 1..4; soloed: 5..8; a fill: 9, armed / on: 10; tap: the beat */
        for (i = 0; i < 4u && !CLIPPER_SYNTH; i++) {   /* (Clipper: one instrument, no mute / solo) */
            if (!trk_silent(&trk[i]))
                m |= 1u << key_of_white(i);
            if ((song.solo >> i) & 1u)
                m |= 1u << key_of_white(4u + i);
        }
        if (fill_now)
            m |= 1u << key_of_white(8);
        if (fill_arm || fill_bar_on)
            m |= 1u << key_of_white(9);
        if (play_led())
            m |= 1u << key_of_white(15);
        return m;
    case LY_ERASE:                                 /* the sounds the pattern holds (NOTES: dim, the hits lit) */
        return (lights_notes ? 0u : erase_lanes(t)) | fm1_in.notes | (lights_notes ? keys_sounding(t) : 0u);
    default:                                       /* playing, ARP roll, SAVE song, every page and the menu */
        break;
    }
    if (kb_grid) {                                 /* the DRUMS grid page: the sound's steps of this page */
        uint32_t len = trk_len(TDRUM), b0 = drum_cursor / 16u * 16u;
        for (i = 0; i < 16u; i++) {
            uint32_t idx = b0 + i, on;
            if (idx >= len)
                continue;
            on = dstep_has(&TDRUM->dstep[idx], drum_lane);
            if (song.playing && idx == TDRUM->seq_idx)
                on = !on || blink;
            if (on)
                m |= 1u << key_of_white(i);
        }
        return m | fm1_in.notes;
    }
    m = fm1_in.notes;
    if (is_drum(t) || lights_notes)                /* the drum track: each hit lights its key; NOTES: the synths too */
        m |= keys_sounding(t);
    return m;
}

/* menu NOTES, on the layers whose keys are tiles (FX effects, SEQ steps, GLO mute / solo): what
 * sounds glows dimly under the tiles, which keep their full light. SEL (the scale) and EDIT on the
 * drum track (the sounds of the pattern): those glow, and what sounds is lit (keys_lit) */
UI_SMALL static uint32_t keys_notes_dim(void)
{
    if (!lights_notes)
        return 0u;
    switch (ui.layer) {
    case LY_FX:
    case LY_STEP:
    case LY_MIX:
        return keys_sounding(TSEL);
    case LY_SCALE:
        return scale_keys(0);
    case LY_ERASE:
        return erase_lanes(TSEL);
    default:
        return 0u;
    }
}

/* the keys' landmarks, dim: the first key of each row of the 4 x 4 grid on screen (white keys
 * 1, 5, 9, 13) while a layer is held, and on the drum track (its 16 sounds, 4 x 4 as on KIT) */
UI_SMALL static uint32_t keys_guide(void)
{
    if (ui.layer == LY_PLAY && !is_drum(TSEL))
        return 0u;
    return 1u << key_of_white(0) | 1u << key_of_white(4) | 1u << key_of_white(8) | 1u << key_of_white(12);
}

/* the keys the backlight lights (menu KEYS): the Cs, every white key, or every key; bit k = key k (0 = F3) */
UI_SMALL static uint32_t lights_keys_mask(void)
{
    uint32_t k, m = 0;
    if (!lights_lvl || !lights_keys)
        return 0u;
    for (k = 0; k < 27u; k++) {
        uint32_t pc = (53u + k) % 12u;             /* key 0 = F3 (53) */
        if (lights_keys == KEYS_ALL || (lights_keys == KEYS_C ? pc == 0u : ((0xAB5u >> pc) & 1u) != 0u))   /* 0xAB5: C D E F G A B */
            m |= 1u << k;
    }
    return m;
}

UI_SMALL static void ui_leds(void)
{
    uint8_t nl[FM1_NCOL] = {0}, dl[FM1_NCOL] = {0}, bl[FM1_NCOL] = {0};
    uint32_t k, c, keys, guide, back;
    static uint8_t ready;
    if (!ready) {
        led_pos_init();
        ready = 1;
    }
    led_put(nl, panel.btn[ui.layer != LY_PLAY ? LAYER_BTN[ui.layer] : cur_btn()],
            ly_lock == LY_PLAY || ((fm1_ms / 300u) & 1u) != 0u || (fm1_in.buttons & ly_bit[ly_lock % LY_COUNT]) != 0u);   /* locked: blinks */
    led_put(nl, panel.btn[B_PLAY], play_led() || (song.playing && !song.rec && ft_on) ||
                                      (ci_on && ci_u % BEAT_U < BEAT_U / 4u));   /* (the count-in's beats) */
    led_put(nl, panel.btn[B_REC], song.rec != 0u || ft_on || (rec_wait && ((fm1_ms / 125u) & 1u)) ||
                                     (ui.hold_kind == 1u && ((fm1_ms / 60u) & 1u)));   /* blinks: armed; fast: clearing */
    if (is_drum(TSEL)) {                           /* the drum track: OCT- / OCT+ lit while ghost / hard */
        led_put(nl, panel.btn[B_OCTDN], (fm1_in.buttons & dyn_bit[0]) != 0u);
        led_put(nl, panel.btn[B_OCTUP], (fm1_in.buttons & dyn_bit[1]) != 0u);
    } else {
        led_put(nl, panel.btn[B_OCTDN], song.octave < 0);
        led_put(nl, panel.btn[B_OCTUP], song.octave > 0);
    }
    keys = keys_lit();
    guide = (keys_guide() | keys_notes_dim()) & ~keys;
    back = lights_keys_mask() & ~keys & ~guide;
    for (k = 0; k < 27u; k++) {
        led_put(nl, 14u + k, (int)((keys >> k) & 1u));
        led_put(dl, 14u + k, (int)((guide >> k) & 1u));
        led_put(bl, 14u + k, (int)((back >> k) & 1u));
    }
    if (lights_lvl)                                /* menu LIGHTS: every button glows, the lit ones stay full */
        for (k = 0; k < NB; k++)
            led_put(bl, panel.btn[k], 1);
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = nl[c];
        fm1_led_dim[c] = dl[c];
        fm1_led_bg[c] = (uint8_t)(bl[c] & ~nl[c]);
    }
    fm1_led_bg_ns = LIGHTS_NS[lights_lvl % LIGHTS_N];
}

/* ---------------------------------------------------------- input --- */
/* Knob acceleration (0.7, SLOOP 2.7's, after JIANT FM by juanjiant-bit, github.com/juanjiant-bit/JIANT, GPL-3.0): a slow
 * turn is fine tuning, one value a detent; the faster the turn the wider the sweep, progressively: the speed is the time
 * per detent, ACC_RATE / ms -> 40 ms x2, 27 ms x3, 20 ms x4, 10 ms x8, 5 ms x16, capped by the range (x2 up to 32
 * values, x4 up to 64, x8 up to 100, x16 above). Only the longer of this read's and the previous read's time counts,
 * and only while the turn goes on in one direction (both under ACC_GAP ms): a slow turn, the first two detents of a
 * turn, a single quick detent and a reversal are one value a detent, so turning back to correct never jumps. Ranges of
 * 16 values or fewer, and lists of names (callers pass 0), never accelerate. MENU > KNOB ACCEL OFF: one value a detent
 * everywhere. (Before 0.7: x3 or x6 at once past 60 ms a detent.) ui.enc_t[role]: bits 0..23 the ms of its last read,
 * bit 24 its direction (+1), 25..31 its ms per detent (127 slow) */
#define ACC_GAP 60u
#define ACC_RATE 80u
UI_SMALL static int32_t accel(uint32_t role, int32_t s, int32_t range)
{
    uint32_t now = fm1_ms & 0xFFFFFFu, st = ui.enc_t[role], up = s > 0, pi = st >> 25, a, i, m = 1;
    if (knob_accel_off || range <= 16 || !s)
        return s;
    a = (uint32_t)(s < 0 ? -s : s);
    i = ((now - st) & 0xFFFFFFu) / a;                   /* ms per detent of this read */
    if (!st || ((st >> 24) & 1u) != up || i >= ACC_GAP)
        i = 127u;                                       /* a new turn, or reversed */
    else if (pi < ACC_GAP) {
        m = ACC_RATE / (i > pi ? i : pi ? pi : 1u);
        a = range > 100 ? 16u : range > 64 ? 8u : range > 32 ? 4u : 2u;   /* (the cap) */
        m = m < 1u ? 1u : m > a ? a : m;
    }
    ui.enc_t[role] = now | up << 24 | (i ? i : 1u) << 25;
    return s * (int32_t)m;
}

/* TRACKS page: KNOB 1 SWING (the groove of every track, MPC 50..75 %), 2 LEVEL (0 = mute; the drum
 * track: GLO > DRUMS LEVEL), 3 LEN of its pattern, 4 PAN. A track muted with MUTE (GLO + key, the
 * editor): the first turn of KNOB 2 unmutes it */
UI_SMALL static void tracks_edit(uint32_t slot, int32_t steps)
{
    track_t *t = TSEL;
    int16_t *vp;
    const param_desc_t *d;
    switch (slot) {
    case 0:
        vp = &song.g[G_SWING];
        d = &GP[G_SWING];
        break;
    case 1:
        if (t->p[P_MUTE]) {
            t->p[P_MUTE] = 0;
            return;
        }
        vp = is_drum(t) ? &song.g[G_DRLVL] : &t->p[P_LEVEL];
        d = is_drum(t) ? &GP[G_DRLVL] : &TP[P_LEVEL];
        break;
    case 2:
        vp = &t->p[P_SLEN];
        d = &TP[P_SLEN];
        break;
    default:
        vp = &t->p[P_PAN];
        d = &TP[P_PAN];
        break;
    }
    *vp = (int16_t)clamp(*vp + accel(EN_K1 + slot, steps, d->names ? 0 : d->max - d->min), d->min, d->max);
}

/* (1.0, after SLOOP 2.7.2) NOTES: one undo per note edited (its pitch, length, force: EDIT + OCT- brings them back) */
UI_SMALL static void note_mark(void)
{
    static uint32_t sess, at = 0xFFFFu;
    uint32_t here = ui.cursor | (uint32_t)song.sel << 8;
    if (here != at || !undo.valid || undo.undone || undo.sess != sess) {
        sess = (undo_sess += 4u) | 3u;
        at = here;
    }
    undo_mark(TSEL, sess);
}
UI_SMALL static int on_notes_page(void)
{
    return !ui.home && song.seq_mode && cur_page()->scope == SC_STEP && !is_drum(TSEL);
}
/* the note's sheet (OCT+ held): KNOB 1 the row, KNOB 2 its value (MOVE: a step a detent); OCT+ acts (SLIDE on / off,
 * COPY: just after it, then MOVE moves the copy; DELETE), OCT- closes */
UI_SMALL static void note_sheet_knob(uint32_t slot, int32_t steps)
{
    track_t *t = TSEL;
    step_t *st = &t->step[ui.cursor];
    uint32_t row = ui.sheet - 1u;
    if (slot == 0u) {
        ui.sheet = (uint8_t)(1 + clamp((int32_t)row + steps, 0, 5));
        return;
    }
    if (slot != 1u)
        return;
    note_mark();
    switch (row) {
    case 0:
        st->flags = (uint8_t)(steps > 0 ? st->flags | SF_SLIDE : st->flags & ~SF_SLIDE);
        break;
    case 1:
        t->micro[ui.cursor] = (int8_t)clamp(t->micro[ui.cursor] + steps, MICRO_MIN, MICRO_MAX);
        break;
    case 2:
        step_fill_set(t, ui.cursor, (uint32_t)clamp((int32_t)step_fill(t, ui.cursor) % 3 + (steps > 0 ? 1 : -1), 0, 2));
        break;
    case 3: {
        int32_t n = steps;
        while (n) {
            int32_t to = note_shift(t, ui.cursor, n, 0), k = ui.note_k;
            if (to < 0) {
                ui_message(step_on(st) ? "NO ROOM" : "NO NOTE");
                break;
            }
            cursor_set(to);
            ui.note_k = (uint8_t)k;
            st = &t->step[ui.cursor];
            n += n > 0 ? -1 : 1;
        }
        break;
    }
    default:
        break;
    }
    sync_reload = 1;
}
UI_SMALL static void note_sheet_do(void)                        /* OCT+ tapped in the sheet */
{
    track_t *t = TSEL;
    uint32_t row = ui.sheet - 1u;
    note_mark();
    if (row == 0u) {
        t->step[ui.cursor].flags ^= SF_SLIDE;
    } else if (row == 4u) {
        int32_t to = note_shift(t, ui.cursor, 1, 1), k = ui.note_k;
        if (to < 0) {
            ui_message(step_on(&t->step[ui.cursor]) ? "NO ROOM" : "NO NOTE");
            return;
        }
        cursor_set(to);
        ui.note_k = (uint8_t)k;
        ui.sheet = 4;                                     /* (MOVE: KNOB 2 moves the copy) */
        ui_message("COPIED");
    } else if (row == 5u) {
        note_del(t, ui.cursor, ui.note_k);
        cursor_fix();
        ui.sheet = 0;
        ui_message("NOTE DELETED");
    }
    sync_reload = 1;
}
/* the NOTES page's knobs (SEQ > NOTES): KNOB 1 NOTE, KNOB 2 PITCH, KNOB 3 LEN, KNOB 4 FORCE */
UI_SMALL static void step_edit(uint32_t slot, int32_t steps)
{
    track_t *t = TSEL;
    step_t *st = &t->step[ui.cursor];
    uint32_t k = ui.note_k;
    if (is_drum(t))
        return;
    if (ui.sheet) {
        note_sheet_knob(slot, steps);
        return;
    }
    switch (slot) {
    case 0:                                               /* NOTE: the notes, a chord's one by one, and the empty steps */
        for (; steps > 0; steps--)
            note_walk(1);
        for (; steps < 0; steps++)
            note_walk(-1);
        break;
    case 1:                                               /* PITCH: the note's; an empty step gets your last note */
        note_mark();
        if (!step_on(st)) {
            memset(st, 0, sizeof *st);
            st->note[0] = last_note;
            st->n = 1;
            st->time = ST_NOTE;
            ui.note_k = 0;
            break;
        }
        st->note[k] = (uint8_t)clamp(st->note[k] + steps, 1, 127);
        st->flags &= (uint8_t)~SF_CONT(k);               /* (0.7: a new pitch starts here, not held over from before) */
        last_note = st->note[k];
        break;
    case 2:                                               /* LEN: the note and the TIEs after it (note_set_len) */
        if (step_on(st)) {
            uint32_t was = note_span(t, ui.cursor), n;   /* (SLOOP 2.8.2: under later notes too, POLY) */
            note_mark();
            fm1_irq_off();
            n = note_span_set(t, ui.cursor, (uint32_t)clamp((int32_t)was + steps, 1, NSTEP));
            fm1_irq_on();
            if (steps > 0 && n == was)
                ui_message(ui.cursor + n >= (uint32_t)clamp(t->p[P_SLEN], 1, NSTEP) ? "END OF PATTERN"
                           : span_why == 2u ? "MONO: SET VOICE POLY" : span_why == 1u ? "STEP FULL: 4 NOTES" : "NOTE AHEAD");
        }
        break;
    default:                                              /* FORCE: GHOST SOFT NORM HARD, ACC (the step's accent) */
        if (step_on(st)) {
            note_mark();
            note_force_set(st, k, (uint32_t)clamp((int32_t)note_force(st, k) + (steps > 0 ? 1 : -1), 0, 4));
        }
        break;
    }
}

/* a new project: every track empty, the default sounds, 90 BPM (TOOLS > NEW) */
UI_SMALL static void project_new(void)
{
    uint32_t i;
    transport_req = 2;
    panic_req = (1u << NTRK) - 1u;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        undo_mark(t, (undo_sess += 4u) | 3u);
        fm1_irq_off();
        track_defaults(t);
        if (i < NPART) {
            set_engine_of(t, TRK_DEF[i][0]);
            apply_preset_to(t, TRK_DEF[i][1]);
        }
        fm1_irq_on();
    }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    for (i = 0; i < G_COUNT; i++)
        if (i != G_SLOT && (i != G_DRCH || CLIPPER_SYNTH) && i != G_SYNC && i != G_MIDI && i != G_ROUTE)
            song.g[i] = GP[i].def;
    song.solo = 0;
    song.octave = 0;
    part_ctl_reset();                                   /* (the wheels back to the middle, the pedal up) */
    sync_reload = 1;
    ui.force = 1;
}

UI_SMALL static void edit_param(uint32_t slot, int32_t steps)
{
    int16_t *vp;
    const page_t *pg = cur_page();
    const param_desc_t *d;
    uint32_t id = pg->id[slot];
    int32_t v;
    if (is_drum(TSEL) && !page_for_drum(pg))
        return;                                           /* "DRUM TRACK": nothing to edit here */
    if (pg->scope == SC_STEP) {
        step_edit(slot, steps);
        return;
    }
    if (pg->scope == SC_TRK) {
        tracks_edit(slot, steps);
        return;
    }
    if (pg->graph == GR_BROWSE) {                         /* KNOB 1: one preset, KNOB 2: the engine filter (1.0) */
        if (slot == 0u && !is_drum(TSEL)) {
            br_step(steps);
        } else if (slot == 1u && !is_drum(TSEL)) {
            br_filter_step(steps > 0 ? 1 : -1);
        } else if (slot == 3u && !is_drum(TSEL)) {          /* (0.7) KNOB 4: FAV, right adds the sound, left removes it */
            fav_toggle(steps > 0);
        }
        return;
    }
    if (pg->graph == GR_USER) {                           /* KNOB 1 slot; LOAD / ERASE / SAVE: GO buttons */
        static const char *const UP_GO[3] = {"LOAD", "ERASE", "SAVE"};
        if (slot == 0u) {
            ui.uslot = (uint8_t)clamp((int32_t)ui.uslot + steps, 0, UP_SLOTS - 1);
            ui.arm = 0;
            return;
        }
        if (steps <= 0)
            return;
        if (ui.arm != 0xE0u + slot) {                     /* one detent arms, a second one within ~1.5 s acts */
            ui.arm = (uint8_t)(0xE0u + slot);
            ui.arm_t = 90;
            ui_say("AGAIN: ", UP_GO[slot - 1u]);
            return;
        }
        ui.arm = 0;
        up_ui(slot - 1u, ui.uslot);
        return;
    }
    d = page_desc(pg, slot, &vp);
    if (!d || !vp || d->max == d->min)
        return;
    v = clamp(*vp + accel(EN_K1 + slot, steps, d->names ? 0 : d->max - d->min), d->min, d->max);
#if CLIPPER_SYNTH
    {
        int32_t old = *vp;
        *vp = (int16_t)v;
        cc_out_knob(vp, old);                             /* (1.0) GLO > MIDI 2 > SEND CC */
    }
#else
    *vp = (int16_t)v;
#endif
#if CLIPPER_SYNTH
    if (pg->scope == SC_SYN) {                            /* the layer, MORPH, RANDOM (ui_synth.c) */
        syn_changed(id, vp);
        return;
    }
#endif
    if (pg->scope != SC_GLOBAL && p_lockable(id))
        ui.lock_par = (uint8_t)id;                        /* the SEQ layer's lock parameter: the last one touched */
    if (!v)
        return;
    if (pg->scope == SC_GLOBAL && (id == G_LOAD || id == G_SAVE || id == G_CLRSEQ || id == G_INITSND || id == G_NEWPRJ) &&
        ui.arm != id) {                                   /* one detent arms, a second one within ~1.5 s acts */
        *vp = 0;
        ui.arm = (uint8_t)id;
        ui.arm_t = 90;
        ui_say("AGAIN: ", d->label);
        return;
    }
    ui.arm = 0;
    if (pg->scope != SC_GLOBAL)
        return;
    switch (id) {                                         /* GO buttons: act, then back to 0 */
    case G_LOAD:
        *vp = 0;
        project_load((uint32_t)song.g[G_SLOT] - 1u);
        break;
    case G_SAVE:
        *vp = 0;
        project_save((uint32_t)song.g[G_SLOT] - 1u);
        break;
    case G_CLRSEQ:
        *vp = 0;
        undo_mark(TSEL, (undo_sess += 4u) | 3u);
        fm1_irq_off();
        track_defaults_steps(TSEL);
        fm1_irq_on();
        ui_message("PATTERN CLEARED");
        break;
    case G_INITSND:
        *vp = 0;
        set_engine(TSEL->eng_req);                              /* engine defaults + its first preset */
        ui_message("SOUND INIT");
        ui.force = 1;
        break;
    case G_NEWPRJ:
        *vp = 0;
#if FELUCCA_ARRANGER
        if (song.playing && arrangement_enabled) {      /* (the song's stop would bring the old loop back) */
            ui_message("STOP THE SONG FIRST");
            break;
        }
#endif
        project_new();
        ui_message("NEW PROJECT");
        break;
    default:
        break;
    }
}

/* 1.0, after SLOOP 2.6: the NOTES page's entry from a MIDI keyboard, as from the keys (seq_entry): the notes played
 * together become the cursor step, as they sounded (no chord built: the keyboard plays the notes it sends); letting them
 * all go moves on (STEP ADVANCE). Called every frame (the queue is read even off the page) */
static uint32_t mi_held[4];
static uint8_t mi_ok;                                     /* the last frame reached the page's entry (seq_entry: no layer,
                                                         * hold or menu in the way) */
UI_SMALL static uint32_t midi_held(void) { return mi_held[0] | mi_held[1] | mi_held[2] | mi_held[3]; }
UI_SMALL static void midi_entry(void)                              /* (top of ui_input: every frame) */
{
    {   /* (SLOOP 2.8.2) recording on a MONO / LEGATO sound, a key over a held one: it replaces it. Said once a session */
        static uint8_t mono_said;
        if (mono_hint && !mono_said) {
            mono_said = 1;
            ui_message("MONO: SET VOICE POLY");
        }
        mono_hint = 0;
    }
    track_t *t = TSEL;
    int on_page = mi_ok && on_notes_page() && !song.rec && !rec_wait && !ft_on && !ui.sheet;
    mi_ok = 0;
    if (step_in_lost) {                                   /* (the queue overflowed: what is in it and what is held are
                                                         * unknown: all dropped) */
        step_in_lost = 0;
        step_in_r = step_in_w;
        mi_held[0] = mi_held[1] = mi_held[2] = mi_held[3] = 0;
    }
    while (step_in_r != step_in_w) {
        uint32_t ev = step_in_q[step_in_r % STEP_IN_Q], note = ev & 127u;
        step_t *st = &t->step[ui.cursor];
        step_in_r++;
        if (!(ev & 0x80u)) {
            mi_held[note >> 5] &= ~(1u << (note & 31u));
            continue;
        }
        if (!on_page)
            continue;
        mi_held[note >> 5] |= 1u << (note & 31u);
        if (!ui.entry_open) {
            ui.entry_open = 1;
            undo_mark(t, (undo_sess += 4u) | 3u);
            st->n = 0;
            st->lvl = st->rat = 0;
            st->flags &= (uint8_t)~(SF_CONT_ALL | SF_LEG);   /* (step entry: the step's own notes only) */
            st->time = ST_NOTE;
            ui.note_k = 0;
        }
        if (!voice_poly(t->p[P_VOICE])) {
            st->note[0] = (uint8_t)note;
            st->n = 1;
        } else if (st->n < 4u) {
            st->note[st->n++] = (uint8_t)note;
        }
        last_note = (uint8_t)note;
    }
    if (!on_page)
        mi_held[0] = mi_held[1] = mi_held[2] = mi_held[3] = 0;
}

/* SEQ step entry, acid style: the keys pressed together (POLY: up to 4, MONO:
 * the last one) become the cursor step; releasing all keys moves on (1.0: MENU > SEQ > STEP ADVANCE OFF: the cursor
 * stays). Not while recording or armed: the keys record live then (they would be written twice) */
UI_SMALL static void seq_entry(uint32_t pressed)
{
    track_t *t = TSEL;
    step_t *st = &t->step[ui.cursor];
    mi_ok = 1;                                            /* (next frame, midi_entry may write) */
    uint32_t k;
    if (is_drum(t) || song.rec || rec_wait || ft_on)
        return;
    for (k = 0; k < 27u; k++) {
        uint32_t note;
        if (!((pressed >> k) & 1u))
            continue;
        note = kb_map(t, k);
        if (note == KB_SILENT)
            continue;
        if (!ui.entry_open) {
            ui.entry_open = 1;
            undo_mark(t, (undo_sess += 4u) | 3u);
            st->n = 0;
            st->lvl = st->rat = 0;
            st->flags &= (uint8_t)~(SF_CONT_ALL | SF_LEG);   /* (0.7: step entry: the step's own notes only) */
            st->time = ST_NOTE;
            ui.note_k = 0;
        }
        if (t->p[P_CHORD] && voice_poly(t->p[P_VOICE])) {            /* chord mode (2.4.1): the chord as it sounded (CHORD+,
                                                            * VLEAD), not only its root; (not played yet: built here) */
            uint8_t c[4];
            uint32_t m, j;
            if (kb_kind[k] == KS_NOTE && kb_n[k] && kb_root[k] == note) {
                m = kb_n[k] > 4u ? 4u : kb_n[k];
                memcpy(c, kb_nt[k], m);
            } else {
                m = chord_notes(t, note, c);
            }
            for (j = 0; j < m && st->n < 4u; j++)
                st->note[st->n++] = c[j];
        } else if (!voice_poly(t->p[P_VOICE])) {
            st->note[0] = (uint8_t)note;
            st->n = 1;
        } else if (st->n < 4u) {
            st->note[st->n++] = (uint8_t)note;
        }
        last_note = (uint8_t)note;
    }
    if (ui.entry_open && !fm1_in.notes && !midi_held()) {
        if (step_adv_off)
            ui.entry_open = 0;                            /* (1.0, SLOOP 2.7.1) STEP ADVANCE OFF: the cursor stays, the
                                                           * next keys write the step again (a chord tried again) */
        else
            cursor_set(ui.cursor + 1);
    }
}

/* HOME: tap on release, hold 0.7 s fires once. t0 = press time | 1,
 * bit 1 = fired (or swallowed: then the release is no tap either) */
enum { BT_NONE, BT_TAP, BT_HOLD };
UI_SMALL static uint32_t btn_hold(uint32_t *t0, uint32_t label, uint32_t now, int hold_ok)
{
    uint32_t tap;
    if ((fm1_in.buttons >> panel.btn[label]) & 1u) {
        if (!*t0)
            *t0 = (now | 1u) & ~2u;
        else if (hold_ok && !(*t0 & 2u) && now - (*t0 & ~3u) > 700u * 1000u * FM1_TICKS_PER_US) {
            *t0 |= 2u;
            return BT_HOLD;
        }
        return BT_NONE;
    }
    tap = *t0 && !(*t0 & 2u);
    *t0 = 0;
    return tap ? BT_TAP : BT_NONE;
}

/* a layer button tapped (pressed and let go, nothing touched): its pages, as before the layers */
UI_SMALL static void layer_tap(uint32_t layer)
{
    switch (layer) {
    case LY_FX:
        open_family(FAM_FX);
        break;
    case LY_ERASE:
    case LY_STEP:
        if (on_drum_page()) {                             /* DRUMS: GRID <-> KIT */
            drum_page = (uint8_t)((drum_page + 1u) % 2u);
            ui.force = 1;
            break;
        }
        if (!ui.home && cur_page()->scope == SC_TRK && is_drum(TSEL)) {
            studio_open(SC_DRUM);
            break;
        }
        if (layer == LY_ERASE && on_notes_page()) {      /* NOTES (1.0, SLOOP 2.7.2): EDIT deletes the note (a chord's
                                                           * others stay) */
            undo_mark(TSEL, (undo_sess += 4u) | 3u);
            if (!step_on(&TSEL->step[ui.cursor]))
                ui_message("NO NOTE");
            else {
                note_del(TSEL, ui.cursor, ui.note_k);
                cursor_fix();
                ui_message("NOTE DELETED");
            }
            break;
        }
        open_family(layer == LY_STEP ? FAM_SEQ : FAM_EDIT);
        break;
    case LY_ROLL:
        open_family(FAM_ARP);
        break;
    case LY_SCALE:
        open_family(FAM_SCL);
        break;
    case LY_MIX:
        open_family(FAM_GLO);
        break;
    case LY_SONG:                                         /* SAVE tapped: TRACKS -> the song, else the SAVE pages */
        if (CLIPPER_SYNTH)
            open_family(FAM_SAVE);                        /* (Clipper: no song: the SAVE pages) */
        else if (on_song_page())
            arrangement_save();
        else if (!ui.home && cur_page()->scope == SC_TRK)
            studio_open(SC_SONG);
        else if (on_drum_page())
            studio_open(SC_SONG);
        else
            open_family(FAM_SAVE);
        break;
    default:
        break;
    }
}

/* a layer locked open: a layer button held + HOME tapped. The layer stays with the button let go (both
 * hands free for the keys and the knobs); any other button but PLAY, REC and OCT- / OCT+ lets it go
 * (and does only that: its press is eaten) */
static uint8_t home_eat;                                  /* HOME pressed to unlock: its tap is eaten */
UI_SMALL static void layer_unlock(void)
{
    if (ly_lock != LY_PLAY) {
        ly_lock = LY_PLAY;
        ui.force = 1;
    }
}

/* #39 of Felucca: a layer that showed lets go; KNOB 1..4 belong to no page for LY_QUIET_MS after (the knob still
 * turning as the button comes up is the layer's, not the page's under it). fm1_ms | 1, 0 = none */
#define LY_QUIET_MS 250u
static uint32_t ly_quiet_t;
UI_SMALL static int ly_quiet(void)
{
    if (ly_quiet_t && fm1_ms - (ly_quiet_t & ~1u) >= LY_QUIET_MS)
        ly_quiet_t = 0;
    return ly_quiet_t != 0;
}
/* the layers, once a frame: which one is held (or locked), the taps on release, its keys and knobs.
 * Returns 1 while one is held or locked (the page does not take the knobs then) */
UI_SMALL static int layers_input(uint32_t note_edges, uint32_t *pressed, uint32_t home)
{
    static uint8_t down[LY_COUNT], used[LY_COUNT];
    static uint32_t t0[LY_COUNT];
    uint32_t l, now = fm1_ms, held = LY_PLAY, eat = 0;
    if (ly_lock != LY_PLAY) {
        uint32_t keep = 1u << panel.btn[B_PLAY] | 1u << panel.btn[B_REC] | 1u << panel.btn[B_OCTDN] | 1u << panel.btn[B_OCTUP];
        eat = *pressed & ~keep;
        if (eat) {
            layer_unlock();
            if (eat & 1u << panel.btn[B_HOME])
                home_eat = 1;
            *pressed &= ~eat;
        }
    }
    for (l = LY_FX; l < LY_COUNT; l++) {
        uint32_t d = (fm1_in.buttons & ly_bit[l]) != 0u;
        if (d && !down[l]) {
            t0[l] = now;
            used[l] = (uint8_t)((eat & ly_bit[l]) != 0u);  /* (the press that unlocked: not a tap) */
        }
        if (d && note_edges)
            used[l] = 1;                                  /* a key while held: not a tap */
        if (!d && down[l] && !used[l] && (now - t0[l] < TAP_MS || !ly_on(l)) && !ui.menu && !ui.confirm)
            layer_tap(l);                                 /* (Clipper: a button with no layer opens its pages
                                                           * on release however long it was held) */
        if (!d && down[l] && l == LY_SONG)
            chain_release();                              /* SAVE let go: the section taps of the hold (ui_layers.c) */
        down[l] = (uint8_t)d;
        if (d && held == LY_PLAY && ly_on(l))
            held = l;
    }
    if (held != LY_PLAY && home == BT_TAP && !home_eat) {  /* held + HOME: locked open */
        ly_lock = (uint8_t)held;
        used[held] = 1;
        ui.layer = (uint8_t)held;
        ui.force = 1;
    }
    if (held == LY_PLAY && ly_lock != LY_PLAY) {
        held = ly_lock;
        used[held] = 1;
    }
    punch.hold = (uint8_t)(held == LY_FX);
    if (held != LY_PLAY && held != ui.layer && ui.layer != LY_PLAY)
        ui.layer = (uint8_t)held;                         /* (from one layer straight to another) */
    if (held == LY_PLAY) {
        while (lk_r != lk_w) {                            /* a key let go after its layer: its release only */
            uint32_t e = lk_q[lk_r % LKQ];
            lk_r++;
            if ((e >> 8) == KB_GRID) {                    /* the DRUMS grid page: a step key */
                if (((e >> 7) & 1u) && grid_keys_on())
                    grid_key(e & 31u);
            } else if (!((e >> 7) & 1u)) {
                layer_key(e >> 8, e & 31u, 0);
            }
        }
        if (ui.layer != LY_PLAY) {
            ly_quiet_t = fm1_ms | 1u;                     /* a layer shown lets go: KNOB 1..4 quiet a while (#39) */
            ui.layer = LY_PLAY;
            ui.step_held = 0;
            ui.force = 1;                                 /* the page comes back */
        }
        ui.step_sess = 0;
        return 0;
    }
    ui.layer_used = used[held];
    while (lk_r != lk_w) {                                /* the keys of SEQ, SEL, GLO (seq.c) */
        uint32_t e = lk_q[lk_r % LKQ];
        lk_r++;
        used[held] = 1;
        layer_key(e >> 8, e & 31u, (e >> 7) & 1u);
    }
    layer_knobs(held);
    if (ui.layer_used)
        used[held] = 1;
    if (ui.layer == LY_PLAY && (used[held] || now - t0[held] >= SHOW_MS)) {
        ui.layer = (uint8_t)held;                         /* show it (not for a quick tap) */
        if (held == LY_STEP && (uint32_t)ui.step_page * 16u >= trk_len(TSEL))
            ui.step_page = 0;
    }
    if (held == LY_ERASE) {                               /* EDIT + OCT- / OCT+: undo / redo */
        uint32_t ob = 1u << panel.btn[B_OCTDN], pb = 1u << panel.btn[B_OCTUP];
        static uint32_t prev;
        uint32_t b = fm1_in.buttons & (ob | pb), press = b & ~prev;
        prev = b;
        if (press) {
            used[held] = 1;
            if (press & ob)
                ui_message(undo_swap(0) ? "UNDO" : "NOTHING TO UNDO");
            else
                ui_message(undo_swap(1) ? "REDO" : "NOTHING TO REDO");
        }
    }
    if (held == LY_STEP) {                                /* SEQ + OCT- / OCT+: the page; a step held: OCT- its nudge, locks and
                                                           * condition go, OCT+ its fill condition (normal, fill only, no fill) */
        uint32_t ob = 1u << panel.btn[B_OCTDN], pb = 1u << panel.btn[B_OCTUP];
        static uint32_t prev;
        uint32_t b = fm1_in.buttons & (ob | pb), press = b & ~prev, pages = (trk_len(TSEL) + 15u) / 16u;
        prev = b;
        if (press) {
            used[held] = 1;
            if ((press & ob) && ui.step_held)
                steps_held_clear();
            else if ((press & pb) && ui.step_held)
                steps_held_fill();
            else
                ui.step_page = (uint8_t)((ui.step_page + ((press & pb) ? 1u : pages - 1u)) % pages);
        }
    }
    return 1;
}

/* the REC screen while armed (ui_studio.c rec_screen_draw): KNOB 1 MODE free / tempo (an empty
 * project), KNOB 2 the length of the selected track (1, 2 or 4 bars), KNOB 3 START note / count;
 * MODE and START are settings of the FM-1 (panel.c lights_word: saved once stopped) */
UI_SMALL static void rec_knobs(void)
{
    static const uint8_t LENS[3] = {16u, 32u, 64u};
    int32_t s;
    uint32_t empty = (uint32_t)project_empty(), tempo = !empty || rec_tempo;
    if ((s = panel_enc(EN_K1)) != 0 && empty) {
        rec_tempo = (uint8_t)(s > 0);
        settings_later = 1;
        ui.hot_col = 0, ui.hot_t = 40;
    }
    if ((s = panel_enc(EN_K2)) != 0 && tempo) {
        track_t *t = TSEL;
        int32_t len = t->p[P_SLEN], i, to = len;
        if (s > 0) {
            for (i = 0; i < 3; i++) if (LENS[i] > len) { to = LENS[i]; break; }
        } else {
            for (i = 2; i >= 0; i--) if (LENS[i] < len) { to = LENS[i]; break; }
        }
        t->p[P_SLEN] = (int16_t)to;
        ui.hot_col = 1, ui.hot_t = 40;
    }
    if ((s = panel_enc(EN_K3)) != 0 && tempo) {
        rec_count = (uint8_t)(s > 0);
        settings_later = 1;
        ui.hot_col = 2, ui.hot_t = 40;
    }
    if ((s = panel_enc(EN_K4)) != 0) {                 /* (2.7) QUANT: right ON, left OFF (the timing kept as nudges) */
        rec_quant_off = (uint8_t)(s < 0);
        settings_later = 1;
        ui.hot_col = 3, ui.hot_t = 40;
    }
}

/* REC: acts on the press (no lag). Held 0.7 s the press is undone and a ring fills: held on to the
 * end, the selected track is cleared (EDIT + OCT- brings it back); let go before, nothing happens.
 * (SAVE is the SONG layer: ui_layers.c; tapped, layer_tap) */
UI_SMALL static void holds_input(uint32_t pressed, uint32_t now_ms)
{
    static uint8_t prev_rec, prev_wait, eaten, rec_on, save_on;   /* *_on: pressed, *_t0 its time */
    static uint32_t rec_t0, save_t0;
    uint32_t rb = 1u << panel.btn[B_REC], sb = 1u << panel.btn[B_SAVE];
    uint32_t rec_down = (fm1_in.buttons & rb) != 0u, save_down = (fm1_in.buttons & sb) != 0u;
    if (on_song_page())
        rec_down = 0, pressed &= ~rb;                     /* (the song page: REC stores a section) */
    /* REC */
    if (pressed & rb) {
        eaten = (uint8_t)ft_owns_press();                 /* (the press closed a free take: seq.c) */
        prev_rec = song.rec;
        prev_wait = rec_wait;
        rec_t0 = now_ms;
        rec_on = 1;
        rec_toggle();
    }
    if (rec_down && rec_on && !eaten && !ui.hold_kind && now_ms - rec_t0 >= 700u) {
        song.rec = prev_rec;                              /* a hold: the press is undone */
        rec_wait = prev_wait;
        ui.msg_t = 0;
        ui.hold_kind = 1;
        ui.hold_t0 = now_ms;
        ui.hold_trk = song.sel;
        ui.force = 1;
    }
    if (ui.hold_kind == 1u && rec_down && now_ms - ui.hold_t0 >= HOLD_CLEAR_MS) {
        track_t *t = &trk[ui.hold_trk % NTRK];
        char b[12] = "1 CLEARED";
        undo_mark(t, (undo_sess += 4u) | 3u);
        if (CLIPPER_SYNTH)
            str_cpy(b, "CLEARED", sizeof b);
        fm1_irq_off();
        track_defaults_steps(t);
        t->nheld = 0;                                     /* and the latched arp chord */
        t->arp_phys = 0;
        fm1_irq_on();
        if (!CLIPPER_SYNTH)
            b[0] = (char)('1' + ui.hold_trk);
        ui_say(CLIPPER_SYNTH ? "PATTERN " : "TRACK ", b);
        ui.hold_kind = 0;
        rec_on = 0;                                       /* (until REC is up: nothing more) */
        ui.force = 1;
    }
    if (!rec_down) {
        if (ui.hold_kind == 1u) {
            ui.hold_kind = 0;                             /* let go before the end: nothing */
            ui.force = 1;
        }
        rec_on = 0;
    }
    (void)sb, (void)save_down, (void)save_on, (void)save_t0;   /* (SAVE is a layer now: ui_layers.c) */
}

UI_SMALL static void ui_input(void)
{
    uint32_t pressed = fm1_input_edges(0), notes = fm1_input_note_edges(), now = fm1_ticks(), id, b, k;
    uint32_t home = btn_hold(&ui.home_t0, B_HOME, now, 1);
    int32_t s;
    int layered;
    if (CLIPPER_SYNTH && song.sel)                      /* Clipper: one instrument (an editor, an old project) */
        song.sel = 0;
#if CLIPPER_SYNTH
    syn_tick();                                         /* MIDI CC 20: MORPH */
#endif
    enc_hold = 0;                                       /* (panel.c: every knob readable again this pass) */
    if (pressed || notes)
        ui_input_ms = fm1_ms;
    fm6_poll();                                         /* FM6: PTCH turned -> its patch */
    if (home == BT_HOLD) {                              /* HOME held: open the menu, or leave it */
        if (ui.menu) {
            menu_close();
        } else {
            layer_unlock();
            ui.menu = 1;
            ui.menu_sel = 0;
            ui.confirm = 0;
            ui.force = 1;
            song.seq_mode = 0;
        }
    }
    kb_grid = (uint8_t)grid_keys_on();                  /* (seq.c: the keys are the grid's steps) */
    midi_entry();                                       /* (1.0: the NOTES page's MIDI notes: read every frame) */
    if (ui.menu) {                                      /* HOME / REC taps do nothing here */
        punch.hold = 0;
        if (!ui.home_t0)
            menu_input(pressed);
        return;
    }
    if (!on_notes_page())
        ui.sheet = 0;                                   /* (1.0: the sheet is the NOTES page's) */
    layered = layers_input(notes, &pressed, home);
    if (home_eat && !((fm1_in.buttons >> panel.btn[B_HOME]) & 1u)) {   /* (the HOME that unlocked: let go) */
        if (home == BT_TAP)
            home = BT_NONE;
        home_eat = 0;
    }
    pressed &= ~(ly_bit[LY_FX] | ly_bit[LY_ERASE] | ly_bit[LY_ROLL] | ly_bit[LY_STEP] | ly_bit[LY_SCALE] | ly_bit[LY_MIX] |
                 ly_bit[LY_SONG]);
    holds_input(pressed, fm1_ms);
    pressed &= ~((on_song_page() ? 0u : 1u << panel.btn[B_REC]) | (1u << panel.btn[B_SAVE]));
    if (layered || ui.hold_kind) {                      /* a layer / a hold: the rest waits */
        if ((s = panel_enc(EN_SELECT)) != 0) {           /* (the tempo always) */
            song.g[G_BPM] = (int16_t)clamp(song.g[G_BPM] + accel(EN_SELECT, s, 200), GP[G_BPM].min, GP[G_BPM].max);
            ui.bpm_t = 40;
        }
        panel_enc(EN_ALGO);                             /* (track and sound wait: no jump afterwards) */
        panel_enc(EN_PRESET);
        if (ui.hold_kind)
            for (k = 0; k < 4u; k++)
                panel_enc(EN_K1 + k);
        pressed &= ~((1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]));   /* (undo, pages, ghost / hard) */
        if (!(pressed & (1u << panel.btn[B_PLAY])))
            return;
        pressed &= 1u << panel.btn[B_PLAY];             /* PLAY still plays */
        enc_hold = (1u << NE) - 1u;                     /* #102: the knobs the layer took are not read again this
                                                         * pass (a detent counted since went to the page too) */
    } else if (ly_quiet()) {                            /* #39: the turns as a layer lets go are dropped */
        for (k = 0; k < 4u; k++)
            panel_enc(EN_K1 + k);
        enc_hold |= 15u << EN_K1;
    }
    if (rec_wait && !ft_on && !ci_on) {                 /* the REC screen, armed: how it records */
        rec_knobs();                                    /* (tempo and sound still work; the track too: */
    } else if (rec_wait || ft_on) {                     /* the arm follows) */
        for (k = 0; k < 4u; k++)                        /* a take / the count-in: KNOB 1..4 edit nothing */
            panel_enc(EN_K1 + k);
    }
#if FELUCCA_ARRANGER
    if (on_song_page()) {
        song_screen_input(pressed, home);
        return;
    }
    if (on_drum_page()) {
        drum_screen_input(pressed, home);
        return;
    }
#endif
    if (home == BT_TAP) {                               /* HOME acts on release: a hold opens the menu */
        if ((CLIPPER_SYNTH ? ui.home : !ui.home && cur_page()->scope == SC_TRK) && !vis_on)
            vis_open();                                 /* HOME on HOME: the visualiser (ui_vis.c) */
        else {
            vis_on = 0;                                 /* (HOME again: back to the TRACKS screen) */
            go_home();
        }
    }
    cursor_fix();                                       /* LEN may have changed (knob, editor, load) */
    if (on_notes_page()) {                              /* (1.0, SLOOP 2.7.2) OCT+ held: the note's sheet; tapped: as
                                                         * before */
        uint32_t ev, ob = 1u << panel.btn[B_OCTDN];
        pressed = octp_filter(pressed, &ev);
        if (ev == 2u && !ui.sheet)
            ui.sheet = 1;
        else if (ev == 1u && ui.sheet)
            note_sheet_do();
        else if (ev == 1u)
            pressed |= 1u << panel.btn[B_OCTUP];
        if (ui.sheet && (pressed & ob)) {
            ui.sheet = 0;
            pressed &= ~ob;
        }
        if (ev)
            ui.force = 1;
    }
    for (id = 0; id < 14u; id++) {
        if (!((pressed >> id) & 1u))
            continue;
        b = panel_btn_of(id);
        switch (b) {
        case B_PLAY:
#if FELUCCA_ARRANGER
            if (!song.playing && arrangement_enabled && !arr_valid(&arrangement, arrangement_ready())) {
                ui_message("EMPTY SECTION: REC");
                break;
            }
#endif
            if (!ft_owns_press())                       /* (it closed or dropped a free take: seq.c) */
                transport_req = song.playing ? 2 : 1;
            break;
        case B_OCTDN:
        case B_OCTUP: {
            uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
            if (is_drum(TSEL))
                break;                                  /* the drum track: ghost / hard while held (seq.c) */
            if ((fm1_in.buttons & both) == both)
                song.octave = 0;
            else
                song.octave += b == B_OCTDN ? (song.octave > -3 ? -1 : 0) : (song.octave < 3 ? 1 : 0);
            break;
        }
        case B_ENV:
            open_family(FAM_ENV);
            break;
        case B_LFO:
            open_family(FAM_LFO);
            break;
        default:
            break;
        }
    }
    if (song.seq_mode && cur_page()->scope == SC_STEP && !ui.sheet)
        seq_entry(notes);                                 /* (and the MIDI keyboard's: midi_entry) */
    if (on_notes_page() && (s = panel_enc(EN_PRESET)) != 0) {   /* (1.0) NOTES: PRESETS zooms, 8 .. 64 steps */
        ui.zoom = (uint8_t)(clamp((int32_t)(int8_t)ui.zoom + (s > 0 ? 1 : -1), -1, 2));
        ui.force = 1;
    }

    if ((s = panel_enc(EN_PRESET)) != 0 && (ui.home || cur_page()->graph == GR_BROWSE || cur_fam() == FAM_TRK)) {
        /* PRESETS browses the selected part's presets (all engines, then user presets) on HOME, the PRESETS
         * page and TRACKS only (the drum track: its kits); elsewhere a stray turn would throw away the sound
         * being edited. Every detent counts */
        uint32_t total, cur = preset_pos(&total);
        if (is_drum(TSEL))
            TDRUM->p[P_E0] = (int16_t)clamp(TDRUM->p[P_E0] + s, 0, DRUM_KITS - 1);
        else if (!ui.home && cur_page()->graph == GR_BROWSE)
            br_step(s);                                 /* (1.0: the PRESETS page: its engine filter) */
        else if (total)
            preset_go((uint32_t)(((int32_t)cur + s % (int32_t)total + (int32_t)total) % (int32_t)total));
    }
#if CLIPPER_SYNTH
    if ((s = panel_enc(EN_ALGO)) != 0 && (ui.home || cur_page()->graph == GR_BROWSE))
        preset_kind_jump(s);                            /* Clipper: ALGORITHM, the kind of sound (BASS, KEYS, PAD...) */
#else
    if ((s = panel_enc(EN_ALGO)) != 0 && !ft_on)     /* ALGORITHM: the selected track, on every page (not in a take) */
        track_select((uint32_t)clamp((int32_t)song.sel + (s > 0 ? 1 : -1), 0, NTRK - 1));
#endif
    if (vis_shown() && !rec_wait && !ft_on) {           /* the visualiser: SELECT its style; KNOB 1..4 (the TRACKS
                                                         * screen's, out of sight) do nothing */
        if ((s = panel_enc(EN_SELECT)) != 0)
            vis_select(s);
        for (k = 0; k < 4u; k++)
            panel_enc(EN_K1 + k);
    }
    if ((s = panel_enc(EN_SELECT)) != 0 && (rec_wait || ft_on || !page_walk(s))) {   /* SELECT: the pages of the
                                                     * family shown, else (and on the REC screen) the tempo */
        song.g[G_BPM] = (int16_t)clamp(song.g[G_BPM] + accel(EN_SELECT, s, 200), GP[G_BPM].min, GP[G_BPM].max);
        ui.bpm_t = 40;                              /* the header's BPM lights up; no message over the header */
    }
    for (k = 0; k < 4u; k++) {
        const page_t *pg = cur_page();
        int16_t *hv;
        if ((s = panel_enc(EN_K1 + k)) == 0)
            continue;
        if (ui.home || pg->scope == SC_STEP || pg->scope == SC_TRK || page_desc(pg, k, &hv) ||
            (pg->graph == GR_USER && k == 0u)) {     /* (not an empty column, nor "DRUM TRACK") */
            ui.hot_col = (uint8_t)k;
            ui.hot_t = 40;
        }
        if (ui.home) {
            int16_t *vp;
            const param_desc_t *d = home_param(k, &vp);
            int32_t old = *vp;
            *vp = (int16_t)clamp(*vp + accel(EN_K1 + k, s, d->names ? 0 : d->max - d->min), d->min, d->max);
#if CLIPPER_SYNTH
            cc_out_knob(vp, old);                     /* (1.0) CC OUT */
#endif
            (void)old;
            if (!is_drum(TSEL) && p_lockable(ENGINES[TSEL->eng_req % NENGINES]->macro[k & 3u]))
                ui.lock_par = ENGINES[TSEL->eng_req % NENGINES]->macro[k & 3u];
        } else {
            edit_param(k, s);
        }
    }
}

/* ---------------------------------------------------- panel setup --- */
/* 30 s without input: give up and keep the old table (a stuck key cannot hang the boot) */
#define SETUP_IDLE_MS 30000u
UI_SMALL static void panel_setup(void)
{
    uint32_t i, used = 0, t0 = fm1_ms;
    const panel_t old = panel;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 10, 240, &FONT_S, "HARDWARE CALIBRATION", C_WHITE, 1);
    draw_text_box(0, 30, 240, &FONT_S, "TEACH EACH BUTTON AND KNOB", C_GRAY, 1);
    while (fm1_in.buttons) {                             /* wait for OCT-/OCT+ release */
        fm1_wdt_feed();
        if (fm1_ms - t0 > SETUP_IDLE_MS)
            goto timeout;
    }
    fm1_input_edges(0);
    for (i = 0; i < NB; i++) {
        uint32_t p = 0, id;
        draw_text_box(0, 80, 240, &FONT_S, "PRESS", C_GRAY, 1);
        draw_text_box(0, 100, 240, &FONT_L, B_NAME[i], C_WHITE, 1);
        t0 = fm1_ms;
        while (!(p & ~used)) {
            fm1_wdt_feed();
            p |= fm1_input_edges(0);
            if (fm1_ms - t0 > SETUP_IDLE_MS)
                goto timeout;
        }
        for (id = 0; id < 14u; id++)
            if (((p & ~used) >> id) & 1u)
                break;
        panel.btn[i] = (uint8_t)id;
        used |= 1u << id;
    }
    used = 0;
    for (i = 0; i < NE; i++) {
        uint32_t e;
        int32_t st = 0;
        draw_text_box(0, 80, 240, &FONT_S, "TURN RIGHT", C_GRAY, 1);
        draw_text_box(0, 100, 240, &FONT_L, E_NAME[i], C_WHITE, 1);
        for (e = 0; e < 7u; e++)
            fm1_enc_take(e);
        t0 = fm1_ms;
        for (;;) {
            fm1_wdt_feed();
            if (fm1_ms - t0 > SETUP_IDLE_MS)
                goto timeout;
            for (e = 0; e < 7u; e++)
                if (!((used >> e) & 1u) && (st = fm1_enc_take(e)) != 0)
                    break;
            if (e < 7u)
                break;
        }
        panel.enc[i] = (uint8_t)e;
        panel.dir[i] = (int8_t)(st > 0 ? 1 : -1);
        used |= 1u << e;
        fm1_delay_ms(300);
        fm1_enc_take(e);
    }
    panel.magic = PANEL_MAGIC;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    layers_init();
    ui.force = 1;
    return;
timeout:
    panel = old;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.force = 1;
    ui_message("SETUP CANCELLED");
}
