<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVN HLE investigation

The current path executes the game's NVN library on the guest CPU. That library writes Maxwell pushbuffers, submits
them through nvdrv, and the GPU thread decodes them before calling the Vulkan renderer. NVN HLE would handle selected
NVN API calls directly, but must keep the same guest memory, shader translation, syncpoint, host1x, and presentation
contracts. It is a separate front end to the renderer, not a replacement for the Vulkan back end.

## Entry point and baseline

The SDK exports `nvnBootstrapLoader`, which resolves a named NVN function to a guest function pointer. The recompiler
routes indirect calls through `CPU.disp`, but a statically resolved direct branch calls its generated C function
directly (`scarletnx/codegen.c:call`). `SNX_NVN_TRACE=1` temporarily wraps the bootstrap entry and
the `nvnDeviceGetProcAddress` pointer it returns. It logs each requested name and guest address, then counts calls to
the returned addresses. The wrappers call the original implementations, so the normal NVN path still runs. They are
installed only when the bootstrap symbol has a recompiled dispatch entry. These wrappers cannot see direct guest calls
to an NVN function, so call counts and command-list coverage are lower bounds. A title that uses a different
bootstrap needs a different discovery path.

Use `SNX_NVN_TRACE=1` when running a game. `nvn: NAME -> ADDRESS` records resolution, `nvn-used: NAME COUNT` records
calls, and the final line gives the number of resolved and called addresses. Names that share an address share one
counter, labeled with the first name resolved there. The log and any game assets stay local.

Initial measurements with no button input: Catherine reached 263 frames in six seconds, resolved 467 addresses, and
called 161. Tears of the Kingdom reached 173 frames in 15 seconds, resolved 533, and called 203. The called sets
overlap on 143 names. These are boot samples, so they cannot establish full-game API coverage. Both runs completed
without a crash. The frequently called shared functions include buffer-address lookup, texture-handle lookup and
binding, blend and vertex state setup, and command-buffer operations.

## Direct-call proof

`SNX_NVN_HLE=1` intercepts `nvnDeviceGetTextureHandle` after the guest resolves it and computes its result in the
runtime. This is a pure query, so it does not mix host recording with a guest command list. With
`SNX_NVN_HLE_VERIFY=1`, the runtime also runs the original SDK function on a copy of the CPU state and compares the
full register state. A mismatch restores the guest result and is reported. In 180-frame boots, Catherine made 1,985
verified direct calls and Tears of the Kingdom made 72,130, with zero mismatches or crashes. Screenshot hashes are
not a reliable equality check for these boot scenes: repeated unmodified fixed-frame runs produced different hashes.

This proves the guest-call interception and fallback mechanism. It does not bypass Maxwell rendering yet.

## Queue submission boundary

The trace also records the queue, list count, first list handle, and the number of GPFIFO submissions that occurred
during each NVN queue call. In Catherine's 120-frame boot, `WaitSync`, `SubmitCommands`, and `FenceSync` each issued
zero GPFIFO submissions during the observed calls; `PresentTexture` issued one each time. TotK likewise issued zero
inside `SubmitCommands` and `FenceSync`, but issued one inside each `QueueFlush` and `PresentTexture`. These are
per-call observations, not a guarantee that the SDK always flushes at those points.

The inspected SDK queue code stages command-list handles, then walks a tagged control stream containing GPU work and
sync operations. A direct front end therefore needs to record semantic command-buffer calls, keep list lifetimes,
and submit them at queue flush or present boundaries. Merely forwarding `nvnQueueSubmitCommands` to the current GPU
thread would lose the delayed submission behavior.

## Dual command recorder

`SNX_NVN_RECORD=1` enables a bounded diagnostic sidecar. The runtime copies the arguments of resolved NVN draw,
clear, compute-dispatch, state/bind, transfer, barrier, sync, and `CallCommands` calls into host command-list records.
It associates records with the handle returned by `EndRecording`, copies nested-list handles before the guest reuses
the argument array, and checks
the handles passed to `QueueSubmitCommands`. The original guest SDK calls still execute and render. The records do
not contain resource state or enough information to replay with the host renderer; this mode is for measuring the
shape and coverage of a future direct front end. Each list keeps at most 16,384 selected operations and each nested
call keeps at most eight handles. The summary reports truncated records, invalid guest arrays, allocation failures,
and storage evictions. A list whose `BeginRecording` was not observed is marked opaque, even when selected operations
on that command buffer were captured. Opaque therefore describes an incomplete lifecycle, not necessarily an empty
record.

In a Catherine 120-frame run, all 122 submitted lists were found, containing 1,239 draws, 610 clears, 16,164 state
or bind calls, 157 transfers, and 1,220 barriers. No operations were dropped. TotK's boot uses a different structure:
its outer lists primarily call secondary lists whose begins were not seen through the resolved API. In a 25-second
run, all 223 submitted outer lists and all 2,223 secondary references were found. The recorder captured 6,517 draws,
6,572 clears, 1,381 compute dispatches, 392,784 state or bind calls, and 11,791 barriers, with no storage evictions,
truncation, or invalid guest arrays. The outer lists themselves contained calls, state setup, and barriers; draws
and dispatches appeared in secondary records. Counts varied substantially across runs at the same frame or time
limit, so these are boot observations rather than stable title-wide coverage numbers. The records retain raw argument
registers rather than resolved resource state. Object lifetime calls and guest memory contents remain outside the
sidecar.

