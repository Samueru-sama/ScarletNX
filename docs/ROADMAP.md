<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Status + roadmap

Recorded run, 2026-10-11: Catherine: Full Body USA (`0100BF00112C0000`, v2.0),
New Game cutscene, Vulkan, 60 FPS, no audio. Older traces use the Asian TID (`0100BC4010896000`).

Implemented: Clang/CMake, Linux/Windows CI, SDL3/Vulkan, shader caches, component libs.

## Next work

- **Regression:** ARM ISA, IPC, VM, GPU-cache tests. Headless smoke runs + reference images.
- **Diagnostics:** stricter warnings, shader vector-boundary tests at R254/RZ. Import thread-dump
  reader and Maxwell instruction scanner.
- **Audio:** host output, `audout` PCM, `audren` mixing/routing/FX. Validate hwopus playback.
- **GPU:** per-channel scheduling so a blocked channel cannot stall its fence producer.
  FB transforms, attachment feedback, BCn fallback.
- **CPU:** ARM NaN propagation/default-NaN and directed FP rounding. Thread suspension at WFE.
- **Host:** vsync during Win32 window drags, native Windows/gamepad coverage.
- **Performance:** runtime/guest profiling, Vulkan measurements, broader title coverage.

Commands: [development](DEVELOPMENT.md).

## Design

Port upstream releases manually to retain independent component layouts.

NVN HLE remains exploratory: Catherine requests 468 exports and uses ~178. Submitted NVN shaders, clears, and a
narrow Catherine draw slice can now execute through opt-in host probes, while the displayed frame still uses guest
Maxwell command decoding.
The [NVN HLE investigation](NVN_HLE.md) records the resource path and the remaining image-coherence and
whole-submission migration boundaries.
