// function_finder.cpp — implementation.
//
// Discovery is a prioritized worklist:
//   - Pop (addr, mode) from the worklist.
//   - If already visited, skip.
//   - Decode instructions from addr forward, in `mode`.
//   - For each direct branch (B / BL / conditional B), record the
//     target and add it to the worklist as a NEW function root near
//     the current function (the
//     finder is intentionally pessimistic — every branch target is
//     treated as a potential function entry; the codegen later
//     consolidates).
//   - For BX/BLX-reg/computed: mark `has_indirect_transfer` and stop
//     this function's body.
//   - Stop at: undefined, return-shaped instr (BX LR / pop {pc} /
//     ldm rN, {..., pc}), or after a hard cap (4096 instrs per
//     function as a sanity bound).

#include "function_finder.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

namespace ndsrecomp {

namespace {

uint64_t visit_key(uint32_t addr, CpuMode mode) {
    return (static_cast<uint64_t>(addr) << 1) |
           static_cast<uint64_t>(mode);
}

std::string default_name(uint32_t addr, CpuMode mode) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s_%08X",
                  mode == CpuMode::Thumb ? "tfunc" : "afunc",
                  addr);
    return std::string(buf);
}

uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    if (n == 0) return v;
    return (v >> n) | (v << (32u - n));
}

// Diagnostic toggle: GBARECOMP_NO_JT=1 disables automatic jump-table
// detection, so a run can isolate the detector's cost from the baseline
// finder. Read once.
bool jump_table_detection_enabled() {
    static const bool enabled = (std::getenv("GBARECOMP_NO_JT") == nullptr);
    return enabled;
}

// Diagnostic toggle: GBARECOMP_JT_REPORT=1 prints, to stderr, every
// confirmed jump table and the emitter's decision (emitted N / rejected
// why). Used to measure how many confirmed tables the count-terminator
// actually emits — the MC-HP-000 coverage metric. Read once.
bool jt_report_enabled() {
    static const bool enabled = (std::getenv("GBARECOMP_JT_REPORT") != nullptr);
    return enabled;
}

// Diagnostic toggle: NDSRECOMP_INDIRECT_REPORT=1 prints, to stderr, every
// non-return indirect transfer the walk reached and its resolution outcome
// (seeded / known-but-unreachable / unknown, with the tracked value when
// one exists). This is the measurement surface for the reachability work
// (beads-yjp.35): the unknown sites are the analysis gap, the unreachable
// sites are the config gap. Read once.
bool indirect_report_enabled() {
    static const bool enabled =
        (std::getenv("NDSRECOMP_INDIRECT_REPORT") != nullptr);
    return enabled;
}

// Does `i` write its decoded `rd` slot? The decoder zero-fills unused
// slots, so compares, stores, branches etc. carry rd == 0; treating that
// as a write made the constant tracker forget r0 at every CMP/STR/B.
bool writes_rd(const armv4t::Instr& i) {
    using armv4t::IrOp;
    switch (i.op) {
        case IrOp::TST: case IrOp::TEQ: case IrOp::CMP: case IrOp::CMN:
        case IrOp::B: case IrOp::BL: case IrOp::BX: case IrOp::BLX_reg:
        case IrOp::BLX_imm: case IrOp::BL_prefix: case IrOp::BL_suffix:
        case IrOp::STR: case IrOp::STRB: case IrOp::STRH: case IrOp::STRD:
        case IrOp::LDM: case IrOp::STM: case IrOp::PLD: case IrOp::SWI:
        case IrOp::MCR: case IrOp::CDP: case IrOp::MSR:
        case IrOp::Undefined:
            return false;
        default:
            return true;
    }
}

// Registers an instruction writes besides `rd` (and besides LDM lists,
// writeback bases and PC), as a bitmask.
uint16_t extra_written_regs(const armv4t::Instr& i) {
    using armv4t::IrOp;
    uint16_t m = 0;
    switch (i.op) {
        case IrOp::UMULL: case IrOp::UMLAL: case IrOp::SMULL:
        case IrOp::SMLAL: case IrOp::SMLALxy:
            if (i.rn < 16) m |= static_cast<uint16_t>(1u << i.rn);  // RdLo
            break;
        case IrOp::LDRD:
            if (i.rd < 15) m |= static_cast<uint16_t>(1u << (i.rd + 1));
            break;
        default:
            break;
    }
    // Calls write LR (THUMB BL writes it in both halves).
    if (i.is_call || i.op == IrOp::BL_prefix || i.op == IrOp::BL_suffix) {
        m |= static_cast<uint16_t>(1u << 14);
    }
    return m;
}

}  // namespace

FunctionFinder::FunctionFinder(const uint8_t* rom_bytes,
                                std::size_t rom_size,
                                uint32_t rom_base)
    : rom_(rom_bytes), rom_size_(rom_size), rom_base_(rom_base) {}

void FunctionFinder::add_seed(const FunctionSeed& seed) {
    seeds_.push_back(seed);
}

void FunctionFinder::add_data_range(uint32_t start, uint32_t end,
                                     const std::string& note) {
    data_ranges_.push_back(DataRange{start, end, note});
}

void FunctionFinder::add_code_copy(uint32_t runtime_start,
                                    uint32_t source_start,
                                    uint32_t size,
                                    const std::string& note) {
    code_copies_.push_back(CodeCopyRange{
        runtime_start, source_start, size, note});
}

void FunctionFinder::add_exclude(uint32_t addr,
                                  const std::string& reason) {
    excluded_.insert(addr);
    exclude_reasons_[addr] = reason;
}

bool FunctionFinder::map_addr_to_source(uint32_t addr,
                                         uint32_t* source) const {
    if (addr_in_rom(addr)) {
        *source = addr;
        return true;
    }
    for (const auto& cc : code_copies_) {
        if (addr >= cc.runtime_start &&
            addr - cc.runtime_start < cc.size) {
            uint32_t mapped = cc.source_start + (addr - cc.runtime_start);
            if (!addr_in_rom(mapped)) return false;
            *source = mapped;
            return true;
        }
    }
    return false;
}

bool FunctionFinder::can_read_at(uint32_t addr, uint32_t len) const {
    uint32_t source = 0;
    if (!map_addr_to_source(addr, &source)) return false;
    return (source - rom_base_ + len) <= rom_size_;
}

bool FunctionFinder::addr_in_data_range(uint32_t addr) const {
    for (const auto& dr : data_ranges_) {
        if (addr >= dr.start && addr < dr.end) return true;
    }
    return false;
}

const DataRange* FunctionFinder::find_data_range(uint32_t addr) const {
    for (const auto& dr : data_ranges_) {
        if (addr >= dr.start && addr < dr.end) return &dr;
    }
    return nullptr;
}

void FunctionFinder::record_collision(uint32_t target,
                                       uint32_t origin_addr,
                                       const std::string& origin_name,
                                       const std::string& origin_kind) {
    DataRangeCollision col{};
    col.flow_target_addr  = target;
    col.flow_origin_addr  = origin_addr;
    col.flow_origin_name  = origin_name;
    col.flow_origin_kind  = origin_kind;
    if (const DataRange* dr = find_data_range(target)) {
        col.range_start = dr->start;
        col.range_end   = dr->end;
        col.range_note  = dr->note;
    }
    collisions_.push_back(std::move(col));
}

uint32_t FunctionFinder::read_u32(uint32_t addr) const {
    uint32_t source = addr;
    map_addr_to_source(addr, &source);
    std::size_t off = source - rom_base_;
    return  static_cast<uint32_t>(rom_[off])
         | (static_cast<uint32_t>(rom_[off + 1]) << 8)
         | (static_cast<uint32_t>(rom_[off + 2]) << 16)
         | (static_cast<uint32_t>(rom_[off + 3]) << 24);
}

uint32_t FunctionFinder::read_u32_public(uint32_t addr) const {
    if (!can_read_at(addr, 4)) return 0;
    return read_u32(addr);
}

uint16_t FunctionFinder::read_u16(uint32_t addr) const {
    uint32_t source = addr;
    map_addr_to_source(addr, &source);
    std::size_t off = source - rom_base_;
    return  static_cast<uint16_t>(rom_[off])
         | (static_cast<uint16_t>(rom_[off + 1]) << 8);
}

