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
shape and coverage of a future direct front end. Each list keeps at most 32,768 selected operations and each nested
call copies up to 4,096 handles. The summary reports truncated records, invalid guest arrays, allocation failures,
and storage evictions. A list whose `BeginRecording` was not observed is marked opaque, even when selected operations
on that command buffer were captured. Opaque therefore describes an incomplete lifecycle, not necessarily an empty
record. In an 80-second hooked Odyssey run, 1,868 frames replayed without a truncated list or dropped operation;
the old 16,384-operation cap had dropped about 3,300 operations. A 30-second Scarlet run replayed 760 frames with
zero truncated nested-list references after replacing the eight-handle snapshot.

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
Odyssey uses an older builder layout with storage, size, and flags at `+8`, `+16`, and `+24`; the newer layout uses
flags, storage, and size at `+4`, `+16`, and `+24`. The observer selects the older layout when the newer size field is
not page-sized and the older size field is. This restored Odyssey's descriptor backing range: a 15-second boot copied
39 TICs with zero missing bases or address mismatches. Catherine and TotK retained zero legacy-layout detections
and copied 199 and 5,773 TICs respectively in their validation boots.
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
Registration now also copies each 32-byte hardware TSC from the sampler pool's readable CPU backing, falling back to a
GPU mapping when one exists. The pool generation and bounds are checked before the copy. Unlike a TIC, a TSC has no
texture address to validate, so a nonzero copy is marked available but is not yet proof that every sampler field maps
correctly to the Vulkan decoder. Submitted draw packets carry the copied TSC alongside the explicit sampler state.
In a 120-frame Catherine run, all 1,526 replayed binds resolved through live sampler registrations, with zero missing
or stale entries; its three sampler objects and one sampler pool were tracked without table loss.
An 85-frame TotK run resolved all 10,965 submitted binds through live sampler registrations across 86 roots and 425
opaque secondary-list visits. It tracked 849 sampler initializations and 91 registrations in one pool, with no
missing or stale sampler result, untracked registration, or table loss.
In later draw-preparation boots, Catherine copied all four registered TSCs and carried one on all 3,897
texture-using draws. TotK copied all 119 registered TSCs and carried one on all 3,562 texture-using draws. Neither
run had a missing or all-zero sampler descriptor.
With `SNX_NVN_DRAW_PREPARE=1`, the GPU-thread preparation pass now feeds captured TSCs to the existing cached Vulkan
sampler constructor. It reports draws whose samplers were created or reused, missing descriptors, and Vulkan creation
failures. These samplers are host resources for future HLE draws; the guest GPU path still renders the frame.
The same pass checks captured TICs with the shared front-end parser used by the existing Vulkan texture loader. It
rejects unsupported formats, invalid dimensions or pitch, missing address, and device sample-image limits without
uploading texels or changing image contents. In later boots, all 3,707 Catherine and 3,078 TotK texture-using draws
that reached GPU-thread preparation passed this check. This does not establish that the source memory is coherent with
earlier GPU writes or that the image can be safely uploaded during draw setup.

