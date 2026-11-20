# Option P - Phase 1: survey

Date: 2026-11-05. Read-only (GitHub API, trees, READMEs, docs, trackers); nothing cloned or built.
84 projects/components in `01_survey.csv` (group A = PS2Recomp network + big recomps, B = further recomps,
C = decomp ports, components, tracker sweep). Per-project notes: `survey_notes/group_{a,b,c}.md`.
Markers in the CSV: [V] seen in source/tree/docs, [U] claim only. Focus (maintainer): resource efficiency first.

## 1. Main findings

1. **VU1 is the CPU wall everywhere.** Interpreted: 1-3 fps (WWE, SRWZ). One program missing from BT3's compiled
   table: 20-25 fps instead of 60. Exact VU+GS on a Snapdragon 888 (Downhill Revived): 42 ms CPU per field over
   3.3 cores (game 12, VU 10, GS 7.7 ms).
2. **Naive PS2Recomp runtimes are worse than PCSX2.** Urban Reign: PCSX2 ~86 % of one core, the port ~320 %
   (IOP thread busy-spins 77 %) [U, measured by the project].
3. **Every fast port gave up VU1/GS exactness somewhere** - by hand, per game: ico-pc (5 VU1 programs -> vertex
   shaders), OpenGOAL (~8 renderers), Tenkaichi3Decomp (all VU1 -> GLSL, 30 -> 10 ms/frame), AC5 (VU1 3D -> vertex
   shaders), rac1-native (VU1 output -> static GPU buffers). **Nobody generates VU1 shaders automatically yet.**
4. **Library-level HLE is the PS2 analogue of our D3D-level renderer (NG2):** TheBozos/BozoEngine (MIT, created
   2026-11-02): no GS/VU/VIF/DMA/IOP/SPU2 at all; draw calls hooked into a native renderer, Sony libraries
   (pad, mc, cd, libmpeg, sound driver) native; TMB 60 fps at ~5 ms/frame; proved against 74 M recorded calls,
   0 differences. Per game (renderer hooks), OpenGL 3.3, 3 days old.
5. **Snapdragon 8 Gen 2 is reachable:** Road Trip recomp (GPL) - 10-min race at 60 fps, no throttling, GPU at its
   lowest clock (AYN Thor = same SoC as Odin 2 Portal); own light Vulkan GS = ~half the GPU time of paraLLEl-GS.
   Burnout 3 (on ps2-port-engine): Pixel 7 Pro ~30 fps; 120 fps on 2 cores + Intel UHD.
6. **paraLLEl-GS:** good desktop numbers (BT3: 2.8 ms CPU + 2.9 ms GPU vs 5.5 + 7.5 for a GL GS), but **no
   public report on any mobile/Adreno GPU** (needs subgroup-size control 16-64) -> exact fallback only.
7. **Adreno pitfalls (measured by others):** Qualcomm driver present stalls 363-583 ms (ico-pc offers Turnip),
   render-target reallocation 90-440 ms (GT3 Revisited), fence waits fixed by 3 frames in flight (Chronicle).

## 2. Ranked efficiency techniques (merged from A/B/C)