void FunctionFinder::discover_one(uint32_t entry_addr, CpuMode entry_mode,
                                   const std::string& seed_name,
                                   uint32_t seed_source_addr,
                                   uint32_t exact_size) {
    uint32_t entry_source = 0;
    if (seed_source_addr != 0) {
        entry_source = seed_source_addr;
        if (!addr_in_rom(entry_source)) return;
    } else if (!map_addr_to_source(entry_addr, &entry_source)) {
        return;
    }
    uint64_t key = visit_key(entry_addr, entry_mode);
    if (visited_.find(key) != visited_.end()) return;

    Function fn{};
    fn.addr = entry_addr;
    fn.source_addr = entry_source;
    fn.mode = entry_mode;
    fn.name = seed_name.empty() ? default_name(entry_addr, entry_mode)
                                : seed_name;
    fn.end_addr = entry_addr;  // updated below

    visited_[key] = functions_.size();  // index will be re-sorted at end

    uint32_t pc = entry_addr;
    const uint32_t step = (entry_mode == CpuMode::Thumb) ? 2u : 4u;
    const uint64_t exact_end64 = static_cast<uint64_t>(entry_addr) + exact_size;
    if (exact_size != 0u &&
        (exact_end64 >= 0x100000000ull ||
         !can_read_at(static_cast<uint32_t>(exact_end64 - step), step))) {
        return;
    }
    const uint32_t exact_end = exact_size == 0u
        ? 0u : static_cast<uint32_t>(exact_end64);
    constexpr std::size_t kMaxInstrs = 4096;
    std::size_t count = 0;

    // Narrow intra-function constant tracker. Lets the finder
    // follow the classic ARM↔THUMB interworking pattern:
    //   add R0, PC, #1
    //   bx  R0
    // (and the THUMB equivalent: `mov Rn, pc; bx Rn`). When we see
    // BX with rm != 14 (non-return), we look up Rm in this table.
    // If it has a tracked value, we add it as a branch target
    // (with mode inferred from the low bit, like the SWI table's
    // entries_mode="auto").
    //
    // Coverage is deliberately narrow:
    //   - tracks `MOV Rn, #imm`  (clears the rest)
    //   - tracks `ADD Rn, PC, #imm`  (computes PC+8+imm for ARM,
    //     PC+4+imm for THUMB)
    //   - any other write to Rn invalidates the entry
    //   - any control-flow break (branch, call) wipes the table
    //
    // False positives are avoided by being conservative: we only
    // resolve a BX target when we're certain we saw the constant
    // load IN THIS BASIC BLOCK with no intervening write to Rm.
    struct RegConst {
        bool     known = false;
        uint32_t value = 0;
    };
    RegConst reg_const[16];
    // Registers whose tracked value must NOT be carried to a branch
    // target: clobbered by a call (AAPCS r0-r3, r12, lr) or last
    // written by a conditional instruction (the tracker models those as
    // unconditional, which is wrong on the not-taken path).
    uint16_t no_carry = 0;
    constexpr uint16_t kCallClobbered =
        0x000Fu | (1u << 12) | (1u << 14);
    // Weaker per-register lane: the static in-image address this register
    // was last loaded from (see CarriedRegs::load_*). Set by any word LDR
    // with a known non-PC base, conditional or not; cleared by any other
    // write and by calls.
    struct RegLoad {
        bool     known = false;
        uint32_t addr  = 0;
    };
    RegLoad reg_load[16];
    {
        auto it = branch_target_regs_.find(visit_key(entry_addr, entry_mode));
        if (it != branch_target_regs_.end() &&
            (it->second.mask | it->second.load_mask) != 0) {
            for (int r = 0; r < 15; ++r) {
                if (it->second.mask & (1u << r)) {
                    reg_const[r] = {true, it->second.value[r]};
                }
                if (it->second.load_mask & (1u << r)) {
                    reg_load[r] = {true, it->second.load_addr[r]};
                }
            }
            ++stats_.carried_state_walks;
        }
    }
    auto carry_state_to = [&](uint32_t target) {
        CarriedRegs snap;
        for (int r = 0; r < 15; ++r) {
            const uint16_t b = static_cast<uint16_t>(1u << r);
            if (reg_const[r].known && (no_carry & b) == 0) {
                snap.mask |= b;
                snap.value[r] = reg_const[r].value;
            }
            if (reg_load[r].known) {
                snap.load_mask |= b;
                snap.load_addr[r] = reg_load[r].addr;
            }
        }
        const uint64_t k = visit_key(target, entry_mode);
        if (visited_.count(k) != 0) return;  // already walked; first wins
        auto [it, inserted] = branch_target_regs_.emplace(k, snap);
        if (!inserted) {
            CarriedRegs& cur = it->second;
            for (int r = 0; r < 15; ++r) {
                const uint16_t b = static_cast<uint16_t>(1u << r);
                if ((cur.mask & b) &&
                    (!(snap.mask & b) || snap.value[r] != cur.value[r])) {
                    cur.mask = static_cast<uint16_t>(cur.mask & ~b);
                }
                if ((cur.load_mask & b) &&
                    (!(snap.load_mask & b) ||
                     snap.load_addr[r] != cur.load_addr[r])) {
                    cur.load_mask = static_cast<uint16_t>(cur.load_mask & ~b);
                }
            }
        }
    };

    // Jump-table address tracking (MC-HP-000). The compiler builds a
    // dispatch like:  LDR Rb,=table ; LSL Ri,Ri,#k ; ADD Ra,Ri,Rb ;
    // LDR Rt,[Ra] ; BX Rt (or BL into the bx-rN veneer). reg_const
    // already resolves Rb from its literal load; these two track the
    // scaled index and the resulting element-address register so the
    // LDR that fetches the entry can reveal the table.
    struct RegScaled {  // Rd = (unknown index) << shift
        bool     known = false;
        uint8_t  shift = 0;
        // Exact range bound carried from a `CMP index,#N; B{hi,cs}` guard
        // (max_index is the largest in-range index). Lets the jump-table
        // emitter size the table precisely instead of guessing.
        bool     bound_known = false;
        uint32_t max_index   = 0;
    };
    RegScaled reg_scaled[16];
    struct RegTableAddr {  // Rd = base + (scaled index)
        bool     known  = false;
        uint32_t base   = 0;
        uint32_t stride = 0;
        bool     bound_known = false;  // carried from RegScaled
        uint32_t max_index   = 0;
    };
    RegTableAddr reg_table[16];

    // Range bound for a register established by a `CMP Ri,#N` switch
    // guard. The bound travels with the index through the scale/add chain
    // and gives the jump-table emitter an EXACT entry count — the precise
    // terminator that replaces the old "stop at first non-prologue" walk.
    // Two guard shapes feed it: the in-block `B{hi,cs}`-to-default form
    // (bound set here as we walk past it), and the inverted
    // `B{ls,cc}`-to-dispatch form, where this walk IS the dispatch target
    // and the bound was parked in branch_target_bounds_ by the predecessor.
    struct RegBound {
        bool     known     = false;
        uint32_t max_index = 0;
    };
    RegBound reg_bound[16];
    // Inverted-guard bound parked for this seed (see header). Re-seed it
    // so the dispatch's scale->add->load chain can size the table.
    {
        auto it = branch_target_bounds_.find(entry_addr);
        if (it != branch_target_bounds_.end() && it->second.reg < 16) {
            reg_bound[it->second.reg] = {true, it->second.max_index};
        }
    }
    struct LastCmp {  // most recent `CMP Ri,#imm`, consumed by the next branch
        bool     known = false;
        uint8_t  reg   = 0;
        uint32_t imm   = 0;
        uint32_t pc    = 0;
    };
    LastCmp last_cmp;

    // A jump-table candidate awaiting confirmation. Set when an indexed
    // load fetches a would-be entry into `dest`; promoted to a real
    // table ONLY when `dest` is subsequently used as a control-flow
    // target (BX dest / MOV pc,dest / BL into a `bx dest` veneer). This
    // is what distinguishes a jump table from an ordinary indexed load
    // of a DATA pointer (which is dereferenced, not branched to) — the
    // distinction that keeps the detector from seeding garbage.
    struct PendingJt {
        bool     active      = false;
        uint32_t base        = 0;
        uint32_t stride      = 0;
        uint8_t  dest        = 0;
        // Exact entry count from the dispatcher's CMP-bound, when present.
        bool     count_known = false;
        uint32_t count       = 0;
        // Unscaled `ldr Rt,[Rb,Ri]` (index pre-multiplied by 4 in code):
        // only the base-relative `add pc,Rb,Rt` confirmation may accept it.
        bool     relative_only = false;
    };
    PendingJt pend;

    struct RegSym {
        bool    known = false;
        uint8_t rn = 0;
        int32_t offset = 0;
    };
    RegSym reg_sym[16];

    struct MemConst {
        bool     known = false;
        uint8_t  rn = 0;
        int32_t  offset = 0;
        uint32_t value = 0;
    };
    MemConst mem_const[8];

    auto clear_mem_const = [&]() {
        for (auto& slot : mem_const) {
            slot.known = false;
        }
    };

    auto invalidate_mem_base = [&](uint8_t rn) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn) {
                slot.known = false;
            }
        }
    };

    auto invalidate_sym_base = [&](uint8_t rn) {
        for (auto& sym : reg_sym) {
            if (sym.known && sym.rn == rn) {
                sym.known = false;
            }
        }
    };

    auto resolve_symbolic_base = [&](uint8_t base_reg, int32_t offset,
                                     uint8_t* rn,
                                     int32_t* resolved_offset) -> bool {
        if (base_reg >= 15) return false;
        if (reg_sym[base_reg].known) {
            *rn = reg_sym[base_reg].rn;
            *resolved_offset = reg_sym[base_reg].offset + offset;
        } else {
            *rn = base_reg;
            *resolved_offset = offset;
        }
        return true;
    };

    auto eval_symbolic_mem_addr = [&](const armv4t::Instr& i,
                                      uint8_t* rn,
                                      int32_t* offset) -> bool {
        if (i.mem.by_register || i.mem.rn >= 15) return false;
        int32_t mem_offset = 0;
        if (!i.mem.pre_indexed) {
            mem_offset = 0;
        } else {
            int32_t imm = static_cast<int32_t>(i.mem.imm_offset);
            mem_offset = i.mem.add ? imm : -imm;
        }
        return resolve_symbolic_base(i.mem.rn, mem_offset, rn, offset);
    };

    auto remember_mem_const = [&](uint8_t rn, int32_t offset,
                                  uint32_t value) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                slot.value = value;
                return;
            }
        }
        for (auto& slot : mem_const) {
            if (!slot.known) {
                slot.known = true;
                slot.rn = rn;
                slot.offset = offset;
                slot.value = value;
                return;
            }
        }
        mem_const[0].known = true;
        mem_const[0].rn = rn;
        mem_const[0].offset = offset;
        mem_const[0].value = value;
    };

    auto lookup_mem_const = [&](uint8_t rn, int32_t offset,
                                uint32_t* out) -> bool {
        for (const auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                *out = slot.value;
                return true;
            }
        }
        return false;
    };

    auto forget_mem_const = [&](uint8_t rn, int32_t offset) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                slot.known = false;
            }
        }
    };

    auto reg_list_count = [](uint16_t list) -> int {
        int n = 0;
        while (list != 0) {
            n += static_cast<int>(list & 1u);
            list >>= 1;
        }
        return n;
    };

    auto regs_before = [](uint16_t list, int reg) -> int {
        int n = 0;
        for (int r = 0; r < reg; ++r) {
            if (list & (1u << r)) ++n;
        }
        return n;
    };

    auto eval_block_reg_offset = [&](const armv4t::Instr& i, int reg,
                                     bool relative_to_writeback_base,
                                     int32_t* offset) -> bool {
        if (i.block.rn >= 15) return false;
        uint16_t list = i.block.reg_list;
        bool empty_list = (list == 0);
        if (empty_list && reg != 15) return false;
        if (!empty_list && (list & (1u << reg)) == 0) return false;
        int n = empty_list ? 1 : reg_list_count(list);

        int32_t start = 0;
        if (empty_list) {
            if (i.block.add) {
                start = i.block.pre_indexed ? 4 : 0;
            } else {
                start = i.block.pre_indexed ? -0x40 : -0x3c;
            }
        } else if (i.block.add) {
            start = i.block.pre_indexed ? 4 : 0;
        } else {
            start = i.block.pre_indexed ? -4 * n : -4 * (n - 1);
        }
        int32_t final_delta = i.block.add
            ? (empty_list ? 0x40 : 4 * n)
            : (empty_list ? -0x40 : -4 * n);
        int32_t addr_offset = start +
            (empty_list ? 0 : 4 * regs_before(list, reg));
        int32_t base_delta =
            (relative_to_writeback_base && i.block.writeback)
                ? final_delta : 0;
        *offset = addr_offset - base_delta;
        return true;
    };

    // Returns true when the target was handled (seeded, or recorded as a
    // data_range collision); false when it points outside the readable
    // image — the caller may want to log that, since a statically-proven
    // target we cannot read is the signature of an undeclared code alias.
    auto enqueue_resolved_target = [&](uint32_t raw, CpuMode target_mode,
                                       const char* kind) -> bool {
        uint32_t target = raw & ~uint32_t{1};
        const uint32_t target_step =
            (target_mode == CpuMode::Thumb) ? 2u : 4u;
        if (target == 0 || !can_read_at(target, target_step)) return false;
        if (addr_in_data_range(target)) {
            record_collision(target, fn.addr, fn.name, kind);
            return true;
        }
        if (target_mode == entry_mode) {
            fn.direct_branch_targets.push_back(target);
        } else {
            mode_switch_seeds_.push_back(
                FunctionSeed{target, target_mode, ""});
        }
        ++stats_.branch_targets_discovered;
        return true;
    };

    // Code-vs-data discriminator for a jump-table entry target. Accepts
    // any plausible CODE target (NOT just a function prologue — a real
    // `switch` table points at mid-function case labels that open with
    // arbitrary instructions, e.g. `add r0,r4,#0`), while still rejecting
    // DATA words that merely happen to fall in ROM. The old gate required
    // a `push`/`stmfd` prologue and so rejected every switch table whose
    // cases aren't function entries (this is exactly what blocked
    // MC-HP-001). Checks, in order: correct alignment for the mode; lands
    // in ROM and not in a known data_range; decodes to defined
    // instruction(s) in `mode`. `strict` (used only when no exact count
    // bound exists) decodes a short run and requires every instruction
    // defined — a single `!is_undefined` is too weak to terminate an
    // unbounded walk, since most 0x08xxxxxx words decode to *something*.
    auto looks_like_code = [&](uint32_t tgt, CpuMode mode,
                               bool strict) -> bool {
        if (mode == CpuMode::Thumb) {
            if (tgt & 1u) return false;
        } else if (tgt & 3u) {
            return false;
        }
        if (addr_in_data_range(tgt)) return false;
        // map_addr_to_source covers both the module span and code_copy
        // ranges — an address inside a declared or scanner-proven copy is
        // as decodable as one in the image proper.
        uint32_t src = 0;
        if (!map_addr_to_source(tgt, &src)) return false;
        const uint32_t k = strict ? 6u : 1u;
        uint32_t a = tgt;
        for (uint32_t j = 0; j < k; ++j) {
            if (mode == CpuMode::Thumb) {
                if (!can_read_at(a, 2)) return j > 0;
                if (armv4t::ThumbDecoder::decode(read_u16(a), a).is_undefined)
                    return false;
                a += 2;
            } else {
                if (!can_read_at(a, 4)) return j > 0;
                if (armv4t::ArmDecoder::decode(read_u32(a), a).is_undefined)
                    return false;
                a += 4;
            }
        }
        return true;
    };

    // Stricter gate for a FUNCTION ENTRY reached only through a pointer
    // value (stored code pointer, code-pointer array). Data that decodes
    // cleanly is overwhelmingly cond=EQ in ARM (image pointers
    // 0x02xxxxxx, small integers) or a non-AL nibble (ASCII), while a
    // function entry starts unconditionally (flags are undefined at
    // entry) and is mostly AL. ARM: the first instruction must be AL (or
    // the unconditional 0xF space) and >= 3 of the first 6 AL (or all of
    // them, when fewer than 3 are readable before the image ends).
    auto looks_like_entry = [&](uint32_t tgt, CpuMode mode) -> bool {
        if (!looks_like_code(tgt, mode, /*strict=*/true)) return false;
        if (mode == CpuMode::Thumb) return true;
        unsigned al = 0, seen = 0;
        bool first_ok = true;
        for (uint32_t j = 0; j < 6u; ++j) {
            const uint32_t a = tgt + 4u * j;
            if (!can_read_at(a, 4)) break;
            const uint32_t c = read_u32(a) >> 28;
            ++seen;
            if (j == 0 && c < 0xEu) { first_ok = false; break; }
            if (c >= 0xEu) ++al;
        }
        if (!first_ok || al < std::min(3u, seen)) {
            ++stats_.entry_gate_rejects;
            if (indirect_report_enabled()) {
                std::fprintf(stderr, "[entry-gate] reject 0x%08X arm\n", tgt);
            }
            return false;
        }
        return true;
    };

    // Recognize an abs32 jump table at `base` revealed by an indexed load
    // at `site_pc`. Mirrors the manual [[jump_table]] expansion in
    // main.cpp (mark table bytes as data + seed each entry) but derives
    // base/count/mode automatically.
    //
    //  * `interworking`  — how the dispatcher consumes an entry. true for
    //    `BX Rt` / a `bx`-veneer reached by BL: bit0 of each word selects
    //    ARM/THUMB (the classic abs32-with-thumb-bit table). false for
    //    `MOV pc,Rt`: a non-interworking write to PC that KEEPS the
    //    dispatcher's current mode, so the words carry no mode bit and
    //    every target inherits `entry_mode`. Deriving mode from bit0 here
    //    (the old behavior) mis-seeded MC-HP-001's even THUMB targets as
    //    ARM.
    //  * `count_hint`    — exact entry count from the dispatcher's
    //    `CMP index,#N; B{hi,cs} default` bound, or 0 if none was
    //    recovered. When known it is the authoritative terminator (no
    //    guessing); a within-range entry that fails the code check then
    //    means the detection was bogus, so the whole table is rejected.
    //    When unknown we fall back to a strict validate-and-stop walk.
    //
    // Overlap with a manual entry is benign: a manual [[jump_table]]
    // already marks the bytes as a data_range, so the walk yields nothing.
    auto emit_jump_table = [&](uint32_t base, uint32_t stride,
                               uint32_t site_pc, bool interworking,
                               uint32_t count_hint) {
        if (stride != 4) return;  // abs32 only in this pass
        // Account each distinct base once, no matter how many overlapping
        // seed walks confirm the same dispatcher.
        if (!jt_seen_bases_.insert(base).second) return;
        const bool bounded = count_hint > 0;
        const uint32_t limit = bounded ? count_hint : 1024u;
        std::vector<FunctionSeed> targets;
        // Benign overlap: the table bytes are already covered by a
        // data_range (a manual [[jump_table]] hint, or a prior auto table).
        // The walk yields nothing and we suppress silently — this is the
        // detector independently rediscovering a hand-annotated table, not
        // a failure. Distinguished from a genuine bound mismatch below.
        bool overlap = false;
        for (uint32_t i = 0; i < limit; ++i) {
            uint32_t pos = base + i * stride;
            if (!can_read_at(pos, 4)) break;
            if (addr_in_data_range(pos)) { if (i == 0) overlap = true; break; }
            uint32_t raw = read_u32(pos);
            CpuMode tmode;
            uint32_t tgt;
            if (interworking) {
                tmode = (raw & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                tgt = raw & ~uint32_t{1};
            } else {
                tmode = entry_mode;  // MOV pc keeps the dispatcher's mode
                tgt = raw & (entry_mode == CpuMode::Thumb ? ~uint32_t{1}
                                                          : ~uint32_t{3});
            }
            if (tgt == 0 || !looks_like_code(tgt, tmode, /*strict=*/!bounded))
                break;
            targets.push_back(FunctionSeed{tgt, tmode, ""});
        }

        const bool ok = bounded
            ? (targets.size() == count_hint)  // every bounded entry was code
            : (targets.size() >= 2);          // long enough to trust
        if (jt_report_enabled()) {
            std::fprintf(stderr,
                "[jt] base=0x%08X site=0x%08X %-6s bound=%-4s "
                "want=%u got=%zu -> %s\n",
                base, site_pc, interworking ? "BX" : "MOVpc",
                bounded ? "yes" : "no", count_hint, targets.size(),
                ok ? "EMIT" : (overlap ? "overlap" : "reject"));
        }
        if (!ok) {
            if (overlap)       ++stats_.jt_overlap_suppressed;
            else if (bounded)  ++stats_.jt_rejected_bound_mismatch;
            else               ++stats_.jt_rejected_unsized;
            return;
        }

        uint32_t count = static_cast<uint32_t>(targets.size());
        data_ranges_.push_back(
            DataRange{base, base + count * stride, "auto jump_table"});
        for (uint32_t i = 0; i < count; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "autojt_%08X_%02u", base, i);
            targets[i].name = buf;
            mode_switch_seeds_.push_back(targets[i]);
            ++stats_.auto_jump_table_targets;
        }
        auto_jump_tables_.push_back(
            AutoJumpTable{base, stride, count, site_pc, entry_mode,
                          interworking, bounded});
        ++stats_.auto_jump_tables;
    };

    // THUMB halfword switch, as ADS/RVCT inline it: the offsets sit right
    // after the BX, and each one is (case - table) | 1.
    //     adds rX,rI,rI
    //     add  rX,pc
    //     ldrh rX,[rX,#8]
    //     lsls rX,rX,#16
    //     asrs rX,rX,#16
    //     add  rX,pc
    //     bx   rX
    // All seven halfwords are fixed, so they are matched exactly rather than
    // inferred through the tracker. The case count comes from the guard on
    // rI (`cmp rI,#N` then `bhi default`, or `bls over; b default`). Without
    // a guard the walk stops at the lowest case after the table, which is
    // where the compiler starts placing them.
    auto thumb_halfword_switch = [&](const armv4t::Instr& bx,
                                     std::vector<uint32_t>& targets) -> bool {
        const uint32_t x = bx.rm;
        const uint32_t scale_pc = bx.pc - 12u;
        if (x >= 8u || bx.pc < 12u || !can_read_at(scale_pc, 14u))
            return false;
        const uint32_t add_pc = 0x4478u | x;
        const uint32_t xx = (x << 3u) | x;
        if (read_u16(bx.pc - 2u) != add_pc ||
            read_u16(bx.pc - 4u) != (0x1400u | xx) ||
            read_u16(bx.pc - 6u) != (0x0400u | xx) ||
            read_u16(bx.pc - 8u) != (0x8900u | xx) ||
            read_u16(bx.pc - 10u) != add_pc)
            return false;
        const uint32_t scale = read_u16(scale_pc);
        const uint32_t index = (scale >> 3u) & 7u;
        if ((scale & 0xFE00u) != 0x1800u || (scale & 7u) != x ||
            ((scale >> 6u) & 7u) != index)
            return false;

        auto cmp_count = [&](uint32_t at) -> uint32_t {
            if (!can_read_at(at, 2u)) return 0u;
            const uint32_t cmp = read_u16(at);
            return (cmp & 0xFF00u) == (0x2800u | (index << 8u))
                ? (cmp & 0xFFu) + 1u : 0u;
        };
        uint32_t count = 0u;
        if (can_read_at(scale_pc - 4u, 4u)) {
            const uint32_t before = read_u16(scale_pc - 2u);
            if ((before & 0xFF00u) == 0xD800u) {
                count = cmp_count(scale_pc - 4u);
            } else if ((before & 0xF800u) == 0xE000u &&
                       (read_u16(scale_pc - 4u) & 0xFF00u) == 0xD900u) {
                count = cmp_count(scale_pc - 6u);
            }
        }

        const uint32_t table = bx.pc + 2u;
        uint32_t lowest = 0xFFFFFFFFu;
        const uint32_t limit = count ? count : 256u;
        for (uint32_t k = 0; k < limit; ++k) {
            const uint32_t at = table + 2u * k;
            if ((!count && at >= lowest) || !can_read_at(at, 2u) ||
                addr_in_data_range(at))
                break;
            const auto offset = static_cast<int16_t>(read_u16(at));
            const uint32_t raw = table + static_cast<uint32_t>(
                static_cast<int32_t>(offset));
            if (!(raw & 1u) || !looks_like_code(raw & ~uint32_t{1},
                                                CpuMode::Thumb, count == 0u))
                break;
            targets.push_back(raw & ~uint32_t{1});
            // A case may also sit before the table, which bounds nothing.
            if (raw > table) lowest = std::min(lowest, raw & ~uint32_t{1});
        }
        return count ? targets.size() == count : targets.size() >= 2u;
    };

    // Code-pointer array at `base`: control flow just went through a
    // register loaded from `base`, so the word there is a code pointer,
    // and arrays of them (static-init tables, ops/callback tables) keep
    // going. Walk forward while each word passes the strict code gate;
    // stop at a zero terminator or the first non-code word. Unlike a jump
    // table the bytes are NOT marked as a data_range: the array can sit
    // inside a larger mutable struct, and a wrong data_range is a hard
    // build error, while a wrong extra seed only costs bloat.
    auto emit_pointer_array = [&](uint32_t base, uint32_t site_pc,
                                  bool interworking) {
        if ((base & 3u) != 0 || !ptr_array_seen_.insert(base).second) return;
        uint32_t n = 0;
        for (uint32_t pos = base; n < 1024u; pos += 4, ++n) {
            if (!can_read_at(pos, 4) || addr_in_data_range(pos)) break;
            const uint32_t raw = read_u32(pos);
            if (raw == 0) break;
            const CpuMode tmode = interworking
                ? ((raw & 1u) ? CpuMode::Thumb : CpuMode::Arm) : entry_mode;
            const uint32_t tgt = raw & ~uint32_t{1};
            if (!looks_like_entry(tgt, tmode)) break;
            char nm[64];
            std::snprintf(nm, sizeof nm, "parr%c_%08X",
                          tmode == CpuMode::Thumb ? 't' : 'a', tgt);
            mode_switch_seeds_.push_back(FunctionSeed{tgt, tmode, nm});
            ++stats_.ptr_array_targets;
        }
        if (n != 0) ++stats_.ptr_arrays;
        if (jt_report_enabled()) {
            std::fprintf(stderr, "[parr] base=0x%08X site=0x%08X %s n=%u\n",
                         base, site_pc, interworking ? "BX" : "MOVpc", n);
        }
    };

    auto eval_dp_imm_pc_write = [&](const armv4t::Instr& i,
                                    uint32_t* out) -> bool {
        if (i.rd != 15 || i.op2.kind != armv4t::Op2::Kind::Imm) {
            return false;
        }

        switch (i.op) {
            case armv4t::IrOp::MOV:
                *out = i.op2.imm_value;
                return true;
            case armv4t::IrOp::MVN:
                *out = ~i.op2.imm_value;
                return true;
            default:
                break;
        }

        uint32_t lhs = 0;
        if (i.rn == 15) {
            lhs = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
        } else if (i.rn < 16 && reg_const[i.rn].known) {
            lhs = reg_const[i.rn].value;
        } else {
            return false;
        }

        uint32_t rhs = i.op2.imm_value;
        switch (i.op) {
            case armv4t::IrOp::ADD:
                *out = lhs + rhs;
                return true;
            case armv4t::IrOp::SUB:
                *out = lhs - rhs;
                return true;
            case armv4t::IrOp::RSB:
                *out = rhs - lhs;
                return true;
            case armv4t::IrOp::AND:
                *out = lhs & rhs;
                return true;
            case armv4t::IrOp::EOR:
                *out = lhs ^ rhs;
                return true;
            case armv4t::IrOp::ORR:
                *out = lhs | rhs;
                return true;
            case armv4t::IrOp::BIC:
                *out = lhs & ~rhs;
                return true;
            default:
                // ADC/SBC/RSC depend on CPSR.C; leave opaque.
                return false;
        }
    };

    auto eval_dp_reg_pc_write = [&](const armv4t::Instr& i,
                                    uint32_t* out) -> bool {
        if (i.rd != 15 ||
            i.op2.kind != armv4t::Op2::Kind::Shifted ||
            i.op2.shifted.by_register ||
            i.op2.shifted.imm_or_rs != 0 ||
            i.op2.shifted.rm >= 16 ||
            !reg_const[i.op2.shifted.rm].known) {
            return false;
        }

        uint32_t rhs = reg_const[i.op2.shifted.rm].value;
        switch (i.op) {
            case armv4t::IrOp::MOV:
                *out = rhs;
                return true;
            case armv4t::IrOp::MVN:
                *out = ~rhs;
                return true;
            default:
                break;
        }

        uint32_t lhs = 0;
        if (i.rn == 15) {
            lhs = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
        } else if (i.rn < 16 && reg_const[i.rn].known) {
            lhs = reg_const[i.rn].value;
        } else {
            return false;
        }

        switch (i.op) {
            case armv4t::IrOp::ADD:
                *out = lhs + rhs;
                return true;
            case armv4t::IrOp::SUB:
                *out = lhs - rhs;
                return true;
            case armv4t::IrOp::RSB:
                *out = rhs - lhs;
                return true;
            case armv4t::IrOp::AND:
                *out = lhs & rhs;
                return true;
            case armv4t::IrOp::EOR:
                *out = lhs ^ rhs;
                return true;
            case armv4t::IrOp::ORR:
                *out = lhs | rhs;
                return true;
            case armv4t::IrOp::BIC:
                *out = lhs & ~rhs;
                return true;
            default:
                return false;
        }
    };

    auto eval_mem_imm_addr = [&](const armv4t::Instr& i,
                                 uint32_t* out) -> bool {
        if (i.mem.by_register) return false;

        uint32_t base = 0;
        if (i.mem.rn == 15) {
            base = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
            if (entry_mode == CpuMode::Thumb) base &= ~uint32_t{3};
        } else if (i.mem.rn < 16 && reg_const[i.mem.rn].known) {
            base = reg_const[i.mem.rn].value;
        } else {
            return false;
        }

        uint32_t indexed = i.mem.add
            ? (base + i.mem.imm_offset)
            : (base - i.mem.imm_offset);
        *out = i.mem.pre_indexed ? indexed : base;
        return true;
    };

    while (count++ < kMaxInstrs && can_read_at(pc, step) &&
           (exact_end == 0u || pc < exact_end)) {
        // If we ever decode from inside a data_range, that's a hard
        // error — TOML asserted "no code here" but our walk got here
        // anyway. Recorded; finder continues so multiple collisions
        // surface at once.
        if (addr_in_data_range(pc)) {
            record_collision(pc, fn.addr, fn.name, "branch");
            break;
        }

        armv4t::Instr ins;
        if (entry_mode == CpuMode::Thumb) {
            ins = armv4t::ThumbDecoder::decode(read_u16(pc), pc);
        } else {
            ins = armv4t::ArmDecoder::decode(read_u32(pc), pc);
        }

        if (ins.is_undefined) {
            ++stats_.undefined_instr_count;
            if (indirect_report_enabled()) {
                std::fprintf(stderr,
                    "[undefined] pc=0x%08X %s raw=0x%08X fn=0x%08X\n", pc,
                    entry_mode == CpuMode::Thumb ? "thumb" : "arm",
                    ins.raw, fn.addr);
            }
            break;
        }

        // Record direct branch targets. The decoder fills
        // branch_target with the absolute guest address.
        const bool thumb_bl_half =
            entry_mode == CpuMode::Thumb &&
            (ins.op == armv4t::IrOp::BL_prefix ||
             ins.op == armv4t::IrOp::BL_suffix);
        // Return pad of a direct-branch call: `add lr,pc,#k; ...; b f`
        // (DQ9 ov016 0x02196158). LR only becomes known through an explicit
        // write in this walk (calls clobber it, entry state is unknown), so
        // an unconditional non-linking B with LR known is a call whose
        // callee returns to that address; nothing else ever targets it.
        if (ins.op == armv4t::IrOp::B && ins.cond == armv4t::Cond::AL &&
            !ins.is_indirect && reg_const[14].known) {
            const uint32_t pad_raw = reg_const[14].value;
            const uint32_t pad = pad_raw & ~uint32_t{1};
            const CpuMode pad_mode = (pad_raw & 1u) ? CpuMode::Thumb
                                                    : CpuMode::Arm;
            if (pad != 0 && pad != ins.branch_target &&
                can_read_at(pad, pad_mode == CpuMode::Thumb ? 2u : 4u) &&
                !addr_in_data_range(pad) &&
                looks_like_code(pad, pad_mode, /*strict=*/false)) {
                char nm[64];
                std::snprintf(nm, sizeof nm, "lpad%c_%08X",
                              pad_mode == CpuMode::Thumb ? 't' : 'a', pad);
                mode_switch_seeds_.push_back(FunctionSeed{pad, pad_mode, nm});
                ++stats_.landing_pads_discovered;
            }
        }
        if (ins.is_branch && !ins.is_indirect &&
            ins.branch_target != 0 && !thumb_bl_half) {
            // BLX (immediate) switches instruction set: an ARM caller
            // targets THUMB and vice-versa. Route it to the opposite-mode
            // seed list so the target is decoded in the right state.
            const bool mode_switch = (ins.op == armv4t::IrOp::BLX_imm);
            const CpuMode target_mode = mode_switch
                ? (entry_mode == CpuMode::Arm ? CpuMode::Thumb : CpuMode::Arm)
                : entry_mode;
            if (addr_in_data_range(ins.branch_target)) {
                record_collision(ins.branch_target, fn.addr,
                                  fn.name, "branch");
                // Don't push the target as a target; the collision
                // is already recorded. Drop it to avoid a worklist
                // entry that would re-trip the same error.
            } else if (mode_switch) {
                mode_switch_seeds_.push_back(FunctionSeed{
                    ins.branch_target & ~uint32_t{1}, target_mode, ""});
                ++stats_.branch_targets_discovered;
            } else {
                fn.direct_branch_targets.push_back(ins.branch_target);
                ++stats_.branch_targets_discovered;
                // B/Bcc (not a call): the target continues this path, so
                // it inherits the tracker state as of the branch.
                if (!ins.is_call) carry_state_to(ins.branch_target);
            }
        }

        // THUMB BL pair: the decoder splits BL into BL_prefix
        // (upper halfword) + BL_suffix (lower halfword). Neither
        // half alone has the FULL target — prefix has
        // branch_target = (PC+4)+(upper<<12) which is just an
        // intermediate base, suffix has branch_target = 0 plus
        // swi_imm = (imm11<<1). Combining them gives the real
        // target. We do the combine here in the finder so the
        // worklist gets the right address.
        if (entry_mode == CpuMode::Thumb &&
            ins.op == armv4t::IrOp::BL_prefix &&
            can_read_at(pc + 2, 2)) {
            armv4t::Instr lower = armv4t::ThumbDecoder::decode(
                read_u16(pc + 2), pc + 2);
            if (lower.op == armv4t::IrOp::BL_suffix) {
                // BLX (branch_exchange) targets ARM, word-aligned, and
                // switches mode; plain BL stays THUMB.
                const bool blx = lower.branch_exchange;
                uint32_t full = ins.branch_target + lower.swi_imm;
                full &= blx ? ~uint32_t{3} : ~uint32_t{1};
                const CpuMode tmode = blx ? CpuMode::Arm : CpuMode::Thumb;
                if (full != 0 && !can_read_at(full, blx ? 4u : 2u)) {
                    external_targets_.emplace(visit_key(full, tmode), pc);
                }
                if (full != 0 && can_read_at(full, blx ? 4u : 2u)) {
                    if (addr_in_data_range(full)) {
                        record_collision(full, fn.addr, fn.name,
                                          "branch");
                    } else if (blx) {
                        mode_switch_seeds_.push_back(
                            FunctionSeed{full, tmode, ""});
                        ++stats_.branch_targets_discovered;
                    } else {
                        fn.direct_branch_targets.push_back(full);
                        ++stats_.branch_targets_discovered;
                    }
                }

                // The instruction after a THUMB BL pair is always a
                // valid continuation address. Some real BIOS paths
                // and gba-suite cases later reach that continuation
                // through an indirect transfer (saved LR, empty-list
                // LDM, callback trampoline). Seed the continuation
                // explicitly instead of relying on the BL_prefix
                // half's intermediate target, which is not the call
                // target and can point into data for long BLs.
                uint32_t return_pc = pc + 4u;
                if (can_read_at(return_pc, 2)) {
                    enqueue_resolved_target(return_pc, entry_mode, "branch");
                }
            }
        }

        // Indirect = computed; we can't follow statically — UNLESS
        // our narrow constant tracker resolved the source register
        // to a known value just above the BX (or MOV PC, Rn).
        if (ins.op == armv4t::IrOp::SWI) {
            enqueue_resolved_target(ins.pc + step, entry_mode, "branch");
        }

        if (ins.is_indirect) {
            fn.has_indirect_transfer = true;
            ++stats_.indirect_transfer_count;

            // Per-site outcome bookkeeping, tallied at the end of this
            // block: seeded (target proven and reachable), known but
            // UNREACHABLE (target proven, outside the readable image —
            // the static signature of code relocated to an undeclared
            // base), or unknown (the tracker had nothing).
            bool ind_have_value = false;
            bool ind_seeded = false;
            uint32_t ind_value = 0;

            // Some PC-writing instructions look "indirect" to the
            // decoder but still have a fully-known target. Keep this
            // intentionally narrow so misses stay loud and false
            // positives stay rare.
            bool indirect_resolved = false;
            uint32_t resolved = 0;
            if (eval_dp_imm_pc_write(ins, &resolved)) {
                indirect_resolved = true;
                ind_have_value = true;
                ind_value = resolved;
                CpuMode target_mode = entry_mode;
                if (ins.rn != 15 && (resolved & 1u)) {
                    target_mode = CpuMode::Thumb;
                }
                ind_seeded |= enqueue_resolved_target(
                    resolved, target_mode, "branch");
            } else if (eval_dp_reg_pc_write(ins, &resolved)) {
                indirect_resolved = true;
                ind_have_value = true;
                ind_value = resolved;
                ind_seeded |= enqueue_resolved_target(
                    resolved, entry_mode, "branch");
            } else if (ins.op == armv4t::IrOp::LDR && ins.rd == 15) {
                uint32_t ea = 0;
                if (eval_mem_imm_addr(ins, &ea) && can_read_at(ea, 4)) {
                    indirect_resolved = true;
                    uint32_t raw = read_u32(ea & ~uint32_t{3});
                    raw = ror32(raw, (ea & 3u) * 8u);
                    ind_have_value = true;
                    ind_value = raw;
                    CpuMode target_mode = (raw & 1u)
                        ? CpuMode::Thumb : entry_mode;
                    ind_seeded |= enqueue_resolved_target(
                        raw, target_mode, "branch");
                } else {
                    uint8_t rn = 0;
                    int32_t offset = 0;
                    uint32_t raw = 0;
                    if (eval_symbolic_mem_addr(ins, &rn, &offset) &&
                        lookup_mem_const(rn, offset, &raw)) {
                        indirect_resolved = true;
                        ind_have_value = true;
                        ind_value = raw;
                        CpuMode target_mode = (raw & 1u)
                            ? CpuMode::Thumb : entry_mode;
                        ind_seeded |= enqueue_resolved_target(
                            raw, target_mode, "branch");
                    }
                }
            } else if (ins.op == armv4t::IrOp::LDM &&
                       ins.block.load &&
                       ((ins.block.reg_list & (1u << 15)) != 0 ||
                        ins.block.reg_list == 0)) {
                int32_t offset = 0;
                uint8_t rn = 0;
                int32_t resolved_offset = 0;
                uint32_t raw = 0;
                if (eval_block_reg_offset(
                        ins, 15, /*relative_to_writeback_base=*/false,
                        &offset) &&
                    resolve_symbolic_base(ins.block.rn, offset,
                                          &rn, &resolved_offset) &&
                    lookup_mem_const(rn, resolved_offset, &raw)) {
                    indirect_resolved = true;
                    ind_have_value = true;
                    ind_value = raw;
                    ind_seeded |= enqueue_resolved_target(
                        raw, entry_mode, "branch");
                }
            }

            // ARM branch-ladder switch: `add{cond} pc, pc, rX, lsl #2`
            // jumps to slot pc+8+4*rX; each slot is (almost always) a
            // `b caseK`. Size from the adjacent `cmp rX,#N` guard for
            // LS/CC; otherwise (guard further upstream, e.g. `subs;
            // addpl`, `cmp; addge`) walk slots while each is an
            // unconditional B, requiring >= 2. A computed jump into
            // straight-line code (unrolled division: `add pc,pc,r2,lsl#2`
            // into `adcs/sublo` runs) fails that and stays unknown.
            if (!indirect_resolved && entry_mode == CpuMode::Arm &&
                ins.op == armv4t::IrOp::ADD && ins.rd == 15 &&
                ins.rn == 15 &&
                ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                !ins.op2.shifted.by_register &&
                ins.op2.shifted.type == armv4t::ShiftType::LSL &&
                ins.op2.shifted.imm_or_rs == 2 &&
                ins.op2.shifted.rm < 15) {
                const uint32_t slot0 = ins.pc + 8u;
                uint32_t count = 0;
                bool bounded = false;
                if (last_cmp.known && last_cmp.reg == ins.op2.shifted.rm &&
                    last_cmp.pc + 4u == ins.pc) {
                    if (ins.cond == armv4t::Cond::LS) {
                        count = last_cmp.imm + 1u;
                        bounded = true;
                    } else if (ins.cond == armv4t::Cond::CC &&
                               last_cmp.imm > 0) {
                        count = last_cmp.imm;
                        bounded = true;
                    }
                }
                auto slot_is_code = [&](uint32_t a, bool need_b) {
                    if (!can_read_at(a, 4) || addr_in_data_range(a))
                        return false;
                    armv4t::Instr s = armv4t::ArmDecoder::decode(
                        read_u32(a), a);
                    if (s.is_undefined) return false;
                    return !need_b || (s.op == armv4t::IrOp::B &&
                                       s.cond == armv4t::Cond::AL);
                };
                uint32_t n = 0;
                if (bounded) {
                    if (count <= 1024u) {
                        while (n < count && slot_is_code(slot0 + 4u * n, false))
                            ++n;
                        if (n != count) n = 0;  // a bounded slot isn't code
                    }
                } else {
                    // Unbounded: walk slots that are an unconditional B or
                    // an unconditional return (`pop {..,pc}` cases that
                    // just return, e.g. `cmp r0,#63; bgt; cmp r0,#0;
                    // addge pc,pc,r0,lsl#2` with mixed b/pop slots).
                    // Needs >= 2 B slots so straight-line computed jumps
                    // (unrolled division) stay rejected.
                    uint32_t b_slots = 0;
                    while (n < 1024u) {
                        const uint32_t a = slot0 + 4u * n;
                        if (slot_is_code(a, true)) { ++b_slots; ++n; continue; }
                        if (!slot_is_code(a, false)) break;
                        armv4t::Instr s = armv4t::ArmDecoder::decode(read_u32(a), a);
                        if (!(s.is_return && s.cond == armv4t::Cond::AL)) break;
                        ++n;
                    }
                    // Trailing returns past the last B are the function's
                    // own epilogue, not slots we can prove; keep them, they
                    // are code either way.
                    if (b_slots < 2) n = 0;
                }
                const bool first = ladder_seen_sites_.insert(ins.pc).second;
                if (first && jt_report_enabled()) {
                    std::fprintf(stderr,
                        "[ladder] site=0x%08X cond=%X bound=%s want=%u "
                        "got=%u\n", ins.pc, static_cast<unsigned>(ins.cond),
                        bounded ? "yes" : "no", count, n);
                }
                if (n != 0) {
                    for (uint32_t k = 0; k < n; ++k) {
                        enqueue_resolved_target(slot0 + 4u * k, entry_mode,
                                                "jump_table");
                    }
                    indirect_resolved = true;
                    ind_seeded = true;
                    if (first) {
                        ++stats_.branch_ladders;
                        stats_.branch_ladder_slots += n;
                    }
                } else if (first) {
                    ++stats_.branch_ladders_rejected;
                }
            }

            if (!indirect_resolved && entry_mode == CpuMode::Thumb &&
                ins.op == armv4t::IrOp::BX) {
                std::vector<uint32_t> cases;
                if (thumb_halfword_switch(ins, cases)) {
                    const auto count = static_cast<uint32_t>(cases.size());
                    const uint32_t table = ins.pc + 2u;
                    if (ladder_seen_sites_.insert(ins.pc).second) {
                        data_ranges_.push_back(DataRange{
                            table, table + 2u * count, "auto jump_table"});
                        auto_jump_tables_.push_back(AutoJumpTable{
                            table, 2u, count, ins.pc, entry_mode, true, true});
                        ++stats_.auto_jump_tables;
                        stats_.auto_jump_table_targets += count;
                    }
                    for (const uint32_t target : cases)
                        enqueue_resolved_target(target, entry_mode,
                                                "jump_table");
                    indirect_resolved = true;
                    ind_seeded = true;
                }
            }

            // Two cases land here with a tracked register source:
            //   1. BX Rm                  — `ins.rm` is the source
            //   2. MOV PC, Rn (DP-MOV)    — `ins.op2.shifted.rm`
            //      is the source (op2 with no shift)
            // BX is the ARMv4T interworking instruction, so it gets
            // target mode from bit 0. Plain DP writes to PC keep the
            // current instruction-set state; bit 0 is not an exchange.
            uint8_t src_reg = 255;  // sentinel
            bool src_is_bx = false;
            if ((ins.op == armv4t::IrOp::BX ||
                 ins.op == armv4t::IrOp::BLX_reg) && ins.rm < 16) {
                // BX and BLX(reg) both interwork from Rm bit0.
                src_reg = ins.rm;
                src_is_bx = true;
            } else if (ins.op == armv4t::IrOp::MOV && ins.rd == 15 &&
                       ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                       !ins.op2.shifted.by_register &&
                       ins.op2.shifted.imm_or_rs == 0 &&
                       ins.op2.shifted.rm < 16) {
                // `mov pc, Rn` (computed jump): if the source register is
                // tracked, we know the target. Only MOV: for `add pc,Rt,Rb`
                // op2 is just one addend (the base of a relative table),
                // and taking it as the target seeded table bases as code
                // and suppressed the call's landing pad (DQ9 ov016).
                // ADD/SUB with both operands known are resolved above by
                // eval_dp_reg_pc_write.
                src_reg = ins.op2.shifted.rm;
            }
            if (src_reg < 16) {
                uint32_t raw = 0;
                bool raw_known = false;
                if (src_reg == 15) {
                    raw = ins.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
                    if (entry_mode == CpuMode::Thumb) raw &= ~uint32_t{3};
                    raw_known = true;
                } else if (reg_const[src_reg].known) {
                    raw = reg_const[src_reg].value;
                    raw_known = true;
                }
                if (raw_known) {
                    indirect_resolved = true;
                    ind_have_value = true;
                    ind_value = raw;
                    uint32_t tgt = raw & ~uint32_t{1};
                    CpuMode tgt_mode = entry_mode;
                    if (src_is_bx) {
                        tgt_mode = (raw & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                    }
                    if (tgt != 0 && can_read_at(
                            tgt, tgt_mode == CpuMode::Thumb ? 2u : 4u) &&
                        !addr_in_data_range(tgt)) {
                        mode_switch_seeds_.push_back(
                            FunctionSeed{tgt, tgt_mode, ""});
                        ++stats_.branch_targets_discovered;
                        ind_seeded = true;
                    }
                }
            }
            if (src_reg < 15 && reg_load[src_reg].known) {
                emit_pointer_array(reg_load[src_reg].addr, ins.pc,
                                   src_is_bx);
            }
            if (ins.op == armv4t::IrOp::BX &&
                ins.rm != 14 &&
                reg_const[14].known) {
                uint32_t raw = reg_const[14].value;
                CpuMode return_mode = (raw & 1u)
                    ? CpuMode::Thumb : CpuMode::Arm;
                enqueue_resolved_target(raw, return_mode, "branch");
            }

            // Tier-2 landing-pad auto-discovery. The return-pad idiom sets LR
            // to a code address, then transfers control OUT via an indirect
            // PC-write the finder can't resolve (e.g. `adr lr,X; ldr pc,
            // [dtcm+0x3FFC]` — the BIOS IRQ handler calling the firmware's
            // installed handler). The callee returns with `bx lr`, landing at
            // X, which NO static branch ever targets — so without this it is a
            // dispatch miss. Seed X as a dispatch entry (the finder then splits
            // the containing function there, exactly like any other entry).
            //
            // Soundness gates (see the multi-entry-dispatch design):
            //   - LR holds a statically-known constant — reg_const tracks the
            //     load and clears on any clobber, so "known" ⇒ live + unclobbered;
            //   - it points into executable ROM and decodes to a valid aligned
            //     instruction (looks_like_code);
            //   - the transfer is genuinely UNRESOLVED (else the callee is known
            //     and there is nothing to call back to);
            //   - the transfer is a call-OUT, not a return shape (bx/mov pc via
            //     LR, or any LDM pop {...,pc}) — those land AT the pad, not past it.
            if (!indirect_resolved && reg_const[14].known) {
                bool is_return_shape = false;
                if (ins.op == armv4t::IrOp::LDM) {
                    is_return_shape = true;  // pop {...,pc} is a return
                } else if ((ins.op == armv4t::IrOp::BX ||
                            ins.op == armv4t::IrOp::BLX_reg) &&
                           ins.rm == 14) {
                    is_return_shape = true;  // bx lr
                } else if (ins.rd == 15 &&
                           ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                           !ins.op2.shifted.by_register &&
                           ins.op2.shifted.imm_or_rs == 0 &&
                           ins.op2.shifted.rm == 14) {
                    is_return_shape = true;  // mov pc, lr
                }
                uint32_t pad_raw = reg_const[14].value;
                uint32_t pad = pad_raw & ~uint32_t{1};
                CpuMode pad_mode = (pad_raw & 1u) ? CpuMode::Thumb
                                                  : CpuMode::Arm;
                if (!is_return_shape && pad != 0 &&
                    can_read_at(pad, pad_mode == CpuMode::Thumb ? 2u : 4u) &&
                    !addr_in_data_range(pad) &&
                    looks_like_code(pad, pad_mode, /*strict=*/false)) {
                    char nm[64];
                    std::snprintf(nm, sizeof nm, "lpad%c_%08X",
                                  pad_mode == CpuMode::Thumb ? 't' : 'a',
                                  pad);
                    mode_switch_seeds_.push_back(
                        FunctionSeed{pad, pad_mode, nm});
                    ++stats_.landing_pads_discovered;
                }
            }

            // Outcome tally. Returns are benign (the callee's caller is
            // already discovered); everything else is either followed,
            // proven-but-unreadable (an undeclared alias — a config gap,
            // recorded for the discovery summary), or a genuine analysis
            // gap.
            if (!ins.is_return) {
                if (ind_seeded) {
                    ++stats_.indirect_resolved_count;
                } else if (ind_have_value) {
                    ++stats_.indirect_known_unreachable_count;
                    unreachable_targets_.push_back(UnreachableTarget{
                        ins.pc, ind_value & ~uint32_t{1}, entry_mode});
                    if (indirect_report_enabled()) {
                        std::fprintf(stderr,
                            "[indirect] pc=0x%08X %s op=%d UNREACHABLE "
                            "target=0x%08X\n",
                            ins.pc,
                            entry_mode == CpuMode::Thumb ? "thumb" : "arm",
                            static_cast<int>(ins.op), ind_value);
                    }
                } else {
                    ++stats_.indirect_unknown_count;
                    if (indirect_report_enabled()) {
                        std::fprintf(stderr,
                            "[indirect] pc=0x%08X %s op=%d UNKNOWN "
                            "rd=%d rn=%d rm=%d\n",
                            ins.pc,
                            entry_mode == CpuMode::Thumb ? "thumb" : "arm",
                            static_cast<int>(ins.op),
                            static_cast<int>(ins.rd),
                            static_cast<int>(ins.rn),
                            static_cast<int>(ins.rm));
                    }
                }
            }
        }

        // Call into a one-instruction `bx rN` veneer (`_call_via_rN`):
        // the call goes through rN, so a load-tracked rN reveals a
        // code-pointer array exactly like a direct `blx rN` would.
        {
            uint32_t veneer = 0;
            CpuMode veneer_mode = entry_mode;
            if (ins.op == armv4t::IrOp::BL && ins.branch_target != 0) {
                veneer = ins.branch_target;
            } else if (entry_mode == CpuMode::Thumb &&
                       ins.op == armv4t::IrOp::BL_prefix &&
                       can_read_at(pc + 2, 2)) {
                armv4t::Instr lo = armv4t::ThumbDecoder::decode(
                    read_u16(pc + 2), pc + 2);
                if (lo.op == armv4t::IrOp::BL_suffix) {
                    const bool blx = lo.branch_exchange;
                    veneer = (ins.branch_target + lo.swi_imm) &
                             (blx ? ~uint32_t{3} : ~uint32_t{1});
                    veneer_mode = blx ? CpuMode::Arm : CpuMode::Thumb;
                }
            }
            const uint32_t vstep = veneer_mode == CpuMode::Thumb ? 2u : 4u;
            if (veneer != 0 && can_read_at(veneer, vstep)) {
                armv4t::Instr v = veneer_mode == CpuMode::Thumb
                    ? armv4t::ThumbDecoder::decode(read_u16(veneer), veneer)
                    : armv4t::ArmDecoder::decode(read_u32(veneer), veneer);
                if (v.op == armv4t::IrOp::BX && v.cond == armv4t::Cond::AL &&
                    v.rm < 14 && reg_load[v.rm].known) {
                    emit_pointer_array(reg_load[v.rm].addr, ins.pc, true);
                }
            }
        }

        // Jump-table confirmation (MC-HP-000). If a candidate from a
        // prior indexed load is pending, check whether THIS instruction
        // uses the loaded register as a control-flow target. That
        // promotion is the proof it's a jump table rather than an
        // indexed DATA-pointer load (which is dereferenced, not branched
        // to). Without this gate the detector seeds garbage and the walk
        // explodes.
        if (pend.active) {
            bool confirmed = false;
            // How the dispatcher consumes the loaded entry decides target
            // mode: BX / bx-veneer interwork on bit0; MOV pc keeps the
            // dispatcher's current mode. See emit_jump_table().
            bool interworking = true;
            const armv4t::Op2& po = ins.op2;
            if (ins.op == armv4t::IrOp::BX && ins.rm == pend.dest) {
                confirmed = true;  // BX dest
                interworking = true;
            } else if (ins.rd == 15 &&
                       po.kind == armv4t::Op2::Kind::Shifted &&
                       !po.shifted.by_register &&
                       po.shifted.imm_or_rs == 0 &&
                       po.shifted.rm == pend.dest) {
                confirmed = true;  // MOV pc, dest (computed jump)
                interworking = false;
            } else if (ins.is_call && ins.branch_target != 0 &&
                       can_read_at(ins.branch_target, 2)) {
                // ARM BL into a `bx dest` veneer (single-instruction BL).
                armv4t::Instr v = armv4t::ThumbDecoder::decode(
                    read_u16(ins.branch_target), ins.branch_target);
                if (v.op == armv4t::IrOp::BX && v.rm == pend.dest) {
                    confirmed = true;
                }
            } else if (entry_mode == CpuMode::Thumb &&
                       ins.op == armv4t::IrOp::BL_prefix &&
                       can_read_at(ins.pc + 2, 2)) {
                // THUMB BL is a prefix/suffix halfword PAIR; the full
                // target only exists once combined. This is the dominant
                // dispatch form — `bl <bx-dest veneer>` — so without it
                // every THUMB-BL-dispatched table is missed. Combine the
                // pair (same math as the finder's direct-BL handling),
                // then check the veneer it lands on.
                armv4t::Instr lo = armv4t::ThumbDecoder::decode(
                    read_u16(ins.pc + 2), ins.pc + 2);
                if (lo.op == armv4t::IrOp::BL_suffix) {
                    uint32_t full =
                        (ins.branch_target + lo.swi_imm) & ~uint32_t{1};
                    if (can_read_at(full, 2)) {
                        armv4t::Instr v = armv4t::ThumbDecoder::decode(
                            read_u16(full), full);
                        if (v.op == armv4t::IrOp::BX &&
                            v.rm == pend.dest) {
                            confirmed = true;
                        }
                    }
                }
            }
            // Base-relative offset table: `sub/add Rb,pc,#k;
            // ldr Rt,[Rb,Ri,lsl#2]; add pc,Rb,Rt` — each word is an offset
            // from the table base (hand-written dispatchers, e.g. codec
            // state machines). Mode stays the dispatcher's.
            bool relative = false;
            if (ins.op == armv4t::IrOp::ADD && ins.rd == 15 &&
                po.kind == armv4t::Op2::Kind::Shifted &&
                !po.shifted.by_register && po.shifted.imm_or_rs == 0 &&
                po.shifted.rm < 16 && ins.rn < 15) {
                const uint8_t other = po.shifted.rm == pend.dest ? ins.rn
                                    : (ins.rn == pend.dest ? po.shifted.rm : 255);
                if (other < 15 && reg_const[other].known &&
                    reg_const[other].value == pend.base) {
                    relative = true;
                }
            }
            if (relative) {
                confirmed = false;  // not the MOV-pc form matched above
                ++stats_.jt_confirmations;
                if (jt_seen_bases_.insert(pend.base).second) {
                    const uint32_t limit = pend.count_known ? pend.count : 256u;
                    if (!pend.count_known && !pc_rel_bases_ready_) {
                        pc_rel_bases_ready_ = true;
                        for (std::size_t off = 0; off + 4 <= rom_size_; off += 4) {
                            const uint32_t pc = rom_base_ + static_cast<uint32_t>(off);
                            const armv4t::Instr d =
                                armv4t::ArmDecoder::decode(read_u32(pc), pc);
                            if (d.cond == armv4t::Cond::AL && d.rn == 15 && d.rd < 15 &&
                                (d.op == armv4t::IrOp::ADD || d.op == armv4t::IrOp::SUB) &&
                                d.op2.kind == armv4t::Op2::Kind::Imm) {
                                pc_rel_bases_.insert(d.op == armv4t::IrOp::ADD
                                    ? pc + 8u + d.op2.imm_value
                                    : pc + 8u - d.op2.imm_value);
                            }
                        }
                    }
                    uint32_t n = 0;
                    std::vector<FunctionSeed> rel_targets;
                    for (; n < limit; ++n) {
                        const uint32_t pos = pend.base + 4u * n;
                        if (!can_read_at(pos, 4) || addr_in_data_range(pos)) break;
                        // Unbounded walk: stop where another table begins.
                        if (n > 0 && !pend.count_known && pc_rel_bases_.count(pos)) break;
                        const uint32_t tgt = (pend.base + read_u32(pos)) &
                            (entry_mode == CpuMode::Thumb ? ~uint32_t{1} : ~uint32_t{3});
                        // Stop at the first word that is not an offset to
                        // code; the table's own bytes are not a target.
                        if (tgt >= pend.base && tgt < pend.base + 4u * (n + 1)) break;
                        if (!looks_like_code(tgt, entry_mode, /*strict=*/true)) break;
                        char nm[64];
                        std::snprintf(nm, sizeof nm, "reljt_%08X_%02u", pend.base, n);
                        rel_targets.push_back(FunctionSeed{tgt, entry_mode, nm});
                    }
                    if (jt_report_enabled()) {
                        std::fprintf(stderr, "[jt] base=0x%08X site=0x%08X REL n=%u\n",
                                     pend.base, ins.pc, n);
                    }
                    if (n >= 2 || (pend.count_known && n == pend.count)) {
                        for (auto& t : rel_targets) {
                            mode_switch_seeds_.push_back(t);
                            ++stats_.auto_jump_table_targets;
                        }
                        data_ranges_.push_back(DataRange{pend.base, pend.base + 4u * n,
                                                         "auto rel jump_table"});
                        auto_jump_tables_.push_back(AutoJumpTable{pend.base, 4, n,
                            ins.pc, entry_mode, false, pend.count_known});
                        ++stats_.auto_jump_tables;
                    }
                }
                pend.active = false;
            } else if (confirmed && !pend.relative_only) {
                ++stats_.jt_confirmations;
                emit_jump_table(pend.base, pend.stride, ins.pc, interworking,
                                pend.count_known ? pend.count : 0u);
                pend.active = false;
            } else if (ins.rd == pend.dest) {
                pend.active = false;  // dest overwritten before any branch
            }
        }

        // Jump-table detection. An indexed load of a would-be table entry
        // whose address is (tracked ROM-pointer base) + (index<<2). Two
        // shapes:
        //   (a) register-offset:  LDR Rt,[Rb, Ri, LSL #2]
        //   (b) the common THUMB form where a prior ADD folded
        //       base + (index<<2) into reg_table[] (see maintenance below).
        // Records a pending candidate; emission waits for the
        // confirmation above. Detection runs before the tracker update so
        // reg_table[] from the preceding ADD is still live even when
        // Rt == the base reg.
        if (jump_table_detection_enabled() && ins.op == armv4t::IrOp::LDR) {
            uint32_t jt_base = 0;
            bool jt = false;
            bool unscaled = false;
            bool     count_known = false;
            uint32_t count = 0;
            if (ins.mem.by_register && ins.mem.rn < 16 &&
                ins.mem.reg_offset.rm < 16 &&
                ins.mem.reg_offset.type == armv4t::ShiftType::LSL &&
                !ins.mem.reg_offset.by_register &&
                ins.mem.reg_offset.imm_or_rs == 2 &&
                reg_const[ins.mem.rn].known &&
                addr_in_rom(reg_const[ins.mem.rn].value)) {
                jt_base = reg_const[ins.mem.rn].value;
                jt = true;
                // Inline-scaled form: the index is the offset register.
                uint8_t ireg = ins.mem.reg_offset.rm;
                if (reg_bound[ireg].known) {
                    count_known = true;
                    count = reg_bound[ireg].max_index + 1u;
                }
            } else if (ins.mem.by_register && ins.mem.rn < 15 &&
                       ins.mem.pre_indexed && ins.mem.add &&
                       ins.mem.reg_offset.rm < 15 &&
                       ins.mem.reg_offset.rm != ins.mem.rn &&
                       ins.mem.reg_offset.type == armv4t::ShiftType::LSL &&
                       !ins.mem.reg_offset.by_register &&
                       ins.mem.reg_offset.imm_or_rs == 0 &&
                       reg_const[ins.mem.rn].known &&
                       addr_in_rom(reg_const[ins.mem.rn].value)) {
                // `ldr Rt,[Rb,Ri]` with Ri built as 0/4/8/.. (DQ9 ov016
                // 0x021989CC: `sub r5,pc,#24; ... addne r8,#8; addge r8,#4;
                // ldr r6,[r5,r8]; add pc,r5,r6`). Accepted only through the
                // base-relative confirmation below.
                jt_base = reg_const[ins.mem.rn].value;
                jt = true;
                unscaled = true;
            } else if (!ins.mem.by_register && ins.mem.rn < 16 &&
                       reg_table[ins.mem.rn].known &&
                       reg_table[ins.mem.rn].stride == 4) {
                int32_t off = 0;
                if (ins.mem.pre_indexed) {
                    int32_t imm = static_cast<int32_t>(ins.mem.imm_offset);
                    off = ins.mem.add ? imm : -imm;
                }
                jt_base = reg_table[ins.mem.rn].base +
                          static_cast<uint32_t>(off);
                jt = true;
                if (reg_table[ins.mem.rn].bound_known) {
                    count_known = true;
                    count = reg_table[ins.mem.rn].max_index + 1u;
                }
            }
            if (jt) {
                pend.active      = true;
                pend.base        = jt_base;
                pend.stride      = 4;
                pend.dest        = ins.rd;
                pend.count_known = count_known;
                pend.count       = count;
                pend.relative_only = unscaled;
            }
        }

        // CMP-bound capture for exact jump-table sizing (MC-HP-000). The
        // canonical switch guard is `CMP Ri,#N` immediately followed by a
        // `B{hi,cs}` to the default case; the in-range index span gives the
        // table's entry count with no guessing. Record the compare, then on
        // the very next instruction (the guard branch) stamp reg_bound[Ri].
        // The bound then rides the index through the scale/add chain below.
        if (ins.op == armv4t::IrOp::CMP &&
            ins.op2.kind == armv4t::Op2::Kind::Imm && ins.rn < 16) {
            last_cmp.known = true;
            last_cmp.reg   = ins.rn;
            last_cmp.imm   = ins.op2.imm_value;
            last_cmp.pc    = ins.pc;
        } else if (last_cmp.known && ins.is_branch &&
                   ins.pc == last_cmp.pc + step) {
            // Max in-range index from the compare. HI/LS bound at N
            // (Ri {>,<=} N); CS/CC bound at N-1 (Ri {>=,<} N).
            bool have = false; uint32_t maxi = 0;
            if (ins.cond == armv4t::Cond::HI ||
                ins.cond == armv4t::Cond::LS) {
                have = true; maxi = last_cmp.imm;
            } else if ((ins.cond == armv4t::Cond::CS ||
                        ins.cond == armv4t::Cond::CC) && last_cmp.imm > 0) {
                have = true; maxi = last_cmp.imm - 1u;
            }
            if (have) {
                // HI/CS: taken branches AWAY to the default, so the
                // indexed dispatch is the fall-through of THIS walk —
                // set the bound directly. LS/CC: taken branches TO the
                // dispatch, which the finder walks as its own seed —
                // park the bound for that target (re-seeded at entry).
                if (ins.cond == armv4t::Cond::HI ||
                    ins.cond == armv4t::Cond::CS) {
                    reg_bound[last_cmp.reg] = {true, maxi};
                } else if (ins.branch_target != 0) {
                    branch_target_bounds_[ins.branch_target] =
                        SeedBound{last_cmp.reg, maxi};
                }
            }
            last_cmp.known = false;
        } else {
            // any other instruction ends the (adjacent-only) cmp window
            last_cmp.known = false;
        }

        if (ins.op == armv4t::IrOp::STR) {
            uint8_t rn = 0;
            int32_t offset = 0;
            uint32_t stored = 0;
            bool known_value = false;
            if (ins.rd == 15) {
                stored = ins.pc + (entry_mode == CpuMode::Thumb ? 4u : 12u);
                if (entry_mode == CpuMode::Thumb) {
                    stored &= ~uint32_t{3};
                }
                known_value = true;
            } else if (ins.rd < 16 && reg_const[ins.rd].known) {
                stored = reg_const[ins.rd].value;
                known_value = true;
            }

            if (!ins.mem.writeback &&
                eval_symbolic_mem_addr(ins, &rn, &offset) &&
                known_value) {
                remember_mem_const(rn, offset, stored);
            } else {
                clear_mem_const();
            }

            // Stored code-pointer promotion (beads-yjp.35). A known
            // constant written to memory that lands in readable code
            // space is a candidate entry: the IRQ handler installed into
            // the vector slot, a registered callback — addresses nothing
            // ever branches to, reached only through hardware or a
            // far-later indirect load. Fail-closed: bit0 selects the
            // mode (the stored-pointer interworking convention), and the
            // target must pass the strict multi-instruction decode gate.
            // A false accept costs bloat, never correctness — dispatch
            // is content-validated with a fail-closed interior default.
            if (known_value && ins.rd != 15) {
                uint32_t ptr = stored & ~uint32_t{1};
                CpuMode ptr_mode = (stored & 1u) ? CpuMode::Thumb
                                                 : CpuMode::Arm;
                if (ptr != 0 &&
                    (ptr & (ptr_mode == CpuMode::Thumb ? 1u : 3u)) == 0 &&
                    can_read_at(ptr, ptr_mode == CpuMode::Thumb ? 2u : 4u) &&
                    !addr_in_data_range(ptr) &&
                    stored_ptr_seen_.insert(visit_key(ptr, ptr_mode)).second &&
                    looks_like_entry(ptr, ptr_mode)) {
                    char nm[64];
                    // Mode is part of the identity: the same address can
                    // be stored as both an ARM and a THUMB pointer, and
                    // two Functions must not share one emitted name.
                    std::snprintf(nm, sizeof nm, "sptr%c_%08X",
                                  ptr_mode == CpuMode::Thumb ? 't' : 'a',
                                  ptr);
                    mode_switch_seeds_.push_back(
                        FunctionSeed{ptr, ptr_mode, nm});
                    ++stats_.stored_ptr_seeds;
                }
            }
        } else if (ins.op == armv4t::IrOp::STRB ||
                   ins.op == armv4t::IrOp::STRH) {
            clear_mem_const();
        } else if (ins.op == armv4t::IrOp::STM && !ins.block.load) {
            if (ins.block.rn >= 15 || ins.block.reg_list == 0) {
                clear_mem_const();
            } else if (ins.block.writeback &&
                       reg_sym[ins.block.rn].known) {
                clear_mem_const();
            } else {
                if (ins.block.writeback) {
                    invalidate_mem_base(ins.block.rn);
                }
                for (int reg = 0; reg < 16; ++reg) {
                    if ((ins.block.reg_list & (1u << reg)) == 0) continue;
                    int32_t offset = 0;
                    if (!eval_block_reg_offset(
                            ins, reg,
                            /*relative_to_writeback_base=*/true,
                            &offset)) {
                        clear_mem_const();
                        break;
                    }

                    uint32_t stored = 0;
                    bool known_value = false;
                    if (reg == 15) {
                        stored = ins.pc +
                            (entry_mode == CpuMode::Thumb ? 4u : 12u);
                        if (entry_mode == CpuMode::Thumb) {
                            stored &= ~uint32_t{3};
                        }
                        known_value = true;
                    } else if (reg_const[reg].known) {
                        stored = reg_const[reg].value;
                        known_value = true;
                    }

                    uint8_t rn = 0;
                    int32_t resolved_offset = 0;
                    bool resolved_base = resolve_symbolic_base(
                        ins.block.rn, offset, &rn, &resolved_offset);

                    if (known_value && resolved_base) {
                        remember_mem_const(rn, resolved_offset, stored);
                    } else if (resolved_base) {
                        forget_mem_const(rn, resolved_offset);
                    } else {
                        clear_mem_const();
                    }
                }
            }
        } else if (ins.op == armv4t::IrOp::LDM &&
                   ins.block.load &&
                   ins.block.writeback &&
                   ins.block.rn < 16) {
            reg_const[ins.block.rn].known = false;
            reg_sym[ins.block.rn].known = false;
            reg_scaled[ins.block.rn].known = false;
            reg_table[ins.block.rn].known = false;
            if (ins.block.rn < 15) reg_load[ins.block.rn].known = false;
            invalidate_mem_base(ins.block.rn);
            invalidate_sym_base(ins.block.rn);
        }
        if (ins.op == armv4t::IrOp::LDM && ins.block.load) {
            // Every loaded register is overwritten with memory contents.
            for (int r = 0; r < 15; ++r) {
                if ((ins.block.reg_list & (1u << r)) == 0) continue;
                reg_const[r].known = false;
                reg_sym[r].known = false;
                reg_scaled[r].known = false;
                reg_table[r].known = false;
                reg_load[r].known = false;
                invalidate_mem_base(static_cast<uint8_t>(r));
                invalidate_sym_base(static_cast<uint8_t>(r));
            }
        }

        // Update the narrow constant tracker. Do this AFTER the
        // BX check above (we want to read the value as-of the BX,
        // not after the BX clears it).
        //
        // MOV Rd, #imm  →  track value
        // ADD Rd, PC, #imm  →  track (pc+8 ARM, pc+4 THUMB) + imm
        // Anything else writing Rd  →  invalidate
        const bool rd_written = ins.rd < 16 && writes_rd(ins);
        if (rd_written) {
            if (ins.op == armv4t::IrOp::MOV &&
                ins.op2.kind == armv4t::Op2::Kind::Imm) {
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = ins.op2.imm_value;
                reg_sym[ins.rd].known = false;
            } else if (ins.op == armv4t::IrOp::MOV &&
                       ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                       !ins.op2.shifted.by_register &&
                       ins.op2.shifted.imm_or_rs == 0 &&
                       ins.op2.shifted.rm < 16) {
                uint8_t rm = ins.op2.shifted.rm;
                if (rm == 15) {
                    uint32_t pc_val = ins.pc +
                        (entry_mode == CpuMode::Thumb ? 4u : 8u);
                    if (entry_mode == CpuMode::Thumb) pc_val &= ~uint32_t{3};
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = pc_val;
                } else if (reg_const[rm].known) {
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = reg_const[rm].value;
                } else {
                    reg_const[ins.rd].known = false;
                }
                if (rm < 15) {
                    if (reg_sym[rm].known) {
                        reg_sym[ins.rd] = reg_sym[rm];
                    } else {
                        reg_sym[ins.rd].known = true;
                        reg_sym[ins.rd].rn = rm;
                        reg_sym[ins.rd].offset = 0;
                    }
                } else {
                    reg_sym[ins.rd].known = false;
                }
            } else if ((ins.op == armv4t::IrOp::ADD ||
                        ins.op == armv4t::IrOp::SUB) &&
                       ins.rn == 15 &&
                       ins.op2.kind == armv4t::Op2::Kind::Imm) {
                uint32_t pc_val = ins.pc +
                    (entry_mode == CpuMode::Thumb ? 4u : 8u);
                if (entry_mode == CpuMode::Thumb) pc_val &= ~uint32_t{3};
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = ins.op == armv4t::IrOp::ADD
                    ? pc_val + ins.op2.imm_value : pc_val - ins.op2.imm_value;
                reg_sym[ins.rd].known = false;
            } else if ((ins.op == armv4t::IrOp::ADD ||
                        ins.op == armv4t::IrOp::SUB) &&
                       ins.rn < 16 &&
                       ins.op2.kind == armv4t::Op2::Kind::Imm &&
                       reg_const[ins.rn].known) {
                reg_const[ins.rd].known = true;
                if (ins.op == armv4t::IrOp::ADD) {
                    reg_const[ins.rd].value =
                        reg_const[ins.rn].value + ins.op2.imm_value;
                } else {
                    reg_const[ins.rd].value =
                        reg_const[ins.rn].value - ins.op2.imm_value;
                }
                reg_sym[ins.rd].known = false;
            } else if (ins.op == armv4t::IrOp::LDR &&
                       !ins.mem.writeback) {
                uint32_t ea = 0;
                if (eval_mem_imm_addr(ins, &ea) && can_read_at(ea, 4)) {
                    uint32_t raw = read_u32(ea & ~uint32_t{3});
                    raw = ror32(raw, (ea & 3u) * 8u);
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = raw;
                } else {
                    reg_const[ins.rd].known = false;
                }
                reg_sym[ins.rd].known = false;
            } else {
                // Any other write to Rd invalidates the tracker
                // entry. We don't try to detect "no write" — a
                // few false-invalidations (over-clearing) are fine
                // and bias toward conservative behavior.
                reg_const[ins.rd].known = false;
                reg_sym[ins.rd].known = false;
            }
            invalidate_mem_base(ins.rd);
            invalidate_sym_base(ins.rd);
            if (ins.cond != armv4t::Cond::AL) {
                no_carry = static_cast<uint16_t>(no_carry | (1u << ins.rd));
            } else if (reg_const[ins.rd].known) {
                no_carry = static_cast<uint16_t>(no_carry & ~(1u << ins.rd));
            }
            // Load-address lane: a word LDR from a static (non-literal-
            // pool) in-image address, conditional or not.
            reg_load[ins.rd].known = false;
            uint32_t load_ea = 0;
            if (ins.op == armv4t::IrOp::LDR && !ins.mem.writeback &&
                !ins.mem.by_register && ins.mem.rn != 15 &&
                eval_mem_imm_addr(ins, &load_ea) && (load_ea & 3u) == 0 &&
                can_read_at(load_ea, 4)) {
                reg_load[ins.rd] = {true, load_ea};
            }
        }
        if (const uint16_t extra = extra_written_regs(ins)) {
            for (int r = 0; r < 15; ++r) {
                if ((extra & (1u << r)) == 0) continue;
                reg_const[r].known = false;
                reg_sym[r].known = false;
                reg_scaled[r].known = false;
                reg_table[r].known = false;
                reg_load[r].known = false;
                invalidate_mem_base(static_cast<uint8_t>(r));
                invalidate_sym_base(static_cast<uint8_t>(r));
            }
        }
        if (ins.is_call || ins.op == armv4t::IrOp::SWI) {
            no_carry |= kCallClobbered;
            for (int r = 0; r < 15; ++r) {
                if (kCallClobbered & (1u << r)) reg_load[r].known = false;
            }
        }

        // Maintain the jump-table address trackers (independent of the
        // reg_const tracker above; consumed by the LDR detection). A
        // write to Rd clears its scaled/table state unless this same
        // instruction re-establishes it.
        if (rd_written) {
            bool set_scaled = false, set_table = false;
            const armv4t::Op2& o = ins.op2;
            if (ins.op == armv4t::IrOp::MOV &&
                o.kind == armv4t::Op2::Kind::Shifted &&
                !o.shifted.by_register &&
                o.shifted.type == armv4t::ShiftType::LSL &&
                o.shifted.imm_or_rs > 0 && o.shifted.rm < 16) {
                // Rd = (unknown index) << k
                reg_scaled[ins.rd].known = true;
                reg_scaled[ins.rd].shift = o.shifted.imm_or_rs;
                reg_scaled[ins.rd].bound_known = false;
                if (o.shifted.rm < 16 && reg_bound[o.shifted.rm].known) {
                    reg_scaled[ins.rd].bound_known = true;
                    reg_scaled[ins.rd].max_index =
                        reg_bound[o.shifted.rm].max_index;
                }
                set_scaled = true;
            } else if (ins.op == armv4t::IrOp::ADD &&
                       o.kind == armv4t::Op2::Kind::Shifted &&
                       !o.shifted.by_register && o.shifted.rm < 16 &&
                       ins.rn < 16) {
                const uint8_t rm = o.shifted.rm;
                auto rom_const = [&](uint8_t r) {
                    return reg_const[r].known &&
                           addr_in_rom(reg_const[r].value);
                };
                if (o.shifted.type == armv4t::ShiftType::LSL &&
                    o.shifted.imm_or_rs > 0) {
                    // One-instruction scaled add: ADD Rd, Rbase, Ri LSL#k
                    // (Ri is the inline-scaled index; bound rides on it.)
                    if (rom_const(ins.rn)) {
                        reg_table[ins.rd] = {true, reg_const[ins.rn].value,
                                             1u << o.shifted.imm_or_rs};
                        if (reg_bound[rm].known) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index =
                                reg_bound[rm].max_index;
                        }
                        set_table = true;
                    }
                } else if (o.shifted.imm_or_rs == 0) {
                    // Plain reg add: one side a ROM-pointer base, the
                    // other a previously-scaled index (bound carried).
                    if (rom_const(ins.rn) && reg_scaled[rm].known) {
                        reg_table[ins.rd] = {true, reg_const[ins.rn].value,
                                             1u << reg_scaled[rm].shift};
                        if (reg_scaled[rm].bound_known) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index =
                                reg_scaled[rm].max_index;
                        }
                        set_table = true;
                    } else if (rom_const(rm) && reg_scaled[ins.rn].known) {
                        reg_table[ins.rd] = {true, reg_const[rm].value,
                                             1u << reg_scaled[ins.rn].shift};
                        if (reg_scaled[ins.rn].bound_known) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index =
                                reg_scaled[ins.rn].max_index;
                        }
                        set_table = true;
                    }
                }
            }
            if (!set_scaled) reg_scaled[ins.rd].known = false;
            if (!set_table)  reg_table[ins.rd].known = false;
            // The cmp-derived bound has been consumed into reg_scaled/
            // reg_table above; any non-branch write to Rd retires it. A
            // branch is skipped because the guard branch itself is what
            // SET the bound (see the CMP-bound capture above).
            if (!ins.is_branch) reg_bound[ins.rd].known = false;
        }

        if ((ins.op == armv4t::IrOp::LDR ||
             ins.op == armv4t::IrOp::LDRB ||
             ins.op == armv4t::IrOp::LDRH ||
             ins.op == armv4t::IrOp::LDRSB ||
             ins.op == armv4t::IrOp::LDRSH ||
             ins.op == armv4t::IrOp::STR ||
             ins.op == armv4t::IrOp::STRB ||
             ins.op == armv4t::IrOp::STRH) &&
            ins.mem.writeback && ins.mem.rn < 16) {
            if (ins.mem.rn < 15) reg_load[ins.mem.rn].known = false;
            reg_const[ins.mem.rn].known = false;
            reg_sym[ins.mem.rn].known = false;
            reg_scaled[ins.mem.rn].known = false;
            reg_table[ins.mem.rn].known = false;
            invalidate_mem_base(ins.mem.rn);
            invalidate_sym_base(ins.mem.rn);
        }

        // Seed every call's return address as a function entry. A guest
        // return reached AFTER a scheduler preemption is re-dispatched by
        // the run loop (the host C call stack having been unwound to swap
        // CPUs), so the return address must be a real dispatch entry, not
        // a mid-body location. THUMB BL pairs already seed their
        // continuation explicitly above, so skip those here.
        if (ins.is_call) {
            const bool thumb_bl_pair =
                entry_mode == CpuMode::Thumb &&
                (ins.op == armv4t::IrOp::BL_prefix ||
                 ins.op == armv4t::IrOp::BL_suffix);
            if (!thumb_bl_pair)
                enqueue_resolved_target(ins.pc + step, entry_mode,
                                        "call-return");
        }

        pc += step;
        fn.end_addr = pc;

        // A conditional indirect transfer is not a terminator when
        // its condition fails. Because the finder stops at indirects,
        // seed the fall-through root explicitly. An indirect CALL
        // (BLX register) likewise returns to the following instruction,
        // so seed its continuation even when unconditional.
        if ((ins.is_return || ins.is_indirect) &&
            (ins.cond != armv4t::Cond::AL || ins.is_call)) {
            enqueue_resolved_target(pc, entry_mode, "branch");
        }

        // Stop at unconditional terminators:
        //   1. A return-shaped instruction (BX LR, pop {pc}, ldm
        //      r?, {..., pc}).
        //   2. An indirect control transfer.
        //   3. An unconditional branch (B with cond=AL) — we'll
        //      reach the target by adding it to the worklist.
        if (ins.is_return || ins.is_indirect) {
            break;
        }
        // A THUMB BL/BLX is a prefix+suffix HALFWORD PAIR; the prefix
        // half is branch-shaped but must NOT end the function — the walk
        // has to continue through the suffix (the actual call) and the
        // code after it. Ending here splits the pair across a function
        // boundary, orphaning the suffix as a non-dispatchable address.
        // (`thumb_bl_half` was computed at the top of this iteration.)
        const bool unconditional_branch =
            ins.is_branch &&
            ins.cond == armv4t::Cond::AL &&
            !ins.is_call &&
            !thumb_bl_half;
        if (unconditional_branch) {
            break;
        }
    }

    functions_.push_back(std::move(fn));
}