`SNX_NVN_TEXTURE_LOAD=1` additionally imports each bound TIC through the existing Vulkan texture cache in the
GPU-thread callback, before a future HLE draw would allocate descriptor sets. The loader reconciles pending guest
memory writes and reuses matching render targets. Depth-only D16 and D32F textures now follow the same deswizzle and
upload path as color textures, using a [single depth aspect for Vulkan copies](https://docs.vulkan.org/spec/latest/chapters/copies.html).
An incomplete upload is rejected and retried when the texture is requested again.
When a recently drawn depth target occupies one subresource of a sampled texture, that subresource can be copied on
the GPU instead of read from stale guest memory. Packed depth/stencil images still use the loader's cleared fallback
and are reported as unavailable for HLE texture use. In separate eight-second Catherine and twenty-two-second TotK
boots, all 3,847 and 3,166 draw callbacks completed image loading without a missing or packed-depth result.
This is a shadow import after guest GPFIFO execution. It observes the state at the end of that guest batch, so it
does not prove the image contents that an earlier draw in the same batch would have sampled. Direct HLE still needs
ordered image ownership shared by clears, copies, draws, and presentation before guest submissions can be removed.

`SNX_NVN_COPY_EXECUTE=1` now snapshots `nvnCommandBufferCopyBufferToTexture` source addresses, destination texture
and view, [six-field copy region](https://codeviewer.zeldamods.org/uking/uking/lib/NintendoSDK/include/nvn/nvn.h.html),
flags, and the active row and image strides. Full 2D mip-level copies with tightly packed rows execute on the GPU
thread into the same NVN-owned image cache as clears. The copy path covers R8, RG8, RGBA8, BC1, BC2, and BC3,
plus the existing scalar and half-float formats. It checks source page mappings and bounds each
image allocation; old images are retired after GPU use when the 128-slot, 256 MiB cache fills. Clear, copy, and draw
packets run in their original replay order. The cache tracks written mip levels separately and has a base-level view
for a draw that requests only level zero. A draw may use a matching image written earlier in its batch only when all
levels in its selected view were written and no unmodeled image writer intervened; all other textures continue through
the guest texture cache. Selected non-base mip, 2D-array-layer, and cubemap-face clears also execute into shadow
images. An array or cubemap image is eligible as a semantic draw source only after every requested layer and mip has
been written in the current semantic batch without an intervening unmodeled writer. Images with more than 64 layers
can receive clears, but they are not eligible as semantic draw sources. The shadow cache maps
NVN R16F, RG16F, and RG16 to matching Vulkan formats. In a 22-second TotK boot, 4,693 clear callbacks issued,
including 616 array clears, 156 cubemap-face clears, and 875 non-base-mip clears. No completed clear callback was
unsupported, and no image-cache capacity failure occurred. These target and format values follow the
[reconstructed NVN header](https://codeviewer.zeldamods.org/uking/uking/lib/NintendoSDK/include/nvn/nvn.h.html).
Catherine's boot
recorded and executed 157/157 buffer-to-texture copies with no rejected copies. TotK recorded no such copies in its
boot run. Neither boot observed a draw sampling a semantic image in the same batch. A temporary lookup probe found
Catherine draws matching the same texture object after its shadow write's batch had ended. TotK sometimes matched the
same object, dimensions, and format within a batch, but an unmodeled write marker followed the last shadow write.
The recorder advances that marker after guest draws because their image writes are not represented in the shadow
stream. Keeping those draws as global invalidations is conservative; relaxing it requires modeling render-target and
storage-image writes and their aliases. The source bytes are still read after guest GPFIFO execution, so this shadow
path does not establish historical texture contents at each NVN command.

`SNX_NVN_ATTACHMENT_PREPARE=1` additionally materializes NVN-owned image views for the exact mip and layer selected
by each recorded draw target. It checks host color or depth attachment support and keeps views in the bounded image
cache. It does not mark those subresources written: the guest draw still owns their pixels. In opt-in boots, Catherine
prepared attachments for all 3,637 completed draws and TotK for all 2,506, with zero failures or image-cache capacity
loss. This establishes attachment allocation and subresource selection for the observed targets, not shader execution
or image coherence.

`SNX_NVN_SHADER_MATERIALIZE=1` also checks the recorded NVN program references on the GPU thread and requests cached
Vulkan shader modules from the existing Maxwell translator. It recognizes the NVN blob wrapper and vertex-A,
vertex-B, and fragment SPH types, combines vertex-A/B where both exist, and maps the recorded uniform-buffer ranges
while translating. It restores the Maxwell renderer's constant-buffer bindings afterward. It selects the vertex and
fragment stage slots, so older bindings in the other four slots do not introduce unrelated shaders. Unknown references,
duplicate stage candidates, unsupported instructions, and
module creation failures leave the draw on the guest path. The preflight keys vertex inputs and color outputs by the
integer formats in the recorded NVN attribute and target state. This is a preflight only: integer texture types,
clip-distance enables, pipeline state, descriptor sets, command recording, and image coherence are not yet supplied by
the NVN draw. Successful module creation does not mean a Vulkan pipeline can render the draw correctly.
In opt-in boots, both modules were available for all 3,827 prepared Catherine draws and all 2,858 prepared TotK
draws, with no shader-module or attachment failures. Neither title's guest draws were replaced.
The same option checks the eight [NVN vertex formats](https://codeviewer.zeldamods.org/uking/uking/lib/NintendoSDK/include/nvn/nvn.h.html)
observed in those boots against Vulkan vertex-buffer format support. Unknown or unsupported formats are reported
separately. In later boots, Catherine passed this format check for all 3,773 prepared draws and TotK for all 2,946,
with zero format failures. The shader-only option does not create a vertex pipeline layout; the pipeline preparation
option below checks bindings, offsets, and divisors.

`SNX_NVN_PIPELINE_PREPARE=1` enables the shader and attachment preflights, then creates a cached Vulkan graphics
pipeline from the observed vertex/fragment modules, primitive class, vertex formats, attribute offsets, stream
divisors, blend state, polygon mode, and color/depth attachment formats. The recorder copies the SDK's pointer-based
channel-mask, color, blend, polygon, depth/stencil, and multisample state at each bind before the caller can reuse the
object, then replays the values in submitted command-list order. The probe checks depth/stencil compatibility with
the attached target, rejects multisample and coverage modes that the single-sample image path cannot represent, and
rejects blend state that the Vulkan device would silently simplify without dual-source or independent blending.
Each used vertex attribute gets its recorded byte offset and a rebased binding; offsets and strides are checked
against device limits. In eight-second Catherine and twenty-two-second TotK boot samples, 3,967/3,967 and
3,946/3,958 sampled pipelines were ready, respectively. TotK's twelve rejections were draws with no vertex buffer
bound for an attribute used by the vertex shader. Counts vary with run timing and scene progression.
This remains an opt-in preparation probe. It does not bind texture descriptors, issue NVN draws, synchronize NVN
images with guest Maxwell surfaces, or replace a whole guest command-list submission. Integer texture typing and
clip-distance enables also need coverage before those pipelines can reproduce every shader's behavior. The guest GPU
path still produces the displayed frames.

`SNX_NVN_PROGRAM=1` records program initialization, finalization, and successful shader setup through either
`nvnProgramSetShaders` or `nvnProgramSetShadersExt`. Descriptor replay enables this observer and checks each
`nvnCommandBufferBindProgram` in submitted lists against a live, configured program. The check uses the exported
program pointer and setup return value. It snapshots up to eight shader references before the guest SDK can reuse the
input array. The plain `SetShaders` path passes 16-byte code/control pairs to shared setup; `SetShadersExt` copies
24-byte entries into those pairs plus a separate array of optional third words. The observer preserves that distinction.
Reading the plain pairs as 24-byte entries previously shifted TotK's second shader reference, making CPU control data
appear to be an unrecognized code address. These snapshots record references, not copies of shader code.
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
Catherine again compiled eight of eight sampled references. An earlier TotK sample compiled four of eight because
the observer used the wrong stride for plain `SetShaders` entries; that capture error is corrected above. This is a
shader-translation preflight, while command recording, resource binding, synchronization, and presentation still run
through the guest SDK.

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
The replay fingerprints the observed constructors, copies the matching view object at render-target binding,
and normalizes its level, layer, format, target, and override fields. An unknown constructor or unreadable view stays
unresolved. This is still a diagnostic snapshot: the host renderer has no NVN clear operation yet, and copied view
fields have not been checked against texture bounds or translated to a Vulkan image view.
Odyssey has another 36-byte constructor sequence: it clears bytes 0–31 with paired stores and byte 32 with a word
store. Its target setter writes at `+32`, matching the 36-byte field decoder. Recognizing this constructor copied
all 10 descriptor views in a 15-second boot, with zero view snapshot failures.
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
buffer-to-texture copies now share the ordered stream, while barriers, dispatches, fence operations, and unsupported
clears remain outside it.
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

Draw packets also carry the ordered viewport, scissor, and depth-range arrays, blend constants, front and back
stencil masks and references, and raster controls observed through NVN setters. Pointer arguments are copied at the
guest call boundary; scalar floating-point arguments are captured from the guest floating-point registers. In an
eight-second Catherine run, all 3,807 replayed draws had viewport, scissor, depth range, blend color, and both-face
stencil state. A twenty-two-second TotK run covered all 3,870 draws for viewport, scissor, depth range, and blend
color; 3,858 had complete stencil state. In a later run, Catherine had polygon offset, depth bounds, rasterizer
discard, and depth clamp on all 3,887 draws, while TotK had line width and polygon offset on all 3,738 draws and
depth bounds and discard on 3,726. Neither boot called the sample-mask setter. These are draw-state snapshots, not
Vulkan dynamic-state commands. The future host path must apply SDK defaults when a setter was not observed and reject
or translate unsupported state before recording a host draw.

Texture-image preparation now checks the queued descriptor's texture and backing-pool generations again on the GPU
thread before using either a semantic image or the copied TIC. In separate Catherine and TotK boot runs with the
texture-load probe enabled, 3,759 and 2,011 draws prepared texture images, while 8 and 6 were rejected as stale;
TotK reused a semantic image for 11 draws. The probe still does not bind these images for presentation. A longer
direct-call-hooked Super Mario Odyssey run reached 1,849 frames and replayed 116,195 draws, but none had a complete
sampler descriptor or a readable texture-header base. Its observer reported 131 untracked sampler registrations out
of 132 and all 3,724 texture-header copies missing a base. That title needs descriptor-pool and sampler-lifetime
coverage before the texture-image path can be evaluated there. All 116,195 replayed draws also lacked a captured
render target, so host draw execution remains unavailable in this trace.

Odyssey also exceeded the original 16,384-entry program registry. Increasing its bounded table to 65,536 retained
all 26,344 initialized programs in an eighty-second run: 124,536 program binds resolved, with no untracked shader
sets or full-table events. The remaining shader-materialization failure was a zero size field in Odyssey's NVN blob
wrapper, despite its valid magic and Maxwell SPH at the expected offset. The loader now accepts that wrapper only
when the SPH identifies a supported shader stage. In a 35-second run, all 2,613 prepared Odyssey draws then had
ready shader modules. Sampler descriptors and draw targets remain separate blockers for the title.
In a subsequent eighty-second rendered-scene run, 142,369/142,369 prepared Odyssey draws had ready shader modules;
95,982 textured draws fit the observed texture-slot layout and 46,387 had no texture instructions. None of the
textured draws had a usable sampler or TIC snapshot, and all 142,491 replayed draws still lacked a captured target.
The descriptor observer reported 3,940 texture registrations with no readable TIC base, 132 sampler registrations
without a readable TSC, and 70,495 stale texture resolutions. Those object-lifetime and pool-layout gaps, plus
render-target capture, now dominate Odyssey's direct-draw path.

An optimized direct-call-hooked Scarlet module reached 1,150 frames in 45 seconds, with 29,653 draws and complete
render-target snapshots, but no draw passed the all-bound-descriptors preflight. A shorter run classified all 42,242
missing texture registrations as handle zero; none used a nonzero handle. The recorder still treats those calls as
bound slots. Resource-use analysis must establish whether each zero handle is an unused slot or a shader-visible null
descriptor before host draw preparation can skip it or supply a defined replacement.

In a 15-second hooked Scarlet run with shader materialization enabled, 7,084 draws had ready shader modules and 612
failed because their fragment shaders first hit `SUST` or `SURED`. The translator's image-store path currently supports
compute only, and the graphics descriptor path has no storage-image bindings for these writes or reductions.
Fragment storage-image resource translation and ordering are required before those modules can participate in host
draws.

The shader translator now gates texture and sampler preparation on actual resource use. If both ready graphics
shaders contain no texture instructions, preflight marks their texture state unused and avoids loading descriptors
that cannot affect the draw. This covered 634 of 7,982 prepared Scarlet draws in one fifteen-second run; the other
7,348 still lacked a texture header. In Catherine and TotK boot runs, the same check skipped unused texture state
for 881 and 110 draws while the remaining 2,906 and 2,544 retained ready texture headers. Of Scarlet's 6,714
ready textured draws with a zero-valued bound handle, none used exclusively bindless texture accesses. The existing
GPFIFO renderer derives non-bindless handles from a texture constant-buffer selector and shader offsets, so the
captured `BindTexture(0)` calls alone cannot establish the images those shaders read. The HLE path must obtain that
resource mapping before replacing their emulated draws.
An instrumented Scarlet boot showed why reading the NVN uniform bindings is insufficient: the emulated fragment
renderer selected texture CBUF index 2 at guest address `0x564637800` and read nonzero handles such as `0x1040011f`,
while the first four captured NVN fragment uniform slots were at `0x5157b5300`, `0x5157b4000`, `0x51576b100`, and
`0x5157ac000`. Reading slot 2 at the same shader offsets yielded unrelated values. These samples are from the two
asynchronous paths, so they do not prove exact draw correspondence, but they disprove the proposed direct mapping
between NVN uniform slot number and Maxwell texture-CBUF index in this trace. The SDK's internal texture-handle
CBUF must be reconstructed from its state or captured from the generated command stream with submission ordering.
An instrumented Catherine boot showed the same separation: the emulated fragment renderer used texture selector 2
at `0x5ec637800`, while the first recorded NVN fragment draw had only uniform slot 0 bound at `0x50cf92400`.
The exact addresses vary between runs; their role and the missing slot 2 are the relevant findings.
That internal CBUF nevertheless has a usable binding layout: Catherine's fragment shader read words 8 and 10 as
texture handles, matching the low 32 bits of recorded NVN binding slots 0 and 1. The resource preflight now maps
translated non-bindless texture accesses at word `8 + 2*slot` to their captured NVN texture and sampler descriptors.
It checks only shader-visible slots when every access fits that layout; bindless or unfamiliar offsets retain the
conservative all-bound-slot check. In a 15-second Scarlet run, 6,438 ready textured draws fit the layout and 5,827
texture images prepared successfully, versus none under the old all-bound-slots check. Catherine and TotK boot runs
prepared 3,048 and 2,468 texture images respectively, with only two and one stale image snapshots. This validates
the preflight's resource selection in those scenes; it does not yet bind descriptors for NVN host draws or prove
that all future shaders use the same internal layout.
The same distinction applies beyond textures. In a textureless Scarlet draw trace, the Maxwell fragment stage had
CBUF banks 0, 1, 2, and 3 at `0x564630000`, `0x4011e0600`, `0x564637800`, and `0x5157b5300`; the captured NVN
fragment uniform slot 0 was `0x5157b5300`. Thus one application uniform slot matched GPU bank 3, while banks 0–2
were populated internally. Even textureless shaders in that scene read CBUF banks 0 and 1 or 3. Host descriptor
binding therefore needs the SDK's internal constant-buffer contents and bank assignment; binding NVN uniform slots
directly to identically numbered shader banks would be wrong.
An additional Scarlet draw probe observed application fragment slots 0 and 1 at the same addresses as Maxwell banks
3 and 4 while banks 0–2 held different SDK-generated data. NVN shader materialization now places application slots
at bank `slot + 3` and leaves the first three banks unbound rather than supplying unrelated application data. This
corrects the known application-buffer mapping for shader translation, but the generated bank contents still need to
be reconstructed before a direct draw can bind complete descriptor sets.
The texture and image handle portion of generated CBUF 2 can be reconstructed from recorded bindings: low texture
handle words are at `8 + 2*slot`, and low image handle words at `72 + 2*slot`. A diagnostic compared bound texture
slots of each batch's last recorded draw with the Maxwell bank visible after the SDK submission. Catherine matched
1,000/1,000 sampled entries and Scarlet matched 16,000/16,000; TotK matched about 82%, with mismatches concentrated
in fragment state after secondary-list execution, so that end-state comparison is not draw-by-draw proof for TotK.
Shader materialization now supplies only those known low words from the semantic NVN bindings to its CBUF reader;
unknown words remain unavailable. This makes the shader translation input independent of the emulated GPU's later
CBUF 2 state. A host draw still needs an uploaded buffer containing the full generated bank and descriptor bindings.
The first Catherine boot draws had vertex/fragment CBUF masks `0/8`, `0/a`, or `8/0`: some need only application
bank 3, while one fragment case also reads the first 16 bytes of generated bank 1. An earlier sample of that prefix
was zero, but a targeted Maxwell draw to Catherine's intermediate `0x5acf62e00` target read nonzero bank-1 word 0
(`0x3f40a3d7`), and another target read three nonzero words. Neither the captured alpha reference, blend color,
viewport, nor application uniform slot 0 matched that word. Bank 1 is embedded after the selected NVN shader's
code blob: its header size `0x190` rounds to offset `0x200`, and size `0x210` rounds to offset `0x300`. Both derived
GPU addresses match the corresponding Maxwell bank-1 bindings and their contents. The same layout is described by
[deko3d's shader initialization](https://github.com/devkitPro/deko3d/blob/master/source/dk_shader.cpp), which
places compiler-generated CBUF 1 in the code section. Shader materialization now reads this embedded bank, and a
direct draw stages the bytes its shader uses. Blobs with a zero size field, seen in Odyssey, still need their data
offset established independently.
`SNX_NVN_SHADOW_DRAW=1` now enables an opt-in executable slice for Catherine's observed fullscreen triangle-strip
draws. It prepares the required resources, binds the shader's application and embedded uniforms and captured vertex
streams, and records the translated
shaders' Vulkan draw into NVN-owned color/depth images in command-list order. The first draw clears its isolated color
target to magenta and reads it back: with the draw disabled for a diagnostic run, all 921,600 pixels stayed magenta;
with the draw enabled, all became black. An eight-second run issued more than 300 shadow draws and completed normally.
The SDK's GPFIFO still supplies the displayed frame; this path does not replace command submission, synchronize its
image writes with guest memory, or establish visual parity. It deliberately accepts only verified application-bank,
single-color triangle-strip or narrow indexed triangle-list state until more generated buffers and dynamic state
are modeled.
The shadow descriptor setup now binds captured application uniform slots to shader banks 3–17 and resolves every
sampled texture used by either vertex or fragment shaders. It decodes each shader's generated handle slot, binds
the captured TIC/TSC, and uses the existing texture view and sampler rules. It accepts guest-memory-backed textures
and independently complete NVN-owned images; sampling a Maxwell render target would make the shadow result depend
on the emulated GPU. The supported draw shape and dynamic state remain narrow; generated banks 0 and 2, storage
buffers, and images remain unsupported. The executable path now binds every pipeline-key vertex attribute to its
captured stream, rather than requiring only attribute 0 on stream 0; it rejects unbound constant-attribute fallbacks.
Indexed triangle draws can use either 16- or 32-bit indices and preserve the captured base vertex and instance.
The texture binder can now select a prior NVN-owned RGBA8 image when the captured TIC has an identity swizzle,
the image's required subresources were fully initialized by modeled operations after the last unknown write, and
the draw does not sample its own attachment. The first magenta diagnostic draw can also establish full coverage
when its readback shows that every pixel changed; later draws preserve provenance only after such initialization.
The command recorder marks guest draws and clears as unknown while GPFIFO remains authoritative. The shadow path
bridges synthetic markers for successfully executed full clears and draws in order; an unmodeled, failed, or
unsupported operation breaks that chain. This enables ordered host-image read-after-write without treating an
unmodeled operation as a valid producer. Catherine's first
batch now executes a three-draw chain: a fullscreen strip certifies all 921,600 color pixels written, a 12-index
triangle-list draw updates that same image, and a later strip samples the NVN-owned image into a target that VI
presents. The shadow path rejects earlier textured draws whose inputs only have Maxwell render-target contents.
Immediately before the modeled chain, Maxwell makes an unblended four-vertex pass to that intermediate target;
the NVN fragment shader uses embedded CBUF 1 and three sampled textures. The bank-1 mapping removes its uniform
blocker, but an earlier probe issued zero bank-1 shadow draws because the first fragment texture lacked an NVN-owned
producer. The sampled texture is render target `0x5acba2c00`; Maxwell clears it to black each frame without drawing
to it. The producer is `nvnCommandBufferClearTexture`, which the recorder had classified as a clear without
snapshotting its texture, view, region, or color. The shadow path now snapshots that command and executes full
subresource clears against an NVN-owned image. Rectangular color clears now use an attachment clear and preserve
full-image provenance only if the untouched pixels were already initialized by modeled operations. In an
eight-second Catherine run, all 766 texture clear targets resolved, 1,900
of 1,915 staged clears executed, and the shadow path issued 1,300 draws, including 364 that used embedded bank 1.
The sampled Catherine, TotK, Scarlet, and Odyssey boots did not issue a rectangular texture clear, so that branch
still needs a title-level visual check when such a command is observed.
These counts establish direct draw execution, not visual parity or guest-memory coherence. The Maxwell surface's
write generation advanced from 2 to 9 over the first eight presented frames while its last guest-memory generation
remained 1, so importing those pixels would reintroduce a dependency on GPU emulation.
Maxwell then uses two blend configurations on the target, including an indexed pass with a different source-alpha
factor. Its sampled inputs still need ordered provenance checks, and diagnostic magenta initialization limits what
the black-frame comparison can establish.
With `SNX_NVN_SHADOW_DRAW=1 SNX_NVN_SHADOW_COMPARE=1`, the first eligible NVN color target is also read back
alongside the Maxwell render target at the next present. Catherine's first 1280x720 comparison found identical RGB
in all 921,600 pixels but different alpha in all of them: the shadow target was `(0,0,0,0)` and the Maxwell target
was `(0,0,0,255)` at the center. This target is an intermediate image, not the VI-presented framebuffer.
The three-draw chain's first presented-target comparison also had identical RGB and a full-image alpha mismatch
(`0` against `255`). One later boot run matched all 921,600 RGBA pixels on that black frame, but a repeat with the
same bank-1 mapping again showed the full alpha mismatch; no bank-1 shadow draw executed in that repeat. The
comparison samples after the guest submission, so later Maxwell operations may affect its contents. Neither result
proves gameplay parity. Texture-clear replay now certifies the sampled producer in later batches too, but the alpha
mismatch and other unmodeled rendering behavior remain to be resolved.
The next draw-eligibility limits differ by title. In an eight-second TotK sample, 831 pipelines prepared, but early
draws used storage buffers, multiple color targets, instancing, and non-Catherine render state. Scarlet prepared
3,669 pipelines in a ten-second sample, while its early fullscreen triangle passes used generated CBUF 0 and
formats or state outside the current executable slice. Odyssey prepared 2,501 pipelines in twelve seconds, but its
early indexed passes used an sRGB target and different polygon state. These are executor and generated-resource
gaps, not shader-module compilation failures.
The Odyssey executor now admits its observed indexed sRGB pass and programs the same flipped viewport, face winding,
and back-face culling as the corresponding Maxwell draw. The first pass samples an RGB10A2 texture at `0x53c910000`.
NVN clears that address before the draw, but the clear's texture object and the sampler's texture object differ while
sharing the same memory pool address, format, and shape. Host images now follow the compatible backing allocation
instead of requiring identical object pointers, and semantic sampling accepts matching formats beyond RGBA8. NVN
`UpdateUniformBuffer` no longer invalidates unrelated image provenance. A nine-second opt-in Odyssey run issued at
least eight direct draws, each sampling the NVN-owned RGB10A2 image. The first diagnostic draw changed 921,600 of
1,440,000 target pixels,
so it did not certify full target coverage. This is an isolated shadow pass, not a complete or presented frame.
Odyssey's following indexed passes blend into an RGB10A2 target. Their polygon-state low bits specify fill, CCW
front face, and no culling; higher bits varied across six observed values while corresponding Maxwell draws kept
depth bias disabled. With zero captured polygon offset, depth clamp, and rasterizer discard, the executor now uses
the prepared blend pipeline for this state family. A ten-second boot issued more than 800 shadow draws and retained
full-image provenance after 165 of them. It still needs a pixel comparison on the actual presented image.
Scarlet's first fullscreen vertex shaders read generated CBUF 0 only at offsets `0x640` and `0x644`, the
base-vertex and base-instance words. An instrumented Maxwell boot found both words zero in those draws. The
translator now identifies shaders whose bank-0 reads are limited to those words, and the direct executor binds a
zeroed generated bank only when the recorded draw also has zero first vertex, base vertex, and base instance. Other
bank-0 users remain ineligible. The executor also accepts observed non-indexed triangle draws and color-only fill
state. In an eight-second Scarlet boot this passed the executor's initial state gate for repeated sRGB passes, but
issued no shadow draw; descriptor, sampled-image provenance, and later resource checks still need investigation. Catherine
continued to issue 1,300 shadow draws in its eight-second check; TotK and Scarlet booted without direct draws.
Odyssey remained at zero frames even in a twelve-second control run with shadow drawing disabled, before any NVN
draw was recorded, so that boot did not test the new executor behavior.
A focused Scarlet probe identified the sampled-image blocker: repeated sRGB fullscreen draws request render targets
at `0x55bb70000` and `0x55c3e0000`. Maxwell owns their current contents, so the shadow executor correctly rejects
them rather than importing a dependent image. The direct draw format gate now admits the already mapped NVN `0x3f`
R11G11B10F target used by earlier passes, but an eight-second boot still issued no direct draw. Replaying its producer
chain requires storage-image descriptors and write ordering in addition to this target format.
The first Scarlet producer uses a color-only R11G11B10F target and a separate R32_UINT storage image at
`0x54461c000`. Its fragment shader reads image handle word 74 from generated CBUF 2, then stores or atomically
updates that image. Shader translation now records static direct bank-2 reads and rejects dynamic or unknown words;
the executor stages verified low texture and image handles from the NVN draw bindings. It binds single-layer
R32_UINT storage views, orders prior writes before fragment storage access, and retains the storage image's layout
and write state for later draws. The observed producer also enables depth clamp, now represented in the Vulkan
pipeline key. No draw yet certified a complete color target, so later sRGB passes still cannot use their outputs as
NVN-owned sampled images. The first
storage-writing shaders have no color output, so their draws preserve existing color provenance instead of
invalidating it. A later eight-second boot issued at least 1,100 shadow draws, including 212 storage-image draws.
The first color-writing pass changed 921,600 of 2,073,600 pixels in its sRGB target, leaving full-image provenance
unproven. The sentinel readback now compares packed pixels in RGBA8, sRGB RGBA8, and R11G11B10F correctly. Storage
image writes likewise remain incomplete until their coverage or later consumers are established.
Tracing the remaining Scarlet image chain found that the R11G11B10F targets at `0x55bb70000` and `0x55c3e0000`
both sample `0x558050000`. That earlier 1920×1080 pass binds depth and 16 textures. Its observed NVN depth state
`0x61` agrees with the corresponding Maxwell registers: depth test enabled, depth writes disabled, compare function
6. The direct draw path now programs the captured depth-test, write, and compare settings and requires an NVN-owned
complete depth image before using a depth test. That pass passes the state gate, but its first texture either
does not resolve through the semantic TIC decoder or points to another Maxwell-owned render target at
`0x5570d0000`. The next dependency is
therefore upstream texture production and descriptor handling, not the depth-state encoding.
With all current pipeline probes enabled, an eight-second Catherine boot prepared 3,787 draw pipelines and
attachments with zero failures; 2,907 draws retained ready texture headers and 880 used no textures in their ready
shaders. This establishes that shader translation and pipeline creation cover that boot sequence. Most draws still
need broader descriptor population, generated constant and storage buffers, vertex bindings, and ordered image state
before the NVN path can execute them.
The TotK pipeline probe exposed two missing vertex-input format mappings: NVN `0x11` (RG16F) and `0x12` (RG16
UNORM). With both mapped to their Vulkan equivalents, a twenty-two-second boot reported 2,688/2,688 vertex-format
preflights ready, compared with 2,220/2,380 before the additions. Pipeline creation in the later run succeeded for
1,983 draws and failed for 705, chiefly where a shader input had no captured vertex stream or depth testing was
enabled without a captured depth target. These counts vary with boot timing and are not a before/after rendering
comparison; the GPFIFO renderer still produces the displayed frames.
The [Vulkan fragment-operations specification](https://docs.vulkan.org/spec/latest/chapters/fragops.html) says that
depth and stencil tests are skipped if their attachments are absent. The preflight now accepts such color-only
pipelines even if the guest left those tests enabled. In a following TotK boot, 2,300 of 2,543 draw pipelines were
ready; the remaining diagnostic samples were missing vertex streams for shader-used attributes. That path still
needs the correct constant-attribute or unbound-stream semantics before it can issue host draws.
The existing Maxwell renderer supplies a shared `(0,0,0,1)` constant for attributes with no readable stream. NVN
pipeline preflight now uses the same constant-attribute binding and numeric type for that case. A later TotK boot
created 2,588 of 2,727 pipelines; the 139 remaining failures lacked captured multisample state. Those draws stay
conservative because the missing state may affect sample coverage even when the current attachment has one sample.
The future NVN draw command must bind the constant buffer described by the pipeline layout.
Scarlet also uses NVN RG16 signed-normalized (`0x13`) render targets. The shadow-image format table now maps that
format to Vulkan RG16 SNORM. A twelve-second Scarlet clear probe no longer reported format `0x13` as unsupported;
648 clear packets still failed because their target shapes were outside the current 2D/array/cubemap shadow-image
model, including 3D textures and 2D targets whose captured depth was greater than one. The format mapping alone
does not make those clear packets executable.
The target packet now preserves both the texture's storage target and its selected view target. Scarlet's `target=1`
case is a 2D view of a two-layer backing array (`storage_target=4`), so allocating it as a one-layer 2D image would
lose the second layer and break aliasing. The probe now keys and allocates the backing array while selecting the
view's layer for attachment and clear operations. Scarlet's observed view also sets swizzle and depth-mode flags
whose effects are not decoded, so those clears remain rejected. Separate Catherine and TotK boots retained ready
attachments for all 3,818 and 2,655 observed draws after this change.
The target packet also retains its backing format separately from any view format override. The shadow cache now
rejects a format reinterpretation until it can create one mutable-format image with compatible views, rather than
silently creating unrelated images for the same texture object. Catherine, TotK, and Scarlet boot probes did not hit
that guard in their observed clear or attachment packets.
Odyssey's older texture builder places its memory-pool pointer and storage offset at `+8` and `+16`, eight bytes
before the newer layout. The observer selects it only when that pointer names a tracked pool and the newer pool
field does not. Before this correction, all 18,669 textures in one long Odyssey trace appeared to have no pool or
an untracked pool, so every replayed clear target was stale. A short corrected boot associated all 42 observed
textures with tracked pools and found no out-of-range offsets. A later long scene associated all 19,061 initialized
textures with tracked pools and reached 2,060 complete draw-target snapshots, but all 8,022 replayed clears still
found stale target objects. That remaining failure is a texture-object lifetime or replay-ordering problem, not the
builder's pool pointer.
Odyssey also copies initialized sampler objects before registration. The copied pointer has no observed initialize
generation, but the registered descriptor's 32-byte TSC is still readable. The descriptor observer now accepts
that TSC under the sampler-pool generation without claiming to know the copied object's builder state. In one
long scene this made all 140,921 draw sampler descriptors and TSCs complete, while filter and wrap state remained
unknown to the NVN observer. The existing Maxwell sampler path can decode TSC bytes directly; NVN draw submission
still needs to bind a host sampler.
Vertex-format preflight now checks only attributes referenced by the translated vertex shader. It also maps NVN
R8 UINT (`0x03`), RG8 UINT (`0x0f`), RGBA8 UINT (`0x27`), and RGBA8 SNORM (`0x26`) to their Vulkan vertex formats.
The [NVN format enum](https://code.botw.link/uking/uking/lib/NintendoSDK/include/nvn/nvn.h.html) identifies these
numeric values. An Odyssey long scene before the last mapping had 125,651 vertex-format-ready draws and 15,148
failures; the next run with RGBA8 SNORM mapped reported 138,407/138,407 ready. These checks still do not validate
actual host vertex-buffer bindings or output pixels.
Odyssey's render-target calls also use copies of initialized texture objects. Their old-layout fields can be
matched exactly against a recorded initialization by pool generation, offset, flags, target, dimensions, mip count,
format, and sample count. The observer caches only unambiguous matches and captures the resolved target at
`SetRenderTargets`, while the object is still available; replay checks that the backing pool generation remains
live. In one long scene this recovered 1,982 color and 3,187 depth clears and 75,539 complete draw-target snapshots,
where the earlier replay had zero resolved clears and only 2,060 complete targets. It does not solve guest-memory
coherence or make mixed draw submissions executable.
Odyssey's dominant captured color target is NVN RGB10A2 (`0x3d`). Mapping it to the renderer's matching
`VK_FORMAT_A2B10G10R10_UNORM_PACK32` removed 37,916 of 37,920 target-format rejections in comparable long scenes;
four remained where the emulated surface had a different format. The remaining target preflight also found 13 size
and 160 view-layer mismatches. These counts identify work needed before a direct draw can replace the emulated one.
When Odyssey omits an observed multisample-state call, the pipeline probe now uses the SDK's zero-initialized
single-sample state only if a captured color or depth attachment exists and every captured attachment has at most
one sample. In a 45-second scene this changed pipeline readiness from 0/3,677 to 4,868/5,613; every pipeline
failure in the latter scene coincided with an attachment failure.
Those attachment failures were sRGB views (`0x38`) of RGBA8 (`0x25`) storage. The shadow cache now creates one
[mutable-format Vulkan image](https://docs.vulkan.org/spec/latest/chapters/resources.html) for this compatible pair,
with an explicit two-format list and format-aware sampled and attachment views. Other format reinterpretations stay
rejected. A repeat Odyssey scene prepared all 5,469 observed attachments and pipelines. Catherine and TotK retained
zero attachment or pipeline failures in their boot probes; Scarlet still had unrelated target-shape and pipeline
failures. Clearing through an alternate sRGB view remains rejected because `vkCmdClearColorImage` operates on the
image's backing format; a correct view-based clear needs a rendering operation.
Odyssey also registers copied texture objects after the original SDK wrapper has been finalized. The descriptor
observer now resolves those copies against initialized texture metadata, verifies their encoded TIC GPU address,
and retains the backing and descriptor-pool generations. The texture loader validates those pools and TIC/GMMU
mapping rather than requiring the short-lived wrapper to survive replay. In a long scene, 1,191 formerly untracked
registrations resolved; 82,544 textured draws prepared with no missing headers or stale bindings. Catherine and
TotK retained complete texture-descriptor coverage in boot probes. Scarlet still has separate missing descriptor
IDs and unsupported texture formats. These checks do not establish descriptor writes or host draw execution.
Scarlet also uses NVN RGBA32F (`0x2e`) render targets. Mapping them to the renderer's existing
`VK_FORMAT_R32G32B32A32_SFLOAT` support removed 306 target-format rejections in a fifteen-second scene;
the remaining target failures were size mismatches, and 624 attachments still failed.
Those attachment failures were 3D render targets. The shadow cache now allocates a Vulkan 3D image with
`VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT`, retains a 3D sampled view, and creates 2D slice attachment views.
Its memory budget accounts for shrinking mip depths, and the selected slice is checked against the depth of its
mip. In a repeat Scarlet scene, attachment readiness rose from 7,227/7,851 to 7,955/7,955 and pipeline readiness
rose from 6,603 to 7,322; 634 pipeline probes still failed for other reasons. Catherine and TotK boot probes
retained zero attachment or pipeline failures. This is host image/view preparation, not a rendered NVN draw.
The command-list recorder now preserves NVN `BindImage` and `BindImages` state alongside sampled-texture state,
including handle-array snapshots at recording time and descriptor snapshots at draw replay. The texture-pool
observer also accepts `RegisterImage`, which writes a hardware image descriptor into the same pool. In one
fifteen-second Scarlet probe, 7,810 draws with a bound image had complete descriptor snapshots, where none resolved
before observing image registrations. Catherine, TotK, and Odyssey boot probes did not bind images and retained
their previous texture and pipeline results. A bound image is not necessarily accessed by its shader; image
format, storage usage, shader operations, and write ordering still need validation before a host draw can use it.
Scarlet's two fragment image shaders use `SUST` and the U32 2D maximum form of `SURED`. The translator now emits
fragment storage-image stores and that verified reduction as `imageAtomicMax`; other `SURED` encodings remain
unsupported. The [Ryujinx decoder](https://git.axenov.dev/Museum/ryujinx/src/commit/888402ecaf76c0ead448baaf52abbb3d48bb4ae9/src/Ryujinx.Graphics.Shader/Decoders/InstDecoders.cs)
identifies the instruction fields, and [NVIDIA's PTX ISA](https://docs.nvidia.com/cuda/archive/12.8.0/parallel-thread-execution/index.html)
describes surface reduction semantics. The observed image handle is bound at fragment slot 1, and the translated
shader reads its handle from CBUF 2 word 74. Its TIC is R32_UINT, so NVN shader materialization now keys image
store declarations on the captured TIC's integer type. A fifteen-second Scarlet probe prepared all 7,332 observed
shader modules and 7,330 pipelines, with two unrelated pipeline misses. The Maxwell GPFIFO renderer still skips
graphics shaders with storage-image writes until it can bind and synchronize those images; the direct NVN probe
still does not issue draws. Catherine, TotK, and Odyssey scene probes retained their previous shader and texture
coverage.
The recorder also retains the registered image's texture target and view for each bound draw. A Vulkan preflight
now checks the shader-visible image slot against its TIC format and allocates the matching single-layer R32_UINT
storage view, without issuing a draw or changing image layout. In a fifteen-second Scarlet probe, all 7,848
image-bound draws retained complete descriptor and target snapshots, and 624 draws whose fragment shader actually
uses an image prepared the storage view. The remaining bound-image draws do not necessarily access that binding.
Host descriptor writes, storage-image layout transitions, write ordering, and draw submission remain required.

In a TotK boot comparison, the first 640×360 RGBA16F shadow target differed from the Maxwell target in exactly
its top 36 rows (23,040 pixels). The direct NVN path staged all 31 recorded draws for that target, but the Maxwell
renderer had issued three earlier draws on the same target with one vertex attribute and index counts 1,656, 3,384,
and 3,384. All three used GPU channel 20, the same channel as the subsequent recorded draw, and occupied one GPFIFO
entry immediately before the first recorded draw's entry. None appeared among those 31 recorded NVN draws.
At the first transition to this workload, TotK reused the same outer submitted list, expanding it from five to
eight secondary-list calls. The outer list had no draws. The first two new secondary lists were opaque because their
begins were inlined. The first had about 9,900 directly captured operations but no direct draw calls; the second
had 39 directly captured draws. Both also held nested `CallCommands` references, so direct operation counts alone
could not assign the missing draws to either list.

A byte comparison resolved that ambiguity. The entire 979-word GPFIFO entry containing the three earlier draws
matched a contiguous span of the NVN command pool supplied to the **second** new secondary list, the one with 39
recorded draws. In one run, the entry matched pool offset `0xc110d4`, within that list's `AddCommandMemory` range
`0xc01e98..0x1000000`. In another run the ring-buffer offsets changed, but the same entry matched at `0x12134`,
within the corresponding list's `0x2ef8..0x400000` range. The renderer flushed the three pending draws while
processing words 489, 814, and 972 of that entry; these are flush positions, not necessarily the encoded draw
packets, because Maxwell MME macros can issue the draw before a later state change flushes it. The preceding list's
zero direct-draw count was a misleading clue: the missing commands are encoded in the same secondary list as later
observed NVN draws.

TotK's command-buffer cursor at object offset zero points into the supplied command pool. Between exported calls,
it exposes 48-byte gaps before recorded operations 103, 128, and 153. Each gap ends in a complete indexed-draw MME
packet with counts 1,656, 3,384, and 3,384 respectively. `SNX_NVN_INLINED_DRAW=1` now recognizes that validated
packet form and inserts a draw record at the gap's position. One 65-second boot captured 20,629 such draws; the
decoder rejects malformed or incomplete packets and leaves other gaps unmodeled. The flag is opt-in because the
resulting draws still lack some shader constant-buffer state, so the direct renderer cannot issue them reliably.
For the first 1,656-index draw, the emulated vertex shader had CBUF banks 3 and 4 bound, while the NVN recorder had
no application uniform slots 0 or 1 at that point. A later recorded slot 5 matched Maxwell bank 8, consistent with
the established application-slot-plus-three mapping where a binding exists. The missing bank 3 and 4 bindings must
be recovered from the encoded stream in order; inventing uniform data would invalidate the comparison.

The shadow renderer now selects the prepared attachment layer and tracks coverage per layer. It also attempts its
NVN-owned draw before testing Maxwell's surface-cache shape, which can describe a different view at the same GPU
address. A default TotK boot issued a shadow Vulkan draw to the first 640×360 target after these changes. This does
not resolve the missing uniform bindings or establish image parity.
Temporarily omitting only those three Maxwell draws
made the two targets match byte-for-byte (0/230,400 different pixels); that omission was removed after diagnosis.
The direct draw itself left a forced magenta target unchanged, including with depth testing, culling, and blending
disabled independently. This established the initial command-coverage gap, rather than a wrong color value from the
31 staged draws. The new draw records close that count gap, but full encoded-state capture and image comparison are
still required before replacing GPFIFO execution.

The next TotK probe found those missing bindings in complete command-buffer cursor gaps before the first draw.
Maxwell methods `0x8e0..0x8e2` set the constant-buffer size and address, and methods `0x904`/`0x924` bind vertex and
fragment banks. The same gaps also set vertex stream starts through `0x701/0x702` and inclusive ends through
`0x7c0/0x7c1`. The opt-in decoder now validates the whole gap before applying any method, retains these registers
in list order, and inserts uniform and vertex bind records before the indexed draw. An ABI-matched TotK build ran
35 seconds with 9,757 captured indexed draws, 37,104 recovered uniform binds, 9,005 recovered vertex binds, no
unknown ranges, and no dropped operations. Its first 1,656/3,384/3,384-index draws all issued on the shadow target.
With `SNX_NVN_SHADOW_COMPARE=1`, `SNX_NVN_SHADOW_COMPARE_SIZE=640x360`, and
`SNX_NVN_SHADOW_COMPARE_FORMAT=0x29`, both images covered exactly 23,040 nonblack pixels in the top 36 rows.
They still differed at 12,116 of those pixels, with small differences in all four half-float components; color
parity remains unproven. Without recovered vertex binds, the first three draws had no bound stream and did not
issue. Without recovered state, the 640×360 shadow target had no eligible image to compare.

A consecutive-present TotK probe narrowed the next coverage gap. The first comparison differed by roughly 12,000
pixels, while the second matched all 230,400 pixels in one run; that first-pass difference is timing-sensitive and
does not yet have a proven cause. Starting with the third pass, the same indexed draws still had prepared Vulkan
pipelines, but the shadow renderer rejected them because their depth attachment at `0x52e160000` had lost coverage.
The color clear still issued, then the rejected draws invalidated the color target. `vk_nvn_batch_begin` clears all
shadow image coverage at every replay batch, and that pass did not reestablish the depth provenance before its first
draw. The next implementation task is to track depth and color writes by resource across replay batches, including
which unmodeled writes can actually alias an attachment; simply bypassing the depth check would let stale shadow
contents pass as valid.

The recorder now proves when an opaque list has no hidden command-memory writes: its single observed command-memory
range must begin at the recorded cursor, every inter-call gap and the EndRecording tail must contain only validated
Maxwell constant-buffer setup/bind methods, and no operations may precede that allocation. Visible draws and clears
retain their own write markers. In a 27-second TotK boot, 2,259 of 2,757 opaque replayed lists met this proof.
`SNX_NVN_CROSS_BATCH=1` is an opt-in shadow probe that carries image coverage across replay batches, checking tracked
guest CPU and GPU writes to the backing memory before reuse. It also lets an unsupported draw with read-only SSBOs
invalidate only its known attachments. This still does not restore the third TotK pass: an earlier six-index draw
uses a writable fragment SSBO whose address the direct recorder cannot yet prove disjoint from the reused depth
image, so its global unknown-write marker remains. Resolving the SDK-generated constant-buffer data that names
that SSBO, or modeling its write range directly, is the next required step.
An instrumented TotK run scanned 198,135 complete SDK-call command spans plus the inter-call gaps and found no raw
binds for Maxwell CBUF banks 0–2. The emulated GPU instead observed one ordinary, non-MME bank-0 bind for all five
graphics stages before the first captured NVN submission: address `0x500390000`, size 28,672 bytes. The writable
fragment SSBO descriptor at bank-0 offset `0x550` named `0x623130000` for 58,848 bytes in that run, separate in GPU
virtual address from the blocked depth target. A diagnostic that borrowed this emulated binding still did not restore
the third pass; other unknown writes remain. The direct path must recover the initialization binding and any needed
physical-alias proof without depending on emulated render state.
The GPU command front end retains the SDK's bank-0 address, size, and GPU write generation per graphics stage and
channel, even when the emulated renderer is inactive. Reading a writable SSBO descriptor from bank 0 after list
execution proved unsafe: its page changes after the original bind, so the write-generation guard rejected every
draw. The direct path now snapshots the semantic `nvnCommandBufferBindStorageBuffer` address and size at recording
time and pairs each call with its raw macro packet. Its side-effect-free MME probe requires exactly one untainted
constant-buffer offset, a base and size matching the front end's stage binding, and no other constant-buffer writes.
Replay carries validated bindings into each draw. An undecodable bind clears the stage's known storage state; a
failed range or physical-alias proof still leaves the global unknown-write marker. This scopes writable SSBO effects
without borrowing descriptor data from after GPFIFO execution. The first selected shadow/emulated target comparison
still differed in all 921,600 pixels; complete-pass and presented-frame parity remain unproven. Direct draws do not
yet receive bank-0 shader data.
A temporary trace at the actual Maxwell draw boundary found the same fragment SSBO descriptor, address
`0x623130000` and size 58,848 bytes, for the first 80 six-index draws to TotK color target `0x5187f0000`.
GPU write generations advanced throughout that sample. The current write-generation guard is page-granular, so it
can reject a descriptor whose twelve bytes did not change. The trace does not prove the value for every submitted
NVN draw, and a direct path cannot depend on the emulated draw boundary as its source of state. The next step is to
decode the command-list writes and pair their per-draw buffer state with the NVN semantic draw stream.
A temporary raw-method scan decoded 9,526 single-allocation NVN command-memory spans of at most 64 KiB during a
15-second TotK run; 1,359 candidate spans did not meet those bounds. It found 51,392 explicit Maxwell
constant-buffer data methods, but initially missed the descriptor upload because the scanner ignored MME effects.
The actual GPFIFO entry mapped back to a recorded 80-byte NVN list span. That span calls macro `0xe20` with parameter
`0x154`, followed by three `0x8e4–0x8e6` data words. The GPU executed those as a 12-byte write at
`0x500390550` (`0x154` words past CBUF 0), repeatedly during the workload. TotK's uploaded macro index 16 began
at word `0xfe` in the captured MME program. A direct command-list decoder must retain the inherited CBUF binding,
execute or validate the macro's offset effect, and attach the resulting descriptor words to draws in list order.
The recorder snapshots the syntactic `0xe20` plus three-data-word packet at EndRecording, before command memory can
be reused. Per-call matching requires those three words to equal the semantic NVN address and size. A 16-second TotK
run matched over 4,000 storage-bind packets with no mismatches and scoped at least 374 writable-storage draws among
4,600 shadow draws. This establishes a bounded write set for those draws, not correct pixels or a visible HLE frame.
The shadow renderer now accepts read-only graphics SSBOs when the recorded NVN bind and the uploaded MME macro prove
the shader's bank-0 descriptor offset. Shader translation tracks each static bank-0 word read; any dynamic or
unreconstructed read rejects the draw. The descriptor and storage-buffer binding then come from the same per-draw
NVN snapshot. If a shader samples the color or depth attachment it is writing, the direct path snapshots the
covered pre-draw attachment with the TIC's component mapping. A TotK probe executed the first two six-index draws
that previously invalidated the shared depth target. The third six-index draw remains ineligible because a preceding
unsupported writable-storage draw invalidated its source color target at `0x512460000`; there is no safe prior color
image to snapshot. That draw has active fragment color output and a full channel mask, so its color write cannot be
ignored. The third 640×360 comparison still had zero nonblack HLE pixels versus roughly 116,000 emulated
pixels. `SNX_NVN_SHADOW_COMPARE_PRESENT=N` selects the Nth present where both versions of the filtered target exist,
so later-pass comparisons can be reproduced without assuming startup presents contain that target. Writable graphics
SSBO execution and ordered shadow buffer contents are required to close this gap.
An ABI-matched Catherine rebuild ran headless for 25 seconds with the cross-batch probe. It issued at least 16,300
direct shadow draws; its 1,374 replayed lists had no missing or truncated entries. This is execution coverage, not
an image-parity result.

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
