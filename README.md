# <3 Dream - Advanced Granular Synthesizer for Daisy Patch

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Overview

**<3 Dream** is a sophisticated real-time granular synthesizer designed for the Electro-Smith Daisy Patch hardware platform. This advanced implementation offers professional-grade granular processing with extensive MIDI control, DSP effects, and CV integration.

- Eurorack Build
- Musician Loved

## Hardware Requirements

- **Platform**: Electro-Smith Daisy Patch
- **Processor**: ARM Cortex-M7 @ 480MHz
- **Memory**:
  - 128KB FLASH
  - 512KB SRAM
  - 64MB SDRAM (for large audio buffers)
- **Sample Rate**: 44.1kHz
- **Audio I/O**: Stereo in/out
- **CV I/O**: 2 CV inputs, 2 CV outputs
- **Gate I/O**: 2 gate inputs, 1 gate output
- **Controls**: 4 potentiometers, 1 encoder with push-button
- **Display**: 128x64 OLED display
# <3 Dream — Granular Synthesizer for Daisy Patch

Lightweight, focused granular synth firmware for the Electro‑Smith Daisy Patch.

This repository contains the optimized `Granular` firmware built for the Daisy Patch hardware. The current build is a trimmed, production-oriented firmware that preserves the granular engine, MIDI I/O, CV outputs, and the OLED UI while removing legacy or optional subsystems (SD card I/O, DSP effects, scale quantization, unused QSPI boot logic).

Status: firmware fits in device flash (binary in `build/Granular.bin`).

## Firmware Versions — Previous vs Clean

Below is a side-by-side comparison showing the prior (legacy) firmware and the current, cleaned/optimized firmware. The right-hand column is the updated firmware that remains in this repository.

| Previous (Legacy Firmware) | Current (Clean, Optimized) |
|---|---|
| Flash usage: ~131,152 bytes (overflowed device limit in earlier builds) | Flash usage: ~122,280 bytes (fits in 129KB device flash; ~92.5% used) |
| Included multiple DSP effects (filters, chorus, decimator, wavefolder) and DSP routing | DSP effects removed to save flash; granular engine retained |
| Scale quantization and CV scale logic present | Scale quantization removed; CV outputs map directly to V/Oct |
| SD card file I/O and menu for loading/saving WAV files | SD features removed and replaced by `Ctrl Options` placeholders |
| Large legacy `prebuf` circular buffer written but never read (wasted SDRAM) | `prebuf` removed, SDRAM reclaimed for useful buffers |
| Experimental QSPI/boot logic present in some branches | QSPI boot/experimental logic removed per project decision |
| More complex menu system with DSP/CV options | Simplified menu; core granular/audio/MIDI controls remain |
| Larger binary and higher risk of runtime issues due to unused code paths | Smaller, auditable codebase focused on stability and core features |

If you want the legacy firmware preserved as a separate branch or tag instead of only listing it here, I can create a `legacy` branch that contains the prior state and keep `main` as the cleaned version.

## What this build includes

- Real-time granular engine (record/playback, freeze, windowed playback)
- USB + hardware MIDI (note/CC/pitch-bend handling)
- CV outputs (direct MIDI→V/Oct mapping)
- Large SDRAM buffer(s) for live recording and playback
- OLED display menu/UI with `Ctrl Options` placeholder menu
- Optimized for flash size (non-essential subsystems removed)

## What was intentionally removed

- All DSP effects (filters, chorus, decimator, wavefolder)
- Scale quantization and related CV scale logic
- SD card file I/O and associated menus
- Unused prebuf circular buffer (freed SDRAM)
- QSPI boot/experimental logic

## Quick Build & Flash

Prerequisites:

- ARM GCC toolchain
- `make` (GNU make)

Build:

```bash
cd /home/eye/Desktop/workingSPACE
make clean
make -j4
```

Flash (DFU):

```bash
dfu-util -d 0483:df11 -a 0 -D build/Granular.bin --dfuse-address 0x08000000
```

Notes:

- If you use a different DFU tool or connection method, adapt the flash command accordingly.
- The produced binary size for the current commit is shown in `make` output (FLASH: X bytes).

## Repo layout

- `Granular.cpp` — main application source
- `Makefile` — build configuration
- `build/` — build outputs (binary, object files)
- `libDaisy/`, `DaisySP/` — vendor libraries (included as subtrees/submodules)

## Contributing / Pushing to your Git

If you want to push this repo to your remote `dream-granular` repository, add the remote and push. Example commands:

HTTPS (will prompt for credentials / use token):

```bash
git remote add origin https://github.com/YOUR_USERNAME/dream-granular.git
git branch -M main
git push -u origin main
```

SSH (recommended if you have SSH keys set up):

```bash
git remote add origin git@github.com:YOUR_USERNAME/dream-granular.git
git branch -M main
git push -u origin main
```

If you want, paste your remote repository URL here and I can run these commands for you (I won't handle credentials; you'll authenticate locally when required).

## Contact

To message **jstefani**, open a [GitHub Issue](https://github.com/eyebrain/Dream/issues) in this repository or visit their GitHub profile at [github.com/jstefani](https://github.com/jstefani).

## License

This project is MIT-licensed. See the `LICENSE` file for terms.

---

If you want more detail (usage examples, menu screenshots, contribution guide, or developer notes), tell me which sections to expand and I'll add them.
