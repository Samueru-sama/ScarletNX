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

`SNX_NVN_TEXTURE=1` captures texture builder metadata at `nvnTextureInitialize` and enables pool tracking. It records
the pool generation, offset, flags, target, dimensions, levels, format, and sample count for successful textures,
then retires them at `nvnTextureFinalize`. Immediately after a successful initialization, the runtime calls the
resolved, pure guest `nvnTextureGetStorageSize` on a copy of the CPU state. That delegates tiled, compressed,
multisample, and mipmapped footprint calculation to the title's SDK without changing the guest call's return state.
The inspected TotK getter reads size from texture offset `+0x5a`; Catherine reads it from `+0x78`. The result checks
the entire `[pool offset, pool offset + storage size)` range with overflow-safe subtraction. A zero pool pointer is
reported separately because externally backed textures can use one. This remains a diagnostic check, not a host
texture allocation or a replacement for guest memory coherence.

In a 250-frame TotK boot, the getter provided sizes for all 3,097 initialized textures and no footprint extended
outside a tracked pool. Catherine reached 120 frames with sizes for all 109 textures and likewise no out-of-range
footprints. Earlier runs tracked 3,142 TotK textures at 255 frames and 109 Catherine textures at 120 frames, with no
missing pools or offsets outside pools.

`SNX_NVN_ALIAS=1` enables the pool, buffer, and texture observers together. It places each valid buffer or texture
footprint in a pool-relative interval registry keyed by pool generation and guest object identity. Registering a new
view counts overlaps with active views in that pool, separating buffer-buffer, buffer-texture, and texture-texture
pairs. Finalizing an object or its pool retires the affected intervals. The registry is bounded and reports zero-size
views, invalid ranges, and capacity loss. It does not resolve which overlapping view contains the newest contents;
that needs GPU-thread ordering and guest-memory coherence before HLE rendering can use the intervals.
In a 289-frame TotK boot, 14,407 intervals were registered, with 22 buffer-buffer and 233 texture-texture overlap
pairs, including 84 exact-range pairs. Catherine registered 201 nonzero intervals in 120 frames and had no overlapping
pairs in that boot scene. Neither run lost records or reported an invalid interval.

`SNX_NVN_DESCRIPTOR=1` observes the texture-ID path. It records texture-pool lifetimes and
`nvnTexturePoolRegisterTexture` mappings from descriptor index to a texture object generation. It follows
`nvnCommandBufferSetTexturePool` on each command buffer, decodes the low 20-bit texture ID in single
`nvnCommandBufferBindTexture` handles, and checks that the bound descriptor still names a live texture generation.
This connects an observed bind to the tracked texture footprint. Registration also copies the 32-byte hardware TIC
from the texture pool's CPU backing when that memory is readable, or from its mapped GPU address as a fallback. The
copy is accepted only when its encoded GPU address matches the registered texture's observed address. A pool or
texture object generation change invalidates the corresponding binding snapshot. Draw packets carry accepted TIC
bytes with their texture and view identities. This is descriptor transport, not texture upload or guest-memory
coherence for HLE rendering.
When a command buffer has no observed pool selection and exactly one texture pool is live, the observer can use that
unique pool for a diagnostic lookup; it reports these binds separately. This does not establish command-list state
inheritance, which requires replaying pool-selection operations in queue submission order.
`BindTextures` arrays, image descriptors, and multi-pool commands whose pool selection was inlined remain outside
this eager observer's coverage.
In a 166-frame TotK boot, all 26,775 observed single-texture binds resolved to live registrations; 26,608 used the
unique-pool inference. Catherine resolved all 1,513 binds from observed pool selections in 120 frames. Both runs had
zero missing descriptors, stale textures, invalid IDs, and table losses. The TotK inference remains conditional on
there being exactly one live texture pool; it does not prove correct state propagation through secondary lists.
In later draw-preparation boots, Catherine copied all 199 registered TICs from CPU backing and carried a valid TIC
on all 3,597 texture-using draws. TotK copied all 7,163 registered TICs and carried one on all 3,650 texture-using
draws, with no missing backing or address mismatch.
The descriptor pool did not expose a GPU base in either run, so CPU backing was essential to capture them.

