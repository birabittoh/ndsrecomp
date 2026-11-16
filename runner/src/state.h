// state.h — runner-internal shared state.
//
// The recompiled banks call the C ABI in runtime_arm.h; this header is
// the C++-side shared state between the runtime, the bus, and CP15.

#pragma once

#include <cstdint>

#include "runtime_arm.h"

// Runner-private native read mappings must refresh on backing replacement,
// remap or restore. Generated banks do not use this implementation detail.
extern "C" uint64_t g_busf_mapping_generation;

// Which core is currently executing (NdsCpu / g_nds_active — declared in
// runtime_arm.h, the ABI boundary generated banks also see, so there is
// one definition shared by C++ runtime code and C generated code). The
// bus view (TCM, per-CPU I/O) and the exception-vector base branch on
// this. Set by the scheduler; the first bring-up slice runs ARM9 only.

// Terminal-halt signal for the active CPU (dispatch miss / unlowered op):
// the emitted per-instruction prologue checks runtime_should_yield(), so
// setting this unwinds the host call stack back to the scheduler and the
// CPU is not resumed. (Cooperative slice preemption is separate — it fires
// only at backward branches via runtime_slice_yield(), where the resume PC
// is a dispatch entry.)
extern bool g_nds_terminal;
extern const char* g_nds_halt_reason;
// Opt-in coverage-discovery build mode: immutable BIOS misses may execute via
// the guest-byte interpreter while their branch targets are logged for static
// promotion. Normal/release execution keeps this false and misses stay fatal.
extern bool g_discover_static_misses;

// Forced-interpreter measurement selector (beads-yjp.42). When true every
// non-BIOS static/live dispatch lookup is reported as a miss, so all game and
// captured-firmware code executes through Tier 3 on BOTH CPUs. This is the
// "candidate path forced on" leg of the one-binary selector contract
// (docs/host_optimization_strategy.md): faithful (false) and forced (true)
// live in the same build, and the harness verifies which one ran.
//
// BIOS banks deliberately stay native. Tier 3 can only execute bytes with
// write provenance in writable RAM; the immutable BIOS ROM has neither, so
// forcing it would produce a fatal dispatch miss rather than a measurement.
// It is also the representative shape: a player who loses native game banks
// still has native BIOS.
extern bool g_nds_force_tier3;
// Cumulative count of lookups the selector converted into a miss. Zero in
// faithful mode is what proves the selector was off.
extern unsigned long long g_nds_force_tier3_misses;

// CP15 (ARM9 system-control) state the bus must honor — TCM placement
// changes what an address means, so the bus reads this directly.
struct Cp15State {
    uint32_t control;          // c1,c0,0 control register (raw)
    bool     high_vectors;     // control bit 13: exceptions at 0xFFFF0000
    bool     itcm_enable;      // control bit 18
    bool     dtcm_enable;      // control bit 16
    uint32_t itcm_size;        // bytes (base is always 0)
    uint32_t dtcm_base;        // region base, 4 KB-aligned
    uint32_t dtcm_size;        // bytes
};
extern Cp15State g_cp15;
// g_cp15_timing_generation is declared in recompiler/armv4t/runtime_arm.h
// (with C linkage): generated code reads it to validate the published ARM9
// code-fetch class — see NDS_ARM9_CODE_K / nds_code_numc there. Declaring it
// here as well would give the same object two language linkages.
uint32_t cp15_debug_mpu_region(unsigned index);
uint32_t cp15_debug_cache_cfg(unsigned index);

void cp15_reset();
// True if ARM9 code fetches from addr are I-cache-served (per-PU-region, C1 bit12).
bool cp15_code_cacheable(uint32_t addr);
bool cp15_data_cacheable(uint32_t addr);
// The original per-access MPU region walks (see cp15.cpp). Kept as the
// fallback for region encodings the page bitmap cannot represent and as
// the oracle runner/tests/mem_timing_test.cpp sweeps the bitmap against.
bool cp15_code_cacheable_reference(uint32_t addr);
bool cp15_data_cacheable_reference(uint32_t addr);
// Force a cacheability-bitmap resync (tests only; production syncs from
// the CP15 write / reset / savestate-import paths).
void cp15_class_rebuild_for_test();

