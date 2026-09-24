/* C half of smoke_gg_host: owns the generated GG1 module's context and code
   memory, and exposes a few plain functions to the C++ driver (gg_host.cpp),
   which plays the emulator side through Core::RecompGuardGen. */
#include "recomp_runtime.h"
#include "code.h"
/* The module's only block unit, included rather than compiled on its own so
   this file can read the unit-private seen words (recomp_gg_seen). */
#include "src/recompiled_smoke_0.c"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define GG_TLS __declspec(thread)
#else
#define GG_TLS _Thread_local
#endif

extern unsigned recomp_image_guard_v2(unsigned);
extern void recomp_image_set_base(uint64_t);
extern void recomp_image_run_slice(GuestContext*);
extern uint32_t* recomp_image_guard_gen_v1(uint32_t, uint64_t*, uint64_t*, const uint64_t**);

/* Guest code at 0x1000; the guard reads it word by word through `load` size 0
   (page_entries stays null), so every verification is visible as reads. */
static union { unsigned char bytes[8192]; uint64_t align; } code_memory;
static uint64_t code_base = 0x1000;
static RecompHostMem bridge;
static GG_TLS unsigned long thread_reads;
static GG_TLS GuestContext* thread_context;
static volatile uint64_t abort_pc;

static uint64_t checked_load(void* user, uint64_t va, uint32_t size) {
    uint64_t word = 0;
    (void)user;
    if (size != 0) return 0;
    ++thread_reads;
    if (va < code_base || va - code_base > sizeof(code_memory.bytes) - 4) return 0;
    memcpy(&word, (const unsigned char*)code_memory.bytes + (va - code_base), 4);
    return word | UINT64_C(0x100000000);
}

static void guard_aborted(int sig) {
    (void)sig;
    /* The guard names the rejected word before any guest effect of the block. */
    _Exit(thread_context && thread_context->pc == abort_pc && thread_context->x[0] == 99 ? 86
                                                                                        : 87);
}

void ggc_init(void) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    memcpy(code_memory.bytes, smoke_code, sizeof(smoke_code));
    bridge.load = checked_load;
    bridge.page_entries = NULL;
    recomp_image_guard_v2(2);
    recomp_image_set_base(0);
    signal(SIGABRT, guard_aborted);
}

uint32_t* ggc_handshake(uint32_t version, uint64_t* lo, uint64_t* end, const uint64_t** base) {
    return recomp_image_guard_gen_v1(version, lo, end, base);
}

void* ggc_new_context(void) {
    return calloc(1, sizeof(GuestContext));
}

/* One entry into the block at `pc` (add x0,x0,#1; b pc), with x0 preset.
   Returns how many code words this entry's guard read (0 = skipped). */
unsigned long ggc_enter(void* ctx, uint64_t pc, uint64_t x0) {
    GuestContext* c = (GuestContext*)ctx;
    const unsigned long before = thread_reads;
    memset(c, 0, sizeof(*c));
    c->pc = pc;
    c->x[0] = x0;
    c->pending_svc = ~UINT64_C(0);
    c->chain_budget = 1;
    c->host_mem = &bridge;
    thread_context = c;
    recomp_image_run_slice(c);
    if (c->x[0] != x0 + 1) {
        fprintf(stderr, "block did not run: x0=%llu\n", (unsigned long long)c->x[0]);
        _Exit(88);
    }
    return thread_reads - before;
}

void ggc_expect_abort_at(uint64_t pc) {
    abort_pc = pc;
}

/* Flip one bit of the first word of the block at 0x1000. */
void ggc_mutate(void) {
    volatile unsigned char* p = code_memory.bytes;
    p[0] ^= 1;
}

/* Guest code now lives at `base`: the same bytes, moved. */
void ggc_move_code(uint64_t base) {
    code_base = base;
    recomp_image_set_base(base - 0x1000);
}

uint32_t ggc_seen(unsigned index) {
    return RECOMP_GG_LOAD(recomp_gg_seen[index]);
}
