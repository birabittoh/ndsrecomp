// Pins the FunctionFinder whole-image static scan (set_static_scan) and the
// two jump-table idioms added with it.
//
// The static scan runs after the seeded CFG walk drains and harvests entry
// candidates from bytes the walk never claimed:
//   * PointerHarvest  an aligned word whose value (bit0 = mode) is a
//                     decode-valid code address (vtables, callback tables);
//   * GapFill         `stmdb sp!,{..,lr}` / word-aligned `push {..,lr}`
//                     prologues, and the slot after a body that ended at an
//                     unconditional terminator (back-to-back leaf functions);
//   * candidates      cross-image call targets handed in by another image.
// Every candidate passes validate_candidate(): a linear walk that must end
// cleanly, never conflict with an existing body of the other mode, and never
// execute a conditional instruction before something set the flags.
//
// Each case below is one rule; the negative cases are the precision bugs
// found on DQ9 (the THUMB push hiding in ARM `ldrlt rX,[sp,..]`, data words
// with random condition nibbles).

#include "function_finder.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using ndsrecomp::CpuMode;
using ndsrecomp::FunctionFinder;
using ndsrecomp::FunctionSeed;

constexpr uint32_t kBase = 0x02000000u;
constexpr uint32_t kBxLr = 0xE12FFF1Eu;
constexpr uint32_t kPushR4Lr = 0xE92D4010u;
constexpr uint32_t kPopR4Pc = 0xE8BD8010u;
constexpr uint32_t kPushR3Lr = 0xE92D4008u;
constexpr uint32_t kPopR3Pc = 0xE8BD8008u;
constexpr uint32_t kMovR0_1 = 0xE3A00001u;
constexpr uint32_t kCmpR0_0 = 0xE3500000u;
constexpr uint32_t kLdrneR1 = 0x15901000u;  // ldrne r1,[r0]

struct Image {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x2000, 0);
    void put(uint32_t addr, uint32_t word) {
        const uint32_t off = addr - kBase;
        for (int i = 0; i < 4; ++i)
            bytes[off + i] = static_cast<uint8_t>(word >> (8 * i));
    }
    void put16(uint32_t addr, uint16_t h) {
        const uint32_t off = addr - kBase;
        bytes[off] = static_cast<uint8_t>(h);
        bytes[off + 1] = static_cast<uint8_t>(h >> 8);
    }
};

uint32_t branch(uint32_t cond, bool link, uint32_t from, uint32_t to) {
    int32_t off = static_cast<int32_t>(to - (from + 8)) >> 2;
    return (cond << 28) | 0x0A000000u | (link ? 0x01000000u : 0u) |
           (static_cast<uint32_t>(off) & 0x00FFFFFFu);
}

// An entry function at kBase that touches nothing else: `bx lr`.
Image base_image() {
    Image img;
    img.put(kBase, kBxLr);
    return img;
}

bool has(const FunctionFinder& f, uint32_t addr, CpuMode mode) {
    for (const auto& fn : f.functions())
        if (fn.addr == addr && fn.mode == mode) return true;
    return false;
}

int g_fail = 0;

void expect(const char* name, bool cond) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++g_fail;
}

FunctionFinder* run(const Image& img, bool scan, uint32_t base = kBase,
                    const std::vector<FunctionSeed>& cands = {}) {
    auto* f = new FunctionFinder(img.bytes.data(), img.bytes.size(), base);
    f->add_seed(FunctionSeed{base, CpuMode::Arm, "entry"});
    f->set_static_scan(scan);
    for (const auto& c : cands) f->add_candidate(c);
    f->run();
    return f;
}

void ptr_harvest_arm() {
    // A leaf with no prologue, far (> the 64-word sweep window) from any
    // walked body, referenced only by a data word.
    Image img = base_image();
    const uint32_t fn = kBase + 0x800;
    img.put(fn, kMovR0_1);
    img.put(fn + 4, kBxLr);
    img.put(kBase + 0x1000, fn);  // the pointer
    FunctionFinder* off = run(img, false);
    FunctionFinder* on = run(img, true);
    expect("ptr_harvest_arm: absent without static scan", !has(*off, fn, CpuMode::Arm));
    expect("ptr_harvest_arm: found from data pointer", has(*on, fn, CpuMode::Arm));
    delete off; delete on;
}

