# Clipper — quick start (1.2)

**Clipper** is a **synthesizer** firmware for the M-VAVE FM-1: one instrument, like the original firmware, Felucca or Baudgirl's FM-1+VA. It is not a groovebox: no tracks, no drum machine, no song mode, no built-in samples. It keeps SLOOP's engines, effects, arpeggiator, step sequencer, USB audio and MIDI, and adds **HYBRID** (three oscillators, two filters, a 16-slot matrix) and many more engines. Pitch bend, mod wheel, aftertouch and the sustain pedal work on every engine. *(En français : [DEMARRAGE-RAPIDE-FR.md](DEMARRAGE-RAPIDE-FR.md).)*

> **Version 1.2.** Clipper runs on the FM-1; 1.2 brings SLOOP 2.8.3's fixes (see [New in 1.2](README.md#new-in-12-sloop-283s-fixes-a-steadier-second-core-the-ewi-as-one-voice)), and there is still a lot to do (see [What is left to do](README.md#what-is-left-to-do)). **Before installing, make a backup** with the editor (Clipper's or SLOOP's, whichever is installed): the Backup tab (Projects → Backup in SLOOP or an older version) → *Save a backup*.

On the FM-1, Clipper shows its boot screen (**1.2** under the logo), and HOME (held) → SYS → ABOUT reads **CLIPPER 1.2**.

## 1. Install

