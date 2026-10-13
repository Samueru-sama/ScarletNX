# ARMv8-A floating-point regression tests

This is a targeted audit of the base ARMv8-A single/double-precision rounding,
min/max and comparison instructions. It is not a claim that every instruction
in `a64_templates.inc` now conforms to ARMv8-A.

## Reproduce

The small specification-witness test has no emulator dependency:

```sh
cmake -S . -B build/fp -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF
cmake --build build/fp --target snx-test-a64-fp snx_codegen_test
ctest --test-dir build/fp -R '^(a64_fp_witnesses|native_codegen)$' --output-on-failure
```

For differential testing on Linux, install Unicorn into a local environment:

```sh
python3 -m venv .venv
.venv/bin/pip install unicorn==2.1.4
.venv/bin/python tests/a64_fp_diff.py --cc clang
# Extended run, including overlapping source/destination registers and UB traps:
.venv/bin/python tests/a64_fp_diff.py --cc clang --random 128 --aliases --sanitize
# Shorter family-specific run:
.venv/bin/python tests/a64_fp_diff.py --cc clang --match FRINT
```

The differential test is also available through CTest with
`-DSNX_TEST_A64_FP_DIFF=ON -DPython3_EXECUTABLE=/path/to/venv/bin/python`.
It compiles temporary sources and a temporary shared library, then removes them.
A compiler failure, unexpected instruction trap, result mismatch or exception
mismatch fails the test. `--sanitize` uses Clang's undefined-behavior trap mode.

## Expected behavior and oracle

`a64_fp_test.c` contains explicit IEEE bit-pattern witnesses derived from the
ARM ARM pseudocode. It checks them under all four host rounding modes. In
particular:

- FRINTN rounds ties to even, while FRINTA rounds ties away from zero.
- All rounding modes preserve the sign of a zero result.
- FRINTX uses FPCR.RMode and raises IXC for a non-integral finite input.
  FRINTI uses the same mode but does not raise IXC. Existing flags stay set.
- An SNaN becomes quiet and raises IOC. A QNaN does not raise IOC for rounding.
  DN produces the positive default NaN, without suppressing an SNaN's IOC.
- FZ flushes a subnormal input to signed zero and raises IDC. FRINTX then
  sees an exact zero and does not additionally raise IXC.
- Min/max selects an SNaN before a QNaN. Numeric min/max ignores a lone QNaN,
  not an SNaN. FMIN chooses -0 and FMAX chooses +0 when both operands are zeros.
- FCMP is quiet for QNaNs, while FCMPE signals invalid for either kind of NaN.
  The conditional forms do not examine operands when their condition is false.
- SIMD equality comparisons are quiet, while ordered comparisons signal for
  either kind of NaN. SIMD comparisons leave PSTATE.NZCV unchanged.

References: ARM DDI 0487, the instruction pseudocode and shared operations
`FPRoundInt`, `FPUnpack`, `FPProcessNaN`, `FPProcessNaNs`, `FPCompare`,
`FPMax`, `FPMin`, `FPMaxNum` and `FPMinNum`. Browsable pseudocode mirrors:

- https://www.scs.stanford.edu/~zyedidia/arm64/frintx_float.html
- https://www.scs.stanford.edu/~zyedidia/arm64/frinti_float.html
- https://www.scs.stanford.edu/~zyedidia/arm64/fmaxnm_float.html
- https://www.scs.stanford.edu/~zyedidia/arm64/fcmp_float.html
- https://www.scs.stanford.edu/~zyedidia/arm64/shared_pseudocode.html

Use the base ARMv8-A behavior, not later optional alternate-FP behavior.

`a64_fp_emit.c` invokes the real decoder, static-step emitter (including AOT
shortcuts), and interpreter emitter. It does not copy instruction semantics
into the tests. `a64_fp_diff.py` executes the same instruction words with
Unicorn 2.1.4's QEMU-derived AArch64 implementation. It compares all 128 bits of
the destination register, FPSR and NZCV, without masking away NaN payloads or
unused vector lanes. This is an independent software oracle, not a measurement
on physical ARM hardware.

The corpus includes signed zeros, both signs and several payloads of QNaNs and
SNaNs, infinities, smallest and largest subnormals, smallest normals, largest
finite values, ties and adjacent representable values, precision boundaries,
and seeded random bit patterns. Binary instructions cross the directed input
corpus. All four guest rounding modes and both values of DN and FZ are tested.
Mixed vector lanes check cumulative exceptions and pairwise reduction order.
The extended run additionally tests Rd=Rn and Rd=Rm where applicable.

## Findings and verification

Starting from shallow-cloned revision `e7c4210`:

- Initial FRINT-only run (`--random 8`): 17,136 mismatches in 67,200 comparisons
  across all 35 scalar/vector forms. Missing IOC/IDC/IXC, DN and FZ behavior
  accounted for the failures. FRINTX and FRINTI previously called the same helper.
- After fixing rounding, the expanded min/max and scalar-comparison run:
  1,257,856 mismatches in 5,374,080 comparisons. The helpers lacked guest FPCR
  and FPSR state. Scalar FCMP/FCMPE and FCCMP/FCCMPE were also conflated.
- The subsequent SIMD-comparison audit (`--match FCM --random 8`): 847,376
  mismatches in 4,245,504 comparisons. NaN exceptions and FZ were missing.
- Extended repaired run with Clang 19.1.7, Unicorn 2.1.4, `--random 128
  --aliases --sanitize`: **24,490,368 comparisons over 331 instruction cases,
  zero mismatches and no UB traps**.
- The CTest specification witnesses and existing `native_codegen` tests pass.

The implemented missing functionality is guest FPCR processing and cumulative
FP exception reporting for the audited families. Raw-bit helpers avoid host
rounding, widening-induced SNaN quieting and host flush modes. Reserved scalar
AdvSIMD FRINT encodings are rejected rather than treated as valid operations.

`SNX_ABI` is now 4. Rebuild the runtime and recompile existing game modules so
that stale generated semantics cannot be silently loaded.

## Remaining scope

Arithmetic, FMA, square root, reciprocal estimates, FP/integer conversions and
FP format conversions were not made fully FPCR/FPSR-conformant by this patch.
Their existing context-free helpers and native arithmetic still require a
separate audit. Integer, memory, system and crypto instruction families are not
covered by this FP corpus. FP exception-enable traps, FP16 arithmetic and later
optional instructions such as FRINT32/FRINT64 are not implemented here.

No game assets were available, so no game/runtime scene validation was performed.
The full runtime was not built. Verification used generated AOT/interpreter
execution, helper witnesses and the existing codegen regression suite.
