# FidelityFX Super Resolution 1 (FSR 1)

[FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (AMD, FSR 1.0.2), MIT licence
(`LICENSE.txt`), tag v1.0.2 = a21ffb8f6c13233ba336352bdff293894c706575, `ffx-fsr/`:

- `ffx_a.h`: AMD's shader portability header (SHA-256 f60e2722...f763f3fb), unchanged.
- `ffx_fsr1.h`: EASU (the edge-adaptive upscale) and RCAS (the sharpening) (SHA-256 93c39223...a5), unchanged.

Used by `gpu/shaders/fsr.comp` (built twice: `-DEASU`, `-DRCAS`) and `gpu/src/vulkan_display.cpp`
(UPSCALING: FSR 1 - the picture brought up to the window's size where it is drawn smaller).
