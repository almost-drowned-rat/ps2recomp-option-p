# ps2recomp-option-p

A toolchain and runtime for turning PlayStation 2 games into **native PC
ports**: the game's program translated into C++ whose behaviour is checked
against the original machine code, then compiled and run on a host that
provides what the PS2 did - the EE kernel, the IOP, sound, the GS drawn with
Vulkan, the VUs, pads, memory cards, the disc - plus what a PC port wants:
higher resolutions, widescreen, frame pacing, settings, rumble.

## How it came about

ps2recomp-option-p is my fork of [ps2-port-engine](https://github.com/danielgomesvieira2000/ps2-port-engine)
(built on *Burnout 3: Takedown*). It began as "option P": before writing any
code I read through the PS2 recompilation, decompilation and port projects
I could find (`docs/option_p/01_survey.md`) to see where they spend their
time. The finding was the same everywhere - the PS2's vector units, its GS
and the parts still interpreted at run time cost the most. Option P is the
answer I chose: build on an engine that already checks its translation
against the original code, and take the remaining interpreted and emulated
parts out of the hot path one by one. The name follows my Xbox toolkit's
[option-c](https://github.com/almost-drowned-rat/xboxrecomp-option-c).

## What it is for

- **Light native ports** - a game should run with less CPU and GPU than
  emulating it, so that it also fits small machines such as handhelds.
- **A checked translation** - every translated function is run side by side
  with an interpreter of the PS2's processor on the game's own data (or proven
  equal to a form that was) before it is used.
- **Nothing interpreted while playing** - vector unit programs and the
  sound processor's modules are translated ahead of time as well.
- **No game code shipped** - a release carries the translator and a compiler;
  the player's own disc is translated and compiled on the player's PC.
- **The same PC features in every port** - a settings menu in the game's own
  menus (render resolution, widescreen, antialiasing, frame pacing, ...),
  controllers with rumble, texture packs.
- **More than Windows** - the host's system-bound parts sit behind one
  platform layer, so that Linux and Android builds can follow.

The engine is game-independent: a game is a directory with a `game.json`,
its own port code and its test runs, in a repository of its own.

> **No game material here.** This repository holds no game code, assets,
> disc images or BIOS files. A port is made from a disc image its player
> owns; everything taken from it stays on that player's machine.

> **AI disclosure:**  AI assistance yes (Claude Code by Anthropic).

## How the pieces fit

```
ps2recomp-option-p/         this repository (GPL-3.0-or-later; its ps2-port-engine
                            parts MIT, CREDITS.md)
  ps2decomp/                the library: ELF/ISO readers, R5900 and VU
                            disassemblers, analysis, the interpreter, symbolic
                            derivation, lifting and structuring, verification
  tools/                    the command-line drivers: isotool, analyze, derive,
                            lift, progress, signatures, port, regress,
                            timeline (the frame-pacing profiler), ...
  runtime/                  ps2rt: PS2 instruction semantics the C++ compiles
                            against (verification and the port alike)
  host/                     the native host: memory, dispatch, threads, kernel,
                            IOP, SPU2, GS + GPU bridge, VIF/VU, devices, window
  gpu/                      the Vulkan display and GS renderer
  signatures/               function names shared between games
  docs/                     METHODOLOGY (what "verified" means), ARCHITECTURE,
                            PORTING (every option of a port)
  tests/                    the toolchain's unit tests
  .claude/skills/           ps2-decomp, ps2-port, ps2-release: the workflow
                            and its rules, for Claude Code

<game>-pc/                  a game's repository (e.g. burnout3-pc)
  engine/                   this repository, a git submodule pinned to a tag
  games/<game>/             game.json, port/ (overrides, PC features, test
                            runs), config/, docs/, tests/
```

A game repository is cloned with `git clone --recursive`. The tools are run
from its root (`python engine/tools/port.py --game games/<game>`), and Claude
Code finds the skills in `engine/.claude/skills`.

## Requirements

* Windows 10/11 x64, Python 3.10+ (standard library; Pillow for the art
  scripts), Visual Studio 2022 with C++ (MSVC, its CMake and Ninja); and
  [LLVM](https://github.com/llvm/llvm-project/releases)'s clang-cl, which
  `tools/port.py` builds with when it is installed (its code is about 17-20%
  faster on the VU and GS, the pictures the same; `PS2_COMPILER=msvc` builds
  with MSVC's own) - and with it, `port.py ... pgo`: a profile-guided build
  from the game's scripted runs (Burnout 3: 10-12% less CPU in a race)
* For a port: a Vulkan 1.3 GPU, a processor with AVX2
* A disc image you own, for each game: an .iso, or a CD's raw .bin with its
  .cue (Mode 1 or Mode 2 sectors)

## Starting a port of a new game

1. A repository for it, with this one as its engine:
   `git submodule add <the ps2recomp-option-p repository> engine`
2. The game's directory, from `templates/`:
   `python engine/tools/port.py init <game> --iso "<your disc image>" --title "<Its Title>"`
   writes `games/<game>/` (game.json with the executable read from the
   disc's SYSTEM.CNF, the port's build, an empty overrides.cpp, the player's
   README, a findings stub) and the repository's `.gitignore`, which keeps
   disc material and the game's code out.
3. The pipeline, from the game repository's root:

   ```sh
   python engine/tools/isotool.py extract "<iso>" <EXECUTABLE> -o games/<game>/extracted/
   python engine/tools/analyze.py   --game games/<game>
   python engine/tools/signatures.py --game games/<game> name
   python engine/tools/derive.py    --game games/<game> --emit --csv
   python engine/tools/lift.py      --game games/<game> --structure --emit --csv
   python engine/tools/port.py      --game games/<game>      # build and run natively
   ```

4. The bring-up loop: each run reports why it stopped; what every game needs
   is fixed here, what only this game needs in its own `port/`. See
   [docs/PORTING.md](docs/PORTING.md) and the skills.

What belongs here: what two games have proved they both need, and the
plumbing every port uses (the host's hooks, settings storage, the profiler).
A game's own features - its menus' mouse support, its widescreen anchors,
its packaging - stay in its repository until a second game needs the same;
then the common part moves here, shaped by both.

An engine version is tagged only after every game's recorded runs pass
against it: `python engine/tools/regress_all.py [<other game repositories>]`.
A game repository then moves its `engine/` submodule to the new tag.

## Tests

```sh
python -m unittest discover -s engine/tests       # from a game repository
python -m unittest discover -s tests              # from this repository
```

The host's own tests build with every port (`ps2host_tests`). A change to the
engine is checked against every game's scripted runs
(`engine/tools/regress.py`) before it is tagged: those need the games' discs,
so they run on a developer's machine, not in CI.

## Credits

- **PaPe** ([@nynjardin](https://github.com/nynjardin)): Sony's inet on the
  PC's sockets and the IOP's fixes for EA's network module (Burnout 3's LAN
  play), the GPU renderer on a thread of its own, the sampler's fix
  ([#1](https://github.com/danielgomesvieira2000/ps2-port-engine/pull/1)).
- [SMAA](https://github.com/iryoku/smaa) (Jimenez et al., MIT licence) in
  `gpu/third_party/smaa`, with its licence.
- [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
  (AMD, MIT licence, v1.0.2) in `gpu/third_party/fidelityfx_fsr1`, with its licence.
- [Liberation Sans](https://github.com/liberationfonts/liberation-fonts) Bold (SIL Open Font
  License 1.1) in `data/fonts`, rasterised with [stb_truetype](https://github.com/nothings/stb)
  (MIT / public domain): a port's own lines in the game's menus.
- [ps2autotests](https://github.com/unknownbrackets/ps2autotests) (ISC licence): the real
  console's results the toolchain is checked against.

## Legal

This repository contains original tooling only - no game code, assets or
binaries, nothing extracted from a disc. Reverse engineering for
interoperability and preservation is protected in many jurisdictions; the
ports made with it redistribute no game assets and need the player's own disc.

ps2recomp-option-p is licensed under the GNU General Public License v3.0 or
later ([LICENSE](LICENSE)); the parts that come from ps2-port-engine keep its
MIT licence (`CREDITS.md`, `LICENSES/`).
