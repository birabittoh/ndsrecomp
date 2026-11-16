// title_hooks.h — ABI for handlers of hooks baked into generated banks by
// `nds_recompile --hooks <manifest>`. See docs/midasm-hooks.md.
//
// A handler is a plain C-linkage function named by the manifest's `name`.
// Its parameters are one `uint32_t*` per entry of `registers`, in order,
// pointing straight at the live guest register (`&g_cpu.R[n]`, or
// `&g_cpu.cpsr` for "cpsr"). It returns `bool` when the hook declares
// `on_true` / `on_false`, otherwise `void`:
//
//     NDS_TITLE_HOOK void Dq9Example(uint32_t* r0, uint32_t* lr);
//     NDS_TITLE_HOOK bool Dq9ExampleSkip(uint32_t* r1);
//
// The generated bank declares the prototype from the manifest. C linkage
// carries no types, so a handler whose signature disagrees still links and
// then misbehaves: keep each handler next to its manifest entry. A handler
// that is missing altogether fails the runner link.
//
// Contract (the generated call site relies on all of these):
//   * Run on the CPU that owns the bank, at the instruction boundary the
//     manifest names, exactly once per executed instance of that
//     instruction. g_cpu.R[15] holds the hooked PC (before) or the next PC
//     (after).
//   * Do not tick, yield, dispatch, raise exceptions or call into the
//     scheduler. A handler is a pure function of guest state: it may read
//     and write guest registers, and guest RAM through bus_read_u* /
//     bus_write_u* (I/O registers have side effects; leave them alone), but
//     must keep no host-side state that changes guest behaviour, or
//     savestates and replays stop being deterministic.
//   * Through the cpsr pointer, change at most N/Z/C/V/Q. Mode and T bits
//     drive register banking and instruction decoding and are not
//     re-derived after the call.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "runtime_arm.h"

#ifdef __cplusplus
#define NDS_TITLE_HOOK extern "C"
#else
#define NDS_TITLE_HOOK
#endif
