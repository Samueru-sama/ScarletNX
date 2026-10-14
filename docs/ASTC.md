# ASTC compute uploads

`src/nx/gpu/texture/astc.spvasm` contains the SPIR-V implementation of the integer LDR decoder from `astc.c`.
Each invocation decodes one 128-bit block. Workgroups contain 32
invocations. The shader requires only core storage buffers and 32-bit integers.
HDR endpoint modes retain the reference decoder's magenta error behavior.
This is a port, not a new ASTC conformance implementation.

`src/nx/gpu/vk/astc.c` creates a compute pipeline from the embedded SPIR-V in
`src/nx/gpu/texture/astc_spv.h`. Normal builds need no additional compiler or
shader files installed alongside the executable. The CPU decoder is no longer
linked into the runtime, but remains available to differential tests.

## Upload path

- `texture.c` still deswizzles guest block-linear memory on the CPU.
- Only compressed blocks enter the host staging ring. Compute expands them into
  a device-preferred scratch buffer, capped at 8 MiB.
- Block-row strips keep both descriptor ranges within `maxStorageBufferRange`.
  Every mip, array layer and depth slice is uploaded. Edge stores are clipped.
- Compute runs on the graphics queue, outside rendering. Device selection now
  requires this queue to support both graphics and compute.
- Storage-write to transfer-read barriers precede image copies. Transfer-read to
  storage-write barriers protect scratch reuse, including across submissions.
  Descriptors come from the existing frame pools. Staging buffers remain alive
  until the frame fence and are flushed when retired as well as when submitted.
- Raw RGBA8 bytes are copied into the existing UNORM or SRGB image. sRGB endpoint
  expansion happens in the decoder, with no extra gamma conversion on upload.
  The existing surface barriers make the image visible to subsequent sampling.

The pipeline, layouts and scratch allocation have device lifetime, matching the
backend's other internal pipelines. Initialization/allocation failure is fatal
rather than silently presenting an uninitialized texture or falling back to CPU
ASTC decoding.

The port also corrects the reference's void-extent header read: checking reserved
bit 11 requires reading 12 bits. Previously every solid-color block failed this
check. Other decoding behavior intentionally follows the reference.

## Rebuild the shader

Use SPIRV-Tools (`spirv-as` and `spirv-val`):

```sh
python3 scripts/build-astc-shader.py
python3 scripts/build-astc-shader.py --check
python3 scripts/build-astc-shader.py --spv astc.spv
```

The script assembles for Vulkan 1.1, validates the binary, and embeds its words in a C header.
`--check` compares regenerated output against the checked-in header.

## Standalone tests

No game files or window are needed. Point the paths below at the dependencies
from the CMake source cache, or equivalent local installations:

```sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
python3 tests/run_astc_vulkan.py \
  --vulkan-include /path/to/Vulkan-Headers/include \
  --volk /path/to/volk \
  --vma-include /path/to/VulkanMemoryAllocator/include
```

Select a specific Vulkan driver with `VK_ICD_FILENAMES` if needed. The harness
links production `vk/astc.c`, `vk/cmd.c`, volk and VMA. It stubs the window/game
services and buffer-cache hooks, while exercising the upload commands and staging
allocator. It checks raw image readbacks against the CPU reference for all 14
footprints in both decode modes.
Synthetic blocks exercise all 2048 block-mode encodings, solid-color and invalid
blocks. Further cases cover non-multiple dimensions, dispatch padding, five mip
levels down to 1x1, six layers, 3D slices, scratch chunking, frame reuse and staging
ring growth.

Verification performed in the development container:

- SPIR-V assembly, validation and embedded-header reproducibility passed.
- 86 Vulkan differential cases passed on Mesa 22.3.6 llvmpipe, with the Khronos
  validation layer and synchronization validation enabled. Zero validation errors.
- Changed Vulkan C translation units passed Clang 15 syntax checks using Vulkan
  1.3.239 headers. The standalone harness also built and ran with Clang 15.

The development-container checks above did not include a full runtime build or
a game run. In particular, synthetic
reference comparisons are not an independent ASTC conformance test, and lavapipe
cannot establish hardware performance or non-coherent memory behavior.

## Integration verification

After applying the patch to ScarletNX `main`, the Vulkan runtime built and all
12 CTest cases passed. The standalone ASTC Vulkan comparisons passed again.
Catherine rendered its menu during a 60-second run. Super Mario Odyssey rendered
a gameplay scene during a 75-second run. Tears of the Kingdom displayed the opening
cave during a 185-second run. The later cave cutscene was not reached in that run.
