# Clipper 1.3

**A free, open-source synthesizer firmware for the M-VAVE FM-1.** It grew out of [SLOOP](https://github.com/isod89/sloop-fm1) and, like SLOOP, it is based on [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita / Hügelton Instruments.

> **Make a backup with the editor before installing.** You can always go back to the official firmware (V16 or V15) from the installer page.

## What it is

- **23 engines, 407 sounds**: HYBRID (three oscillators, two filters, a 16-slot matrix), FMX (six operators after the opsix), FM6 (DX7, loads `.syx` banks), DIGITAL, WAVE (wavetables, and your own), WSEQ (wave sequencing), VECTOR, JUNO (with its chorus), ANALOG (eight filter models), ACID, STRINGS, MACRO, PHASE, VPS, TERRAIN, PHYS (physical models), TRIO, LOFI, WHEEL, VOICE, NOISE, GRAIN and SAMPLE (on Clipper's own synthesized sources).
- **On every engine**: a 12-slot modulation matrix, LFO 2 and 3, ENV 2 (one-shot or looping), the MOD SEQ, four macros (BRITE, TIMBR, MOTN, SPACE, also on CC 16-19), SPREAD (each voice its own place in the stereo field), VINTAGE, an insert effect.
- **Effects**: distortion, chorus, a tempo-synced delay (mono, ping-pong, tape), room, plate, hall and shimmer reverbs, a master EQ, compressor and limiter.
- **Playing**: 8 voices on both cores (12 / 16 as a build option), poly unison, layer / split / multi, a 15-mode arpeggiator with a Turing machine, a step sequencer, MPE, MIDI over USB and TRS (omni or one channel, program changes, expression, panic, the bend range, local control, CC out, a breath controller), microtuning with Scala scales; the FM-1 is a USB audio interface too.
- **A web editor** (Chrome or Edge, nothing to install) for every parameter, the HYBRID and FMX patches, DX7 banks, wavetables, the tuning, and full backups.

## New in 1.3: a bug-fix release

A bug-fix release. Thanks to everyone who reported issues on GitHub: every fix below has its own test, and the whole firmware was checked again (the editor's protocol, MIDI in and a damaged flash fuzzed, every engine, sound and key measured).

**Cleaner sound**
- No more distortion at the end of notes. The mix rounded every part down to 16 steps of the 16-bit output, so a fading note carried a buzz that followed it, on the 3.5 mm output and on USB. The mix now rounds properly: about 10 times less error, the quiet tails are clean.
- UNISON no longer stacks its voices in phase. Most engines started every unison voice at the same phase, up to +18 dB louder than POLY on some sounds, and FMX and MACRO peaked far over full scale. Each voice now starts apart, as a unison should, and the level matches POLY.
- PHYS strings are in tune. They played sharp, by up to a semitone on the low notes of HARPSICH (STR PLUCK, BANJO, PLUCK BASS, the guitars). Now within 2 cents; KOTO, SITAR and TANPURA bend with their curved bridge, as designed.
- The top of the keyboard follows the keys. NOISE's NZ ARCADE, SAMPLE and GRAIN played one pitch over the upper keys (SAMPLE CHOIR from A6, VOX CLOUD from G6); they now go on rising. A GRAIN note played while the one before is fading out is no longer almost silent.
- VECTOR: changing the FILTER MODEL while a note plays no longer gives a loud click.
- FMX: a voice taken back from another note now starts clean (before, it kept the old note's state: a DC offset and a changing timbre).

**Controls**
- A firm press on a layer button (GLO, EDIT, ARP, SEQ, SEL) opens its pages. Before, a press longer than 0.45 s opened nothing; now up to one second counts as a tap.
- On REC READY, a knob changes only its own value: KNOB 3 (START) no longer moves the loop LENGTH too. The same fix covers every page: a knob turned alone changes one value.

**Your data**
- Projects loaded from SLOOP keep Clipper's own values (SPREAD, LFO 3, ENV 2 LOOP, the macros, matrix slots 7 to 12, the delay MODE) across a power cycle.
- Editor backups now include your user wavetables (WAVE USR1..4), which a restore used to erase, and every value of your user presets.
- Safer loading: a user drum kit with an odd root note, or a project file with out-of-range velocities, plays normally.

**Install and go back**
- Return to the official firmware accepts M-VAVE's new V16 (and V15). M-VAVE now offers only V16, which the installer refused.

**Compatibility**
- Projects, presets, samples and settings are kept when you update, and the project format is unchanged.
- A few sounds change a little on purpose: the PHYS strings (now in tune), the unison sounds, NZ ARCADE.

The full list is in the [README](https://github.com/isod89/clipper-fm1/blob/main/README.md#new-in-13-a-bug-fix-release).

## Install

- **From the browser**: open the [installer page](https://isod89.github.io/clipper-fm1/webapp/installer/) (Chrome or Edge, the FM-1 on USB; the [editor](https://isod89.github.io/clipper-fm1/webapp/editor/) is next to it), make a backup with the editor first, then click Install.
- **From the source, on Windows**: see [QUICKSTART.md](https://github.com/isod89/clipper-fm1/blob/main/QUICKSTART.md) (French: [DEMARRAGE-RAPIDE-FR.md](https://github.com/isod89/clipper-fm1/blob/main/DEMARRAGE-RAPIDE-FR.md)) — `INSTALL-CLIPPER.bat` builds the firmware (WSL and the JieLi toolchain, as SLOOP) and opens the installer.

On the SMK-37 (Pro, Elite, MKE-P37, Starrykey 37 Play), the same synthesizer is [Clipper Pro](https://github.com/isod89/clipper-pro).

**Please report what you find** in the [Issues](https://github.com/isod89/clipper-fm1/issues): bugs, crashes (photograph the FELUCCA CRASH screen), stuck notes, crackles, CPU readings (GLO → SYSTEM).

Custom firmware with no warranty. Clipper is not affiliated with M-VAVE. GPL-3.0.