std::vector<FunctionFinder::ScanRegion>
FunctionFinder::build_scan_regions() const {
    // Image span minus code_copy sources, split into pieces.
    std::vector<std::pair<uint32_t, uint32_t>> cuts;  // [lo, hi) source spans
    for (const auto& cc : code_copies_) {
        if (!addr_in_rom(cc.source_start)) continue;
        uint64_t hi = static_cast<uint64_t>(cc.source_start) + cc.size;
        uint64_t img_hi = static_cast<uint64_t>(rom_base_) + rom_size_;
        cuts.emplace_back(cc.source_start,
                          static_cast<uint32_t>(std::min(hi, img_hi)));
    }
    std::sort(cuts.begin(), cuts.end());
    std::vector<ScanRegion> out;
    uint64_t cur = rom_base_;
    const uint64_t img_end = static_cast<uint64_t>(rom_base_) + rom_size_;
    for (const auto& [lo, hi] : cuts) {
        if (lo > cur) {
            out.push_back(ScanRegion{static_cast<uint32_t>(cur),
                                     static_cast<uint32_t>(lo - cur), {}});
        }
        cur = std::max<uint64_t>(cur, hi);
    }
    if (cur < img_end) {
        out.push_back(ScanRegion{static_cast<uint32_t>(cur),
                                 static_cast<uint32_t>(img_end - cur), {}});
    }
    for (const auto& cc : code_copies_) {
        uint32_t src = 0;
        if (!map_addr_to_source(cc.runtime_start, &src)) continue;
        out.push_back(ScanRegion{cc.runtime_start, cc.size, {}});
    }
    for (auto& r : out) r.cover.assign(r.size, 0);
    // Mark every walked body. end_addr is the linear walk end here.
    for (const auto& fn : functions_) {
        const uint8_t bit = fn.mode == CpuMode::Thumb ? 2u : 1u;
        for (auto& r : out) {
            if (fn.addr < r.start || fn.addr - r.start >= r.size) continue;
            uint64_t end = std::min<uint64_t>(
                std::max(fn.end_addr, fn.addr + 1u),
                static_cast<uint64_t>(r.start) + r.size);
            for (uint64_t a = fn.addr; a < end; ++a) r.cover[a - r.start] |= bit;
            break;
        }
    }
    return out;
}

