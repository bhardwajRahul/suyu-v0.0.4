#include "recomp_runtime.h"
#include "code.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned recomp_image_abi(void);
extern unsigned recomp_image_guard_v2(unsigned);
extern void recomp_image_set_base(uint64_t);
extern void recomp_image_run_slice(GuestContext*);
extern BlockFn recomp_image_lookup(uint64_t);

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static unsigned char memory[8192];
static uintptr_t pages[3];
static unsigned reads;
static uintptr_t stack_min = UINTPTR_MAX, stack_max;
static int unmapped;
static uint64_t unmapped_from = UINT64_MAX;
static GuestContext context;
static uint64_t abort_pc;
static uint64_t checked_load(void* user, uint64_t va, uint32_t size) {
    uint64_t word = 0;
    (void)user;
    if (size != 0) return 0;
    if ((uintptr_t)&word < stack_min) stack_min = (uintptr_t)&word;
    if ((uintptr_t)&word > stack_max) stack_max = (uintptr_t)&word;
    ++reads;
    if (unmapped || va >= unmapped_from || va < 0x1000 || va - 0x1000 > sizeof(memory) - 4) return 0;
    memcpy(&word, memory + va - 0x1000, 4);
    return word | UINT64_C(0x100000000);
}
static RecompHostMem bridge;
static void reset(uint64_t pc, int budget) {
    memset(&context, 0, sizeof(context));
    context.pc = pc;
    context.mem = memory;
    context.mem_size = sizeof(memory);
    context.mem_base_vaddr = 0x1000;
    context.pending_svc = ~UINT64_C(0);
    context.chain_budget = budget;
    context.host_mem = &bridge;
}
static void guard_aborted(int sig) {
    (void)sig;
    /* Prove this is the intended guard failure before any guest effect. */
    _Exit(context.pc == abort_pc && context.x[0] == 99 ? 86 : 87);
}
int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "slice";
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    memcpy(memory, smoke_code, sizeof(smoke_code));
    bridge.load = checked_load;
    bridge.page_entries = pages;
    bridge.page_entry_stride = sizeof(uintptr_t);
    bridge.page_bits = 12;
    bridge.pointer_mask = ~(uint64_t)0;
    bridge.address_space_max = 0x3000;
    pages[1] = pages[2] = (uintptr_t)memory - 0x1000;
    CHECK(recomp_image_abi() == 5);
    CHECK(recomp_image_guard_v2(2) == 2);
    /* Exercise the interval lookup before the flat index is constructed. */
    CHECK(recomp_image_lookup(0x1018) == recomp_image_lookup(0x1014));
    CHECK(recomp_image_lookup(0x1002) == NULL);
    recomp_image_set_base(0);
    if (!strcmp(mode, "slice")) {
        const int budgets[] = {1, 3, 4096};
        unsigned i;
        for (i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
            int spent, counted;
            reset(0x1000, budgets[i]);
            recomp_image_run_slice(&context);
            CHECK(context.x[0] == (uint64_t)budgets[i]);
            CHECK(context.chain_budget == 0 && context.pc == 0x1000);
            /* Mirrors the host: first block already counted; exhausted final
               decrement parks a PC without executing an extra block. */
            spent = budgets[i] - context.chain_budget;
            counted = 1 + spent - (context.chain_budget == 0 ? 1 : 0);
            CHECK(counted == (int)context.x[0]);
        }
        reset(0x1008, 3);
        recomp_image_run_slice(&context);
        CHECK(context.pending_svc == 7 && context.pc == 0x100c);
        CHECK(context.x[1] == 0 && context.chain_budget == 3);
        reset(0x100c, 3);
        recomp_image_run_slice(&context);
        CHECK(context.halted == RECOMP_HALT_BREAKPOINT && context.x[1] == 1);
        CHECK(context.chain_budget == 3 && context.pc == 0x1010);
        reset(0x1018, 3);
        context.x[2] = 99;
        context.x[30] = 0x3000;
        recomp_image_run_slice(&context);
        CHECK(context.x[2] == 99 && context.pc == 0x3000 && !context.halted);
        CHECK(context.chain_budget == 3);
        reset(0x1020, 3);
        context.x[30] = 0x1028;
        recomp_image_run_slice(&context);
        CHECK(context.x[30] == 0x1024 && context.x[3] == 1);
        CHECK(context.pc == 0x102c && context.halted == RECOMP_HALT_BREAKPOINT);
        CHECK(context.chain_budget == 2);
        CHECK(reads == 0); /* All guards used the ordinary same-page path. */
        /* Force callbacks to observe stack depth through a long module slice. */
        bridge.page_entries = NULL;
        reset(0x1000, 4096);
        recomp_image_run_slice(&context);
        CHECK(context.x[0] == 4096 && context.chain_budget == 0);
        CHECK(reads == 8192 && stack_max - stack_min < 4096);
    } else if (!strcmp(mode, "mutated-entry")) {
        reset(0x1000, 3);
        context.x[0] = 99;
        memory[0] ^= 1;
        abort_pc = 0x1000;
        signal(SIGABRT, guard_aborted);
        recomp_image_run_slice(&context);
        CHECK(0); /* No generated effect may precede guard rejection. */
    } else {
        uint32_t expected[] = {0xd503201f, 0};
        const int cross = !strncmp(mode, "cross-page", 10);
        uint64_t pc = cross ? 0x1ffc : 0x1080;
        memcpy(memory + pc - 0x1000, expected, sizeof(expected));
        reset(pc, 3);
        context.x[0] = 99;
        if (!strcmp(mode, "mutated") || !strcmp(mode, "cross-page-mutated")) {
            /* For cross-page, the changed word is the one on the second page. */
            memory[pc - 0x1000 + 4] ^= 1;
            abort_pc = pc + 4;
            signal(SIGABRT, guard_aborted);
        } else if (!strcmp(mode, "unmapped-zero")) {
            pc += 4;
            expected[0] = 0;
            pages[1] = 0;
            unmapped = 1;
            abort_pc = pc;
            signal(SIGABRT, guard_aborted);
        } else if (!strcmp(mode, "cross-page-unmapped")) {
            /* The second page is gone, though its expected word (0) matches. */
            pages[2] = 0;
            unmapped_from = 0x2000;
            abort_pc = pc + 4;
            signal(SIGABRT, guard_aborted);
        } else if (!strcmp(mode, "special-page")) {
            /* A nonzero mapping tag without an ordinary backing pointer. */
            pages[1] = 1;
            bridge.pointer_mask = ~(uint64_t)3;
        }
        recomp_code_guard(&context, pc, expected,
                          !strcmp(mode, "unmapped-zero") ? 1 : 2, 2);
        CHECK(strcmp(mode, "mutated") && strcmp(mode, "unmapped-zero") &&
              strcmp(mode, "cross-page-mutated") && strcmp(mode, "cross-page-unmapped"));
        /* A page crossing is compared per page without callbacks; only the
           special mapping needs the checked loop. */
        CHECK(reads == (!strcmp(mode, "special-page") ? 2u : 0u));
        CHECK(context.x[0] == 99);
    }
    printf("PASS %s\n", mode);
    return 0;
}