void ptr_harvest_thumb() {
    Image img = base_image();
    const uint32_t fn = kBase + 0x800;
    img.put16(fn, 0xB510);      // push {r4,lr}
    img.put16(fn + 2, 0x2000);  // movs r0,#0
    img.put16(fn + 4, 0xBD10);  // pop {r4,pc}
    img.put(kBase + 0x1000, fn | 1u);
    FunctionFinder* on = run(img, true);
    expect("ptr_harvest_thumb: bit0 pointer seeds THUMB", has(*on, fn, CpuMode::Thumb));
    expect("ptr_harvest_thumb: not seeded as ARM", !has(*on, fn, CpuMode::Arm));
    delete on;
}

void gap_prologue_arm() {
    Image img = base_image();
    const uint32_t fn = kBase + 0x800;
    img.put(fn, kPushR4Lr);
    img.put(fn + 4, kMovR0_1);
    img.put(fn + 8, kPopR4Pc);
    FunctionFinder* on = run(img, true);
    expect("gap_prologue_arm: stmdb sp!,{..,lr} in uncovered bytes", has(*on, fn, CpuMode::Arm));
    delete on;
}

void terminator_sweep() {
    // entry: bx lr ; literal 0x02001AAC ; leaf: mov r0,#1 ; bx lr
    Image img = base_image();
    img.put(kBase + 4, 0x02001AACu);  // pool word (cond EQ: rejected)
    img.put(kBase + 8, kMovR0_1);
    img.put(kBase + 12, kBxLr);
    FunctionFinder* off = run(img, false);
    FunctionFinder* on = run(img, true);
    expect("terminator_sweep: absent without static scan", !has(*off, kBase + 8, CpuMode::Arm));
    expect("terminator_sweep: leaf after bx lr + pool", has(*on, kBase + 8, CpuMode::Arm));
    expect("terminator_sweep: pool word not an entry", !has(*on, kBase + 4, CpuMode::Arm));
    delete off; delete on;
}

void thumb_push_in_arm_word() {
    // ARM `ldrlt r0,[sp,#0x10]` = 0xB59D0010: its upper half at 2 mod 4 is
    // a THUMB `push {r0,r2,r3,r4,r7,lr}`, followed by `b` garbage. GapFill
    // must not seed it.
    Image img = base_image();
    const uint32_t a = kBase + 0x800;
    img.put(a, 0xB59D0010u);
    img.put(a + 4, 0xE58D3034u);  // str r3,[sp,#0x34] -> 0x3034 adds, 0xE58D b
    FunctionFinder* on = run(img, true);
    expect("thumb_push_in_arm_word: 2-mod-4 THUMB push not seeded",
           !has(*on, a + 2, CpuMode::Thumb));
    delete on;
}

void cond_before_flags() {
    Image img = base_image();
    // Pointer to `mov r0,#1; ldrne r1,[r0]; bx lr`: a conditional with
    // undefined flags -> data, rejected.
    const uint32_t bad = kBase + 0x800;
    img.put(bad, kMovR0_1);
    img.put(bad + 4, kLdrneR1);
    img.put(bad + 8, kBxLr);
    img.put(kBase + 0x1000, bad);
    // Pointer to `cmp r0,#0; ldrne r1,[r0]; bx lr`: flags set first -> code.
    const uint32_t good = kBase + 0xC00;
    img.put(good, kCmpR0_0);
    img.put(good + 4, kLdrneR1);
    img.put(good + 8, kBxLr);
    img.put(kBase + 0x1004, good);
    FunctionFinder* on = run(img, true);
    expect("cond_before_flags: conditional before flags rejected", !has(*on, bad, CpuMode::Arm));
    expect("cond_before_flags: cmp then conditional accepted", has(*on, good, CpuMode::Arm));
    delete on;
}

