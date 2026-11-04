## Clipper 1.3.1: FM-1 units with the newer flash chip

FM-1 units sold recently (mostly on AliExpress) carry a different flash chip, the UC25HQ80 (JEDEC ID 0xB36014), instead of the Puya P25Q80 (0x856014). It has the same size (1 MiB) and the same commands, but Clipper only knew the old one, so on these units:
- projects, presets and settings were not saved (the screen said "flash error": everything stayed in RAM until power-off);
- the update loader refused to install a new version.

Clipper 1.3.1 knows both chips: in the firmware, in the USB rescue and in the update loader. Nothing else changes.

If you have one of these units and already installed an older Clipper, its update loader does not know the chip and will refuse 1.3.1 too: go back to the official firmware first (the installer page's *Return to the official firmware*, or M-VAVE's M-UPGRADE with FM-1 V16), then install 1.3.1. From 1.3.1 on, updates install normally.

Thanks to Sven (The Firmware Hub) for the heads-up.

**Compatibility:** projects, presets, samples and settings are kept; the project format is unchanged.

Install from the browser: https://isod89.github.io/clipper-fm1/webapp/installer/ (Chrome or Edge, the FM-1 on USB).
