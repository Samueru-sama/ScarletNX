<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Cortex-A57 instruction audit harness

Read [DIVERGENCES.md](../docs/DIVERGENCES.md) for the target profile, public ARM
PDFs, remaining defects and architectural limitations. This is an audit that
currently finds defects, not a certification suite or an expected-failure list.

## Run

Tested on Linux x86-64 with Clang 19.1.7. From the repository root:

```sh
python3 -m venv .venv
.venv/bin/pip install unicorn==2.1.4 capstone==5.0.7
.venv/bin/python tests/a64_audit.py --cc clang --states 48 --aliases --report audit.json
```

The command returns nonzero for the documented FP differences. Reports contain
counts, diagnostic instruction names and the first complete counterexample for
each failing word. No differing NaN payload or FPSR bit is suppressed.

Other useful runs:

```sh
# Trap undefined C behavior, including signed overflow hidden by -fwrapv.
.venv/bin/python tests/a64_audit.py --cc clang --states 48 --aliases \
  --sanitize --strict-c --report audit-sanitized.json

# Scalar FP and conversion families under all RMode/FZ/DN/AHP combinations.
.venv/bin/python tests/a64_audit.py --cc clang --match '^(FP|FCVT)' \
  --states 1536 --aliases --report fp-controls.json

# Successful and failed exclusive stores, plus exclusive/acquire loads.
.venv/bin/python tests/a64_audit.py --cc clang --match '^LDSTX$' \
  --states 48 --sanitize --strict-c --report exclusive.json

# One counterexample. Word input is hexadecimal.
.venv/bin/python tests/a64_audit.py --cc clang --word 1e221820 \
  --states 48 --report fdiv.json
```

The optional CTest audit uses `-DSNX_TEST_A64_AUDIT=ON` and
`-DPython3_EXECUTABLE=/path/to/venv/bin/python`. It is off by default because
remaining defects make it fail. Its report is `build/tests/a57-audit.json`
when the build directory is `build`.

The normal specification witnesses and native codegen regression do pass:

```sh
cmake -S . -B build/a57 -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF \
  -DCMAKE_C_COMPILER=clang
cmake --build build/a57 --target snx-test-a64-fp snx_codegen_test
ctest --test-dir build/a57 -R '^(a64_fp_witnesses|native_codegen)$' --output-on-failure
```

## What is exercised

`a64_audit_emit.c` gets the actual decoder inventory and invokes the real
single-instruction static AOT-step and interpreter emitters. The emitted C
includes the real CPU helpers. `a64_audit_state.h` applies the runtime's host
FPCR mapping and restores the host environment before returning to Python.
The reference is Unicorn 2.1.4, explicitly configured as Cortex-A57 at EL0
with FP access enabled. Capstone supplies names only.

Each executed word is compared through both emitters. Checks cover X0-X30/SP,
all 32 complete vector registers, NZCV, FPCR, FPSR and PC. Memory instructions
also compare the 64 KiB data arena. The mapped code image is shared with the
native bridge so literal loads read identical bytes.

The corpus includes decoder-table variants, integer boundaries, SIMD
opcode/size/Q/U dispatch combinations, signed zeros, QNaNs, SNaNs, infinities,
subnormals and extreme finite values. It also probes invalid/later encodings,
selected aliases, structure writeback and V31 register-list wrapping. Odd
exclusive-store states prime a reservation, while even states test failure.
This is not a multi-PE reservation or memory-ordering test.

The default 48 states use FPCR=0. At 1,536 states, edge patterns repeat under
all 32 RMode/FZ/DN/AHP combinations. These are correlated directed operands,
not an exhaustive Cartesian product or a random exploration of every source
operand. Seeded random values in otherwise untouched registers help detect
unintended writes. More states do not imply exhaustive immediate, condition,
addressing-mode or lane coverage. The earlier [FP-specific harness](a64_fp.md)
provides a separate, much denser rounding/min/max/comparison corpus.

Native compilation uses GNU C11, `-O2`, `-fno-strict-aliasing`, `-fwrapv` and
`-march=native -mtune=native`. `--portable` omits the last two flags.
`--strict-c` omits `-fwrapv`. `--sanitize` enables undefined-behavior traps but
disables alignment instrumentation, so it is not an alignment certification.
Different native/FMA compiler paths can produce different FP mismatch counts.

## Report format and retained evidence

`tests/a64_a57_results.json` is a compact snapshot of the completed runs. It
contains full-report hashes, per-family counts, failing words in the broad
native run and representative full counterexamples. Re-running with `--report`
produces the complete per-word evidence rather than just this summary.

Counterexample arrays use these indices:

- 0-31: X0-X30/SP.
- 32-95: low/high 64-bit halves of V0-V31.
- 96: NZCV nibble.
- 97: FPCR.
- 98: FPSR.
- 99: next PC.

Values are raw hexadecimal bits. `delta` lists differing indices, `memory`,
`unexpected_UNDEF`, or `accepted_oracle_rejection`. An oracle rejection is an
observation, not automatically proof of an architectural violation. Consult
allocation pseudocode and constrained-unpredictable rules before changing code.
Runtime/system callback families are excluded from dynamic execution here.
Their source-reviewed status is recorded in DIVERGENCES.md.

## Instruction ledger

Download the official DDI0487A.k PDF linked in DIVERGENCES.md, verify its hash,
and install `pymupdf` locally to regenerate the ledger:

```sh
.venv/bin/pip install pymupdf
.venv/bin/python tests/a64_audit_catalogue.py DDI0487A_k_armv8_arm.pdf audit.json \
  docs/A57_INSTRUCTION_AUDIT.tsv
```

All 575 instruction-description entries are indexed. The status deliberately
refers to mnemonic-family evidence, including canonical aliases, rather than
claiming every form in a manual section was tested. The PDFs are not bundled.

The audit does not validate gameplay, full-system exception delivery, the MMU,
concurrency, timing, privileged state or physical A57 behavior. These limits
remain even when an individual instruction subset passes.
