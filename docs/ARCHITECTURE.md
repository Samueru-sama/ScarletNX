# Architecture

ScarletNX AOT: AArch64 -> C -> native module, shared by Horizon HLE/Maxwell
and CPU-only Linux AArch64 ABI runtimes.
Source map: [CLAUDE.md](../CLAUDE.md).

## Runtime boundaries

| Source / CMake target | Ownership |
| --- | --- |
| `src/common/` / `scarletnx-common` | AArch64 CPU state, instruction semantics, code discovery/emission, native module execution, runtime callback interface, host utilities |
| `src/nx/` / `scarletnx-nx` | Horizon SVC/kernel/services/filesystems, NSO/NRO loading, Switch memory, Maxwell and graphics/media backends, Switch frontend/tooling |
| `src/gnuabi/` / `scarletnx-gnuabi` | Linux syscall/errno/structure translation, ELF loading, isolated VFS, Linux process/memory/signal/futex compatibility |

The common target has no runtime-specific imports. NX and GNU ABI each link
common and have no dependency on each other. The GNU executable target is
`scarletnx-gnuabi-run`, whose output is `scarletnx-gnuabi`; its library output is
`scarletnx-gnuabi-runtime`. `SNX_BUILD_NX=OFF` builds GNU ABI without fetching or
linking NX graphics/media packages. CTest checks include ownership and the actual
transitive CMake dependency graph and target source ownership.

`src/common/runtime/interface.h` defines `RtAPI`; the active runtime supplies
the callbacks on `CPU.rt`. NX routes SVC to Horizon. GNU ABI accepts `SVC #0`,
reads Linux syscall number `x8` and arguments `x0` through `x5`, and writes result
`x0`. CPU helpers and instruction emission are shared; Horizon handlers are never
used as Linux syscalls.

`scarletnx-erista`, `scarletnx-mariko`, and `scarletnx-aula` share the existing
`scarletnx` NX implementation through Unix symlinks or Windows executable copies.
The original game-name entry points and CLI remain available. GNU ABI has an
independent CPU-only entry point; see [GNUABI.md](GNUABI.md) for implemented
syscalls, ELF execution, filesystem scope, and differential testing.

## Recompiler + module ABI

NX tooling under `src/nx/runtime/driver/` and `src/nx/loader/`:
`work/<Game>/{exefs,romfs}` -> `games/<Game>[.exe]`, `<Game>_code.{so,dll}`, `<Game>_data/`:

- `driver.c`: CLI dispatch only; `extract.c` and `recompile.c` own their workflows.
- `src/nx/loader/loader.c`: NSO/NRO, LZ4, SHA-256, NPDM, MOD0, symbols/relocs/FDEs.
- `src/common/cpu/aarch64/a64.c` + `a64_templates.inc`: shared instruction semantics for AOT and interpreter.
- `src/common/cpu/recompiler/codegen.c`: function/CFG discovery over platform-neutral image views.
  `src/nx/loader/codegen_view.c` adapts NX images; `module.c` owns NX chunks (whole functions batched up to
  `SNX_CHUNK_INSTRUCTIONS`, 90000, per unit, for both backends), dispatch tables, `SnxModule`.