bool FunctionFinder::validate_candidate(
        uint32_t addr, CpuMode mode,
        const std::vector<ScanRegion>& regions, bool strict) const {
    auto cover_at = [&](uint32_t a, bool* inside) -> uint8_t {
        for (const auto& r : regions) {
            if (a >= r.start && a - r.start < r.size) {
                *inside = true;
                return r.cover[a - r.start];
            }
        }
        *inside = false;
        return 0;
    };
    static const uint32_t trace_addr = [] {
        const char* e = std::getenv("NDSRECOMP_SCAN_TRACE");
        return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
    }();
    const bool trace = trace_addr != 0 && trace_addr == addr;
#define REJECT(why) do { if (trace) std::fprintf(stderr, \
        "[scan-trace] 0x%08X %s reject at 0x%08X: %s\n", addr, \
        mode == CpuMode::Thumb ? "thumb" : "arm", pc, why); return false; } while (0)
    const bool thumb = mode == CpuMode::Thumb;
    const uint32_t step = thumb ? 2u : 4u;
    uint32_t pc = addr;
    if (addr & (step - 1u)) REJECT("align");
    const uint8_t other_bit = thumb ? 1u : 2u;
    constexpr uint32_t kMaxWalk = 384;
    unsigned zero_run = 0;
    unsigned non_al = 0;
    bool flags_set = false;
    for (uint32_t n = 0; n < kMaxWalk; ++n) {
        bool inside = false;
        const uint8_t cov = cover_at(pc, &inside);
        if (!inside || !can_read_at(pc, step)) REJECT("outside");
        if (cov & other_bit) REJECT("mode-conflict");           // mode conflict
        if (n > 0 && (cov & (thumb ? 2u : 1u))) return true;  // joins known code
        if (addr_in_data_range(pc)) REJECT("data-range");
        armv4t::Instr ins = thumb
            ? armv4t::ThumbDecoder::decode(read_u16(pc), pc)
            : armv4t::ArmDecoder::decode(read_u32(pc), pc);
        if (ins.is_undefined) REJECT("undefined");
        const uint32_t raw = thumb ? read_u16(pc) : read_u32(pc);
        if (raw == 0) {
            // ARM `andeq r0,r0,r0` / THUMB `movs r0,r0`: zero fill.
            if (!thumb || ++zero_run >= 2) REJECT("zero");
        } else {
            zero_run = 0;
        }
        // Flags are undefined at a function entry, so a conditional
        // instruction before anything set them is data, not code. Real
        // conditional runs (`cmp; ldrne; cmpne; strhne...`) pass this;
        // random data words carry random condition nibbles and fail it.
        const bool conditional = ins.cond != armv4t::Cond::AL &&
                                 (thumb || (raw >> 28) != 0xFu);
        if (conditional && !flags_set) REJECT("cond-before-flags");
        if (!thumb) {
            const uint32_t c = raw >> 28;
            if (n == 0 && c < 0xEu) REJECT("first-not-AL");
            // A long all-conditional run with flags set is still code
            // (`cmp; ldrshne; cmpne; strhne x6; bxne`); only a wholly
            // conditional window is suspicious.
            if (c < 0xEu) ++non_al; else non_al = 0;
            if (non_al > 24) REJECT("non-AL");
        }
        if (ins.set_flags || ins.op == armv4t::IrOp::CMP ||
            ins.op == armv4t::IrOp::CMN || ins.op == armv4t::IrOp::TST ||
            ins.op == armv4t::IrOp::TEQ) {
            flags_set = true;
        }
        if (thumb && ins.op == armv4t::IrOp::BL_suffix) REJECT("orphan-bl");
        uint32_t target = 0;
        bool have_target = false;
        if (thumb && ins.op == armv4t::IrOp::BL_prefix) {
            if (!can_read_at(pc + 2, 2)) REJECT("bl-end");
            armv4t::Instr lo = armv4t::ThumbDecoder::decode(read_u16(pc + 2), pc + 2);
            if (lo.op != armv4t::IrOp::BL_suffix) REJECT("bl-prefix-alone");
            target = (ins.branch_target + lo.swi_imm) &
                     (lo.branch_exchange ? ~uint32_t{3} : ~uint32_t{1});
            have_target = true;
            pc += 2;  // consume the suffix too
        } else if (ins.is_branch && !ins.is_indirect && ins.branch_target != 0) {
            target = ins.branch_target & ~uint32_t{1};
            have_target = true;
        }
        // A callee can return its result in the flags (`bl f; strcs ...`),
        // so the instruction after a call may be conditional.
        if (ins.is_call || (thumb && ins.op == armv4t::IrOp::BL_prefix))
            flags_set = true;
        if (have_target) {
            if (addr_in_data_range(target)) REJECT("target-data");
            if (!can_read_at(target, 2)) {
                // Cross-image control flow is normal (overlay -> main,
                // main -> ITCM); accept a target outside this image only
                // inside a DS code window (ITCM, main RAM, ARM7 WRAM).
                const bool ds_code =
                    (target >= 0x01FF8000u && target < 0x02400000u) ||
                    (target >= 0x027E0000u && target < 0x02800000u) ||
                    (target >= 0x037F8000u && target < 0x03810000u);
                if (!ds_code) REJECT("target-outside");
            }
        }
        pc += step;
        // strict (terminator-sweep candidates, which have no pointer or
        // prologue evidence): the linear path must end in a real return or
        // a tail branch to an already-walked entry; joining known code
        // above also counts.
        if (ins.is_return && ins.cond == armv4t::Cond::AL) return true;
        if (ins.is_return) continue;
        if (ins.is_indirect && ins.cond == armv4t::Cond::AL && !ins.is_call) {
            if (strict) { REJECT("strict-indirect"); }
            return true;
        }
        if (ins.is_branch && !ins.is_indirect && !ins.is_call &&
            ins.cond == armv4t::Cond::AL && !(thumb && ins.op == armv4t::IrOp::BL_prefix)) {
            if (!strict) return true;
            if (visited_.count(visit_key(target, mode)) == 0) { REJECT("strict-tail"); }
            return true;
        }
    }
    // kMaxWalk decoded instructions with every check above passing is a
    // long straight-line body, not data.
    if (strict) REJECT("no-terminator");
    return true;
