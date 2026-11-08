# Performance HLE architecture

The governing policy is [recomp-template/HLE.md](../../recomp-template/HLE.md):
keep a functioning LLE implementation, select implementations at build time,
and preserve the caller-facing contract while allowing substantial shortcuts
inside HLE. Small practically unnoticeable differences are permitted for
substantial measured gains. Exact internal timing and state are not universal
HLE requirements.

## Boundaries and selection

Several LLE routines or devices may map to one HLE service. Choose a boundary
that removes the actual cost: setup, dispatch, polling, transfer and completion
can belong to one native operation. Its inputs, results, required memory,
callbacks, interrupts and completion ordering remain its contract. Never skip
required work or return fabricated success.

The developer chooses LLE or HLE when configuring and rebuilding. Maintain the
LLE configuration in the current source tree. No master runtime accuracy toggle,
automatic HLE/LLE switching, mixed-mode comparison or live state conversion is
required. Existing renderer/debug controls are independent existing interfaces;
they do not establish a requirement for future HLE runtime switches.

Current build choices:

- `NDS_MPH_MATH_IMPLEMENTATION=LLE|HLE`: three MPH vector operations.
- `NDS_MKDS_IMPLEMENTATION=LLE|HLE`: integrated MKDS vector operations.
- `NDS_MPH_SUBMIT_IMPLEMENTATION=LLE|HLE`: experimental MPH native DMA setup and
  resumable geometry submission, contiguous transfers and decoded-command reuse
  (`mph-submit-hle-v2`). The SERVER canary shows a material observed gain;
  LLE remains the supported default pending owner playtest feedback. See
  [the contract and qualification limits](docs/mph-submit-hle.md).
- `nds_recompile --hooks` + `NDS_TITLE_HOOK_SOURCES`: title-owned calls baked
  into generated banks at chosen instructions. Generating without `--hooks` is
  the LLE build. See [mid-instruction hooks](docs/midasm-hooks.md).

## Binding and continuations

A bare PC is not a code identity. Scope title services to the supported ROM,
CPU, mode and fingerprinted resident code, including literals and dependencies
owned by the replacement. Use the existing native cache/page-generation and
active execution guards. A generated entry may prove only a short basic block;
expand the HLE proof to its complete required range before caching/direct linking.
Rejected bindings must also observe that range so repaired/replaced code can
be reconsidered. Never edit generated C by hand.

A resumable native service may use guest state or serializable private state.
Define its continuation and interruption model; do not leave an operation
half-complete in host stack locals across a scheduler unwind. Cross-build
savestate compatibility is not implied. The existing straight-line leaf heat
profiler remains useful instrumentation, not a restriction on HLE scope.

## Determinism and online services

Simulation completion is driven by deterministic guest-time rules. Host clock,
thread scheduling and GPU readiness cannot decide guest-visible results or
completion order. Model the synchronization required by ARM9, ARM7, memory,
DMA, interrupts, audio and Wi-Fi; do not widen scheduler slices indiscriminately.
Presentation work may differ only while it cannot affect guest readback or
simulation. Preserve ordinary Wiimmfi wire compatibility with existing peers.
Where a recomp-controlled session protocol exists, exchange capabilities and
agree on a qualified common simulation profile once before play; do not inject
a new mandatory handshake into the DS protocol or claim a handshake exists
where none is implemented. A shared build string or same-host check does not
prove cross-build deterministic compatibility.

## Validation and promotion

Reuse existing evidence. Compare meaningful outputs and completion at the
promised boundary, exactly where exactness is promised and within documented
bounds where approximation is intentional. Isolate differential fixtures;
shipping binaries need not execute both implementations.

Follow AGENTS.md's small run budget: one matched performance pair on equivalent
useful gameplay, repeat only for a specific defect/noise question, one basic
visual sanity inspection, then normal-audio owner playtest after a material gain.
Do not anchor HLE performance to retired instructions: HLE intentionally removes
them. Record simulated frame progress, wall time, whole-process work, build and
scenario identity, and actual service hits. Build/check the LLE configuration.
Keep neutral or regressing candidates draft. Online compatibility and
cross-build determinism require their own evidence; offline speed is not proof.
