# Clipper 1.2

**A free, open-source synthesizer firmware for the M-VAVE FM-1.** It grew out of [SLOOP](https://github.com/isod89/sloop-fm1) and, like SLOOP, it is based on [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita / Hügelton Instruments.

> **Make a backup with the editor before installing.** You can always go back to the official V15 firmware from the installer page.

## What it is

- **23 engines, 407 sounds**: HYBRID (three oscillators, two filters, a 16-slot matrix), FMX (six operators after the opsix), FM6 (DX7, loads `.syx` banks), DIGITAL, WAVE (wavetables, and your own), WSEQ (wave sequencing), VECTOR, JUNO (with its chorus), ANALOG (eight filter models), ACID, STRINGS, MACRO, PHASE, VPS, TERRAIN, PHYS (physical models), TRIO, LOFI, WHEEL, VOICE, NOISE, GRAIN and SAMPLE (on Clipper's own synthesized sources).
- **On every engine**: a 12-slot modulation matrix, LFO 2 and 3, ENV 2 (one-shot or looping), the MOD SEQ, four macros (BRITE, TIMBR, MOTN, SPACE, also on CC 16-19), SPREAD (each voice its own place in the stereo field), VINTAGE, an insert effect.
- **Effects**: distortion, chorus, a tempo-synced delay (mono, ping-pong, tape), room, plate, hall and shimmer reverbs, a master EQ, compressor and limiter.
- **Playing**: 8 voices on both cores (12 / 16 as a build option), poly unison, layer / split / multi, a 15-mode arpeggiator with a Turing machine, a step sequencer, MPE, MIDI over USB and TRS (omni or one channel, program changes, expression, panic, the bend range, local control, CC out, a breath controller), microtuning with Scala scales; the FM-1 is a USB audio interface too.
- **A web editor** (Chrome or Edge, nothing to install) for every parameter, the HYBRID and FMX patches, DX7 banks, wavetables, the tuning, and full backups.

## New in 1.2

- **A note held under others keeps sounding** (after SLOOP 2.8.2 and 2.8.3): a key held while you play or record over it is never restarted nor cut by the sequencer, live, in a free take or an overdub, on POLY and on UNI2 / UNI4; a hold overdubbed over a recorded melody goes on through its notes, and the notes the sustain pedal holds are left alone too. Recording a key over a held one on a MONO or LEGATO sound says **MONO: SET VOICE POLY**.
- **SEQ > NOTES: LEN under later notes**: a note's length can run on under the notes after it (a long bass under a melody, programmed).
- **No stuck notes when MIDI comes in too fast.**
- **SAMPLE and GRAIN: a sample played again starts from silence with its ATK**: no click on MONO, LEGATO or UNISON, or on the same note on POLY.
- **No crash on FMX when ALG is turned past 32** (FM KICK and the other FMX sounds).
- **The second core, hardened**: a backup over USB could crash an FM-1 running on two cores. Every job core 1 runs is now checked; a wrong one is refused and the FM-1 goes on with one core until it is switched off. If a unit still does it: HOME (held) → SYS → 2ND CORE OFF.
- **A quick tap no longer flashes a layer** (SEQ, EDIT, ARP, GLO).
- **The editor without internet**: download it from the installer page as one file (`clipper-editor-1.2.html`, also attached here), or install the site as an app.
- **The EWI plays one voice**: with GLO > MIDI 2 > EWI on, the instrument plays one note at a time, legato, and the breath alone shapes the level (a soft note no longer adds a second one at full level).

The full list is in the [README](https://github.com/isod89/clipper-fm1/blob/main/README.md#new-in-12-sloop-283s-fixes-a-steadier-second-core-the-ewi-as-one-voice).

## Install

- **From the browser**: open the [installer page](https://isod89.github.io/clipper-fm1/webapp/installer/) (Chrome or Edge, the FM-1 on USB; the [editor](https://isod89.github.io/clipper-fm1/webapp/editor/) is next to it), make a backup with the editor first, then click Install.
- **From the source, on Windows**: see [QUICKSTART.md](https://github.com/isod89/clipper-fm1/blob/main/QUICKSTART.md) (French: [DEMARRAGE-RAPIDE-FR.md](https://github.com/isod89/clipper-fm1/blob/main/DEMARRAGE-RAPIDE-FR.md)) — `INSTALL-CLIPPER.bat` builds the firmware (WSL and the JieLi toolchain, as SLOOP) and opens the installer.

On the SMK-37 (Pro, Elite, MKE-P37, Starrykey 37 Play), the same synthesizer is [Clipper Pro](https://github.com/isod89/clipper-pro).

**Please report what you find** in the [Issues](https://github.com/isod89/clipper-fm1/issues): bugs, crashes (photograph the FELUCCA CRASH screen), stuck notes, crackles, CPU readings (GLO → SYSTEM).

Custom firmware with no warranty. Clipper is not affiliated with M-VAVE. GPL-3.0.
