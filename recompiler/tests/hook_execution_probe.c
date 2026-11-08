/* Executes the hook fixture bank built by test_hooks.py. Prepended by the
   test with runtime_arm.h and definitions of the header's globals; every
   other runtime symbol the bank needs is a weak no-op stub. */
#include <stdio.h>
#include <stdlib.h>
#include "hook_fixture.h"

#define FA 0x02010000u
#define FB 0x02010020u
#define FT 0x02010040u
#define RET_ARM 0x02000000u

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "hook probe line %d: %s (R15=%08X)\n", __LINE__, #c, \
            g_cpu.R[15]); ++failures; } } while (0)

static char order[16];
static unsigned order_len;
static unsigned add_calls, after_calls, skip_calls, thumb_calls;
static int skip_enabled, thumb_continue;

void HookAdd(uint32_t* r0, uint32_t* r1) {
    ++add_calls;
    order[order_len++ & 15u] = 'A';
    CHECK(r0 == &g_cpu.R[0] && r1 == &g_cpu.R[1]);
    CHECK(*r0 == 1u);                  /* mov r0,#1 already ran */
    CHECK(g_cpu.R[15] == FA + 4u);     /* published before the call */
    *r1 = 10u;                          /* handler writes are guest-visible */
}
void HookAfterMov(uint32_t* cpsr) {
    ++after_calls;
    order[order_len++ & 15u] = 'M';
    CHECK(cpsr == &g_cpu.cpsr);
    CHECK(g_cpu.R[0] == 1u);           /* after: the instruction ran */
    CHECK(g_cpu.R[15] == FA + 4u);     /* and R15 already advanced */
}
bool HookSkip(uint32_t* r2) {
    ++skip_calls;
    return skip_enabled && *r2 == 5u;
}
bool HookThumbRet(uint32_t* r0) {
    ++thumb_calls;
    CHECK(*r0 == 1u);
    return thumb_continue != 0;
}

/* Host call-stack model: every bx in this probe is a matching C return. */
int runtime_call_should_return(uint32_t pc) { (void)pc; return 1; }

/* One forced yield at a chosen PC; the inline fast path is off because
   g_nds_fast_limit is zero, so every instruction polls this. */
static uint32_t yield_at;
static unsigned yields;
bool runtime_should_yield_slow(void) {
    if (yield_at && g_cpu.R[15] == yield_at) {
        yield_at = 0u;
        ++yields;
        return true;
    }
    return false;
}

void runtime_dispatch_bad_entry(uint32_t pc) {
    fprintf(stderr, "bad entry %08X\n", pc);
    exit(2);
}
void runtime_unimplemented_op(const char* op, uint32_t pc) {
    fprintf(stderr, "unimplemented %s at %08X\n", op, pc);
    exit(2);
}
void runtime_dispatch_with_exchange(uint32_t target) {
    fprintf(stderr, "unexpected dispatch to %08X\n", target);
    exit(2);
}

static void enter(uint32_t pc, int thumb) {
    g_cpu.R[15] = pc;
    g_cpu.cpsr = 0x1Fu | (thumb ? CPSR_T_BIT : 0u);
}

int main(void) {
    /* fa: after-hook on mov, before-hook on add, in that order. */
    enter(FA, 0);
    g_cpu.R[1] = 2u;
    g_cpu.R[14] = RET_ARM;
    hook_fixture_fa();
    CHECK(g_cpu.R[0] == 11u);
    CHECK(g_cpu.R[15] == RET_ARM);
    CHECK(order_len == 2u && order[0] == 'M' && order[1] == 'A');

    /* Yield exactly at the hooked add: the hook must not run before the
       yield, and must run exactly once when the resume enters the label. */
    add_calls = after_calls = 0u;
    enter(FA, 0);
    g_cpu.R[1] = 2u;
    yield_at = FA + 4u;
    hook_fixture_fa();
    CHECK(yields == 1u);
    CHECK(g_cpu.R[15] == FA + 4u);
    CHECK(add_calls == 0u && after_calls == 1u);
    hook_fixture_fa();                  /* resume: R15 still FA+4 */
    CHECK(add_calls == 1u && after_calls == 1u);
    CHECK(g_cpu.R[0] == 11u && g_cpu.R[15] == RET_ARM);

    /* fb: on_true jump over two instructions to the bx. */
    enter(FB, 0);
    g_cpu.R[3] = 0xDEADu;
    skip_enabled = 1;
    hook_fixture_fb();
    CHECK(skip_calls == 1u);
    CHECK(g_cpu.R[2] == 5u && g_cpu.R[3] == 0xDEADu);
    CHECK(g_cpu.R[15] == RET_ARM);
    enter(FB, 0);
    skip_enabled = 0;
    hook_fixture_fb();
    CHECK(skip_calls == 2u);
    CHECK(g_cpu.R[2] == 6u && g_cpu.R[3] == 7u);

    /* ft (Thumb): on_false return emulates bx lr, including interworking. */
    enter(FT, 1);
    g_cpu.R[1] = 4u;
    g_cpu.R[14] = RET_ARM;              /* ARM caller: T must clear */
    thumb_continue = 0;
    hook_fixture_ft();
    CHECK(thumb_calls == 1u);
    CHECK(g_cpu.R[0] == 1u);            /* adds skipped */
    CHECK(g_cpu.R[15] == RET_ARM && (g_cpu.cpsr & CPSR_T_BIT) == 0u);
    enter(FT, 1);
    g_cpu.R[14] = 0x02000101u;          /* Thumb caller: T stays set */
    thumb_continue = 1;
    hook_fixture_ft();
    CHECK(thumb_calls == 2u);
    CHECK(g_cpu.R[0] == 5u);
    CHECK(g_cpu.R[15] == 0x02000100u && (g_cpu.cpsr & CPSR_T_BIT) != 0u);

    if (failures == 0) puts("hook probe: ok");
    return failures == 0 ? 0 : 1;
}