**Without building anything**: open the [installer page](https://isod89.github.io/clipper-fm1/webapp/installer/) in Chrome or Edge with the FM-1 on USB. Make a backup with the [online editor](https://isod89.github.io/clipper-fm1/webapp/editor/) first, then click **Install**. The firmware package is also attached to each [release](https://github.com/isod89/clipper-fm1/releases).

**From the source, on Windows**:

1. Close SLOOP's window if it is still running (not required, just clearer). Clipper lives in its own folder, `CLIPPER`, next to SLOOP's; nothing is shared between the two (Clipper's local pages are on port **8767**, SLOOP's on 8766).
2. Double-click **`INSTALL-CLIPPER.bat`**. It uses the same tools as SLOOP (WSL MinUI-Build, the JieLi toolchain). The first time it copies the three SDK files from `..\SLOOP\build\deps\ac79`; if it cannot find them, copy SLOOP's `build\deps\ac79` folder into `CLIPPER\build\deps\`.
3. The installer opens at http://localhost:8767/webapp/installer/ (Chrome or Edge, FM-1 on USB). Click **Install**.
4. The FM-1 restarts on the **clipper** screen. Your SLOOP user presets and settings are still there.

Elsewhere than Windows: see [BUILDING.md](BUILDING.md).

## 2. On the FM-1

| | |
| --- | --- |
| **HOME** | The synth screen: the sound's name, its family, its number in the list, the engine and a scope. The four knobs play the sound's main controls. |
| **PRESETS** | Every sound, one by one, factory and user presets. On the SAVE > PRESETS page, KNOB 2 filters the list by engine (ALL, an engine, USER). |
| **ALGORITHM** | Jumps to the next family (PAD, BASS, LEAD, PLCK, KEYS, STAB, FX…). |
| **FX, EDIT, ARP, GLO, SAVE** | A tap opens the pages; while FX or SAVE is held, the keys still play (EDIT, ARP and GLO held are sequencer layers, see below). FX pages: FX (sends), FILTER (and MODL, the filter model), INSERT, INSERT 2, DLY, DLY 2 (the delay's MODE), REVERB (TYPE: ROOM, PLATE, HALL, SHIMMER), CHORUS, EQ+COMP. EDIT: the engine's pages, MACROS, VOICE 1–3 (SPREAD on VOICE 3), the envelopes, LFOs and the 12-slot MATRIX. GLO: GLOBAL (tempo, swing, metronome, fine tune), MASTER, **TUNING** (the system, its root, and **A4** from 400 to 480 Hz: 432, 415, 442...), SYSTEM (MIDI out, sync, CPU / LATE / PEAK), **MIDI** (IN CH: OMNI or 1 to 16; OUT; BEND range; LOCAL), **MIDI 2** (PROG: program changes; SEND: the knobs' CCs; EWI: a breath controller). SAVE: PRESETS, USER (your presets), TOOLS. |
| **SEQ, SEL, EDIT, ARP, GLO** | Held, the sequencer's layers (as SLOOP 2.7.2): SEQ the 16 steps on the keys (a step held: KNOB 1 note, 2 velocity, 3 ratchet, 4 nudge, PRESETS a lock), SEL the key and the chords, EDIT erase (a key: that note out of the pattern; KNOBs SHIFT, LENGTH, TRANSPOSE; OCT- / OCT+ undo / redo), ARP note repeat, GLO fill (key 9), fill bar (key 10), tap tempo (key 16), level (KNOB 1). A tap opens their pages. |
| **SEQ tapped: NOTES** | The pattern as a piano roll. KNOB 1 walks the notes, KNOB 2 pitch, KNOB 3 length, KNOB 4 force (GHOST SOFT NORM HARD ACC), PRESETS zoom; the keys or a MIDI keyboard write the step; EDIT tapped deletes the note; **OCT+ held**: the note's sheet (SLIDE, NUDGE, FILL, MOVE, COPY, DELETE; KNOB 1 the row, KNOB 2 the value, OCT+ does it, OCT- closes). MENU > SEQ > STEP ADVANCE OFF: the cursor stays after a chord. |
| **REC** | Records what you play into the pattern. Hold REC to clear the pattern. |
| **MIDI** | GLO > **MIDI**: **IN CH** OMNI (every channel, the default) or one channel 1 to 16; **OUT**, the channel Clipper sends on; **BEND**, the pitch bend range (AUTO or 1 to 24 semitones); **LOCAL** OFF: the keys only go out (a DAW sending the track back). GLO > **MIDI 2**: **PROG** ON loads a sound on a program change; **SEND** CC: the knobs send their CCs to a DAW; **EWI** VOL: a breath controller (CC 2) plays the volume, V+AT the aftertouch too (set the MOD page's AT to CUT: the breath opens the filter; VOICE MONO or LEGATO). Everything Clipper receives and sends: [docs/midi.md](docs/midi.md). |

What you are playing is kept by the autosave across power cycles; save a sound you like as a **user preset** (SAVE → USER).

## 3. Playing HYBRID

- Turn **PRESETS** (or **ALGORITHM** to change family). HYBRID's sounds are spread over the families: **CLIPPER**, POLY PAD, STRINGS, VOX, GLASS PAD and DRONE (PAD); ACID, WOBBLE, REESE, FOLD BASS and 808 SUB (BASS); SYNC LEAD, HOOVER, CHIP LEAD and TRANCE (LEAD); PLUCK, SAW PLUCK, FM BELL, SPACE BEL and METAL (PLCK); FM KEYS (KEYS); BRASS (STAB); RISER and INIT (FX); and MOOG BASS, MOOG LEAD, LADDER PAD, CLIP WIDE, DARK PAD, ACID HARD, BRIGHT BEL and SOFT SYNC.
- On **HOME** the four knobs play **CUT**, **RES**, **M1** and **M2**.
- **EDIT**: CUT, RES, FENV (offsets on both filters), **PTCH** (the factory patch), and **M1 to M4**, four macros whose effect each patch decides. On CLIPPER, for example, M1 opens the filter and M2 widens the supersaws.
- From a MIDI keyboard: pitch bend, mod wheel (vibrato on several patches), aftertouch, sustain, and M1 to M4 on CC 16 to 19. On every other engine, CC 16 to 19 play the four MACROS (BRITE, TIMBR, MOTN, SPACE).
- From a controller or a DAW: **program changes** (and bank select, CC 0) pick the sounds in the list's order; **CC 11** is the expression (the volume); **CC 120 / 123** (panic) stop every note, **CC 121** resets the controllers; **RPN 0** sets the bend range; CC 7 level, 10 pan, 71..75 resonance and envelope, 74 cutoff, 76..78 the LFO, 91 / 93 / 94 the sends. The list: [docs/midi.md](docs/midi.md).

## 4. The web editor

Open the [online editor](https://isod89.github.io/clipper-fm1/webapp/editor/) (or double-click `OPEN-EDITOR.bat`: http://localhost:8767/webapp/editor/), click **Connect**, then pick a sound. In the **Sound** tab, the **HYBRID patch** panel shows everything: oscillators, filters and routing, ENV2 and ENV3, LFO2 and LFO3 (tempo-synced if you want), the 16-slot matrix and the bend. With **live** ticked, every change is heard at once. **Export patch** and **Import patch** keep your sounds in `.clipper.json` files.

The other panels of the **Sound** tab: **FMX patch** (the six operators, the routing grid, STORE into the bank, `.clipper-fmx.json` files), **FM6** (DX7 `.syx` banks), **MOD SEQ** (the four lanes to draw), **Tuning** (the system, the root, A4 in Hz, load a `.scl` scale), **Sound tools** (undo / redo, compare, A / B, randomize, what changed, the envelope drawn). The **Library**, **Wavetables** (your own tables for WAVE) and **Backup** tabs complete it.

The **Sequencer** tab is a full step sequencer: undo / redo, a range (shift-click on the grid) for transpose, shift, reverse, double, copy / paste and chords; generators (Euclidean rhythms, random melodies in the key, 22 ready-made basslines, arpeggios and chord patterns, a pattern text, humanize); lanes to draw velocity, nudge, ratchet and level; and a pattern library (a bank in the browser, `.clipper-pattern.json` files).

To try the editor without an FM-1, open `web/editor.html?mock=1` in Chrome.

## 5. The test that helps most: both cores and the CPU load

This decides how many voices Clipper can play. The FM-1 must be connected to the computer by USB, otherwise the CPU box shows the USB state instead.

1. After installing, hold HOME (the menu) → **SYS** → **ABOUT**: it should read **2 CORES**. If it reads **1 CORE**, please report it.
2. Pick the **CLIPPER** sound with PRESETS, go to **GLO → SYSTEM**: the fourth box cycles through **CPU**, **LATE** and **PEAK**.
3. Hold **8 notes** and note CPU and PEAK. Then MENU → SYS → **2ND CORE OFF** and measure again: the difference is what the second core brings. Set it back **ON**.
4. Do the same with **POLY PAD**, **TRANCE**, an **FMX** sound (FMX EP) and **ENSEMBLE** (STRINGS).
5. Then with 1.0's features: **SPREAD** at 127 (VOICE 3), the reverb on **SHIMMER** (REVERB → TYPE) and the delay on **TAPE** (DLY 2 → MODE), 8 notes. Turn **BRITE** and **SPACE** (EDIT → MACROS) while a chord is held, and change TYPE and MODE: no click should be heard.
6. Save a user preset with some 1.0 settings, switch the FM-1 off and on, and load it again: the settings should be there.
7. If PEAK stays under ~70 % and LATE does not climb, try a 12-voice build (**`INSTALL-CLIPPER.bat 12`**) with 12 notes, then 16.

Please report the numbers (with and without 2ND CORE) in the [Issues](https://github.com/isod89/clipper-fm1/issues), and anything odd: a crackling sound, a stuck note, a **FELUCCA CRASH** screen (photograph it: it stays 10 s), a strange screen.

## A noise on one side?

Some players hear a crackle or a noise on the left channel, with the reverb on and on some sounds, that the original firmware does not have. Found and fixed in 1.1: **PLATE, HALL and SHIMMER** (FX > REVERB > TYPE) read one of their left taps outside its delay line, a crackle on the left only; and with REVERB > DAMP turned down and a loud sound, ROOM overflowed into a crackle that went on after the notes. 1.1 also moves the reverb's memory. The sound Clipper computes is otherwise clean on both sides (every factory sound checked, with every effect at full). If you still hear it, these tell us where it comes from (a reply on GitHub or YouTube with the answers helps a lot):
1. FX > FX: REV at 0. Does the noise stop? FX > REVERB > TYPE ROOM, then PLATE: on both?
2. Leave HOME for a still page (GLO > TUNING): does the noise change?
3. GLO > SYSTEM, the last column (CPU, then LATE, then PEAK, in turn): does LATE go up while you hear it?
4. MENU > SYS > 2ND CORE OFF, power off and on, the same sound: still there? (Set it back ON after.)
5. Is it there with no note playing?

## Going back

- **To SLOOP**: run `INSTALL-SLOOP.bat` from SLOOP's folder. A sound set to HYBRID plays another engine there.
- **To the official firmware**: use the installer page ("Return to the official firmware (V15)"), after making a backup.