- `toolchain.c`: parallel Clang jobs, ABI/source-keyed object cache, linking, shader prebuild.
- Code generators (`--backend`, `src/common/cpu/recompiler/backend.h`): `portable` prints C through
  `codegen.c`/`module.c` and compiles it with `toolchain.c`; it is chosen when `--cc`, a cross `--target` or
  `--no-build` is given, or when the recompiler has no LLVM. `llvm` (the default; `src/common/cpu/recompiler/llvm/`,
  built when `llvm-config` is found) builds LLVM IR with the LLVM-C API: `ir_a64.c`, `ir_simd.c` and `ir_fp.c` emit each
  `a64_templates.inc` case as the IR of the C it holds, `ir_codegen.c` the functions of `codegen.c`, and
  `src/nx/loader/module_llvm.c` the chunks and `SnxModule` of `module.c`. It links the objects itself (`link_elf.c`,
  `link_coff.c`) with `rtlib.c`, the helpers and interpreter compiled when the recompiler is built, into a module
  with no imports, for the host it runs on. `tests/llvm_differential.c` runs both backends' step functions on random
  states (every pattern, failing if any instruction falls back to the interpreter's `snx_step`), `tests/llvm_module.c`
  a whole program built by both. `gcc` (libgccjit, `module_gccjit.c`) is reserved.
- `src/common/utilities/common.c` and `process.c`: host files/processes; argv-based subprocesses.
  NX extraction retains its isolated key environment.

Host tooling uses C90 syntax with SDK fixed-width types, formatting and OS APIs.
The runtime and generated guest code retain their existing Clang/ABI requirements.
Normal builds link the driver into the runtime; `SNX_TOOLS_ONLY` builds the same CLI independently.

Shared CPU ABI (`src/common/cpu/aarch64/`): guest functions take `CPU*`; `snx_lookup` dispatches indirect branches, with interpreter fallback.
`CPU.rt` supplies `RtAPI` callbacks: SVC, UNDEF, BRK, hints, CNTVCT, FPCR, SEV/WFE; no named runtime imports.
`cpu_helpers.c` + `helpers/`: SIMD, crypto, FP.

GNU supplies an optional fetch callback that checks execute permissions and
compares the live instruction against the compiled word. Modified code falls
back to shared interpreter semantics. NX leaves this callback unset and keeps
its existing execution path.

NX module ABI is in `src/nx/loader/module_abi.h`; the shared CPU layout is in
`src/common/cpu/aarch64/cpu.h`. Rebuild game modules after shared ABI changes;
bump `SNX_ABI` for ABI changes. Version mismatches reject loading.

## Runtime ownership

| `src/nx/` directory | Responsibility |
| --- | --- |
| `runtime/` | Module loading, INI, clock, `RtAPI`, crashes |
| `hle/kernel/`, `hle/svc/`, `memory/` | Objects, threads, sync, SVC, Switch memory, IPC |
| `hle/services/`, `hle/filesystem/` | Services, nvdrv, guest filesystem services |
| `gpu/` and `gpu/maxwell/` | GPFIFO, Maxwell engine implementation, shaders, Vulkan renderer |
| `gpu/video/` | host1x, NVDEC/FFmpeg, VIC |
| `runtime/platform/` | Host APIs, SDL3/headless, input, screenshots, timed exit |

### Guest exec

Guest thread = host thread + `CPU` + TLS. `hle/kernel/thread.c`: one owner/core, four cores; release on kernel block,
sleep/yield, IPC, WFE. Waiters: priority then FIFO; higher-priority preemption at the next SVC.
SVC/WFE-free spins retain their core.

`memory/mem.c`: one guest VA reservation; `GP(addr) = g_mem + addr`, accessed directly by generated code.

SVC -> `hle/svc/svc.c`; `svcSendSyncRequest` -> `hle/kernel/ipc.c` -> session's HLE service object.

[Filesystem](FILESYSTEM.md): startup scan; indexed lookup, metadata, enumeration, mutations.

### GPU submit/present

1. NVN command buffers -> `/dev/nvhost-gpu` GPFIFO ioctl -> `hle/services/nvdrv.c` -> `gpu_submit` (`gpu/gpu.c`).
2. Copy entries to GPU queue; `nv.submit_lk` orders fence promises and cross-channel submits.
   Promised GPU-channel fences wait on the GPU thread; host1x/CPU/unpromised fences wait on the submitter.
3. GPU-thread dispatch: 3D, MME, compute, 2D, DMA, inline-to-memory. Draws -> `g_renderer` (`gpu/renderer.h`).
   Shaders: Maxwell SASS -> GLSL -> SPIR-V (`gpu/shader/`, glslang).
4. Vk `stream_incr` lands compute writes in guest memory before incrementing the syncpoint.
5. vi waits on unpromised fences, then queues present after preceding submits. GPU thread checks remaining fences;
   `present_buffer` uses matching RT + vi crop, or converts guest memory.

### Renderer coherence + lifetime

- **Vk**: Vulkan 1.3 in windowed or headless mode, with guest buffer mirrors; separate compute queue when available,
  graphics queue otherwise.

Invalidation: host write tracking + `mem_unmap` -> `gpu_mem_changed`. Vk treats zeroed unmap/heap-return ranges
as written at the next cache check.

`Renderer.save`: optional pipeline-cache persistence every 300 frontend ticks (~5s) + exit, concurrent with execution.
Exit skips renderer teardown and guest-thread joins.

### Videodec/VIC channels

Each NVDEC/VIC fd owns a `host1x_open` channel, registers and syncpoints; NVDEC also owns a decoder.
NVDEC writes guest surfaces; VIC composites output.
