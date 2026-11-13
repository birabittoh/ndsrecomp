// bank_hidden_visibility.h -- force-included (-include) into the generated
// banks where the runner is a shared library (Android's libnds_runner.so).
//
// There -fPIC makes clang ignore -fdirect-access-external-data, so every
// runtime global a bank touches per guest instruction (g_cpu,
// g_runtime_cycles, g_nds_fast_limit, g_insn_count, ...) is reached through
// a GOT load, and huge bank bodies reload those entries over and over.
// Declaring everything after this point hidden tells the compiler the
// symbols are defined in the same library: globals are then addressed
// PC-relative and calls branch directly. The linker merges the visibility
// onto the runner's definitions, so they leave the library's export table;
// nothing on Android looks them up by name (no live-overlay shards there).
//
// The libc headers the banks use come first so their declarations keep
// default visibility and still bind to libc.
#ifndef NDS_BANK_HIDDEN_VISIBILITY_H
#define NDS_BANK_HIDDEN_VISIBILITY_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#pragma GCC visibility push(hidden)
#endif
