<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVN HLE investigation

The current path executes the game's NVN library on the guest CPU. That library writes Maxwell pushbuffers, submits
them through nvdrv, and the GPU thread decodes them before calling the Vulkan renderer. NVN HLE would handle selected
NVN API calls directly, but must keep the same guest memory, shader translation, syncpoint, host1x, and presentation
contracts. It is a separate front end to the renderer, not a replacement for the Vulkan back end.

## Entry point and baseline

The SDK exports `nvnBootstrapLoader`, which resolves a named NVN function to a guest function pointer. The recompiler
routes both direct and indirect calls through `CPU.disp`. `SNX_NVN_TRACE=1` temporarily wraps the bootstrap entry and
the `nvnDeviceGetProcAddress` pointer it returns. It logs each requested name and guest address, then counts calls to
the returned addresses. The wrappers call the original implementations, so the normal NVN path still runs. They are
installed only when the bootstrap symbol has a recompiled dispatch entry. A title that uses a different bootstrap
needs a different discovery path.

Use `SNX_NVN_TRACE=1` when running a game. `nvn: NAME -> ADDRESS` records resolution, `nvn-used: NAME COUNT` records
calls, and the final line gives the number of resolved and called addresses. Names that share an address share one
counter, labeled with the first name resolved there. The log and any game assets stay local.

Initial measurements with no button input: Catherine reached 263 frames in six seconds, resolved 467 addresses, and
called 161. Tears of the Kingdom reached 173 frames in 15 seconds, resolved 533, and called 203. The called sets
overlap on 143 names. These are boot samples, so they cannot establish full-game API coverage. Both runs completed
without a crash. The frequently called shared functions include buffer-address lookup, texture-handle lookup and
binding, blend and vertex state setup, and command-buffer operations.

## Migration boundaries

1. Keep GPFIFO execution as the fallback until an entire command-list submission path is implemented. Mixing HLE
   recording with guest NVN pushbuffers for one list could duplicate draws or omit state changes.
2. Start with guest object and resource lifetimes. Translate memory pools, textures, samplers, shaders, command
   buffers, queues, and fences to host-owned records with checked guest addresses. Preserve aliasing and CPU/GPU
   coherence; a copied texture is not sufficient when the guest writes the same pool again.
3. Record an explicit command stream for draws, clears, copies, dispatches, barriers, and synchronization. Queue it
   through the existing GPU-thread ordering before calling the renderer. A guest NVN call must not submit Vulkan work
   on a guest CPU thread: descriptor and staging lifetime belongs to the GPU thread.
4. Retain Maxwell shader translation initially. NVN shader binaries are still Maxwell programs, and the existing
   translator and SPIR-V cache can supply shaders to a direct NVN front end.
5. Preserve nvdrv and host1x for non-NVN clients and video. NVN fence behavior must still order with vi presents,
   NVDEC/VIC writes, and CPU-visible syncpoints. Per-channel scheduling remains a separate prerequisite for cases
   where a blocked channel waits for another channel's work.

The first executable HLE milestone should handle a small command list with a clear and a present. It needs a
switchable fallback, a deterministic image comparison against the existing path, and a title run beyond boot. Expand
coverage by observed API calls and rendered operations rather than by the number of names returned at bootstrap.

deko3d's [primer](https://github.com/devkitPro/deko3d/blob/master/Primer.md) is a useful public reference for
command-buffer, queue, and fence lifetime on Switch. It is a related API, not a specification for NVN behavior.
