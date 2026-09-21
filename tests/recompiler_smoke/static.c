#include "recomp_runtime.h"
#include "code.h"
#include <stdio.h>
#include <string.h>

#define MODULE(name) \
    extern unsigned recomp_image_abi_##name(void); \
    extern unsigned recomp_image_guard_v2_##name(unsigned); \
    extern void recomp_image_set_base_##name(uint64_t); \
    extern void recomp_image_run_slice_##name(GuestContext*); \
    extern BlockFn recomp_image_lookup_##name(uint64_t); \
    extern int recomp_image_index_##name(uint64_t*, uint64_t*, BlockFn**); \
    extern int g_recomp_guard_host_v2_##name
MODULE(smoke);
MODULE(second);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void context_at(GuestContext* c, uint64_t base, int budget) {
    memset(c, 0, sizeof(*c));
    c->mem = (uint8_t*)smoke_code;
    c->mem_size = sizeof(smoke_code);
    c->mem_base_vaddr = c->pc = base + 0x1000;
    c->chain_budget = budget;
    c->pending_svc = ~UINT64_C(0);
}

int main(void) {
    const uint64_t first_base = 0x10000, second_base = 0x20000;
    uint64_t first_lo, first_hi, second_lo, second_hi;
    BlockFn *first_index, *second_index;
    GuestContext first, second;
    CHECK(recomp_image_abi_smoke() == 5 && recomp_image_abi_second() == 5);
    CHECK(g_recomp_guard_host_v2_smoke == 0 && g_recomp_guard_host_v2_second == 0);
    CHECK(recomp_image_guard_v2_smoke(2) == 2);
    CHECK(g_recomp_guard_host_v2_smoke == 2 && g_recomp_guard_host_v2_second == 0);
    CHECK(recomp_image_guard_v2_second(2) == 2);
    recomp_image_set_base_smoke(first_base);
    recomp_image_set_base_second(second_base);
    CHECK(recomp_image_index_smoke(&first_lo, &first_hi, &first_index));
    CHECK(recomp_image_index_second(&second_lo, &second_hi, &second_index));
    CHECK(first_lo == first_base + 0x1000 && second_lo == second_base + 0x1000);
    CHECK(first_hi - first_base == second_hi - second_base);
    CHECK(first_index != second_index);
    CHECK(first_index[0] == recomp_image_lookup_smoke(first_lo));
    CHECK(second_index[0] == recomp_image_lookup_second(second_lo));
    CHECK(first_index[0] != second_index[0]);
    CHECK(recomp_image_lookup_smoke(second_lo) == NULL);
    CHECK(recomp_image_lookup_second(first_lo) == NULL);
    CHECK(recomp_image_lookup_smoke(first_base + 0x1018) ==
          recomp_image_lookup_smoke(first_base + 0x1014));
    CHECK(recomp_image_lookup_second(second_base + 0x1018) ==
          recomp_image_lookup_second(second_base + 0x1014));
    context_at(&first, first_base, 3);
    context_at(&second, second_base, 5);
    recomp_image_run_slice_smoke(&first);
    recomp_image_run_slice_second(&second);
    CHECK(first.x[0] == 3 && first.pc == first_lo && first.chain_budget == 0);
    CHECK(second.x[0] == 5 && second.pc == second_lo && second.chain_budget == 0);
    /* Rebase one image after both indexes exist; the second must stay put. */
    recomp_image_set_base_smoke(0x30000);
    CHECK(recomp_image_lookup_smoke(first_lo) == NULL);
    CHECK(recomp_image_lookup_smoke(0x31000) == first_index[0]);
    CHECK(recomp_image_lookup_second(second_lo) == second_index[0]);
    context_at(&second, second_base, 1);
    recomp_image_run_slice_second(&second);
    CHECK(second.x[0] == 1 && second.pc == second_lo && second.chain_budget == 0);
    puts("PASS two static modules, isolated indexes/bases/guards, one shared runtime");
    return 0;
}