| # | Technique | Best sources | Gain seen | Fit |
|---|---|---|---|---|
| 1 | Sync-HLE: waits sleep; PCSX2 idle-loop rule (self-loop that only reads -> jump to next event); worker-owned status polls | PCSX2, ps2-port-engine AddIdleLoop, Burnout 3, Road Trip, VCS | Burnout 3 thread 51 -> 17 % CPU; Road Trip +20 % fps on Thor; VCS -25 % game thread | generic, high |
| 2 | Lean EE codegen: registers as locals, PC stored only where read, direct calls with cycle budget, `__restrict`, 32-bit width analysis, ThinLTO/PGO | Killzone, VCS, PS2RC (ideas only), DQ8 fork | Killzone dispatches 923k -> 8k/s; VCS ~-50 % EE | generic, high |
| 3 | VU1 AOT, timing resolved at build time, flags only where read, interpreter fallback, fuzz/replay oracle | Road Trip, GT4Recomp (MIT), BT3, Project Veronica (MIT), ARMSX2 (NEON regs across blocks) | Veronica game thread 4.5-7.6 -> 1.0-1.3 ms | generic, high |
| 4 | Ordered draw recording + same-state batching (kick order, never sort), markers in the DMA stream | ico-pc, Project Veronica (reg 0x7F), Kazarov (MIT), WWE, VCS | VCS submit 36.7 -> 12.1 ms | generic, high |
| 5 | Lazy GS memory: write uploads into the GS-memory model only when read; readback only on demand | Project Veronica, GT4 (readbacks /6, passes /40), PCSX2 HW | large GPU + CPU | generic, medium-high |
| 6 | Own light Vulkan GS as default, paraLLEl-GS exact fallback | Road Trip, GT4 CoreGS 2.0 (MIT) | ~half GPU vs paraLLEl-GS | generic, high |
| 7 | Library-level HLE (no IOP, native pad/mc/cd/mpeg/sound) | BozoEngine (MIT), Play! (~50 HLE IOP modules, BSD-2), DQ8 (IOP/SPU2 thread clocked by audio) | Killzone 1.25-1.4x game thread; GoW boot 95 -> 20 s | generic per library, high |
| 8 | VU1 -> SPIR-V automatically, gated by a CPU oracle | (new; starting points ARMSX2 VU AOT front end, Downhill vu1_aot.py, both GPL) | the biggest lever (VU1 off the CPU) | research, medium |
| 9 | Draw-call hooks into a native renderer (per game / per middleware, e.g. RenderWare) | BozoEngine/TMB, WWE, VCS, BT3 | TMB ~5 ms/frame | per game, high gain |
| 10 | One SIMD layer with bit-identical SSE/AVX/NEON/C backends; NEON VIF unpack | ARMSX2, ps2-port-engine (sse2neon today) | ARM64 | generic, high |
| 11 | Mobile: render-pass discipline for tilers, pipeline pre-build from a saved list, ADPF hints, big-core pinning, fixed-size targets, 3 frames in flight, 0 % CPU in background | Burnout 3, Road Trip, T3, Chronicle, GT3 | Burnout 3 -41 % passes | generic |
| 12 | High fps: 60 Hz simulation + transform interpolation from guest RAM, or per-game timers to real time; EE cycles scaled with vblank rate | Burnout 3, Road Trip, VCS, Killzone (81.8 -> 118 fps) | 120/240 Hz | generic + per game |

**Avoid:** x86 JITs as main path (microVU, MrCool VU JIT), PCSX2 GS/microVU as libraries (x86/AVX, runtime
shader compile), full-frame GPU->CPU copies (TheBozos' paraLLEl-GS glue), OpenGL + fragment interlock (no mobile),
busy-spinning helper threads, raising vblank rate without scaling EE cycles, frame skip that hides hitches.

## 3. Licence map for taking code (Option P = GPL-3.0)

| OK | Projects |
|---|---|
| GPL-3.0 | ps2-port-engine (MIT), PS2Recomp + forks, ZOE-Recompiled, Road Trip, VCS, BT3-Recomp, WWE, rac-rs, SOCOM, Battle Stadium, ARMSX2, Downhill Revived |
| MIT / BSD / ISC / Apache / MPL | BozoEngine, GT4Recomp, ps2kit, Project Veronica, Kazarov, exoteric/snuri, Play! (BSD-2), ps2autotests (ISC), AC5 (Apache-2.0), librashader (MPL-2.0), paraLLEl-GS (LGPL-3.0+) |
| Ideas only | Tenkaichi3Decomp (AGPL; code before 06e4671 MIT), PS2RC (custom), ico-pc (custom), no-licence repos (God Hand Android, CoN, Gitaroo Man, ProjectCane, Baroque, REOF2, Chronicle) |

## 4. Corrections to the brief's lists

OpenTricky = Xbox (xboxrecomp, Option C only). PS2RC = GTA SA on **PSP**, custom licence. VCS PS2 = gothamor/gta-vcs-recomp.
ico-recomp (PS2) = nathanialf, superseded by ico-pc. RE Outbreak: RaccoonRecomp has no source; online port = Ninjacornix/reo.
Ridge Racer V = danisandoval/fukami; AC5 = sal063 (Apache-2.0); AC04 derived from AC5. Road Trip = silentsudin/RoadTripAdventure-recomp.
New generic toolkits: BozoEngine, ps2kit (MIT), ps-recomp (Odin, Vulkan 1.3), PS2Recomp-Fixer, fabegalo/ps2-native, luisbraganca/ps2n.

## 5. Open points

- No paraLLEl-GS and no Precursor/Downhill numbers on Adreno 740 -> measure on the Odin 2 Portal ourselves.
- Road Trip's runtime fork (silentsudin/PS2Recomp branch `roadtrip`: hardware GS, VU1 compiler) read in docs only.
- ps2autotests: IOP tests need an IOP path; an all-HLE runtime runs the EE subset only (exoteric's 54/55 is a claim).
