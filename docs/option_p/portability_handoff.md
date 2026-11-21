# Option P - portability hand-off (for L: Linux, Android)

Status 2026-11-17 (P, toolkit branch `p/portability`). Windows is still the only system the host builds for; this
is the preparation the maintainer ordered (M8: "Windows-gebundene Host-Teile hinter eine neutrale Schnittstelle,
Windows bleibt einzige Implementierung; NEON-Paritaetscheck; Buildsystem bereit; Uebergabedokument fuer L").
No Linux or Android build has been made (not ordered yet).

## 1. Where the system is touched now

Everything of the host (`engine/host`) and the GPU display (`engine/gpu`) that is not standard C++17 or SDL3 goes
through one library, `engine/platform` (target `ps2platform`):

| file | what |
|---|---|
| `platform/include/ps2platform/platform.h` | the interface (fibers, waits, timers, threads, cores, paths, process, dialogs, sound out, overlay text, fallback window) |
| `platform/include/ps2platform/keys.h` | the host's key codes (Windows' virtual-key numbers, by name) and scan-code calls |
| `platform/src/platform_win32.cpp` | Windows: everything but sound |
| `platform/src/audio_win32.cpp` | Windows: sound out (WASAPI shared, waveOut fallback) |
| `platform/CMakeLists.txt` | one source list per system; a FATAL_ERROR names the missing file on any other system |

A new system adds `src/platform_linux.cpp` (and `audio_sdl.cpp`, say) and its branch in `platform/CMakeLists.txt`.
Nothing in `host/` or `gpu/` should need to change for it - if something does, it belongs in the interface.

Outside `engine/platform` these still include `<windows.h>`, on purpose:

| file | why it stays | what another system does |
|---|---|---|
| `host/src/profile.cpp` | the `--sample` profiler and the crash report walk x64 Windows stacks (CONTEXT, dbghelp); built only for `_WIN32` + x64 (`PS2HOST_PROFILE_WIN64`), stubs elsewhere | Linux: `perf` from outside, or a SIGPROF sampler with `backtrace()`; crash report: a SIGSEGV handler with `backtrace_symbols_fd`. Android: none needed (logcat tombstones). Dev tools only, not in the player's path |
| `host/src/cpu_check.cpp` | MSVC builds only: the AVX2/SSE4 executable picked before the C runtime starts | x86-64 Linux: the same idea with `__builtin_cpu_supports` in a tiny launcher, or ship the SSE4.1 build only. AArch64: nothing (NEON is in the base) |
| `host/src/iop_inet.cpp` | Winsock vs BSD sockets: already written for both (`#ifdef _WIN32`, the POSIX branch is there) | nothing - compile the POSIX branch |
| `launcher/launcher.c` | the player's Windows launcher (disc import, the player's own build with llvm-mingw) | a Linux launcher is a separate piece of work (the AppImage model of Option C) |

## 2. The interface, call by call, with the way on Linux and Android