void ladder_with_return_slots() {
    // push {r3,lr}; cmp r0,#0; addge pc,pc,r0,lsl#2; pop {r3,pc};
    // slots: pop / b A / b B / pop / b C ; then case bodies.
    Image img;
    uint32_t pc = kBase;
    img.put(pc, kPushR3Lr); pc += 4;
    img.put(pc, kCmpR0_0); pc += 4;
    img.put(pc, 0xA08FF100u); pc += 4;       // addge pc,pc,r0,lsl#2
    img.put(pc, kPopR3Pc); pc += 4;          // fallthrough (r0 < 0)
    const uint32_t slot0 = pc;
    const uint32_t cases = slot0 + 5 * 4;
    img.put(slot0 + 0, kPopR3Pc);
    img.put(slot0 + 4, branch(0xE, false, slot0 + 4, cases));
    img.put(slot0 + 8, branch(0xE, false, slot0 + 8, cases + 8));
    img.put(slot0 + 12, kPopR3Pc);
    img.put(slot0 + 16, branch(0xE, false, slot0 + 16, cases + 16));
    for (uint32_t k = 0; k < 3; ++k) {
        img.put(cases + 8 * k, kMovR0_1);
        img.put(cases + 8 * k + 4, kPopR3Pc);
    }
    FunctionFinder* f = run(img, false);
    expect("ladder: return slot after b slots is an entry", has(*f, slot0 + 12, CpuMode::Arm));
    expect("ladder: case reached through slot past a return slot",
           has(*f, cases + 16, CpuMode::Arm));
    delete f;
}

void relative_jump_table() {
    // table @T: offsets from T ; code: sub r6,pc,#k ; ldr r7,[r6,r0,lsl#2]
    // ; add pc,r6,r7 ; targets after it.
    Image img;
    const uint32_t T = kBase + 0x10;
    const uint32_t code = kBase + 0x20;
    img.put(kBase, branch(0xE, false, kBase, code));
    const uint32_t k = code + 8 - T;
    img.put(code, 0xE24F6000u | k);   // sub r6,pc,#k  (r6 = T)
    img.put(code + 4, 0xE7967100u);   // ldr r7,[r6,r0,lsl#2]
    img.put(code + 8, 0xE086F007u);   // add pc,r6,r7
    const uint32_t t0 = code + 0x10, t1 = code + 0x20;
    img.put(T, t0 - T);
    img.put(T + 4, t1 - T);
    img.put(T + 8, 0);                // terminator (offset into the table)
    img.put(t0, kMovR0_1); img.put(t0 + 4, kBxLr);
    img.put(t1, kMovR0_1); img.put(t1 + 4, kBxLr);
    FunctionFinder* f = run(img, false);
    expect("relative_jt: entry 0", has(*f, t0, CpuMode::Arm));
    expect("relative_jt: entry 1", has(*f, t1, CpuMode::Arm));
    expect("relative_jt: one table", f->stats().auto_jump_tables == 1);
    delete f;
}

