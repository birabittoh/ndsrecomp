// Pins cross-block register-constant carrying in FunctionFinder.
//
// The finder walks linearly and treats every B/Bcc target as a separate
// seed. Before carrying, each such seed started with an empty constant
// tracker, so a `bx rN` whose rN was loaded one block earlier stayed
// UNKNOWN. The canonical victim is NitroSDK crt0's tail:
//
//     ldr r1, =NitroMain ; tst sp,#4 ; bne L1 ; b L2
//   L1: sub sp,sp,#4
//   L2: bx r1
//
// (DQ9 lost its entire ARM9 game behind this one site.) The cases below pin
// that shape plus the two rules that keep carrying sound: a value written by
// a conditional instruction, or held in a call-clobbered register across a
// call, is not carried to the branch target.

#include "function_finder.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using ndsrecomp::CpuMode;
using ndsrecomp::FunctionFinder;
using ndsrecomp::FunctionSeed;

constexpr uint32_t kBase = 0x02000000u;
constexpr uint32_t kNop = 0xE1A00000u;     // mov r0, r0
constexpr uint32_t kBxR1 = 0xE12FFF11u;
constexpr uint32_t kBxLr = 0xE12FFF1Eu;
constexpr uint32_t kThumbBxLrPair = 0x47704770u;

struct Image {
    std::vector<uint8_t> bytes;
    void put(uint32_t addr, uint32_t word) {
        uint32_t off = addr - kBase;
        if (bytes.size() < off + 4) bytes.resize(off + 4, 0);
        for (int i = 0; i < 4; ++i) {
            bytes[off + i] = static_cast<uint8_t>(word >> (8 * i));
        }
    }
};

// ARM B/BL/Bcc encoder: cond<<28 | 101 L | imm24.
uint32_t branch(uint32_t cond, bool link, uint32_t from, uint32_t to) {
    int32_t off = static_cast<int32_t>(to - (from + 8)) >> 2;
    return (cond << 28) | 0x0A000000u | (link ? 0x01000000u : 0u) |
           (static_cast<uint32_t>(off) & 0x00FFFFFFu);
}

// ldr<cond> rd, [pc, #imm] targeting `lit`.
uint32_t ldr_lit(uint32_t cond, uint32_t rd, uint32_t pc, uint32_t lit) {
    return (cond << 28) | 0x059F0000u | (rd << 12) | (lit - (pc + 8));
}

bool has_function(const FunctionFinder& f, uint32_t addr, CpuMode mode) {
    for (const auto& fn : f.functions()) {
        if (fn.addr == addr && fn.mode == mode) return true;
    }
    return false;
}

bool run_case(const char* name, const Image& img, uint32_t probe,
              bool expect_found, CpuMode mode = CpuMode::Thumb) {
    FunctionFinder f(img.bytes.data(), img.bytes.size(), kBase);
    f.add_seed(FunctionSeed{kBase, CpuMode::Arm, "entry"});
    f.run();
    const bool found = has_function(f, probe, mode);
    const bool ok = found == expect_found;
    std::printf("%s %s: %s 0x%08X %s (expected %s), carried=%zu "
                "ptr_arrays=%zu/%zu\n",
                ok ? "PASS" : "FAIL", name,
                mode == CpuMode::Thumb ? "thumb" : "arm", probe,
                found ? "found" : "absent", expect_found ? "found" : "absent",
                f.stats().carried_state_walks, f.stats().ptr_arrays,
                f.stats().ptr_array_targets);
    return ok;
}

constexpr uint32_t AL = 0xE, NE = 0x1;

// crt0 tail: the constant crosses both the Bcc edge and the B edge.
Image crt0_shape() {
    Image img;
    img.put(kBase + 0x00, ldr_lit(AL, 1, kBase + 0x00, kBase + 0x1C));
    img.put(kBase + 0x04, 0xE31D0004u);  // tst sp, #4
    img.put(kBase + 0x08, branch(NE, false, kBase + 0x08, kBase + 0x10));
    img.put(kBase + 0x0C, branch(AL, false, kBase + 0x0C, kBase + 0x14));
    img.put(kBase + 0x10, 0xE24DD004u);  // sub sp, sp, #4
    img.put(kBase + 0x14, kBxR1);
    img.put(kBase + 0x18, kNop);
    img.put(kBase + 0x1C, kBase + 0x21);  // Thumb target
    img.put(kBase + 0x20, kThumbBxLrPair);
    return img;
}

