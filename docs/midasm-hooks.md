# Mid-instruction hooks

A hook manifest tells `nds_recompile` to call a host function at a guest
instruction boundary, and optionally to return or branch based on it. The
call is baked into the generated bank, so a hook costs one direct C call and
nothing when absent. It is the primitive for title performance hacks
(replace a hot sequence) and for observing or changing guest state at a known
point.

    nds_recompile ... --bank dq9_arm9_ov003 --hooks dq9_arm9_ov003.toml

The runner links the handlers from `NDS_TITLE_HOOK_SOURCES` (semicolon list
of absolute paths, compiled into `nds_runner` only). Handlers include
`runner/src/title_hooks.h`.

## Manifest

One manifest per bank. Unknown keys, duplicates and contradictory outcomes
fail closed, as in the HLE profile manifest.

```toml
version = 1

[program]
bank = "dq9_arm9_ov003"                         # must equal --bank
sha1 = "bc2b5555cffa43f0d97ee51a8aa30bf32c6edaa5" # SHA-1 of the --bin image

[[hook]]
address = 0x0216CC70
mode = "arm"                 # "arm" | "thumb": the same address decodes differently
name = "Dq9ExampleObserve"   # C identifier of the handler
registers = ["r0", "r10"]    # optional; r0..r14, sp, lr, cpsr. No r15/pc.
when = "before"              # optional; "before" (default) | "after"
action = "continue"          # optional; "continue" (default) | "return" | "jump"
note = "free text"

[[hook]]
address = 0x0217F914
mode = "thumb"
name = "Dq9ExampleSkip"
registers = ["r2"]
on_true = "jump"             # on_true / on_false make the handler return bool;
jump_address = 0x0217F920    # each defaults to "continue"
```

Rules the loader enforces:

- `address` and `jump_address` are aligned to the instruction width.
- `action` is exclusive with `on_true` / `on_false`; the two outcomes must
  differ, at most one may jump, and `jump_address` is present exactly when
  some outcome jumps.
- At most one `before` and one `after` hook per (address, mode). Several
  things at one point go in one handler, so their order is explicit.
- A handler name used twice must have the same arity and return type.
- An `after` hook may only continue.

Rules the emitter enforces, against the functions it is about to emit:

- `bank` and `sha1` match `--bank` and the verified image.
- `address` is an instruction start of an emitted function of that mode. An
  address the finder does not reach needs an `[[entry_point]]` first.
- Its dispatch row is not pruned by `--prune-preceding-owned` (the preceding
  bank would own entries there, without the hook).
- An `after` hook does not follow a branch, call or PC write.
- `jump_address` is an emitted instruction of the same mode in this bank.
- `--hooks` requires body emission and is refused with `--flat-execution`.

## Handler ABI

```cpp
#include "title_hooks.h"
NDS_TITLE_HOOK void Dq9ExampleObserve(uint32_t* r0, uint32_t* r10);
NDS_TITLE_HOOK bool Dq9ExampleSkip(uint32_t* r2);
```

One `uint32_t*` per `registers` entry, in order, pointing at the live guest
register (`&g_cpu.R[n]`, `&g_cpu.cpsr`). Writes are guest-visible. The
generated bank declares the prototype from the manifest; C linkage carries no
types, so keep handler and manifest entry in sync. A missing handler fails
the runner link.

A handler must not tick, yield, dispatch or enter the scheduler, and should
change at most N/Z/C/V/Q through `cpsr` (mode and T are not re-derived after
the call). It must be a function of guest state only: host-side state that
changes guest behaviour breaks savestates and replay determinism.

## Semantics

Every generated instruction begins with
`g_cpu.R[15] = pc; if (runtime_should_yield()) return;` and its `L_<pc>` label
is also where a preempted body resumes.

- **before** is emitted after the label, behind its own copy of that yield
  check. A yield there returns before the handler runs; the resume enters the
  label and checks again. The handler therefore runs exactly once per
  executed instance of the instruction, with `R15 = pc`. The instruction's own
  check that follows sees the same scheduler state, since a handler does not
  tick. A hooked conditional instruction calls the handler whether or not its
  condition passes.
- **after** is emitted after the instruction's tick and unwind check and
  before the next label, so a resume at the next PC does not re-run it. It
  sees `R15 = pc + width`. It runs on both outcomes of a conditional
  instruction.
- **return** lowers a synthetic `bx lr` at the hooked PC through the normal
  instruction emitter: interworking from LR bit 0, the host call-stack return
  check, dispatch otherwise. It is only meaningful where LR still holds the
  return address, i.e. at a function entry before LR is spilled.
- **jump** lowers a synthetic `b jump_address` the same way: a local `goto`
  inside the function or superblock, otherwise a linked dispatch. B keeps the
  instruction set, so the target is in the hook's mode.

Timing: the synthetic transfer charges what that `bx`/`b` would. Skipped
guest instructions are not charged, and the hooked instruction is not charged
when control leaves before it. This is the HLE policy's "small differences
for measured gains"; a hook that skips work owns the caller-visible contract.

## Interaction with other passes

- Superblocks (`--coalesce-fallthroughs`) need nothing special: members are
  emitted by the same per-instruction loop.
- A tiny leaf containing a hook is not inlined at its call sites (the copy
  would bypass the hook). The emitter reports how many leaves this kept out
  of line.
- Each body shard declares only the handlers it calls, so with
  `--stable-address-shards` an edited hook rewrites only the shards that
  contain hooked instructions. A title build that stamps on the argument
  files (DQ9's `gen_bank.py`) regenerates only that bank.
- Without `--hooks` the emitted bytes are unchanged; that build is the LLE
  reference a hook is measured against.

## Binding and limits

A hook is a statement about guest bytes. The manifest binds it to the bank
name and image SHA-1, and the bank's rows are already gated by ROM SHA-1 and
(with `--validate-live-bytes`) by the live bytes of each function, so a hook
only runs while the exact code it was written against is resident. This is
the "a bare PC is not a code identity" rule of `HLE_ARCHITECTURE.md`.

Not covered: code that does not run from a generated bank — the live-overlay
tier, the bounded dirty-RAM interpreter, and live shards compiled by the
runtime (they are built without the manifest). Code a bank emits at a
relocated address (`[[code_copy]]`, relocation-scan copies) is hooked by its
run-time address, like any other emitted function.

Tests: `recompiler/tests/hook_manifest_test.cpp` (loader) and
`recompiler/tests/test_hooks.py` (emitter checks, inline-leaf exclusion, and
execution of a hooked ARM/Thumb fixture including a forced yield at a hooked
PC).
