# Credits and licences

ps2recomp-option-p (Option P) is licensed under the **GNU General Public License v3.0 or later** (`LICENSE`).
It contains or adapts code from the projects below. Their licence texts are kept in `LICENSES/`.

| Part | Origin | Version | Licence | Where |
|---|---|---|---|---|
| Toolchain and runtime backbone (ps2decomp, tools, runtime, host, gpu, signatures, templates, tests) | [danielgomesvieira2000/ps2-port-engine](https://github.com/danielgomesvieira2000/ps2-port-engine), Copyright (c) 2026 Daniel Gomes Vieira | v0.13.2 (01b9843) | MIT (`LICENSES/ps2-port-engine-MIT.txt`) | whole tree except `docs/option_p/` |
| The launcher's Windows 95 window (bevels, title bar, owner-drawn buttons, progress blocks) | NinjaGaiden2-Recompiled's `launcher/ng2_launcher.cpp` (the maintainer's own project) | 2026-11-20 (local copy) | GPL-3.0 (as this repository) | `launcher/game_setup_ui_win95.c` (adapted to C) |
| Later ps2-port-engine changes: the paced present's freeze fix, crash reports, the sound's lag cap, settings files with a byte order mark, PRs #3, #4, #5 and #7 | [danielgomesvieira2000/ps2-port-engine](https://github.com/danielgomesvieira2000/ps2-port-engine) | 28bd074, 65c46e9, 1c30d28, a6d1f8e, d5a17d8, 90281dd, PRs #3 #4 #5 #7 | MIT (`LICENSES/ps2-port-engine-MIT.txt`) | `gpu/src/vulkan_display.cpp`, `host/src/crash_report.cpp`, `host/src/audio.cpp`, `host/src/settings.cpp`, `data/`, `tools/lift.py` |
| CHD disc images | [rtissera/libchdr](https://github.com/rtissera/libchdr) with its bundled lzma, zstd and miniz | 47d794151b2b | BSD-3-Clause (`LICENSES/libchdr-BSD-3.txt`; the bundled libraries' licences in their folders) | `host/third_party/libchdr/` |

Every further part taken from another project gets a row here (origin, commit, licence, files) before it is committed.
Projects used as idea sources only (no code taken) are listed in `docs/option_p/01_survey.md`.