// ldr r1,=A ; cmp r0,#0 ; ldrne r1,=B ; b L ; ... L: bx r1
// r1 is path-dependent at L, so nothing may be carried for it.
Image conditional_write() {
    Image img;
    img.put(kBase + 0x00, ldr_lit(AL, 1, kBase + 0x00, kBase + 0x20));
    img.put(kBase + 0x04, 0xE3500000u);  // cmp r0, #0
    img.put(kBase + 0x08, ldr_lit(NE, 1, kBase + 0x08, kBase + 0x24));
    img.put(kBase + 0x0C, branch(AL, false, kBase + 0x0C, kBase + 0x14));
    img.put(kBase + 0x10, kNop);
    img.put(kBase + 0x14, kBxR1);
    img.put(kBase + 0x18, kNop);
    img.put(kBase + 0x1C, kNop);
    img.put(kBase + 0x20, kBase + 0x31);  // A (Thumb)
    img.put(kBase + 0x24, kBase + 0x35);  // B (Thumb)
    img.put(kBase + 0x28, kNop);
    img.put(kBase + 0x2C, kNop);
    img.put(kBase + 0x30, kThumbBxLrPair);
    img.put(kBase + 0x34, kThumbBxLrPair);
    return img;
}

// ldr r1,=A ; <bl f | nop> ; b L ; ... L: bx r1 ; f: bx lr
Image across_call(bool with_call) {
    Image img;
    img.put(kBase + 0x00, ldr_lit(AL, 1, kBase + 0x00, kBase + 0x20));
    img.put(kBase + 0x04, with_call
        ? branch(AL, true, kBase + 0x04, kBase + 0x18) : kNop);
    img.put(kBase + 0x08, branch(AL, false, kBase + 0x08, kBase + 0x10));
    img.put(kBase + 0x0C, kNop);
    img.put(kBase + 0x10, kBxR1);
    img.put(kBase + 0x14, kNop);
    img.put(kBase + 0x18, kBxLr);
    img.put(kBase + 0x1C, kNop);
    img.put(kBase + 0x20, kBase + 0x31);
    img.put(kBase + 0x24, kNop);
    img.put(kBase + 0x28, kNop);
    img.put(kBase + 0x2C, kNop);
    img.put(kBase + 0x30, kThumbBxLrPair);
    return img;
}

// NitroSDK static-initializer loop (DQ9 0x0200EDC8): a zero-terminated
// array of ctor pointers, read with a CONDITIONAL load whose value only
// reaches the `blx` through a Bcc edge.
//   push {r4,lr} ; ldr r4,=table ; b test
//   body: blx r0 ; add r4,r4,#4
//   test: cmp r4,#0 ; ldrne r0,[r4] ; cmpne r0,#0 ; bne body ; pop {r4,pc}
// table: .word thumb_ctor|1, arm_ctor, 0
Image sinit_loop() {
    Image img;
    img.put(kBase + 0x00, 0xE92D4010u);  // push {r4, lr}
    img.put(kBase + 0x04, ldr_lit(AL, 4, kBase + 0x04, kBase + 0x2C));
    img.put(kBase + 0x08, branch(AL, false, kBase + 0x08, kBase + 0x14));
    img.put(kBase + 0x0C, 0xE12FFF30u);  // blx r0
    img.put(kBase + 0x10, 0xE2844004u);  // add r4, r4, #4
    img.put(kBase + 0x14, 0xE3540000u);  // cmp r4, #0
    img.put(kBase + 0x18, 0x15940000u);  // ldrne r0, [r4]
    img.put(kBase + 0x1C, 0x13500000u);  // cmpne r0, #0
    img.put(kBase + 0x20, branch(NE, false, kBase + 0x20, kBase + 0x0C));
    img.put(kBase + 0x24, 0xE8BD8010u);  // pop {r4, pc}
    img.put(kBase + 0x28, kNop);
    img.put(kBase + 0x2C, kBase + 0x40);  // literal: table
    for (uint32_t a = 0x30; a < 0x40; a += 4) img.put(kBase + a, kNop);
    img.put(kBase + 0x40, kBase + 0x51);  // Thumb ctor
    img.put(kBase + 0x44, kBase + 0x58);  // ARM ctor
    img.put(kBase + 0x48, 0);             // terminator
    img.put(kBase + 0x4C, kNop);
    img.put(kBase + 0x50, kThumbBxLrPair);
    img.put(kBase + 0x54, kNop);
    img.put(kBase + 0x58, kBxLr);
    return img;
}