| platform.h | Windows (now) | Linux | Android |
|---|---|---|---|
| `ThreadAsFiber`, `CreateFiber`, `SwitchFiber`, `DeleteFiber` | Win32 fibers | `ucontext` (`makecontext`/`swapcontext`) works but saves the signal mask (a syscall per switch: too slow for the scheduler's rate) - use a small assembly context switch (x86-64 SysV / AArch64 AAPCS64: callee-saved registers + sp; ~20 lines each) or Boost.Context's `fcontext` (header + .S, BSL-1.0). Stacks: `mmap` with a guard page, 1 MiB | the same AArch64 switch; bionic has no `ucontext` makecontext, so the assembly one is required |
| `WaitOnValue`, `WakeAll` | WaitOnAddress / WakeByAddressAll | `futex(FUTEX_WAIT_PRIVATE)` with a timespec timeout / `FUTEX_WAKE_PRIVATE, INT_MAX` | the same (`syscall(__NR_futex, ...)`) |
| `SleepUntil` | high-resolution waitable timer | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` - precise already | the same |
| `FineTimers` | timeBeginPeriod(1), power throttling off | nothing | nothing (a sustained-performance hint belongs to the activity, SDL3 handles it) |
| `SetThreadPriority` | THREAD_PRIORITY_* | `setpriority(PRIO_PROCESS, gettid(), nice)`: kAboveNormal -5, kHighest -10, kTimeCritical -15 where allowed (RLIMIT_NICE), else unchanged; SCHED_FIFO needs rights - do not require it | `setpriority` the same (the app's own threads may raise to -10 or so; `ANDROID_PRIORITY_URGENT_AUDIO` = -19 for the audio thread) |
| `NoPowerThrottling` | thread power throttling off | nothing | nothing |
| `NameThread`, `ThreadCpuNs`, `ProcessCpuNs` | SetThreadDescription, GetThreadTimes, GetProcessTimes | `pthread_setname_np` (15 chars), `pthread_getcpuclockid` + `clock_gettime` (kernel part: `/proc/self/task/<tid>/stat` utime/stime, or report 0), `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` | the same |
| `ProcessCores`, `HoldToCores` | GetProcessAffinityMask, SetProcessAffinityMask | `sched_getaffinity` / `sched_setaffinity` (CPU_COUNT) | the same; big.LITTLE: `HoldToCores` is for measurements only, never in the player's path |
| `ExePath`, `ExeDir` | GetModuleFileNameW | `readlink("/proc/self/exe")`; an AppImage: `$APPDIR`-relative is the read-only image - settings and saves must go to `$XDG_DATA_HOME/<game>` instead (the host takes the settings folder from `--memcard`'s parent, so the launcher passes that) | no executable folder: the app's files dir (`SDL_GetAndroidInternalStoragePath`) for settings and saves, assets for boot.mpg; same `--memcard` route |
| `Exists`, `CompareNoCase` | GetFileAttributesA, _stricmp | `access(F_OK)` / `std::filesystem::exists`, `strcasecmp` | the same |
| `AttachTerminal`, `RedirectOutput` | AttachConsole, freopen/_dup2 | nothing / `dup2` onto an `open`ed file | `__android_log_write` (a stderr pipe read by a thread into logcat) |
| `RestartSelf`, `ExitNow` | CreateProcessW, TerminateProcess | `execv("/proc/self/exe", argv)` (argv kept from main), `_exit` | restart: finish the activity and let the launcher start it again; `_exit` |
| `LocalTime` | localtime_s | `localtime_r` | the same |
| `ShowMessage`, `PickFile` | MessageBoxA, GetOpenFileNameA | `SDL_ShowSimpleMessageBox`, `SDL_ShowOpenFileDialog` (asynchronous - wait on its callback) | the same SDL3 calls (the storage access framework underneath) |
| `RunAudioOutput` | WASAPI shared, waveOut | `SDL_OpenAudioDeviceStream` with a get-callback calling `render` (float stereo at `rate`); set `opened`, return when `stop` | the same (AAudio underneath) |
| `DrawTextPanel` | GDI Consolas | stb_truetype (MIT/public domain, one header) with a bundled monospace font under an open licence (DejaVu Sans Mono, Liberation Mono) | the same |
| `OpenFallbackWindow` ... `DrawFallbackWindow` | a GDI window when no Vulkan device can be had | return false: no fallback, the port needs Vulkan 1.3 (the maintainer's rule) | the same |
| keys.h `KeyOfScanCode`, `ScanCodeName`, `SystemKeyDown` | MapVirtualKeyW, GetKeyNameTextW, GetAsyncKeyState | a table set-1 scan code -> key code (the US layout; SDL's scancodes are positional, which is what the bindings mean), `SDL_GetScancodeName`, `SystemKeyDown` false (SDL's window answers `HostKeyDown` before it is asked) | the same |

## 3. NEON parity (the AArch64 check)

Every source of `host/`, `runtime/`, `platform/` and `gpu/` compiles for `aarch64-w64-mingw32` (llvm-mingw
20260908, `-fsyntax-only`, 53 of 53, 2026-11-17; script in the P session's scratchpad, `arm64_check.sh`): no x86
header or intrinsic is reached on ARM. Fixed for it:

- `runtime/include/ps2rt/fp_mode.h` (new): the VU's rounding mode - toward zero, denormals flushed - was MXCSR
  only (`_mm_getcsr`/`_mm_setcsr` in vu.cpp and vu0.cpp). On AArch64 it sets FPCR (RMode = toward zero, FZ). This
  is a correctness item, not speed: without it the VUs round to nearest on ARM (Burnout 3's smoke puffs were the
  example on x86 once). L should run `ps2host_tests` (VuCompiledIsTheInterpreter, the lane tests) on the device.
- `host/src/gs_memory.cpp`: the 32-bit pair store had no non-SSE2 form; now `memcpy` pairs.
- `host/src/profile.cpp`: x64 Windows only (above).

What ARM gets is the portable form of each SIMD path; none has a NEON form yet. These are the gaps, all speed only
(the scalar forms are the reference the SIMD ones are tested against):

| path | x86 form | ARM now | NEON worth it? |
|---|---|---|---|
| VU lanes (`ps2rt/vu_math.h`, `host/include/ps2host/vu_core.h`: FMAC add/sub/mul/madd/msub/max/min, clamps, flags) | SSE2/SSE4.1 (`PS2RT_VU_SSE`, 20+5 blocks) | scalar per lane | **yes, first**: ZOE is VU0/VU1 heavy (11-14 survey). `float32x4_t` maps 1:1 (vaddq/vmulq/vfmaq - but keep the separate multiply-then-add rounding, not fused), `vminq_u32`/`vmaxq_s32` for the integer-compare max/min trick. Since b607247 the adds/subs/madd/msub align their operands first (AlignLanes; scalar: AlignOperands - an operand 25+ exponents below the other a zero of its sign, ps2autotests vu/games/triace): the NEON form needs the same step |
| VU0 macro FMAC (`ps2rt/lifted.h` MacroFmac, `runtime/src/vu0.cpp`) | SSE2 | falls back to Cop2Slow | yes, with the lanes above |
| MMI lanes (`runtime/src/lifted_support.cpp` LanesSse2: pcgt*/pceq*/pmaxh/pminh/padd*/psubw, shifts, unpacks) | SSE2 | scalar (LanesScalar) | medium: `vcgtq_s8/16/32`, `vceqq`, `vmaxq_s16`, `vqaddq_u8`, `vzip1q` |
| GS memory PSMCT16 run store (`gs_memory.cpp`) | SSE4.1 packus | per pixel | low (`vmovn_u32` + `vzip`) |
| GS memory PSMT8 / PSMT4 run stores (`gs_memory.cpp`, PageTable groups8 / groups4, 2026-11-19) | SSSE3 pshufb + SSE4.1 blendv / and-or masks | per pixel | **medium**: ZOE's texture uploads (were 8.6 % of the GS thread, 3.6 % with SIMD); `vqtbl4q_u8` gathers the 64-byte column in one step, `vbslq_u8` for the masks |
| IPU colour conversion (`ipu.cpp`) | AVX2 | scalar | low-medium (movies only) |
| MPEG IDCT (`mpeg_common.h`) | AVX2 (double) | scalar | low-medium (movies only) |

The GPU side has no x86 code. The Vulkan renderer uses no fragment-shader interlock and no vendor extensions
(the Android-safe rule); L's check on Adreno/Mali is the device test, not code here.

## 4. Build system

- `host/CMakeLists.txt` has a third branch beside MSVC and MinGW for any other system: x86-64 gets the same
  instruction floors (`PS2HOST_AVX2` on: -mavx2 ...; off: -msse4.1), AArch64 nothing (NEON is armv8-a's base),
  `Threads::Threads` linked. The Windows libraries (dbghelp, winmm, synchronization, ws2_32, comdlg32) are only
  in the MSVC/MinGW/WIN32 branches or in `ps2platform`'s Windows list.
- `gpu/CMakeLists.txt` fetches SDL3, volk, Vulkan-Headers and glslang (all portable). On Android SDL3 is usually
  built as the shared `libSDL3.so` the Java side loads: L decides (`SDL_SHARED`/`SDL_STATIC` are set in
  gpu/CMakeLists.txt; an `if(ANDROID)` switch there).
- `ps2_add_port` makes an executable; Android wants a shared library (`libmain.so`, SDL3's `SDL_main`): an
  `if(ANDROID) add_library(${target} SHARED ...)` in `ps2_add_port`, with `main.cpp`'s `main` as SDL_main.
- The player's build on Windows (launcher + llvm-mingw, `tools/player_release.py`) takes its object list and
  libraries from the ninja link line, so `libps2platform.a` came in by itself. A Linux release would be built on
  the developer's side (the AppImage model), not on the player's PC - but the lift still needs the player's disc:
  the Linux launcher has to run `player_build.py` with a bundled Python and compile with a bundled clang, the
  Option C Linux release did the same (memory: linux-build-downloads-allowed - LLVM 23.1.1, python-build-standalone
  3.12, glibc 2.35 sysroot, appimagetool).
- Not tried: configuring for Linux or Android at all (not ordered). Expect small things the syntax check cannot
  see: `std::filesystem` with `wchar_t` titles (`OpenWindow(const wchar_t*)` - make it UTF-8 when porting), the
  `%llu` format strings (fine on LP64), `_CRT_SECURE_NO_WARNINGS` (harmless).

## 5. The translated code itself

The lifted C++ and the compiled microprograms use only `ps2rt` (C++17, the inline VU math with its scalar forms):
no system call, no intrinsic outside the `PS2RT_VU_SSE` blocks. The same generated sources build for ARM as they
are. The game module (`games/zoe_us/port`) includes no Windows header.

## 6. Proof on Windows (the order's condition)

- Hangar 168/168 and the per-frame CPU of the scenes before/after, on the branch: see status.md (the row for
  `p/portability`), measured under the exclusive lock. The fiber switch, the waits and the timers are the same
  Win32 calls behind a direct (non-virtual) call - nothing added on the hot path.

## 7. Open for L

1. `platform_linux.cpp` (+ `audio_sdl.cpp`, the text rasteriser) - section 2.
2. The fiber switch in assembly for x86-64 SysV and AArch64 - the one piece with real risk; test it with
   `ps2host_tests` and a short scripted ZOE run (the guest threads and coroutines all run on it) first.
3. NEON forms of the VU lanes (section 3, first row) once a device runs: measure first.
4. Android packaging (SDL3's Android project, `libmain.so`, storage paths) - section 4.
5. Questions to P via coordination.md; generic fixes go into the toolkit (branch, gate, as every change).