`SNX_NVN_DESC_REPLAY=1` enables command recording and descriptor observation together, then walks submitted lists in
recorded order. `SetTexturePool` and `SetSamplerPool` change queue descriptor-pool state; `CallCommands` recursively
executes secondary list records with the caller's state. Each single `BindTexture` checks the low 20-bit texture ID
against its descriptor, texture generation, and backing pool generation, and checks the next 12-bit sampler ID against
its registration and sampler generation. Queue state carries
across submitted lists. The report separates resolved binds from missing pool state, descriptors, stale textures,
opaque or missing lists, truncated references, and recursion depth limits. This is a diagnostic walk of observed NVN
calls, not a substitute for executing the guest command stream or proof that inlined operations were captured.
In a 120-frame Catherine run, all 1,511 replayed binds resolved across 122 submitted lists, with no opaque or missing
lists. In a 25-second TotK run, all 61,586 replayed binds resolved across 337 submitted roots and 2,263 opaque
secondary-list visits, with no missing lists, truncated references, depth-limit hits, or stale pool generations. The
69,205 eager TotK bind calls include work not yet submitted during that interval; the replay count measures submitted
list executions rather than calls made while building lists.

`SNX_NVN_SAMPLER=1` tracks sampler object initialization, finalization, and generations without decoding builder bytes:
the inspected TotK and Catherine SDKs copy different builder layouts. Descriptor observation also enables sampler
tracking and records `nvnSamplerPoolInitialize`, `Finalize`, and `RegisterSampler` by pool generation and descriptor
index. The replay reports sampler resolution separately from texture resolution, including missing pool selections,
missing registrations, and sampler objects that were finalized or reused. This validates observed SDK object identity;
it does not decode sampler state or establish whether finalized sampler objects leave copied hardware descriptors valid.
In a 120-frame Catherine run, all 1,526 replayed binds resolved through live sampler registrations, with zero missing
or stale entries; its three sampler objects and one sampler pool were tracked without table loss.
An 85-frame TotK run resolved all 10,965 submitted binds through live sampler registrations across 86 roots and 425
opaque secondary-list visits. It tracked 849 sampler initializations and 91 registrations in one pool, with no
missing or stale sampler result, untracked registration, or table loss.

`SNX_NVN_PROGRAM=1` records program initialization, finalization, and successful shader setup through either
`nvnProgramSetShaders` or `nvnProgramSetShadersExt`. Descriptor replay enables this observer and checks each
`nvnCommandBufferBindProgram` in submitted lists against a live, configured program. The check uses the exported
program pointer and setup return value. It also snapshots up to eight 24-byte shader references before the guest SDK
can reuse the input array. In the ordinary entries observed so far, the first word names GPU code, the second names
CPU control data, and the third is an extension word; the TotK exception below prevents treating that as a universal
layout. The inspected TotK `SetShadersExt` copies entries in 24-byte steps, and Catherine's shared setup
routine advances by the same stride. This is a source for future program materialization, not a copy of shader code.
In a 120-frame Catherine run, all 1,462 replayed program binds found configured live programs. Its 133 initialized
programs each received a successful shader setup, with no failed or untracked setup calls.
In a 279-frame TotK run, all 1,028 replayed program binds resolved, and all 3,449 initialized programs received
successful shader setup. Neither run reported missing or unconfigured program binds or table loss. These are object
lifetime checks at submission time; actual shader-stage contents and command-list snapshot semantics remain unproven.