// ldr r0,=thumb|1 ; cmp r1,#0 ; bx r0 — CMP decodes with rd=0 and must
// not be treated as a write to r0.
Image cmp_keeps_r0() {
    Image img;
    img.put(kBase + 0x00, ldr_lit(AL, 0, kBase + 0x00, kBase + 0x10));
    img.put(kBase + 0x04, 0xE3510000u);  // cmp r1, #0
    img.put(kBase + 0x08, 0xE12FFF10u);  // bx r0
    img.put(kBase + 0x0C, kNop);
    img.put(kBase + 0x10, kBase + 0x21);
    img.put(kBase + 0x14, kNop);
    img.put(kBase + 0x18, kNop);
    img.put(kBase + 0x1C, kNop);
    img.put(kBase + 0x20, kThumbBxLrPair);
    return img;
}

// Branch-ladder switch. `guard` selects the shape:
//   LS: cmp r0,#2 ; addls pc,pc,r0,lsl#2 ; b dflt ; b c0 ; b c1 ; b c2
//   PL: subs r0,r0,#1 ; addpl pc,pc,r0,lsl#2 ; (same ladder, unbounded)
// Cases are ARM `bx lr` at 0x40/0x44/0x48, default at 0x50, and the word
// after the ladder (0x20) is a `mov` so the unbounded walk stops at 3.
Image branch_ladder(bool ls_guard) {
    Image img;
    img.put(kBase + 0x00, ls_guard ? 0xE3500002u    // cmp r0, #2
                                   : 0xE2500001u);  // subs r0, r0, #1
    img.put(kBase + 0x04, (ls_guard ? 0x9u : 0x5u) << 28 | 0x008FF100u);
    img.put(kBase + 0x08, branch(AL, false, kBase + 0x08, kBase + 0x50));
    img.put(kBase + 0x0C, branch(AL, false, kBase + 0x0C, kBase + 0x40));
    img.put(kBase + 0x10, branch(AL, false, kBase + 0x10, kBase + 0x44));
    img.put(kBase + 0x14, branch(AL, false, kBase + 0x14, kBase + 0x48));
    img.put(kBase + 0x18, kNop);
    for (uint32_t a = 0x1C; a < 0x40; a += 4) img.put(kBase + a, kNop);
    img.put(kBase + 0x40, kBxLr);
    img.put(kBase + 0x44, kBxLr);
    img.put(kBase + 0x48, kBxLr);
    img.put(kBase + 0x4C, kNop);
    img.put(kBase + 0x50, kBxLr);
    return img;
}

// add pc,pc,r2,lsl#2 into straight-line code (unrolled division). Not a
// switch: nothing past the computed jump may be seeded from it.
Image computed_jump_not_ladder() {
    Image img;
    img.put(kBase + 0x00, 0xE08FF102u);  // add pc, pc, r2, lsl #2
    img.put(kBase + 0x04, kNop);
    img.put(kBase + 0x08, 0xE0B33081u);  // adcs r3, r3, r1, lsl #1
    img.put(kBase + 0x0C, branch(AL, false, kBase + 0x0C, kBase + 0x40));
    img.put(kBase + 0x10, branch(AL, false, kBase + 0x10, kBase + 0x44));
    for (uint32_t a = 0x14; a < 0x40; a += 4) img.put(kBase + a, kNop);
    img.put(kBase + 0x40, kBxLr);
    img.put(kBase + 0x44, kBxLr);
    return img;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= run_case("crt0-tail", crt0_shape(), kBase + 0x20, true);
    ok &= run_case("conditional-write/A", conditional_write(),
                   kBase + 0x30, false);
    ok &= run_case("conditional-write/B", conditional_write(),
                   kBase + 0x34, false);
    ok &= run_case("no-call-control", across_call(false), kBase + 0x30, true);
    ok &= run_case("call-clobbers-r1", across_call(true), kBase + 0x30, false);
    ok &= run_case("sinit-loop/thumb-ctor", sinit_loop(), kBase + 0x50, true);
    ok &= run_case("sinit-loop/arm-ctor", sinit_loop(), kBase + 0x58, true,
                   CpuMode::Arm);
    ok &= run_case("cmp-keeps-r0", cmp_keeps_r0(), kBase + 0x20, true);
    for (bool ls : {true, false}) {
        const char* tag = ls ? "ladder-ls" : "ladder-pl";
        for (uint32_t c : {0x40u, 0x44u, 0x48u}) {
            ok &= run_case(tag, branch_ladder(ls), kBase + c, true,
                           CpuMode::Arm);
        }
        ok &= run_case(tag, branch_ladder(ls), kBase + 0x50, true,
                       CpuMode::Arm);  // default (fall-through b)
    }
    ok &= run_case("computed-jump-not-ladder", computed_jump_not_ladder(),
                   kBase + 0x40, false, CpuMode::Arm);
    ok &= run_case("computed-jump-not-ladder", computed_jump_not_ladder(),
                   kBase + 0x44, false, CpuMode::Arm);
    return ok ? 0 : 1;
}
