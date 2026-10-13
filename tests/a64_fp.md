# ARMv8-A floating-point regression tests

The portable specification witnesses exercise base Cortex-A57 FP semantics.
They need neither game assets nor an emulator:

```sh
cmake -S . -B build/fp -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF
cmake --build build/fp --target snx-test-a64-fp snx-test-a64-fp-arithmetic snx_codegen_test
ctest --test-dir build/fp -R '^(a64_fp_.*witnesses|native_codegen)$' --output-on-failure
```

Use the project's supported Clang toolchain. The integer-only FP backend uses
unsigned 128-bit integers, as do the existing CPU integer helpers. It does not
use host floating-point arithmetic, libm, host rounding state or ISA-specific
FP intrinsics. Guest FPCR writes and context switches must not alter the host
FP environment. `SNX_ABI` is 6: rebuild the runtime and recompile guest modules.

## Specification witnesses

`a64_fp_test.c` covers rounding, min/max and comparisons. All operands and
expected results are raw IEEE encodings. The checks include:

- Ties-to-even and ties-away, signed zeros, and FRINTX versus FRINTI exceptions.
- Signalling-NaN priority, payload/sign preservation, and default NaNs (DN).
- Input flushing (FZ/IDC), numeric min/max and unordered comparisons.
- Conditional comparisons, vector lane clearing and cumulative FPSR flags.

`a64_fp_arithmetic_test.c` adds witnesses for:

- Invalid arithmetic and divide-by-zero, NaN selection, directed overflow,
  gradual underflow and FZ output flushing.
- Addend-first fused NaN selection and the QNaN-addend/zero-times-infinity rule.
  Finite fused results must not inherit an intermediate multiply's overflow
  or underflow. Reciprocal refinements likewise round only their final result.
- Square root, reciprocal estimates and exponent estimates, including zeros,
  infinities and denormals. Estimates follow ARM's algorithm, not host estimates.
- Integer and fixed-point conversions: rounding, signed/unsigned bounds,
  invalid saturation, inexact results and scaling before rounding.
- Half/single/double conversion, AHP, round-to-odd and NaN payload conversion.
  Half-format conversion is supported on A57; half arithmetic is not.
- Pairwise/vector aliasing and cumulative flags; the A57 FPCR/FPSR RES0 masks.

Both tests run under all four host rounding modes. The arithmetic test also
checks that operations leave host rounding and exception state unchanged.
`native_codegen` checks selected scalar, vector and by-element witnesses
through the actual decoder and both emitted AOT/static and interpreter paths.

References: ARM DDI0487A.k instruction and shared FP pseudocode (`FPUnpack`,
`FPRound`, `FPProcessNaNs`, `FPProcessNaNs3`, arithmetic, conversions and estimates)
and Cortex-A57 DDI0488H Tables 14-3/14-4. A57 FP exception-enable bits and FZ16
are RES0; later optional alternate-FP behavior is not part of this target.

## Additional rounding/min/max/comparison differential tests

The optional Linux harness covers these earlier FP families, not the entire
arithmetic/conversion suite:

```sh
python3 -m venv .venv
.venv/bin/pip install unicorn==2.1.4
.venv/bin/python tests/a64_fp_diff.py --cc clang
.venv/bin/python tests/a64_fp_diff.py --cc clang --random 128 --aliases --sanitize
.venv/bin/python tests/a64_fp_diff.py --cc clang --match FRINT
```

CTest enables it with `-DSNX_TEST_A64_FP_DIFF=ON` and
`-DPython3_EXECUTABLE=/path/to/venv/bin/python`. It builds temporary emitted
sources and a shared library, then removes them. `--sanitize` enables Clang's
undefined-behavior trap mode. Any compilation failure, unexpected trap, result
mismatch or exception mismatch fails the test.

`a64_fp_emit.c` uses the actual decoder, AOT shortcuts and interpreter emitter;
it does not duplicate instruction semantics. The Python harness compares all
128 destination bits, FPSR and NZCV with Unicorn's QEMU-derived AArch64 model.
NaN payloads, status bits and unused vector lanes are not masked away.

Inputs include signed zeros, NaNs of both kinds/signs and multiple payloads,
infinities, subnormal boundaries, finite extremes, rounding boundaries and
seeded random patterns. Binary operations cross the directed edge corpus;
mixed lanes check cumulative exceptions and pairwise ordering. Guest rounding,
DN and FZ are varied. `--aliases` additionally exercises overlapping registers.

These tests are regressions, not architectural certification, hardware
measurements, full-runtime validation or gameplay/performance benchmarks.