For deeper investigation, recompile a game with `--hook-direct-calls`. This opt-in mode routes statically resolved
guest calls through `CPU.disp` too, so the NVN wrappers can observe direct calls. It requires a new game module; the
default recompiler path keeps direct C calls. A list can otherwise contain commands or lifecycle transitions that
bypass the recorder, even when every handle lookup succeeds. The diagnostic's zero-miss counts prove handle
correlation for observed calls, not complete command coverage. A 120-frame Catherine build with `--hook-direct-calls`
rendered the network prompt and completed with 123 submitted lists found, no opaque lists, no dropped operations, and
zero texture-handle verification mismatches. This validates the opt-in dispatch path for Catherine's boot, not TotK's
secondary-list coverage or full-game command completeness. The recorder now filters calls by its selected NVN API names
even when `SNX_NVN_TRACE=1` wraps all resolved functions; otherwise unrelated calls were misclassified as command-buffer
activity.

An opt-in TotK module compiled with `--hook-direct-calls -O 1` ran for 25 seconds and presented 281 frames. The
recorder found all 283 submitted lists and all 1,464 referenced secondary lists, with no dropped operations or
texture-handle verification mismatches. Yet all 1,464 secondary references remained opaque: only 284 exported
`BeginRecording` calls were observed against 9,649 `EndRecording` calls, and 6,493 command buffers first appeared
through another selected operation. Direct calls to resolved exports therefore do not explain the missing starts.
The missing starts are inlined in TotK's game code. At `0x8714394` and `0x87164ac`, the game writes the same recording
flag and command-buffer fields as the exported begin function at `0xd15a480`. A temporary caller probe saw opaque
`EndRecording` calls from game sites `0x88e29c4` and `0x8716e34`; the command buffer's recording flag was set before
each end call. The probe was removed after this check. A replayable HLE path cannot assume every list starts through
the exported begin entry point. It must either recognize the inlined producer or take over at a later boundary where
its command stream and resource state can be recovered.

TotK calls the exported `AddControlMemory` and `AddCommandMemory` functions immediately before at least one inlined
begin sequence. The diagnostic recorder now retains those calls as memory-preparation events, separate from command
state and draw operations. They identify supplied command/control storage but do not prove that all subsequent
operations were observed or make an opaque list replayable. In a later 25-second TotK run it retained 24,926
memory-preparation events in 12,463 opaque ended lists, with no dropped operations or missing submitted or referenced
lists. Catherine's preparation calls occur before its exported begins; these are discarded when the begin resets the
record and do not inflate its implicit-begin count.

## Migration boundaries

`SNX_NVN_POOL=1` now records memory-pool lifetimes alongside guest execution. At the exported
`nvnMemoryPoolInitialize` boundary it snapshots the builder's flags, backing pointer, and size, so inlined builder
setters are covered too. Only a successful initialization creates or advances a pool record. Finalize retires it;
Map and GetBufferAddress attach the returned CPU and GPU addresses. The bounded table reports failed initialization,
invalid builders, reuse without finalize, capacity loss, and operations on untracked pools. It does not own the backing
memory or change rendering. The backing pointer is intentionally not required to be CPU-readable: GPU-only and virtual
pools can be valid. A later resource front end can use the pool generation and address mapping to resolve buffers and
textures without assuming that copying a texture once preserves aliasing or coherence.
In instrumented boot runs, TotK reached 511 frames with 1,820 successful pool initializations, 112 finalizations,
1,134 maps, and 98 GPU-address queries. Catherine reached 120 frames with 139 initializations and 133 GPU-address
queries. Neither run reported an invalid builder, capacity loss, or a Map/GetBufferAddress call on an untracked pool.
These observations validate the exported lifecycle path during boot, not complete resource coverage in gameplay.

`SNX_NVN_BUFFER=1` also tracks buffer views and enables pool tracking automatically. At exported
`nvnBufferInitialize`, it snapshots the builder's pool pointer, offset, and size. On success it binds the buffer to
the pool generation and checks its range against the pool size. Finalize retires the record; Map and GetAddress retain
the returned addresses. The first nonzero buffer address can establish a tentative pool GPU base; later addresses
check base plus offset, and `nvnMemoryPoolGetBufferAddress` can independently check the inference. The summary
separates untracked pools, out-of-range views, stale pool generations, and address mismatches.
This remains an observation layer: it does not replace guest buffer operations or synchronize host memory.
In opt-in direct-call boot runs, TotK reached 245 frames with 11,291 tracked buffer initializations, 556 inferred
pool GPU bases, and 20,082 consistent address checks. Catherine reached 120 frames with 93 buffer initializations,
two inferred bases, and 2,151 consistent checks. Both runs reported zero untracked pools, invalid ranges, stale
generations, and address mismatches. These checks establish internal address consistency for observed buffers, not
that all guest resource accesses have been intercepted.

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
