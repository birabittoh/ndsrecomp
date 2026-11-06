// tier3_absent.cpp — Tier 3 for a build with no interpreter (NDS_INTERPRETER=OFF).
//
// Stands in for tier3.cpp so a title can prove it needs no interpreter: nothing
// here can execute a guest instruction, and armv4t::Interpreter is not linked at
// all. Code with no live bank is then a hard failure that names the address
// instead of something that quietly runs slower.

#include "tier3.h"

#include <cstdio>

#include "runtime_arm.h"
#include "scheduler.h"

namespace {

Tier3Stats g_stats{};

void no_interpreter(uint32_t pc) {
    std::fprintf(stderr,
                 "[no-interpreter] no live bank for pc=0x%08X; this build "
                 "cannot interpret it\n", pc);
    runtime_dispatch_miss(pc);
    // One CPU stopping alone would leave the other spinning on a frozen game.
    scheduler_terminal_halt_all("no live bank and no interpreter");
}

}  // namespace

void tier3_run(uint32_t pc) { no_interpreter(pc); }
void tier3_step_one(uint32_t pc) { no_interpreter(pc); }
bool tier3_canonical_state_enabled() { return false; }

uint32_t tier3_debug_trace_copy(Tier3TraceEvent*, uint32_t) { return 0u; }
void tier3_reset() { g_stats = {}; }
void tier3_note_clean_ram_reject() {}
Tier3Stats tier3_stats() { return g_stats; }
uint32_t tier3_coverage_copy(Tier3CoverageEntry*, uint32_t) { return 0u; }