// ── Dispatch + run control (implemented in runtime_arm.cpp) ─────────────
// Layout-compatible with the generated <bank>_dispatch.c table entries.
using DispatchEntry = NdsDispatchEntry;

extern "C" void nds_register_dispatch(int cpu, const DispatchEntry* t,
                                      unsigned len, uint32_t exc_base);
extern "C" void nds_unregister_dispatch(int cpu, const DispatchEntry* t,
                                        unsigned len);
// Both registration calls are emulation-thread-only (see the convention
// comment in runtime_arm.cpp). Debug builds pin the first caller's thread;
// call this to re-pin if emulation ever moves to another thread.
extern "C" void nds_dispatch_bind_thread(void);
extern "C" void nds_set_cycle_cap(unsigned long long cap);
extern "C" void nds_reschedule_slice(unsigned long long system_deadline);
extern "C" void nds_halt(const char* reason);
// Begin a scheduler slice: clear yield/terminal and arm the cycle cap.
extern "C" void nds_slice_begin(unsigned long long cap);
extern "C" int nds_slice_over(void);
// Clear the slice-yield unwind flag (scheduler calls this before each
// dispatch so normal returns aren't mistaken for a preemption unwind).
extern "C" void nds_clear_unwinding(void);

// Optional diagnostic sources; addresses remain stable across CPU slice swaps.
struct NdsGuestJobSources;
NdsGuestJobSources runtime_guest_job_profile_sources();
// Tier-3 can be nested under a static BIOS IRQ dispatch. An exact-index stop
// must unwind those host frames without allowing their saved continuation PC
// to overwrite the interpreted state at which observation stopped.
void nds_preserve_unwind_state();
void nds_restore_unwind_state();

// Re-arm the per-instruction cycle deadline (beads-yjp.42): force the next
// poll back onto the faithful path. See recompiler/armv4t/runtime_arm.h for
// the contract and runner/tests/test_machinery_perf_guards.py for the list
// of sites obliged to call this.
extern "C" void runtime_clear_fast_limit(void);

// Bus lifecycle / image loading (implemented in bus.cpp).
void bus_init();
void bus_debug_history_reset();
void bus_load_arm9_bios(const uint8_t* p, uint32_t n);
void bus_load_arm7_bios(const uint8_t* p, uint32_t n);
void bus_dump_access_ring(uint32_t max_entries);