#undef REJECT
}

std::vector<FunctionSeed> FunctionFinder::static_scan_candidates() {
    std::vector<ScanRegion> regions = build_scan_regions();
    std::vector<FunctionSeed> out;
    std::unordered_set<uint64_t> picked;
    auto in_exec_region = [&](uint32_t a) {
        for (const auto& r : regions)
            if (a >= r.start && a - r.start < r.size) return true;
        return false;
    };
    std::unordered_set<uint64_t> rejected_now;
    auto consider = [&](uint32_t tgt, CpuMode m, const char* tag,
                        std::size_t* seeds, std::size_t* rejects,
                        bool strict = false) {
        const uint64_t k = visit_key(tgt, m);
        if (visited_.count(k) || picked.count(k) || rejected_now.count(k)) return;
        if (!validate_candidate(tgt, m, regions, strict)) {
            rejected_now.insert(k);
            ++*rejects;
            return;
        }
        picked.insert(k);
        char nm[64];
        std::snprintf(nm, sizeof nm, "%s%c_%08X", tag,
                      m == CpuMode::Thumb ? 't' : 'a', tgt);
        out.push_back(FunctionSeed{tgt, m, nm});
        ++*seeds;
    };
    for (const auto& c : pending_candidates_) {
        if (!in_exec_region(c.addr)) continue;
        consider(c.addr, c.mode, "ext", &stats_.extern_candidate_seeds,
                 &stats_.extern_candidate_rejects);
    }
    pending_candidates_.clear();
    // GapFill (terminator sweep): code after a walked body that ended at
    // an unconditional terminator is usually the next function (leaf
    // functions sit back to back after `bx lr`), possibly behind a literal
    // pool. Try each following uncovered slot in the body's mode, skipping
    // up to kPoolSkip pool words, and take the first that validates.
    {
        auto cover_of = [&](uint32_t a, bool* in) -> uint8_t {
            for (const auto& r : regions)
                if (a >= r.start && a - r.start < r.size) {
                    *in = true;
                    return r.cover[a - r.start];
                }
            *in = false;
            return 0;
        };
        constexpr uint32_t kPoolSkip = 64;  // words
        for (const auto& fn : functions_) {
            const bool thumb = fn.mode == CpuMode::Thumb;
            const uint32_t step = thumb ? 2u : 4u;
            if (fn.end_addr < fn.addr + step) continue;
            const uint32_t last = fn.end_addr - step;
            if (!can_read_at(last, step)) continue;
            armv4t::Instr li = thumb
                ? armv4t::ThumbDecoder::decode(read_u16(last), last)
                : armv4t::ArmDecoder::decode(read_u32(last), last);
            if (li.is_undefined) continue;
            const bool term = li.is_return ||
                (li.is_indirect && !li.is_call && li.cond == armv4t::Cond::AL) ||
                (li.is_branch && !li.is_call && !li.is_indirect &&
                 li.cond == armv4t::Cond::AL);
            if (!term) continue;
            uint32_t a = fn.end_addr;
            for (uint32_t n = 0; n < kPoolSkip * (thumb ? 2u : 1u); ++n, a += step) {
                bool in = false;
                if (cover_of(a, &in) != 0 || !in) break;
                if (addr_in_data_range(a)) break;
                const uint64_t k = visit_key(a, fn.mode);
                if (visited_.count(k) || picked.count(k)) break;
                if (thumb && (a & 3u)) continue;
                if (validate_candidate(a, fn.mode, regions, /*strict=*/true)) {
                    picked.insert(k);
                    char nm[64];
                    std::snprintf(nm, sizeof nm, "gapt%c_%08X",
                                  thumb ? 't' : 'a', a);
                    out.push_back(FunctionSeed{a, fn.mode, nm});
                    ++stats_.gap_fill_seeds;
                    break;
                }
            }
        }
    }
    for (const auto& r : regions) {
        // PointerHarvest: aligned words outside any walked body.
        for (uint32_t off = (4u - (r.start & 3u)) & 3u; off + 4 <= r.size; off += 4) {
            if (r.cover[off] | r.cover[off + 2]) continue;
            const uint32_t a = r.start + off;
            if (addr_in_data_range(a)) continue;
            const uint32_t w = read_u32(a);
            if (w < 0x100u) continue;
            const CpuMode m = (w & 1u) ? CpuMode::Thumb : CpuMode::Arm;
            const uint32_t t = w & ~uint32_t{1};
            if (m == CpuMode::Arm && (t & 3u)) continue;
            if (!in_exec_region(t)) continue;
            consider(t, m, "ptr", &stats_.ptr_harvest_seeds,
                     &stats_.ptr_harvest_rejects);
        }
        // GapFill: function prologues in uncovered bytes.
        for (uint32_t off = (2u - (r.start & 1u)) & 1u; off + 2 <= r.size; off += 2) {
            if (r.cover[off]) continue;
            const uint32_t a = r.start + off;
            if (addr_in_data_range(a)) continue;
            if ((a & 3u) == 0 && off + 4 <= r.size && !r.cover[off + 2]) {
                const uint32_t w = read_u32(a);
                // stmdb sp!, {..., lr}, or its one-register form
                // `str lr,[sp,#-4]!` (table-called wrappers in DQ9 ov016).
                if ((w & 0xFFFF4000u) == 0xE92D4000u || w == 0xE52DE004u) {
                    consider(a, CpuMode::Arm, "gap", &stats_.gap_fill_seeds,
                             &stats_.gap_fill_rejects);
                    continue;
                }
            }
            const uint16_t h = read_u16(a);
            // push {..., lr}. Only at 4-aligned addresses: the upper half of
            // every ARM word sits at 2 mod 4, and ARM `ldrlt rX,[sp,..]`
            // (0xB59Dxxxx) puts a perfect THUMB push there. The compilers
            // here word-align THUMB functions (~95% of observed entries);
            // the rest are reached by direct calls. Strict validation: a
            // prologue followed by a branch to nowhere known is not trusted.
            if ((a & 3u) == 0 && (h & 0xFF00u) == 0xB500u) {
                consider(a, CpuMode::Thumb, "gap", &stats_.gap_fill_seeds,
                         &stats_.gap_fill_rejects, /*strict=*/true);
            }
        }
    }
    // ARM before THUMB. The upper half of every ARM word is a plausible
    // THUMB instruction stream, so a THUMB candidate is only judged once
    // the ARM bodies around it exist and the mode-conflict check can see
    // them. Defer harvested/gap THUMB candidates while any ARM candidate is
    // pending; direct-call (ext) candidates carry real evidence and stay.
    bool any_arm = false;
    for (const auto& c : out)
        if (c.mode == CpuMode::Arm) { any_arm = true; break; }
    if (any_arm) {
        std::vector<FunctionSeed> kept;
        for (auto& c : out) {
            if (c.mode == CpuMode::Thumb && c.name.rfind("ext", 0) != 0) {
                ++deferred_thumb_;
                // Re-found (and re-counted) in a later round.
                if (c.name.rfind("ptr", 0) == 0) --stats_.ptr_harvest_seeds;
                else                             --stats_.gap_fill_seeds;
                continue;
            }
            kept.push_back(std::move(c));
        }
        out.swap(kept);
    }
    return out;
}