void relative_jt_call_adjacent_tables() {
    // DQ9 ov016 0x02196064 shape: two base-relative tables back to back
    // (A then B, two entries each), each used as a computed CALL with a
    // preset return pad:
    //   sub r3,pc,#kA ; add lr,pc,#4 ; ldr r8,[r3,r7,lsl#2] ; add pc,r8,r3
    //   pad1: sub r3,pc,#kB ; add lr,pc,#4 ; ldr r8,[r3,r5,lsl#2] ; add pc,r8,r3
    //   pad2: bx lr
    // Targets are 4-instruction routines, so B's words read as A-relative
    // offsets also land on code: an unbounded walk of A overruns into B.
    Image img;
    const uint32_t A = kBase + 0x10, B = kBase + 0x18;
    const uint32_t code = kBase + 0x40;
    img.put(kBase, branch(0xE, false, kBase, code));
    const uint32_t tg[4] = {kBase + 0x100, kBase + 0x110, kBase + 0x120, kBase + 0x130};
    for (uint32_t t : tg) {
        img.put(t, kMovR0_1); img.put(t + 4, kMovR0_1);
        img.put(t + 8, kMovR0_1); img.put(t + 12, kBxLr);
    }
    img.put(A, tg[0] - A); img.put(A + 4, tg[1] - A);
    img.put(B, tg[2] - B); img.put(B + 4, tg[3] - B);
    img.put(code, 0xE24F3000u | (code + 8 - A));        // sub r3,pc,#kA
    img.put(code + 4, 0xE28FE004u);                      // add lr,pc,#4
    img.put(code + 8, 0xE7938107u);                      // ldr r8,[r3,r7,lsl#2]
    img.put(code + 12, 0xE088F003u);                     // add pc,r8,r3
    const uint32_t pad1 = code + 16;
    img.put(pad1, 0xE24F3000u | (pad1 + 8 - B));         // sub r3,pc,#kB
    img.put(pad1 + 4, 0xE28FE004u);
    img.put(pad1 + 8, 0xE7938105u);                      // ldr r8,[r3,r5,lsl#2]
    img.put(pad1 + 12, 0xE088F003u);
    const uint32_t pad2 = pad1 + 16;
    img.put(pad2, kBxLr);
    FunctionFinder* f = run(img, false);
    expect("reljt_call: return pad after table call", has(*f, pad1, CpuMode::Arm));
    expect("reljt_call: second pad", has(*f, pad2, CpuMode::Arm));
    expect("reljt_call: A entry 1", has(*f, tg[1], CpuMode::Arm));
    expect("reljt_call: B entry 0", has(*f, tg[2], CpuMode::Arm));
    expect("reljt_call: B entry 1", has(*f, tg[3], CpuMode::Arm));
    expect("reljt_call: A not overrun into B (no target at B0-8)",
           !has(*f, tg[2] + 8, CpuMode::Arm));
    expect("reljt_call: two tables", f->stats().auto_jump_tables == 2);
    delete f;
}

void pad_after_direct_branch_call() {
    // DQ9 ov016 0x02196158: `add lr,pc,#4; push {r4,lr}; b f` returns to
    // the pad (lr) through f's `pop {r4,pc}`.
    Image img;
    const uint32_t code = kBase + 0x40, f = kBase + 0x80;
    img.put(kBase, branch(0xE, false, kBase, code));
    img.put(code, 0xE28FE004u);                 // add lr,pc,#4 -> code+12
    img.put(code + 4, kPushR4Lr);
    img.put(code + 8, branch(0xE, false, code + 8, f));
    const uint32_t pad = code + 12;
    img.put(pad, kMovR0_1); img.put(pad + 4, kBxLr);
    img.put(f, kPopR4Pc);
    FunctionFinder* fi = run(img, false);
    expect("b_call: return pad seeded", has(*fi, pad, CpuMode::Arm));
    delete fi;
    // Negative: a plain `b` with LR unknown seeds nothing after it.
    Image n;
    n.put(kBase, branch(0xE, false, kBase, f));
    n.put(kBase + 4, kMovR0_1); n.put(kBase + 8, kBxLr);
    n.put(f, kBxLr);
    fi = run(n, false);
    expect("b_call: no pad without known lr", !has(*fi, kBase + 4, CpuMode::Arm));
    delete fi;
}

void gap_str_lr_prologue() {
    // `str lr,[sp,#-4]!; mov r5,#4; bl g; ldr pc,[sp],#4` wrappers.
    Image img = base_image();
    const uint32_t w = kBase + 0x100, g = kBase + 0x200;
    img.put(w, 0xE52DE004u);
    img.put(w + 4, 0xE3A05004u);
    img.put(w + 8, branch(0xE, true, w + 8, g));
    img.put(w + 12, 0xE49DF004u);               // ldr pc,[sp],#4
    img.put(g, kBxLr);
    FunctionFinder* f = run(img, true);
    expect("gap: str lr,[sp,#-4]! prologue", has(*f, w, CpuMode::Arm));
    delete f;
}