struct BusRegion {
    const uint8_t* ptr;
    uint32_t len;
};
bool bus_get_region(const char* name, BusRegion* out);
uint8_t bus_debug_read8(int cpu, uint32_t addr);
// ARM7-owned hardware bus masters (currently the SPU fetch/capture engines)
// access memory outside a guest instruction. These helpers select the ARM7
// memory view for the duration of the transfer and still feed the always-on
// bus ring, independent of which CPU the scheduler last loaded.
uint32_t bus_device_read32(int cpu, uint32_t addr);
void bus_device_write32(int cpu, uint32_t addr, uint32_t value);
void bus_device_write16(int cpu, uint32_t addr, uint16_t value);
// Overlay bytes onto the loaded ARM9 BIOS image (direct boot on a
// reimplemented BIOS copies the cartridge's Nintendo logo in at +0x20).
void bus_patch_arm9_bios(uint32_t offset, const uint8_t* p, uint32_t n);
// Tier-3 provenance. A writable mapping alone is not permission to interpret:
// its physical backing page must have been written by a guest CPU or hardware
// bus master since reset. Aliases share the same backing generation.
bool bus_addr_is_writable_ram(uint32_t addr);
bool bus_addr_has_write_provenance(uint32_t addr);
bool bus_range_has_write_provenance(uint32_t addr, uint32_t size);
uint32_t bus_exec_page_generation(uint32_t addr);
// True while a DMA channel performs its bus access. The ARM9 TCMs are
// core-local (melonDS: DMA goes through the bus, not ARMv5's TCM path), so
// resolve() skips them; the inline windows are hidden by BusDmaScope.
extern bool g_bus_dma_master;
struct BusDmaScope {
    uint32_t itcm_limit = 0u, dtcm_limit = 0u;
    bool active = false;
    explicit BusDmaScope(int cpu);
    ~BusDmaScope();
    BusDmaScope(const BusDmaScope&) = delete;
    BusDmaScope& operator=(const BusDmaScope&) = delete;
};
// The page-generation cell bus_exec_page_generation(addr) reads when addr
// resolves to a code-tracked fast window (main RAM, ITCM, ARM7 WRAM) under
// the current mapping, else nullptr.
const uint32_t* bus_main_page_generation_cell(uint32_t addr);
// Exact code-write tracking for main RAM, ITCM and ARM7 WRAM
// (NdsBusFastWin::code_mask). A cached snapshot that depends on bytes of the
// 4 KiB virtual page `page` marks the 64-byte blocks it covers and
// snapshots the returned code generation cell. Only stores that touch a
// marked block bump that cell. Returns nullptr when `page` does not resolve
// to a tracked backing under the active CPU's current mapping; such
// snapshots keep the plain page generation (bus_exec_page_generation).
struct NdsBusFastWin;
uint32_t* bus_code_watch(uint32_t page, uint64_t blocks);
// Watch the whole physical page behind `addr` (any RAM backing): every
// store to it publishes (runtime_note_code_store). For snapshots and caches
// of untracked pages, which compare plain page generations but rely on
// publication for the active static guard and the coverage epoch.
void bus_code_watch_page(uint32_t addr);
// Store [offset, offset + width) through tracked window `w` (offsets
// relative to the window): bump the code generation of every page whose
// marked blocks it overlaps. Returns the first bumped cell and the number
// of consecutive cells, or nullptr.
uint32_t* bus_code_store(const NdsBusFastWin* w, uint32_t offset,
                         uint32_t width, uint32_t* count);
// Map tracked page-generation cells to their code generation cells and
// bump those unconditionally (writers that report only pages: older shards).
uint32_t* bus_code_store_pages(const uint32_t* page_gen, uint32_t count);
// An unlocated write (backing replacement, savestate load): bump every
// main-RAM code generation.
void bus_code_store_all();
// NDS_CODE_STORE_REPORT diagnostic: a store published by an untracked window.
struct NdsBusFastWin;
void bus_code_diag_untracked(const NdsBusFastWin* w);
// (runtime_note_code_write moved to runtime_arm.h with C linkage — the
// inline bus fast path's write side calls it from generated C banks.)
// Recompute the inline fast-map windows (runtime_arm.h) from the current
// mapping inputs. Call after anything that changes what an address means:
// bus_init, WRAMCNT writes, CP15 control/TCM writes.
void bus_fast_refresh();
// Compare generated source bytes with the active CPU's live executable view
// without producing bus events or consuming guest cycles.
bool bus_live_bytes_equal(uint32_t addr, const uint8_t* expected,
                          uint32_t size);
// Bulk diagnostic read: one resolve per aligned page instead of one per byte.
// Returns false when any part fell back to the per-byte path (VRAM/IO/unmapped).
bool bus_debug_copy(int cpu, uint32_t addr, uint8_t* dst, uint32_t size);
struct BusExecProvenance {
    bool writable;
    bool written;
    uint32_t generation;
};
BusExecProvenance bus_debug_exec_provenance(int cpu, uint32_t addr);

struct BusWatchEvent {
    uint64_t seq;
    uint64_t cycles;
    uint64_t insn;
    uint8_t cpu;
    uint8_t write;
    uint8_t width;
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
};
uint32_t bus_debug_watch_copy(BusWatchEvent* out, uint32_t max_entries);
