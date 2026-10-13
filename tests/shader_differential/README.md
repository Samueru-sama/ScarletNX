<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors -->
# Isolated shader differential harness

This optional Linux harness builds ScarletNX's actual shader sources and a subset
of Eden's actual Maxwell visitors and IR implementation. Eden is not modified or
vendored. Its `tools/maxwell-ir` supplies the isolation model and its
`tools/maxwell-disas/file_environment.h` supplies the environment.

## Requirements and commands

Use Clang with C++23, CMake, Ninja, Python 3, Boost 1.83 headers and fmt 11.1.1
headers. The tested compiler is Clang 21.1.5. fmt is used header-only.
`DEPENDENCY_INCLUDE` is a directory containing `boost/` and `fmt/`.
The Eden checkout must be clean at
`6352612425ae0765802eaa8c5904ff652b97222a`.

From the ScarletNX checkout, with Eden in the sibling directory:

```sh
cmake -S tests/shader_differential -B build/shader-differential -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DEDEN_DIR="$PWD/../Eden" \
  -DDEPENDENCY_INCLUDE=/absolute/path/to/dependency/include
cmake --build build/shader-differential -j 2
python3 tests/shader_differential/compare.py \
  --eden ../Eden --build build/shader-differential \
  --cxx clang++ --out build/shader-differential/results \
  --glslang /absolute/path/to/glslangValidator \
  --spirv-val /absolute/path/to/spirv-val
python3 tests/shader_differential/check_decode.py \
  --eden ../Eden \
  --reference build/shader-differential/eden_frontend \
  --scarlet build/shader-differential/scarlet_frontend \
  --out build/shader-differential/results/decode.json
build/shader-differential/scarlet_frontend --merge-test
```

The GLSL/SPIR-V tools are optional but must be supplied together. Tested versions
are glslang 15.2.0 and SPIRV-Tools 2025.1, targeting Vulkan 1.1.
The harness is deliberately separate from the ordinary CMake build so normal
ScarletNX builds do not acquire an Eden dependency.

For baseline replay, configure another build with
`-DSCARLET_SHADER_DIR=/absolute/path/to/original/runtime/gpu/shader`, build only
`scarlet_frontend`, then supply that executable through `compare.py --scarlet`.
A differential mismatch exits with status 1. Compiler failures, reference
exceptions, unsupported interpreter operations and sanitizer failures are errors,
not skipped vectors. `check_decode.py` also exits 1 on mismatches.

## What executes

- `scarlet_frontend` calls the real `decode()` and `emit_tracked()` functions.
- `eden_frontend` calls the real decoder and selected `TranslatorVisitor` methods.
  It serializes their unoptimized IR, including typed immediates and flags.
- `compare.py` executes that IR with a deliberately small CPU interpreter. It
  compiles and executes ScarletNX's emitted scalar expressions using the C++
  compatibility layer in `scalar.hpp`. It compares all 256 register slots,
  seven predicates, four condition flags and the supplied constant-buffer words.
- GLSL float literals receive an `f` suffix for C++. `-fwrapv` models modular
  signed integer arithmetic in the CPU adapter. It prevents C++ overflow rules
  from creating a false failure in the old emitted I2I code. Production C/C++
  frontend sources and the emitted adapter are built with UBSan.
- If validation tools are supplied, all distinct tested encodings are assembled
  into a synthetic scheduled program and passed through `shader_to_glsl()`.
  The complete generated compute shader is compiled and SPIR-V validated.
- The decoder check enumerates all 65,536 high-word prefixes. It executes Eden's
  native decoder on all 48,084 table-selected prefixes, then checks only the six
  corrected entries and STP guard against ScarletNX. Unknown Eden inputs are not
  passed to its asserting NOP fallback.
- The dual-vertex check exercises the real merge function across predicate and
  flow-condition fields. It checks the rewritten encoding, not GPU execution.

Eden logging is replaced with a stderr sink. Its soft assertions are made fatal
in this harness, so an assertion fallback cannot be accepted as a reference
result. Visitor/IR semantics are not replaced. The reference environment's
texture queries are not used.

## Bounds of the evidence

This is not execution of either GPU backend. It does not establish hardware or
driver equivalence, FTZ/rounding fidelity, optimizer behavior, shader program
control flow, or game compatibility. Instruction-level vectors use unconditional
instruction predication and supported operand subsets. NaN comparison cases are
included, but F2I vectors use finite F32-to-S32 inputs only.

The IR interpreter does not model every FP-control flag. Undefined intermediate
bitfield values are tracked as poison and may only survive if an IR select
discards them. This is not proof that every backend handles those cases alike.
ABS(INT_MIN) and signed wrapping use the CPU adapter's modular model, not a claim
about undefined backend operations. The harness fails on unknown IR operations.

Tests are generated from synthetic values. No game data or captured shaders are
required. Generated binaries, IR and GLSL stay in the chosen output directory.
See `docs/shader-audit/fixes-001.md` for the implemented scope and remaining gaps.