void relative_jt_unscaled_index() {
    // DQ9 ov016 0x021989BC: index built as a byte offset, no lsl #2:
    //   sub r5,pc,#k ; ldr r6,[r5,r8] ; add pc,r5,r6
    Image img;
    const uint32_t T = kBase + 0x10, code = kBase + 0x40;
    img.put(kBase, branch(0xE, false, kBase, code));
    const uint32_t t0 = kBase + 0x100, t1 = kBase + 0x110;
    img.put(T, t0 - T); img.put(T + 4, t1 - T);
    img.put(code, 0xE24F5000u | (code + 8 - T));  // sub r5,pc,#k
    img.put(code + 4, 0xE7956008u);               // ldr r6,[r5,r8]
    img.put(code + 8, 0xE085F006u);               // add pc,r5,r6
    for (uint32_t t : {t0, t1}) { img.put(t, kMovR0_1); img.put(t + 4, kBxLr); }
    FunctionFinder* f = run(img, false);
    expect("reljt_unscaled: entry 0", has(*f, t0, CpuMode::Arm));
    expect("reljt_unscaled: entry 1", has(*f, t1, CpuMode::Arm));
    delete f;
    // Negative: unscaled load + `mov pc,r6` (absolute) is not a table.
    Image n;
    n.put(kBase, branch(0xE, false, kBase, code));
    n.put(T, t0); n.put(T + 4, t1);
    n.put(code, 0xE24F5000u | (code + 8 - T));
    n.put(code + 4, 0xE7956008u);
    n.put(code + 8, 0xE1A0F006u);                 // mov pc,r6
    for (uint32_t t : {t0, t1}) { n.put(t, kMovR0_1); n.put(t + 4, kBxLr); }
    f = run(n, false);
    expect("reljt_unscaled: absolute form not accepted", !has(*f, t1, CpuMode::Arm));
    delete f;
}

void external_targets_and_candidates() {
    // An "overlay" at 0x02100000 calls 0x02000040 (another image).
    const uint32_t ob = 0x02100000u;
    std::vector<uint8_t> bytes(0x100, 0);
    auto put = [&](uint32_t a, uint32_t w) {
        for (int i = 0; i < 4; ++i) bytes[a - ob + i] = static_cast<uint8_t>(w >> (8 * i));
    };
    put(ob, kPushR4Lr);
    put(ob + 4, branch(0xE, true, ob + 4, 0x02000040u));
    put(ob + 8, kPopR4Pc);
    FunctionFinder f(bytes.data(), bytes.size(), ob);
    f.add_seed(FunctionSeed{ob, CpuMode::Arm, "entry"});
    f.run();
    const uint64_t key = (uint64_t{0x02000040u} << 1) | 0u;
    expect("external: BL into another image exported",
           f.external_targets().count(key) == 1);

    // The receiving image validates candidates: code accepted, zeros not.
    Image img = base_image();
    img.put(kBase + 0x40, kMovR0_1);
    img.put(kBase + 0x44, kBxLr);
    FunctionFinder* on = run(img, true, kBase,
        {FunctionSeed{kBase + 0x40, CpuMode::Arm, "ext"},
         FunctionSeed{kBase + 0x1800, CpuMode::Arm, "ext"}});
    expect("candidates: valid cross-image target walked", has(*on, kBase + 0x40, CpuMode::Arm));
    expect("candidates: zero-filled target rejected", !has(*on, kBase + 0x1800, CpuMode::Arm));
    delete on;
}

}  // namespace

int main() {
    ptr_harvest_arm();
    ptr_harvest_thumb();
    gap_prologue_arm();
    terminator_sweep();
    thumb_push_in_arm_word();
    cond_before_flags();
    ladder_with_return_slots();
    relative_jump_table();
    relative_jt_call_adjacent_tables();
    pad_after_direct_branch_call();
    gap_str_lr_prologue();
    relative_jt_unscaled_index();
    external_targets_and_candidates();
    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