void FunctionFinder::run(std::size_t max_functions) {
    // Validate seeds against data_ranges before any walking. A seed
    // landing inside a data_range is a collision (the TOML author
    // is contradicting themselves; or a jump_table expanded onto
    // bytes that another data_range marked as "not code").
    for (const auto& s : seeds_) {
        if (addr_in_data_range(s.addr)) {
            record_collision(s.addr, 0, s.name, "seed");
        }
    }

    // Snapshot seed (addr, mode) keys so we can later compute the
    // manual-seed/walk-overlap breakdown for the discovery summary.
    std::unordered_set<uint64_t> seed_keys;
    std::unordered_map<uint64_t, FunctionSeed> seed_by_key;
    for (const auto& s : seeds_) {
        uint64_t k = visit_key(s.addr, s.mode);
        seed_keys.insert(k);
        seed_by_key.emplace(k, s);
    }
    stats_.manual_seeds_total = seeds_.size();

    // Dedup-at-push: every (addr,mode) ever queued. discover_one already
    // dedups at visit time, so the worklist otherwise fills with
    // already-queued/visited no-ops — on Minish Cap it bloated to ~170k
    // of mostly-duplicate branch targets. Skipping a re-push is
    // output-neutral (the discovered closure is unchanged) and is the
    // dominant regen-speed cost. Seeded with the initial seed keys.
    std::unordered_set<uint64_t> enqueued = seed_keys;

    struct QueuedSeed {
        FunctionSeed seed;
        bool required;
    };

    // Seed worklist. Walk-discovered control-flow roots are inserted
    // immediately after the current item so local continuations cannot
    // be starved behind thousands of metadata seeds when max_functions
    // is intentionally bounded during bring-up.
    std::vector<QueuedSeed> worklist;
    worklist.reserve(seeds_.size());
    for (const auto& s : seeds_) {
        worklist.push_back(QueuedSeed{s, true});
    }
    std::size_t required_remaining = seeds_.size();

    // Per-key origin marker: was this (addr, mode) reached as a
    // seed, by walk, or both? Walks add "walk"; seeds add "seed".
    // Both → "redundant_manual".
    std::unordered_map<uint64_t, uint8_t> origin_mask;  // 1=seed, 2=walk
    for (const auto& s : seeds_) {
        origin_mask[visit_key(s.addr, s.mode)] |= 1u;
    }

    std::size_t pos = 0;
    for (unsigned scan_round = 0;; ++scan_round) {
    while (pos < worklist.size() &&
           (functions_.size() < max_functions || required_remaining > 0)) {
        // Safety net: discovery must converge (worklist drains toward
        // empty). A runaway past this bound means a detector or decoder
        // change is seeding garbage — halt loudly rather than exhaust
        // memory. Baseline Minish Cap discovery peaks near ~170k, so
        // 2M is far above any healthy run.
        if (worklist.size() > 2000000u) {
            std::fprintf(stderr,
                "[finder] WORKLIST BRAKE at %zu (pos=%zu functions=%zu) "
                "— halting; discovery is not converging\n",
                worklist.size(), pos, functions_.size());
            std::fflush(stderr);
            break;
        }
        QueuedSeed queued = worklist[pos++];
        if (queued.required && required_remaining > 0) {
            --required_remaining;
        }

        FunctionSeed s = queued.seed;
        uint64_t s_key = visit_key(s.addr, s.mode);
        auto seed_it = seed_by_key.find(s_key);
        bool current_required = queued.required || seed_it != seed_by_key.end();
        if (!current_required && functions_.size() >= max_functions) {
            continue;
        }
        if (seed_it != seed_by_key.end()) {
            if (!seed_it->second.name.empty()) {
                s.name = seed_it->second.name;
            }
            if (s.source_addr == 0) {
                s.source_addr = seed_it->second.source_addr;
            }
            if (s.exact_size == 0) {
                s.exact_size = seed_it->second.exact_size;
            }
        }

        std::size_t before = functions_.size();
        discover_one(s.addr, s.mode, s.name, s.source_addr, s.exact_size);
        if (functions_.size() == before) {
            uint32_t src = 0;
            if (s.source_addr == 0 && s.addr != 0 &&
                !map_addr_to_source(s.addr, &src)) {
                external_targets_.emplace(s_key, 0);
            }
            continue;
        }

        // The function we just added is at the end of functions_.
        const Function& fn = functions_.back();
        std::vector<QueuedSeed> discovered_seeds;
        static const bool prov = std::getenv("NDSRECOMP_PROVENANCE") != nullptr;
        if (prov) {
            for (uint32_t t : fn.direct_branch_targets)
                std::fprintf(stderr, "[prov] %08X%c <- %08X%c\n", t,
                             fn.mode == CpuMode::Thumb ? 't' : 'a', fn.addr,
                             fn.mode == CpuMode::Thumb ? 't' : 'a');
            for (const auto& ms : mode_switch_seeds_)
                std::fprintf(stderr, "[prov] %08X%c <- %08X%c\n", ms.addr,
                             ms.mode == CpuMode::Thumb ? 't' : 'a', fn.addr,
                             fn.mode == CpuMode::Thumb ? 't' : 'a');
        }

        // Each direct branch target becomes a new seed at the SAME
        // mode (control flow within a function doesn't switch
        // ARM/THUMB without a BX, which is indirect).
        for (uint32_t t : fn.direct_branch_targets) {
            if (authoritative_seeds_) break;
            uint64_t k = visit_key(t, fn.mode);
            // Mark walk-origin. If this address was ALSO a seed,
            // it now has both bits set (redundant_manual).
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;  // already queued
            discovered_seeds.push_back(
                QueuedSeed{FunctionSeed{t, fn.mode, ""}, current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        // Drain any mode-switching seeds discovered during this
        // function's walk (BX Rm to a tracked-constant THUMB
        // target from ARM code, and vice versa).
        for (const auto& ms : mode_switch_seeds_) {
            if (authoritative_seeds_) break;
            uint64_t k = visit_key(ms.addr, ms.mode);
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;  // already queued
            discovered_seeds.push_back(QueuedSeed{ms, current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        mode_switch_seeds_.clear();
        if (!discovered_seeds.empty()) {
            worklist.insert(worklist.begin() + static_cast<std::ptrdiff_t>(pos),
                            discovered_seeds.begin(),
                            discovered_seeds.end());
        }
        // If THIS function came from a seed (not a walk), it was
        // discovered "as a seed", but the function's body still
        // counts toward "walked from here" for its branches.
        uint64_t this_key = visit_key(fn.addr, fn.mode);
        if (seed_keys.count(this_key) == 0) {
            // It came from a walk (a branch target reached during
            // a prior function's discovery).
            origin_mask[this_key] |= 2u;
        }
    }
    // Static scan fixed point: harvest new candidates from the bytes the
    // walk has not claimed, walk them, repeat. Candidates are required
    // (they bypass max_functions) because they are validated entries.
    if (!static_scan_ || authoritative_seeds_ || scan_round >= 32) break;
    std::vector<FunctionSeed> cands = static_scan_candidates();
    std::size_t added = 0;
    for (auto& c : cands) {
        uint64_t k = visit_key(c.addr, c.mode);
        origin_mask[k] |= 2u;
        if (!enqueued.insert(k).second) continue;
        worklist.push_back(QueuedSeed{c, true});
        ++required_remaining;
        ++added;
    }
    ++stats_.static_scan_rounds;
    if (added == 0) break;
    }  // static-scan rounds

    // Apply exclude_func filter — remove functions whose entry
    // address was declared in [[exclude_func]].
    if (!excluded_.empty()) {
        std::size_t before = functions_.size();
        auto last = std::remove_if(functions_.begin(), functions_.end(),
            [&](const Function& fn) {
                return excluded_.count(fn.addr) > 0;
            });
        functions_.erase(last, functions_.end());
        stats_.excluded_count = before - functions_.size();
    }

    // Sort by address.
    std::sort(functions_.begin(), functions_.end(),
              [](const Function& a, const Function& b) {
                  if (a.addr != b.addr) return a.addr < b.addr;
                  return a.mode < b.mode;
              });
    // Set end_addr to the next same-mode function start when adjacent
    // (approximate boundary). ARM and THUMB entries occupy the same byte
    // address space but do not share instruction boundaries; a false-start
    // THUMB entry in the middle of ARM code must not truncate the ARM body.
    for (std::size_t i = 0; i < functions_.size(); ++i) {
        for (std::size_t j = i + 1; j < functions_.size(); ++j) {
            if (functions_[j].mode != functions_[i].mode) continue;
            if (functions_[j].addr > functions_[i].addr) {
                functions_[i].end_addr = std::min(
                    functions_[i].end_addr,
                    functions_[j].addr);
            }
            break;
        }
    }

    // Never end a THUMB function in the middle of a BL/BLX prefix+suffix
    // pair. If the last halfword is a BL prefix, pull in its suffix so the
    // call is emitted whole — otherwise the function's fall-through would
    // dispatch the orphaned suffix address, which is no function entry
    // (the boundary can land here because the prefix is branch-shaped).
    for (auto& fn : functions_) {
        if (fn.mode != CpuMode::Thumb) continue;
        for (int guard = 0; guard < 4; ++guard) {
            if (fn.end_addr < fn.addr + 2 || !can_read_at(fn.end_addr - 2, 2))
                break;
            armv4t::Instr last = armv4t::ThumbDecoder::decode(
                read_u16(fn.end_addr - 2), fn.end_addr - 2);
            if (last.op == armv4t::IrOp::BL_prefix &&
                can_read_at(fn.end_addr, 2)) {
                fn.end_addr += 2;  // include the suffix
            } else {
                break;
            }
        }
    }

    // Stats
    stats_.functions_total = functions_.size();
    stats_.functions_arm = 0;
    stats_.functions_thumb = 0;
    stats_.discovered_by_walk_only = 0;
    stats_.redundant_manual = 0;
    stats_.manual_seeds_only = 0;
    for (const auto& fn : functions_) {
        if (fn.mode == CpuMode::Thumb) ++stats_.functions_thumb;
        else                            ++stats_.functions_arm;
    }
    // Walk-source breakdown for emitted functions only.
    // origin_mask: 1=seed, 2=walk, 3=both.
    for (const auto& fn : functions_) {
        uint8_t mask = origin_mask[visit_key(fn.addr, fn.mode)];
        switch (mask) {
            case 1:
                ++stats_.manual_seeds_only;
                break;
            case 2:
                ++stats_.discovered_by_walk_only;
                break;
            case 3:
                ++stats_.redundant_manual;
                break;
            default: break;
        }
    }
}

}  // namespace ndsrecomp
