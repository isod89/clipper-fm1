# Building Clipper

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `felucca.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `felucca.fwsc` | the installable package (app + loader) |

## Windows (WSL)

`INSTALL-CLIPPER.bat` (`INSTALL-CLIPPER.bat 12` or `16` for more voices) runs `build-clipper.ps1`:
it builds in the WSL distribution `MinUI-Build` with the JieLi toolchain at
`/root/fm1-codex-deps/jieli/toolchain` (SLOOP's setup; edit the `build_windows.py` line of
`build-clipper.ps1` if yours differ), makes the site and opens the installer on
`http://localhost:8767/webapp/installer/` (SLOOP uses 8766). It needs Python 3 with Pillow on
Windows and the three SDK files (below) in `build/deps/ac79`: the first run copies them from a
SLOOP folder next to this one (`..\sloop-fm1`, `..\SLOOP`) or from `CLIPPER_SDK`.
`OPEN-EDITOR.bat` serves the editor of the last build on the same port.

## Prerequisites (macOS)

- Python 3 with Pillow: `pip3 install Pillow`
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- Node.js (optional, for the web tests).

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 0.9-beta` makes a release build: the package identity becomes
`FM-1_909` and the version string `0.9-BETA`; the package is `build/felucca-0.9-beta.fwsc`.

Build options (environment, `0` or `1`; defaults in `firmware/src/felucca.c`):

| Flag | Default | |
| --- | --- | --- |
| `FELUCCA_FLASH` | 1 | settings, presets and projects in flash |
| `FELUCCA_OTA` | 1 | update entry (needs `FELUCCA_FLASH`) |
| `FELUCCA_CDC` | 1 | USB serial console |
| `FELUCCA_UAC` | 1 | USB audio input: the master output, 44.1 kHz stereo (after Felucca 1.0) |
| `FELUCCA_UART` | 1 | TRS MIDI IN (the 3.5 mm jack) |

## Samples

The Clipper synth (`CLIPPER_SYNTH 1`, the default) has no built-in samples: SAMPLE and GRAIN
play the four user slots sent from the editor, and the flash goes to synthesis. The CC0
instrument and drum samples in `assets/samples-cc0/` (Versilian Studios VSCO-2 CE and VCSL,
Sonic Pi for SCRCH: see `ATTRIBUTION.txt` there) are only built into the groovebox
(`CLIPPER_SYNTH 0`, which the tests use).

## Tests

```
tests/run_tests.sh
```

Runs the host tests (flash storage, user presets, MIDI parser, update entry, update
loader, every synth engine and page, the modulation matrix, the MOD SEQ, the filter models,
VINTAGE, the tunings, the arpeggiator, both cores, a click test of the whole mix, project
formats, the groovebox's 4-track mix, the regression suite, the command-line installer)
and, with Node.js, the web page tests. `HOST_ONLY=1` skips what needs the JieLi toolchain. Run it after `./build.sh`
(it uses `build/` and needs `AC79_SDK` set as for the build).

The regression suite (`tests/regress.c`, built as the groovebox, `CLIPPER_SYNTH 0`) renders
each of its 13 engines and their presets and compares a hash of each render with
`tests/golden.txt` (the synth's own engines and presets have their own tests: `synth*_test`,
`hybrid_test`, `wave_test`, `clean_test`...); it also checks levels, voices and the CPU
cost (`tests/cpu_baseline.txt`, `tests/target_budget.txt`). The host CPU cost is relative
to the idle + drums mix, counted by the kernel on macOS and under callgrind on Linux when
valgrind is installed (exact, about 45 s more); without either it is timed, a rough check.
After an intended change of the sound, `GOLDEN_UPDATE=1 sh tests/run_tests.sh` rewrites
the hashes; `BUDGET_UPDATE=1` does the same for the cost files.

## Install

On Windows, `INSTALL-CLIPPER.bat` builds and opens the web installer (Chrome or Edge). The
`.fwsc` of each release is on the GitHub releases page.

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/felucca.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Or, to install your own build from the web installer, make a local copy of the site and open it from `localhost`
(Web MIDI needs a secure context):

```
python3 web/make_site.py build/felucca.fwsc dev /tmp/felucca-site
cd /tmp/felucca-site && python3 -m http.server 8000
# open http://localhost:8000/webapp/installer/
```

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).

## Publishing a release on GitHub

`PUBLISH-GITHUB.bat` (Windows, after `INSTALL-CLIPPER.bat` has built the firmware) publishes from the CLIPPER folder with
your own Git and GitHub CLI login: the source from `clipper-1.3.bundle` to `YOUR-NAME/clipper-fm1` (created if needed;
an existing repository with another history stops it, nothing is forced on `main`), the tag and the release
`v1.3` (notes: `docs/release-1.3.md`, the `.fwsc` package and the demo WAV attached), and the installer and
the editor (`build/clipper-site`) on GitHub Pages (branch `gh-pages`, replaced each time):
`https://YOUR-NAME.github.io/clipper-fm1/webapp/installer/`. `PUBLISH-GITHUB.bat -Repo another-name` for another name.