`SNX_NVN_SHADER_TRANSLATE=1` enables descriptor replay and a bounded execution probe. Submitted `BindProgram` calls
stage shader code addresses in their NVN queue; after the guest SDK's `QueueFlush` or `PresentTexture`, the first eight
staged addresses are queued behind guest GPFIFO work on the GPU thread. The worker reads each NVN code blob from GPU
memory, checks its `0x12345678` header, strips its 0x30-byte wrapper,
and passes the following [Maxwell shader program header and instructions](https://download.nvidia.com/open-gpu-doc/Shader-Program-Header/1/Shader-Program-Header.html)
to the existing translator and glslang. Stage classification currently recognizes the observed vertex and fragment
headers; other stages are reported as unknown. The resulting SPIR-V is discarded after compilation. The probe does
not bind constant buffers or use the compiled shader for a draw, so successful compilation is not yet evidence of
correct rendered output. In initial Catherine and TotK boot samples, eight sampled shaders from each title compiled
to SPIR-V with no unsupported instructions when translated at program setup. At the submitted-program boundary,
Catherine again compiled eight of eight sampled references. TotK compiled four of eight: the other four all named
one repeated address whose GPU-memory read returned zeros. One TotK `SetShadersExt` call has a second 24-byte entry
whose first word is CPU-address-shaped, while its preceding entry's extension word is GPU-address-shaped. The current
observer cannot assign that entry to a shader stage safely, so these lists must stay on the guest path. This is the
first direct NVN-to-host shader execution path, while command recording, resource binding, synchronization, and
presentation still run through the guest SDK.

Descriptor replay now also snapshots the scalar values of `ClearColor` and `ClearDepthStencil`, including the depth
float passed in an ARM SIMD register, and copies `SetRenderTargets` texture-handle arrays before the caller can reuse
them. It carries the selected targets through secondary lists and checks clear commands against live texture records.
An optional texture view is kept as an unresolved requirement: a handle alone does not identify the selected mip or
layer. In an eight-second TotK boot, all 135 submitted color clears had a target and all 135 used a view. In an
eight-second Catherine boot, all 1,104 submitted depth clears had a target and all 1,104 used a view. Neither run
lost a clear or render-target snapshot. The guest SDK still executes every clear. The next implementation step is to
validate subresource selection against the texture, then add an NVN texture-to-image binding and semantic clear
operation to the renderer. Only complete submissions may switch to HLE.
The view representation itself differs between the two inspected SDKs: TotK's `TextureViewSetDefaults` clears 16 bytes
and its setters pack selection and override flags into that object, while Catherine's clears 36 bytes and stores
separate flag and value fields. An HLE view decoder therefore needs a verified SDK layout or getter-based adapter;
interpreting a view pointer as one fixed structure would select the wrong subresource in one of these games.
The replay now fingerprints those two observed constructors, copies the matching view object at render-target binding,
and normalizes its level, layer, format, target, and override fields. An unknown constructor or unreadable view stays
unresolved. This is still a diagnostic snapshot: the host renderer has no NVN clear operation yet, and copied view
fields have not been checked against texture bounds or translated to a Vulkan image view.
In six-second boot runs, Catherine copied 4,845 views using the 36-byte layout with zero unreadable pointers; its
765 submitted depth clears all reached live texture records. TotK copied 1,293 views using the 16-byte layout with
zero unreadable pointers; its 87 submitted color clears all reached live texture records. Catherine's views in this
sample always override levels, layers, and target, and some override format. TotK's sampled views mostly override
levels and layers, with some format overrides. The existing Vulkan render-target path builds images from Maxwell
registers, so the HLE path needs an NVN texture-to-image binding path before these packets can be executed.
The texture observer now calls the SDK's pure `nvnTextureGetTextureAddress` getter after successful initialization,
alongside its storage-size getter, and retains the returned GPU address with texture dimensions and format. It checks
that address against a tracked pool base when one is known. A semantic target resolver combines the texture with a
copied view, validates pool generation and explicit subresource bounds, and keeps zero view counts as SDK-defined
sentinels for later renderer interpretation. In an eight-second Catherine replay, 202 texture addresses were known,
none disagreed with a known pool base, and 1,143 submitted depth clears resolved a target. In a twelve-second TotK
replay, 3,108 texture addresses were known with no pool disagreement; 414 color clears resolved targets. No guest
rendering was replaced in these runs. NVN format and tiling still need a direct image path in the renderer.

`SNX_NVN_CLEAR_EXECUTE=1` enables an opt-in execution probe. Submitted clear packets carry their selected NVN texture,
view, GPU address, and scalar clear arguments through queue replay. After the guest flushes, the first eight packets
per queue batch run on the GPU thread behind guest GPFIFO work. The renderer owns separate Vulkan images keyed by
texture and pool generation; it creates a 2D image and issues `vkCmdClearColorImage` or
`vkCmdClearDepthStencilImage` for supported full clears. The initial format set covers NVN RGBA8, R11G11B10F,
D16, D32F, and D24S8. Unsupported dimensions, masks, views, and formats return without modifying an image. These
images are intentionally separate from the guest's Maxwell surfaces and are not yet read back or presented, so the
probe exercises host command execution without replacing the displayed frame. The bounded image cache and per-batch
packet cap are probe limits, not a completed HLE resource manager. In an eight-second Catherine run, 1,137 clears
were issued with zero renderer rejections; in a fourteen-second TotK run, 281 were issued with zero rejections.
The next work is coherent NVN image storage and complete-list scheduling: a queue submission can switch only when
every command and synchronization effect in that submission has an HLE implementation.
Queue replay now conservatively preflights each flushed batch. It only labels a batch observed-clear-only when every
recorded list and nested call is present and non-opaque and contains only render-target bindings, clears, and command
memory setup. An unresolved clear or any other observed operation makes it mixed. This is a necessary condition for
replacement, not sufficient proof: the SDK may emit inlined operations beyond the intercepted APIs, and the renderer
still needs matching guest-memory and synchronization effects. In eight seconds of Catherine, all 375 flushed batches
were mixed. In fourteen seconds of TotK, 252 were mixed and one contained no clear. Neither boot offers a clear-only
batch to replace, so the next visual HLE milestone needs draw and state support inside mixed lists.

Replay now copies `BindVertexBuffers` ranges at the call boundary and carries vertex slots and the selected program
through nested command lists. It constructs bounded semantic packets for direct arrays, indexed, base-vertex, and
instanced draws, including index GPU addresses and a snapshot of the program's shader references. In a sixteen-second
TotK run, all 1,714 observed draws were indexed and instanced; all had a tracked program and shader references, and
1,702 had an explicit vertex binding. The remaining twelve may use vertex ID or state that the recorder has not
captured. The packet queue retains only eight samples per flush and discards them after counting. These packets do
not execute, and binding coverage does not establish renderability: vertex attribute layout, uniform and texture
bindings, shader stage semantics, image aliasing, indirect draws, and synchronization still need complete treatment
before a mixed batch can replace its guest pushbuffers.
In an eight-second Catherine run, all 4,007 observed draws were represented: 3,384 arrays and 623 indexed
base-vertex calls. All had a tracked program, shader references, and an explicit vertex binding. Neither measured
boot used the array form of `BindVertexBuffers`; the recorder still snapshots it so later scenes can be checked.
Uniform buffer bindings now follow the same ordered replay path for all six NVN shader stages. Single-buffer calls
retain their GPU address and size, while array calls copy the guest ranges before the SDK can reuse the pointer.
Every observed draw in subsequent Catherine and TotK boot runs had at least one uniform binding. That confirms the
packets preserve the binding calls seen in these runs; it does not establish that the GPU address contents are
coherent or that the shader's constant-buffer slots have been mapped to the host renderer.
Texture bindings now travel with the same packets as raw combined NVN handles, for direct calls and copied handle
arrays. In subsequent eight-second Catherine and sixteen-second TotK runs, every observed draw had a texture binding,
with no invalid stage or slot and no failed handle-array snapshots. The existing scalar descriptor check still runs at
bind time. This is a handle snapshot, not an NVN texture-to-Vulkan descriptor binding; aliasing, lifetime, and
sampler state remain part of that implementation.
`BindProgram` applies to a stage mask, so replay now keeps a separate program identity and shader-reference snapshot
for each stage. A later partial bind updates only its selected stages instead of erasing the others. Draw diagnostics
report vertex and fragment program coverage independently. The packet still needs a validated mapping from each NVN
shader record to the exact host shader stage before it can create a graphics pipeline.
The two inspected SDKs agree on vertex-state packing: an attribute uses one 32-bit word with an enable bit,
five-bit stream index, 14-bit byte offset, and ten-bit format; a stream uses two 32-bit words for stride and
divisor. Replay copies these small arrays before the caller can reuse them and normalizes their fields in each draw
packet. In an eight-second Catherine run, 8,205 enabled attributes across 3,927 draws all referenced a present
stream and bound vertex buffer. TotK exposed twelve indexed instanced draws whose saved attribute state references
an unbound stream. Their draw counts are nonzero, so they remain an unresolved binding or shader-use case rather
than evidence that the vertex path is complete.
An opt-in translation probe on eight of those TotK vertex programs found seven with no generic attribute reads and
one that reads attribute zero. The host draw path must preserve the guest GPU's value for an unbound attribute rather
than assuming every enabled attribute requires a bound vertex buffer. The shader probe previously stopped forever
after its first eight jobs because its in-flight counter was never released. It now releases each slot after a job
and skips code addresses already inspected, allowing later shaders to be checked without repeated compilation.
Draw packets now resolve their selected color and depth attachments through the same texture-and-view resolver used
by clear packets. They retain texture and pool generations, GPU addresses, formats, dimensions, and selected
subresources rather than a bare guest texture pointer. In an eight-second Catherine run, all 3,847 observed draws
had complete targets; in a sixteen-second TotK run, all 1,274 observed draws did too. These are semantic target
snapshots only. The host renderer still needs a coherent image allocation and binding path shared by clears, draws,
copies, and presentation.

`SNX_NVN_DRAW_PREPARE=1` now sends submitted draw packets to the GPU thread and asks the Vulkan renderer to prepare
exact guest vertex, uniform, and index-buffer snapshots through its strict buffer cache. A draw is rejected if a
bound range is empty, an index type is unsupported, a range exceeds 1 MiB, the total vertex and uniform data exceeds
4 MiB, or the guest range cannot be read. This probe does not issue Vulkan draws or replace guest GPFIFO execution.
The recorder uses a growable batch capped at 4,096 packets per queue flush; the former eight-packet diagnostic cap
missed most draws in both games. An eight-second Catherine run captured and flushed all 3,787 observed draws with no
staging overflow; 3,747 had finished preparation and none had been rejected when the exit report printed. A
sixteen-second TotK run captured and flushed all 1,582 observed draws with no staging overflow; 1,538 had finished
preparation and none had been rejected at report time. The prepared count can lag the flushed count because callbacks
run asynchronously. These results demonstrate buffer transport, not render correctness: texture descriptors, image
coherence, shader binding, and draw execution are still outstanding.
The same opt-in probe compares each resolved draw target with the renderer's existing guest-addressed surface cache.
All 3,727 completed Catherine draws in one eight-second run found matching surfaces. In TotK, initially only 465 of
4,090 completed draws matched because the probe lacked mappings for NVN R32F (`0x0a`), RGBA16F (`0x29`), and RGBA8
sRGB (`0x38`); those enum values are defined by the [NVN format header](https://code.botw.link/uking/uking/lib/NintendoSDK/include/nvn/nvn.h.html).
After adding the three Vulkan format mappings, a twenty-two-second TotK run prepared 3,298 draws with no rejected
buffers. Of those, 2,925 found matching existing surfaces, 223 first differed in depth target size, and 150 selected
a non-base texture layer. No target lacked a surface at its GPU address, and no format mismatch remained in that run.
This is a compatibility probe, not an image-coherence guarantee: the existing `Surf` cache represents one base layer
per image, while NVN targets may name other layers or a differently sized view of the same pool address. The probe
runs after the guest GPFIFO batch and can see a later active `Surf` at a reused address, so the size counts do not
establish that those draws lacked a matching surface when they executed. Direct HLE needs to queue semantic commands
in submission order and own image state at each command rather than reconstructing it afterward.
When both execution probes are enabled, resolved clear and draw packets now carry a queue-local sequence and enter
one GPU-thread batch in their original list order. This preserves the order of the commands the probes understand;
copies, barriers, dispatches, fence operations, and unsupported clears are still absent from the semantic stream.
Both packet arrays grow to 4,096 entries per flush. In a twenty-two-second TotK run, all 4,992 observed clear
packets and 3,166 draw packets were staged without overflow. Of the 4,934 clear callbacks completed by report time,
506 issued into the probe's separate images and 4,428 were rejected; 2,347 rejections were due to its fixed
16-image cache. Expanding that cache alone would not establish correctness because those images do not yet share
the guest renderer's content or lifetime rules.
Draw packets now snapshot the registered texture and sampler object identities and their pool and object generations
for every bound texture handle. Texture-view selections are copied when the guest registers them; the existing
16-byte and 36-byte view decoders are shared with render-target snapshots. Later descriptor-pool changes therefore
cannot silently retarget an already queued draw, and the view selection no longer depends on its guest pointer
remaining valid. In separate Catherine and TotK boot runs, all observed draws had complete texture, view, and
sampler identities (3,727/3,727 and 3,166/3,166 respectively); 199 and 3,882 registered views were copied with
zero failures. The Vulkan renderer still needs image-layout translation, sampler-state decoding, and resource
coherence before it can bind these identities for a host draw.
Sampler builders now record explicit setter arguments instead of assuming object byte offsets. The draw packets carry
known filter, wrap, LOD clamp and bias, compare, anisotropy, reduction, LOD snap, and border-color fields; fields not
set through an observed setter remain marked unknown. In separate Catherine and TotK runs, every observed draw had
explicit filter and wrap state (3,687/3,687 and 3,738/3,738). TotK called the LOD clamp, LOD bias, compare,
reduction, and border setters for 2,001 samplers, anisotropy for 131, and LOD snap for none. Its 2,001 border-color
pointers were copied without a read failure. Correctly translating the unobserved SDK defaults and custom border
colors remains necessary before creating Vulkan samplers for all draws.
The observer now calls the guest SDK's pure `nvnTextureGetStride` and `nvnTextureGetStorageClass` getters after a
successful texture initialization, alongside its existing storage-size and GPU-address getters. [The SDK API declares
both getters](https://code.botw.link/uking/uking/lib/NintendoSDK/include/nvn/nvn_FuncPtrInline.h.html), and using
their results avoids guessing version-specific object offsets. Catherine returned both values for all 202 textures
observed in one run; TotK did so for all 5,959. In that TotK run, 5,935 textures had zero stride and 24 had a
positive stride; six storage-class values appeared. A zero stride does not by itself specify block height, mip
offsets, or array-layer packing. Those address rules still need to be translated before sharing NVN images with the
Vulkan surface cache.

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
