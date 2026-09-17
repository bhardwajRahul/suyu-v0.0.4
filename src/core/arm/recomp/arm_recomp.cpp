// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/fs/path_util.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_lock_trace.h"
#include "core/hle/kernel/k_process.h"
#include "core/arm/debug.h"
#ifndef SUYU_NO_JIT
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#endif
#include "core/arm/exclusive_monitor.h"
#include "core/memory.h"

namespace Core {

namespace {
// Mirrors the prefix of the GuestContext the recompiler emits. Only the fields
// the emulator needs to observe or mutate are modelled; the generated struct
// carries additional members after these (heap bookkeeping, save-data handles)
// which the recompiled code manages itself and we never touch.
struct GuestContextView {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
    u8* mem;
    u64 mem_size;
    u64 mem_base_vaddr;
    int halted;
    u64 pending_svc;
    // The SIMD/FP register file and thread pointer sit immediately after
    // pending_svc in the emitted struct. They have to be modelled here rather
    // than left off the end: the recompiler emits SIMD code, so a context
    // switch that did not carry these would silently lose every floating-point
    // and vector register the guest had live.
    u64 vreg[32][2];
    u64 tpidr_el0;
    const void* host_mem;
    // TPIDRRO_EL0 - the kernel-published thread-local region, whose first
    // 0x100 bytes are the IPC message buffer KServerSession reads a
    // SendSyncRequest out of. Kept apart from tpidr_el0 (the guest's own
    // thread pointer) because the two have different owners and different
    // lifetimes; see the note on GuestContext in core/recompiler/arm64_to_c.h.
    u64 tpidrro_el0;
    // FP control and status. Modelled here so they survive the marshal to and
    // from the fallback JIT - without these fields the guest's rounding mode
    // was silently reset to the host default on every engine transition.
    u64 fpcr;
    u64 fpsr;
    // Must sit here rather than at the end: the emitted struct continues with
    // heap bookkeeping this view does not model, so a field appended after that
    // point would be at a different offset on each side.
    int chain_budget;
};

// Matches RecompHostMem in the generated runtime. The recompiled code calls
// through this for every guest access, so that it reads and writes the
// emulator's address space rather than the flat buffer the standalone runtime
// would otherwise own - without it the recompiled code and the HLE kernel
// would be looking at two different memories.
struct RecompHostMem {
    void* user;
    u64 (*load)(void* user, u64 va, u32 size);
    void (*store)(void* user, u64 va, u32 size, u64 value);
    // Exclusive access, routed at the kernel's own monitor so recompiled code
    // and the fallback JIT contend correctly against each other.
    u64 (*excl_load)(void* user, u64 va, u32 size);
    u32 (*excl_store)(void* user, u64 va, u32 size, u64 value);
    void (*clear_excl)(void* user);
    // The physical counter, read from the emulator's timing source so the
    // recompiled code and the JIT agree about time.
    u64 (*read_cntpct)(void* user);
    // Exclusive pair forms; `size` is the width of one register, 4 or 8.
    void (*excl_load_pair)(void* user, u64 va, u32 size, u64* lo, u64* hi);
    // Page table, so generated code can resolve a mapped address inline instead
    // of calling out for every load and store. Mirrors GetPointerImpl's fast
    // path; a null backing pointer (unmapped, debug, or GPU-tracked memory)
    // falls through to `load`/`store` so rasterizer invalidation still happens.
    u32 (*excl_store_pair)(void* user, u64 va, u32 size, u64 lo, u64 hi);
    const void* page_entries;
    u64 page_entry_stride;
    u64 page_bits;
    u64 pointer_mask;
    u64 address_space_max;
};

// This struct is duplicated by hand in the emitter (arm64_to_c.h, RuntimeH's
// RecompHostMem) because the generated project is plain C and shares no headers
// with the emulator. Nothing links the two, so a field added in the middle of
// one and at the end of the other compiles cleanly on both sides and hands the
// generated code a function pointer where it expects data.
//
// That is not hypothetical: inserting the page-table fields after
// excl_load_pair here, while the emitter appended them after excl_store_pair,
// made recompiled code dereference excl_store_pair's code pointer as a page
// table. suyu died during boot with no diagnostic. Hence these.
static_assert(offsetof(RecompHostMem, excl_load_pair) == 56);
static_assert(offsetof(RecompHostMem, excl_store_pair) == 64);
static_assert(offsetof(RecompHostMem, page_entries) == 72);
static_assert(offsetof(RecompHostMem, page_entry_stride) == 80);
static_assert(offsetof(RecompHostMem, page_bits) == 88);
static_assert(offsetof(RecompHostMem, pointer_mask) == 96);
static_assert(offsetof(RecompHostMem, address_space_max) == 104);
static_assert(sizeof(RecompHostMem) == 112);

// Nothing links these two builds together, so the shared layout is pinned on
// both sides: the generated runtime asserts the same four offsets against its
// own GuestContext. If a field is ever inserted rather than appended, one of
// the two fails to compile instead of the emulator silently reading the wrong
// registers.
static_assert(offsetof(GuestContextView, pc) == 256);
static_assert(offsetof(GuestContextView, pending_svc) == 304);
static_assert(offsetof(GuestContextView, vreg) == 312);
static_assert(offsetof(GuestContextView, tpidr_el0) == 824);
static_assert(offsetof(GuestContextView, chain_budget) == 864);

// Blocks a chain of direct calls may run before returning here. Only this side
// sets it - the generated code just decrements - so the emitter does not need
// to agree on the value.
//
// It bounds two things. How long a guest loop can run without the interrupt and
// SVC checks below getting a look in; and, because the generated calls are not
// guaranteed to be tail calls, how deep the host stack goes. Guest threads run
// on 512 KB fibers (common/fiber.cpp) and a block frame carrying SIMD locals is
// not small, so this has to stay well under what that stack can hold. 256
// overflowed it and crashed on boot.
// Overridable so the depth can be measured rather than guessed, but only
// upwards from a value known to be safe, and only when the generated code was
// built with -foptimize-sibling-calls - without real tail calls a large budget
// is a stack overflow, which is exactly how 256 crashed on boot.
// Refuse the JIT entirely. Without this, "the JIT was never reached" is an
// observation about one run; with it, reaching the JIT is a loud, fatal failure
// that names the address, which is the difference between evidence and proof.
bool StrictNoFallback() {
    const char* e = std::getenv("SUYU_RECOMP_STRICT");
    return e && *e && *e != '0';
}

const int kChainBudgetOverride = [] {
    const char* e = std::getenv("SUYU_RECOMP_CHAIN_BUDGET");
    if (!e) {
        return 0;
    }
    const int v = std::atoi(e);
    return (v >= 1 && v <= 8192) ? v : 0;
}();
std::atomic<int> g_recomp_chain_budget{32};

int RecompChainBudget() {
    return kChainBudgetOverride ? kChainBudgetOverride
                                : g_recomp_chain_budget.load(std::memory_order_relaxed);
}

const bool kSamplePc = [] {
    const char* e = std::getenv("SUYU_RECOMP_SAMPLE_PC");
    return e && *e && *e != '0';
}();

// How many blocks a thread retires between PC samples, as a power of two.
//
// The default of 18 samples proportionally to *executed blocks*, which is the
// wrong denominator for a stall: a title that runs 260M blocks of boot work in
// four seconds and then sits in a 4,000 block/s poll loop for three minutes
// puts 99.7% of its samples in the four seconds nobody is asking about. Lower
// this to sample the plateau, and use SUYU_RECOMP_SAMPLE_AFTER_SEC to throw
// away the burst entirely.
const unsigned kSamplePcShift = [] {
    const char* e = std::getenv("SUYU_RECOMP_SAMPLE_SHIFT");
    if (!e || !*e) {
        return 18u;
    }
    const int v = std::atoi(e);
    return (v >= 6 && v <= 30) ? static_cast<unsigned>(v) : 18u;
}();

// Seconds of run time to discard before any sample is kept. 0 keeps everything.
const double kSampleAfterSec = [] {
    const char* e = std::getenv("SUYU_RECOMP_SAMPLE_AFTER_SEC");
    return (e && *e) ? std::atof(e) : 0.0;
}();

const std::chrono::steady_clock::time_point kRecompStart = std::chrono::steady_clock::now();

// Naming the instruction that writes a guest field is not possible from the
// store callback, because emitted code resolves mapped memory through the
// inline page-table walk in recomp_host_ptr and never calls back. That walk
// has one runtime-controlled escape: it gives up on any address at or above
// host_mem->address_space_max and takes the callback path instead. Lowering
// that bound sends every access above a chosen address through HostStore,
// where the storing block's PC is visible - c->pc holds the entry address of
// the block currently running, because the emitted code writes it at branches
// rather than per instruction.
//
// The bound applies to loads too, so it is expensive: it is gated on a block
// count so the fast part of boot runs at full speed and only the window around
// the fault is instrumented.
const u64 kSlowPathAbove = [] {
    const char* e = std::getenv("SUYU_RECOMP_SLOWPATH_ABOVE");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();
const u64 kSlowPathAfterBlocks = [] {
    const char* e = std::getenv("SUYU_RECOMP_SLOWPATH_AFTER_BLOCKS");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();
const u64 kTrapStoreLo = [] {
    const char* e = std::getenv("SUYU_RECOMP_TRAP_STORE_LO");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();
const u64 kTrapStoreHi = [] {
    const char* e = std::getenv("SUYU_RECOMP_TRAP_STORE_HI");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();

// How much of the guest's relocation work we do ourselves, and what we do to
// DT_RELASZ/DT_PLTRELSZ afterwards. See ApplyAllRelocations.
//
// Only rtld's own self-relocation actually needs us: it is a hand-written
// bootstrap loop that the recompiled path exits early out of. Everything
// after that is rtld's ordinary C++ relocation pass, which runs correctly
// once rtld itself is relocated.
//
//   rtld-only       - default. Pre-apply and zero rtld alone; leave every
//                     other module untouched for rtld to relocate, exactly as
//                     under dynarmic. Smash reaches an actual match on this.
//   zero-all        - the previous default: pre-apply every module and zero
//                     every module's sizes. Boots, but main's .dynamic then
//                     reports no relocations, so when nn::ro loads a fighter
//                     NRO it cannot bind main's deferred imports. Measured:
//                     0 of main's 909 lua2cpp::create_agent_fighter_* slots
//                     are ever bound, and the match never starts.
//   zero-rtld       - pre-apply every module but zero only rtld's sizes.
//                     Measured not to work: rtld's pass then re-relocates
//                     modules we already did and svcBreaks immediately
//                     (12071 breaks, first at 0.1s after the pre-apply).
//   restore-on-main - zero every module, then put the sizes back on first
//                     execution in main. Measured not to work: rtld caches
//                     the sizes into its own module state when it parses
//                     .dynamic at boot, so a later write is never re-read and
//                     the slots stay unbound just as under zero-all.
const std::string kRelaPolicy = [] {
    const char* e = std::getenv("SUYU_RECOMP_RELA_POLICY");
    return std::string{(e && *e) ? e : "rtld-only"};
}();
// Symbol-name prefix whose unresolved JUMP_SLOT/GLOB_DAT slots are recorded at
// relocation time and re-read periodically, so a log can say whether anything
// ever bound them. Smash's fighter agents are `lua2cpp::create_agent_fighter`.
const std::string kTrackSlotPrefix = [] {
    const char* e = std::getenv("SUYU_RECOMP_TRACK_SLOT_PREFIX");
    return std::string{(e && *e) ? e : ""};
}();
struct TrackedSlot {
    u64 got_va = 0;
    u64 stub_va = 0;
    std::string name;
};
std::mutex g_tracked_lock;
std::vector<TrackedSlot> g_tracked_slots;

// The original DT_RELASZ/DT_PLTRELSZ values, as (address, size) pairs, for the
// restore-on-main policy. Process-global rather than per-Impl: every CPU Impl
// runs its own relocation pass, but only the first one sees the real sizes -
// the rest parse .dynamic after it has already zeroed them, so a per-Impl copy
// records zeros and a later Impl's restore writes those zeros back over the
// first Impl's correct one. That happened before the first NRO load and put
// the sizes back at 0 exactly where they were needed.
std::mutex g_rela_restore_lock;
std::vector<std::pair<u64, u64>> g_rela_restore;
std::atomic<bool> g_rela_restored{false};

bool SampleWindowOpen() {
    if (kSampleAfterSec <= 0.0) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - kRecompStart).count() >= kSampleAfterSec;
}

static_assert(offsetof(GuestContextView, host_mem) == 832);
static_assert(offsetof(GuestContextView, tpidrro_el0) == 840);
static_assert(offsetof(GuestContextView, fpcr) == 848);
static_assert(offsetof(GuestContextView, fpsr) == 856);

// The generated code signals an SVC by parking with this set. Kept in sync
// with the emitted recomp_svc contract in core/recompiler/arm64_to_c.h.
constexpr u64 kNoPendingSvc = ~0ULL;

// Mirrors RECOMP_HALT_UNHANDLED in the generated runtime: a block that halts
// with this parked its PC on an instruction the decoder cannot translate and
// is asking for that address to be executed by the interpreter fallback.
constexpr int kHaltUnhandled = 2;
// Mirrors RECOMP_HALT_BREAKPOINT. PC already names the faulting instruction.
constexpr int kHaltBreakpoint = 3;
constexpr int kHaltIcIvau = 4;
std::atomic<bool> g_code_guard_ready{false};
std::atomic<u64> g_forced_cutoff_pc{0};
std::atomic<u64> g_forced_cutoff_blocks{0};

// An unresolved GOT/JUMP_SLOT relocation used to be left untouched, which
// means a call through it branches to whatever the raw NSO file already had
// sitting in that GOT slot - typically a small placeholder/addend value the
// static linker left for a symbol it expected the *dynamic* linker to fill
// in later (lazy-binding stub offset, or just zero-adjacent garbage), not a
// real address. The guest's BLR then lands on that small value directly -
// e.g. 0xe7ff0 - which is unmapped, and the JIT fallback that "No recompiled
// block" hands off to can't execute there either, so the whole thread dies.
// Every unresolved slot is patched to this fixed, recognizable sentinel
// instead: the dispatch loop below special-cases it as an immediate "return
// to caller" (PC = LR) rather than a real guest address, so a genuinely
// call-but-never-actually-invoked unresolved import (the common case - most
// entries in a large import table exist for code paths a given boot never
// takes) fails soft instead of crashing the thread outright.
constexpr u64 kUnresolvedImportTrap = 0xFFFF'FFFF'0000'0000ULL;
} // namespace

namespace {
std::atomic<RecompLookupFn> g_recomp_lookup{nullptr};
std::atomic<RecompBaseFn> g_recomp_base_setter{nullptr};
std::mutex g_process_init_lock;

/// Execution coverage for the AOT path (mk8-recomp #13).
///
/// The exporter's static coverage says what fraction of the *image* translates.
/// It cannot say what fraction of *execution* stays on the recompiled path,
/// and those differ by orders of magnitude: one untranslated instruction inside
/// a hot loop costs a full engine transition on every iteration, while a
/// thousand untranslated instructions in code that never runs cost nothing.
///
/// Only this decides whether the AOT path is worth anything, and only this can
/// rank the missing opcodes by what actually executes.
struct RecompCounters {
    std::atomic<u64> svc_calls{0};
    std::atomic<u64> fallback_from_miss{0};
    std::atomic<u64> fallback_from_unhandled{0};
    std::atomic<u64> jit_to_static{0};
    std::atomic<u64> unresolved_import_traps{0};
    std::atomic<u64> no_fallback_available{0};

    // Guarded rather than atomic: these are touched only on a transition, which
    // is by definition already the slow path.
    std::mutex hist_lock;
    std::map<u32, u64> unhandled_insn;  ///< guest encoding -> times it forced a fallback
    std::map<u64, u64> miss_pc;         ///< PC with no block -> times it forced a fallback
    std::map<u32, u64> svc_numbers;     ///< SVC imm -> times the guest issued it
    std::map<u64, u64> sampled_pc;      ///< sparse samples for zero-transition stalls
    /// Load address -> module name, so a PC in this report can be resolved to
    /// module+offset. Without it the addresses mean nothing except beside the
    /// matching boot log, and a report read against another run's log resolves
    /// to the wrong place without saying so.
    std::map<u64, std::string> modules;

    void RecordModules(const std::map<u64, std::string>& m) {
        std::scoped_lock lk{hist_lock};
        modules = m;
    }

    void RecordSvc(u32 num) {
        std::scoped_lock lk{hist_lock};
        ++svc_numbers[num];
    }

    void RecordUnhandled(u32 insn) {
        std::scoped_lock lk{hist_lock};
        ++unhandled_insn[insn];
    }
    void RecordMiss(u64 pc) {
        std::scoped_lock lk{hist_lock};
        ++miss_pc[pc];
    }
    void RecordSample(u64 pc) {
        std::scoped_lock lk{hist_lock};
        ++sampled_pc[pc];
    }
};

RecompCounters g_counters;
std::atomic<int> g_live_instances{0};
std::atomic<Kernel::KProcess*> g_report_process{nullptr};

/// The block tally is incremented once per executed block by every guest
/// thread. As one shared atomic that is a contended cache line on the hottest
/// path there is: it was 23% of the dispatch loop's own cycles. Each thread
/// counts into its own line instead, and the report sums them.
struct alignas(64) ThreadBlocks {
    std::atomic<u64> n{0};
    char pad[64 - sizeof(std::atomic<u64>)];
};
std::mutex g_tally_lock;
std::vector<ThreadBlocks*> g_tallies;
std::atomic<u64> g_retired_blocks{0};

struct ThreadBlockSlot {
    ThreadBlocks* slot = new ThreadBlocks{};
    ThreadBlockSlot() {
        std::scoped_lock lk{g_tally_lock};
        g_tallies.push_back(slot);
    }
    // The slot outlives the thread: the reporter may be summing while a guest
    // thread exits, and the count still belongs in the total.
};
thread_local ThreadBlockSlot t_blocks;

u64 TotalStaticBlocks() {
    std::scoped_lock lk{g_tally_lock};
    u64 sum = g_retired_blocks.load(std::memory_order_relaxed);
    for (const ThreadBlocks* s : g_tallies) {
        sum += s->n.load(std::memory_order_relaxed);
    }
    return sum;
}

template <typename Map>
auto TopN(const Map& m, size_t n) {
    // Not Map::value_type: that has a const key and so is not assignable, which
    // partial_sort requires.
    using Entry = std::pair<typename Map::key_type, typename Map::mapped_type>;
    std::vector<Entry> v(m.begin(), m.end());
    std::partial_sort(v.begin(), v.begin() + std::min(n, v.size()), v.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > n) {
        v.resize(n);
    }
    return v;
}

/// Formats the run's execution-coverage report.
///
/// Built as a string rather than logged line by line so the same text can go to
/// both the log and a file. The file matters: the emulated process is not
/// always torn down at shutdown, so ~ArmRecomp may never run and the whole
/// run's measurement is lost with it. Writing periodically means a report
/// always exists for the last completed interval however the process ends.
std::string FormatRecompCoverage() {
    const u64 blocks = TotalStaticBlocks();
    const u64 miss = g_counters.fallback_from_miss.load();
    const u64 unh = g_counters.fallback_from_unhandled.load();
    const u64 transitions = miss + unh;

    if (blocks == 0 && transitions == 0) {
        return {};  // backend never ran; saying nothing is better than printing zeros
    }

    std::string o = "=== RECOMP EXECUTION COVERAGE ===\n";
    o += fmt::format("  static blocks executed : {}\n", blocks);
    o += fmt::format("  SVCs to HLE            : {}\n", g_counters.svc_calls.load());
    o += fmt::format("  static -> JIT          : {} ({} lookup miss, {} unimplemented opcode)\n",
                     transitions, miss, unh);
    o += fmt::format("  JIT -> static          : {}\n", g_counters.jit_to_static.load());
    o += fmt::format("  unresolved import traps: {}\n", g_counters.unresolved_import_traps.load());
    if (const u64 nofb = g_counters.no_fallback_available.load(); nofb) {
        o += fmt::format("  threads killed with no JIT fallback: {}\n", nofb);
    }

    // Blocks per transition is the number that matters. Each transition costs a
    // 32-GPR + 32-vector marshal in each direction, so a high static block count
    // next to a comparable transition count is worse than it looks.
    if (transitions) {
        o += fmt::format("  blocks per transition  : {:.1f}\n",
                         double(blocks) / double(transitions));
    } else {
        o += fmt::format("  blocks per transition  : no transitions - fully static\n");
    }

    std::scoped_lock lk{g_counters.hist_lock};

    if (!g_counters.unhandled_insn.empty()) {
        o += "  --- unimplemented opcodes by execution count ---\n";
        for (const auto& [insn, count] : TopN(g_counters.unhandled_insn, 24)) {
            o += fmt::format("    {:08X}  {:>10}  {:5.2f}%  (sig {:08X})\n", insn, count,
                             unh ? 100.0 * double(count) / double(unh) : 0.0, insn & 0xFFC00000u);
        }
        o += fmt::format("    {} distinct encodings\n", g_counters.unhandled_insn.size());
    }

    if (!g_counters.svc_numbers.empty()) {
        // What the guest actually asks the kernel for. A boot that stops making
        // system calls while still executing millions of blocks is spinning on
        // something, and this says on what.
        o += "  --- SVCs by call count ---\n";
        for (const auto& [num, count] : TopN(g_counters.svc_numbers, 40)) {
            o += fmt::format("    svc 0x{:02X}  {:>10}\n", num, count);
        }
        o += fmt::format("    {} distinct SVCs\n", g_counters.svc_numbers.size());
    }

    if (!g_counters.modules.empty()) {
        o += "  --- loaded modules ---\n";
        for (const auto& [base, name] : g_counters.modules) {
            o += fmt::format("    {:#018x}  {}\n", base, name);
        }
    }

    if (!g_counters.sampled_pc.empty()) {
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            for (const auto& [base, module_name] : g_counters.modules) {
                if (pc >= base && base >= best) {
                    best = base;
                    name = &module_name;
                }
            }
            return name ? fmt::format("  {}+{:#x}", *name, pc - best) : std::string{};
        };
        o += "  --- sampled PCs by count ---\n";
        for (const auto& [pc, count] : TopN(g_counters.sampled_pc, 48)) {
            o += fmt::format("    {:#018x}  {:>10}{}\n", pc, count, resolve(pc));
        }
        o += fmt::format("    {} distinct sampled PCs\n", g_counters.sampled_pc.size());
    }

    if (!g_counters.miss_pc.empty()) {
        // Resolved here rather than left to the reader: a bare guest PC
        // needs this run's load addresses to mean anything, and pairing a
        // report with another run's log gives a confident wrong answer.
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            for (const auto& [base, module_name] : g_counters.modules) {
                if (pc >= base && base >= best) {
                    best = base;
                    name = &module_name;
                }
            }
            return name ? fmt::format("  {}+{:#x}", *name, pc - best) : std::string{};
        };
        o += "  --- uncovered PCs by execution count ---\n";
        for (const auto& [pc, count] : TopN(g_counters.miss_pc, 16)) {
            o += fmt::format("    {:#018x}  {:>10}{}\n", pc, count, resolve(pc));
        }
        o += fmt::format("    {} distinct PCs\n", g_counters.miss_pc.size());
    }
    if (auto* process = g_report_process.load(std::memory_order_relaxed)) {
        const auto state_name = [](Kernel::ThreadState s) {
            switch (s & Kernel::ThreadState::Mask) {
            case Kernel::ThreadState::Initialized: return "Initialized";
            case Kernel::ThreadState::Waiting: return "Waiting";
            case Kernel::ThreadState::Runnable: return "Runnable";
            case Kernel::ThreadState::Terminated: return "Terminated";
            default: return "?";
            }
        };
        const auto wait_name = [](Kernel::ThreadWaitReasonForDebugging r) {
            switch (r) {
            case Kernel::ThreadWaitReasonForDebugging::None: return "-";
            case Kernel::ThreadWaitReasonForDebugging::Sleep: return "Sleep";
            case Kernel::ThreadWaitReasonForDebugging::IPC: return "IPC";
            case Kernel::ThreadWaitReasonForDebugging::Synchronization: return "Sync";
            case Kernel::ThreadWaitReasonForDebugging::ConditionVar: return "CondVar";
            case Kernel::ThreadWaitReasonForDebugging::Arbitration: return "Arbitration";
            case Kernel::ThreadWaitReasonForDebugging::Suspended: return "Suspended";
            default: return "?";
            }
        };
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            for (const auto& [base, module_name] : g_counters.modules) {
                if (pc >= base && base >= best) {
                    best = base;
                    name = &module_name;
                }
            }
            return name ? fmt::format("  {}+{:#x}", *name, pc - best) : std::string{};
        };
        std::string live;
        for (auto& t : process->GetThreadList()) {
            // For a thread parked in address arbitration or a condvar, the
            // address it is keyed on and what that address holds right now is
            // the whole diagnosis: a waiter still queued on a word that no
            // longer says "locked" is a lost wakeup, not contention.
            //
            // Which field holds the live address depends on the wait:
            //
            //   svcArbitrateLock and svcWaitProcessWideKey set m_address_key,
            //   reported by GetAddressKey(), along with m_address_key_value.
            //
            //   svcWaitForAddress sets only m_condvar_key, via
            //   SetAddressArbiter, and touches neither of those.
            //
            // m_address_key is never cleared when a wait ends, so printing it
            // for an address-arbiter waiter shows residue from some earlier
            // mutex wait: an address that thread is not parked on and a value
            // that means nothing. Earlier dumps did exactly that, so the
            // addresses they reported for the Arbitration threads were not the
            // addresses those threads were waiting on.
            std::string key;
            const auto reason = t.GetWaitReasonForDebugging();
            const bool waiting =
                (t.GetState() & Kernel::ThreadState::Mask) == Kernel::ThreadState::Waiting;
            u64 keyed_on = 0;
            if (waiting && reason == Kernel::ThreadWaitReasonForDebugging::Arbitration) {
                keyed_on = t.GetAddressArbiterKey();
                if (keyed_on != 0) {
                    key = fmt::format("  arb_key={:#x} mem_now={:#010x}", keyed_on,
                                      process->GetMemory().Read32(keyed_on));
                }
            } else if (waiting && reason == Kernel::ThreadWaitReasonForDebugging::ConditionVar) {
                keyed_on = GetInteger(t.GetAddressKey());
                if (keyed_on != 0) {
                    key = fmt::format("  key={:#x} key_val={:#010x} mem_now={:#010x}", keyed_on,
                                      t.GetAddressKeyValue(),
                                      process->GetMemory().Read32(keyed_on));
                }
            }
            // Every blocked thread sits in one of a handful of supervisor-call
            // stubs, so the stub PC says only which call it is. The link
            // register names the nn::os routine that made the call, which is
            // the part that differs between threads.
            live += fmt::format(
                "    id={:<4} prio={:<3} {:<11} wait={:<11} pc={:#018x}{} lr={:#018x}{}{}\n",
                t.GetId(), t.GetPriority(), state_name(t.GetState()), wait_name(reason),
                t.GetContext().pc, resolve(t.GetContext().pc), t.GetContext().r[30],
                resolve(t.GetContext().r[30]), key);
            if (keyed_on != 0 && Kernel::LockTrace::Enabled()) {
                live += fmt::format("      --- lock history for {:#x} ---\n", keyed_on);
                live += Kernel::LockTrace::DumpForAddress(keyed_on);
            }
            // A thread that is Runnable at the stall is not deadlocked, it is
            // spinning. The loop it is in tests fields of the object in its
            // callee-saved base register, so the registers and the bytes they
            // point at are what says which field never advances. x19-x23 cover
            // the base pointers these loops actually use.
            if (!waiting && (t.GetState() & Kernel::ThreadState::Mask) ==
                                Kernel::ThreadState::Runnable) {
                const auto& ctx = t.GetContext();
                for (int r = 19; r <= 23; ++r) {
                    const u64 base = ctx.r[r];
                    live += fmt::format("      x{}={:#018x}", r, base);
                    // Only chase a register the guest has actually mapped. A
                    // range test is not enough: a plain integer can land inside
                    // the address space and the read then reports itself as an
                    // unmapped guest access, which is indistinguishable in the
                    // log from a real one.
                    if (process->GetMemory().IsValidVirtualAddress(base) &&
                        process->GetMemory().IsValidVirtualAddress(base + 0x10c)) {
                        live += "  [+0xd0..0x110]=";
                        for (u64 off = 0xd0; off < 0x110; off += 4) {
                            live += fmt::format("{:08x} ", process->GetMemory().Read32(base + off));
                        }
                    }
                    live += "\n";
                }
            }
        }
        // The end-of-run report runs from ~ArmRecomp, by which point the
        // process has already unlinked its threads and the live list is empty.
        // The periodic writes during the run are the ones that see it, so the
        // last non-empty rendering is kept and reported instead of nothing.
        static std::mutex snapshot_lock;
        static std::string snapshot;
        static u64 snapshot_blocks = 0;
        {
            std::scoped_lock snapshot_guard{snapshot_lock};
            if (!live.empty()) {
                snapshot = live;
                snapshot_blocks = TotalStaticBlocks();
            }
            if (!snapshot.empty()) {
                o += fmt::format("  --- guest threads (as of {} blocks) ---\n", snapshot_blocks);
                o += snapshot;
                if (Kernel::LockTrace::Enabled()) {
                    o += "  --- lock supervisor calls ---\n";
                    o += Kernel::LockTrace::DumpSummary(80);
                }
            }
        }
    }
    o += "=== END RECOMP EXECUTION COVERAGE ===\n";
    return o;
}

void WriteRecompCoverageFile(const std::string& text) {
    const char* explicit_path = std::getenv("SUYU_RECOMP_COVERAGE_PATH");
    const auto path = explicit_path && *explicit_path
                          ? std::filesystem::path{explicit_path}
                          : Common::FS::GetSuyuPath(Common::FS::SuyuPath::LogDir) /
                                "recomp_coverage.txt";
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << text;
    }
}

// The stall reduces to one single-valued question: does anything ever write
// the load-state byte the two runnable threads poll? No store-side hook can
// answer it - emitted code stores through the inline page-table fast path, so
// HostStore never sees the write. Reading the byte from a host thread can.
//
// The object address is picked by the guest allocator and differs per run, so
// it is derived rather than configured: find a Runnable guest thread whose
// link register is the polling call site (module offset given by the env var)
// and take its x19. That is exactly how the address was identified by hand.
//
// What this can and cannot show: every transition it prints is real, and the
// block count next to it says where in the run it happened. A value that never
// moves is weaker evidence - a write reverted between two samples would be
// missed - but at 20 kHz against a byte that is supposed to latch and stay,
// that gap is small.
// Poll the recorded unresolved slots and log, once a second, how many have
// been written over by something other than us. Answers "did nn::ro ever bind
// main's deferred imports" directly, which no counter can.
void StartSlotWatcher() {
    if (kTrackSlotPrefix.empty()) {
        return;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            size_t last_bound = SIZE_MAX;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::scoped_lock lk{g_tracked_lock};
                if (g_tracked_slots.empty()) {
                    continue;
                }
                auto* proc = g_report_process.load(std::memory_order_relaxed);
                if (!proc) {
                    continue;
                }
                auto& memory = proc->GetMemory();
                size_t bound = 0;
                std::string first_bound;
                for (const auto& t : g_tracked_slots) {
                    if (!memory.IsValidVirtualAddress(t.got_va)) {
                        continue;
                    }
                    const u64 now = memory.Read64(t.got_va);
                    if (now != t.stub_va) {
                        ++bound;
                        if (first_bound.empty()) {
                            first_bound = fmt::format("{} -> {:#x}", t.name, now);
                        }
                    }
                }
                if (bound != last_bound) {
                    last_bound = bound;
                    LOG_INFO(Core_ARM,
                             "recomp: tracked slots bound {}/{} at t={:.1f}s{}{}", bound,
                             g_tracked_slots.size(),
                             std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                           kRecompStart)
                                 .count(),
                             first_bound.empty() ? "" : " first=", first_bound);
                }
            }
        }).detach();
    });
}

void StartStateWatcher(Kernel::KProcess* process) {
    const char* lr_env = std::getenv("SUYU_RECOMP_WATCH_LR_OFFSET");
    if (!lr_env || !*lr_env) {
        return;
    }
    const u64 lr_offset = std::strtoull(lr_env, nullptr, 0);
    const char* addr_env = std::getenv("SUYU_RECOMP_WATCH_ADDR");
    const u64 fixed_addr = (addr_env && *addr_env) ? std::strtoull(addr_env, nullptr, 0) : 0;
    const char* path_env = std::getenv("SUYU_RECOMP_WATCH_PATH");
    const std::string path =
        (path_env && *path_env)
            ? std::string{path_env}
            : (Common::FS::GetSuyuPath(Common::FS::SuyuPath::LogDir) / "recomp_watch.txt").string();

    std::thread([process, lr_offset, fixed_addr, path] {
        std::ofstream out(path, std::ios::trunc);
        const auto stamp = [] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - kRecompStart)
                .count();
        };
        auto& memory = process->GetMemory();
        u64 base = 0;
        int last_thread_count = -1;
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            std::scoped_lock lk{g_counters.hist_lock};
            for (const auto& [b, n] : g_counters.modules) {
                if (pc >= b && b >= best) {
                    best = b;
                    name = &n;
                }
            }
            return name ? fmt::format(" {}+{:#x}", *name, pc - best) : std::string{};
        };
        const auto dump_threads = [&](std::ofstream& o) {
            for (auto& t : process->GetThreadList()) {
                o << fmt::format("        id={:<4} state={} pc={:#x}{} lr={:#x}{}\n", t.GetId(),
                                 static_cast<int>(t.GetState() & Kernel::ThreadState::Mask),
                                 t.GetContext().pc, resolve(t.GetContext().pc),
                                 t.GetContext().r[30], resolve(t.GetContext().r[30]));
            }
        };

        // Phase 1: wait for the polling thread to exist, and meanwhile record
        // every change in the guest thread population. Baseline reaches 43
        // threads across this point and static never exceeds 10, so when the
        // count stops growing is itself a measurement.
        while (base == 0) {
            if (fixed_addr != 0 && memory.IsValidVirtualAddress(fixed_addr) &&
                memory.IsValidVirtualAddress(fixed_addr + 0x10c)) {
                base = fixed_addr;
                break;
            }
            int count = 0;
            u64 found = 0;
            for (auto& t : process->GetThreadList()) {
                ++count;
                if ((t.GetState() & Kernel::ThreadState::Mask) != Kernel::ThreadState::Runnable) {
                    continue;
                }
                const u64 lr = t.GetContext().r[30];
                for (const u64 mod : [&] {
                         std::vector<u64> v;
                         std::scoped_lock lk{g_counters.hist_lock};
                         for (const auto& [b, n] : g_counters.modules) v.push_back(b);
                         return v;
                     }()) {
                    if (lr - mod == lr_offset) {
                        found = t.GetContext().r[19];
                    }
                }
            }
            if (count != last_thread_count) {
                last_thread_count = count;
                out << fmt::format("[{:8.3f}s blocks={:<12}] threads={}\n", stamp(),
                                   TotalStaticBlocks(), count);
                for (auto& t : process->GetThreadList()) {
                    out << fmt::format("    id={:<4} state={} pc={:#x} lr={:#x}\n", t.GetId(),
                                       static_cast<int>(t.GetState() & Kernel::ThreadState::Mask),
                                       t.GetContext().pc, t.GetContext().r[30]);
                }
                out.flush();
            }
            if (found != 0 && memory.IsValidVirtualAddress(found) &&
                memory.IsValidVirtualAddress(found + 0x10c)) {
                base = found;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        out << fmt::format("[{:8.3f}s blocks={:<12}] latched object {:#x} from x19 of the thread "
                           "polling at module+{:#x}\n",
                           stamp(), TotalStaticBlocks(), base, lr_offset);
        out.flush();

        // Phase 2: poll the object. The whole 0xd0..0x110 window rather than
        // the one byte, because "the state byte never moved but the field next
        // to it did" and "nothing in the object ever moved" are different
        // findings and cost the same to collect.
        u32 prev[16];
        for (int i = 0; i < 16; ++i) {
            prev[i] = memory.Read32(base + 0xd0 + 4 * i);
        }
        out << fmt::format("[{:8.3f}s blocks={:<12}] initial", stamp(), TotalStaticBlocks());
        for (int i = 0; i < 16; ++i) out << fmt::format(" {:08x}", prev[i]);
        out << "\n";
        out.flush();

        u64 polls = 0;
        auto last_heartbeat = std::chrono::steady_clock::now();
        while (true) {
            ++polls;
            u32 now[16];
            for (int i = 0; i < 16; ++i) {
                now[i] = memory.Read32(base + 0xd0 + 4 * i);
            }
            {
                // Same arithmetic as recomp_host_ptr in the generated header.
                const auto view = memory.GetPageTableView();
                for (int i = 0; i < 16; ++i) {
                    const u64 va = (base + 0xd0 + 4 * i) & 0xffffffffffffULL;
                    u32 fast = 0;
                    bool have = false;
                    if (view.entries && va < view.address_space_max) {
                        uintptr_t raw;
                        std::memcpy(&raw,
                                    reinterpret_cast<const unsigned char*>(view.entries) +
                                        (va >> view.page_bits) * view.entry_stride,
                                    sizeof(raw));
                        const uintptr_t p = raw & static_cast<uintptr_t>(view.pointer_mask);
                        if (p) {
                            std::memcpy(&fast, reinterpret_cast<const void*>(p + va), 4);
                            have = true;
                        }
                    }
                    if (have && fast != now[i]) {
                        out << fmt::format("[{:8.3f}s blocks={:<12}] VIEW MISMATCH +{:#x} "
                                           "slow={:08x} fast={:08x}\n",
                                           stamp(), TotalStaticBlocks(), 0xd0 + 4 * i, now[i],
                                           fast);
                        out.flush();
                    }
                    if (!have) {
                        static bool once = false;
                        if (!once) {
                            once = true;
                            out << fmt::format("[{:8.3f}s] note: inline walk declines +{:#x} - the "
                                               "guest takes the callback path for this object\n",
                                               stamp(), 0xd0 + 4 * i);
                            out.flush();
                        }
                    }
                }
            }
            if (std::memcmp(now, prev, sizeof(prev)) != 0) {
                out << fmt::format("[{:8.3f}s blocks={:<12}] CHANGE after {} polls", stamp(),
                                   TotalStaticBlocks(), polls);
                for (int i = 0; i < 16; ++i) {
                    out << (now[i] == prev[i] ? fmt::format(" {:08x}", now[i])
                                              : fmt::format(" [{:08x}]", now[i]));
                }
                out << "\n";
                dump_threads(out);
                out.flush();
                std::memcpy(prev, now, sizeof(prev));
            }
            const auto t = std::chrono::steady_clock::now();
            if (t - last_heartbeat >= std::chrono::seconds(5)) {
                last_heartbeat = t;
                int count = 0;
                for (auto& th : process->GetThreadList()) {
                    (void)th;
                    ++count;
                }
                out << fmt::format("[{:8.3f}s blocks={:<12}] heartbeat polls={} threads={} "
                                   "+0xe4={:08x}\n",
                                   stamp(), TotalStaticBlocks(), polls, count, prev[5]);
                out.flush();
            }
            std::this_thread::yield();
        }
    }).detach();
}

void ReportRecompCoverage() {
    const std::string report = FormatRecompCoverage();
    if (report.empty()) {
        return;
    }
    WriteRecompCoverageFile(report);
    // Split by hand: the report is already newline-delimited and the logger
    // takes one line at a time.
    size_t pos = 0;
    while (pos < report.size()) {
        const size_t nl = report.find('\n', pos);
        const std::string_view line{report.data() + pos,
                                    (nl == std::string::npos ? report.size() : nl) - pos};
        if (!line.empty()) {
            LOG_INFO(Core_ARM, "{}", line);
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
}

} // namespace

void SetRecompLookup(RecompLookupFn lookup) {
    std::scoped_lock lock{g_process_init_lock};
    g_code_guard_ready.store(false, std::memory_order_release);
    g_recomp_lookup.store(lookup, std::memory_order_release);
}

void SetRecompLongSlices(bool enabled) {
    g_recomp_chain_budget.store(enabled ? 4096 : 32, std::memory_order_relaxed);
}

void SetRecompCodeGuardReady(bool ready) {
    g_code_guard_ready.store(ready, std::memory_order_release);
}

bool IsRecompCodeGuardReady() {
    return g_code_guard_ready.load(std::memory_order_acquire);
}

void SetRecompBaseSetter(RecompBaseFn setter) {
    g_recomp_base_setter.store(setter, std::memory_order_release);
}

RecompLiveStats GetRecompLiveStats() {
    return RecompLiveStats{
        TotalStaticBlocks(),
        g_counters.fallback_from_miss.load(std::memory_order_relaxed) +
            g_counters.fallback_from_unhandled.load(std::memory_order_relaxed),
        g_forced_cutoff_pc.load(std::memory_order_relaxed),
        g_forced_cutoff_blocks.load(std::memory_order_relaxed),
        g_live_instances.load(std::memory_order_relaxed) > 0,
#ifdef SUYU_NO_JIT
        false,
#else
        true,
#endif
        StrictNoFallback(),
    };
}

RecompLookupFn GetRecompLookup() {
    return g_recomp_lookup.load(std::memory_order_acquire);
}

struct ArmRecomp::Impl {
    Impl(System& system_, RecompLookupFn lookup_) : system{system_}, lookup{lookup_} {
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.pending_svc = kNoPendingSvc;
        // Point the recompiled code at the emulator's address space.
        bridge.user = this;
        bridge.load = &Impl::HostLoad;
        bridge.store = &Impl::HostStore;
        bridge.excl_load = &Impl::HostExclusiveLoad;
        bridge.excl_store = &Impl::HostExclusiveStore;
        bridge.clear_excl = &Impl::HostClearExclusive;
        bridge.read_cntpct = &Impl::HostReadCntpct;
        bridge.excl_load_pair = &Impl::HostExclusiveLoadPair;
        bridge.excl_store_pair = &Impl::HostExclusiveStorePair;
        // Filled in by RefreshPageTable once a process exists; until then the
        // fields stay null and every access takes the callback path.
        bridge.page_entries = nullptr;
        ctx.host_mem = &bridge;
    }

    // The monitor is per-core, and core_index is what distinguishes one guest
    // thread's reservation from another's. Without a monitor (no owning process,
    // so no fallback either) these degrade to plain accesses with STXR always
    // succeeding - the old behaviour, and wrong under threads, but that
    // configuration cannot run a real title anyway.
    static u64 HostExclusiveLoad(void* user, u64 va, u32 size) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            return HostLoad(user, va, size);
        }
        const auto core = self->core_index;
        switch (size) {
        case 1: return self->exclusive_monitor->ExclusiveRead8(core, va);
        case 2: return self->exclusive_monitor->ExclusiveRead16(core, va);
        case 4: return self->exclusive_monitor->ExclusiveRead32(core, va);
        case 8: return self->exclusive_monitor->ExclusiveRead64(core, va);
        default: return HostLoad(user, va, size);
        }
    }

    /// Returns 0 on success, 1 when the reservation was lost - the sense of the
    /// status register STXR writes.
    static u32 HostExclusiveStore(void* user, u64 va, u32 size, u64 value) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            HostStore(user, va, size, value);
            return 0;
        }
        const auto core = self->core_index;
        bool ok = false;
        switch (size) {
        case 1: ok = self->exclusive_monitor->ExclusiveWrite8(core, va, static_cast<u8>(value)); break;
        case 2: ok = self->exclusive_monitor->ExclusiveWrite16(core, va, static_cast<u16>(value)); break;
        case 4: ok = self->exclusive_monitor->ExclusiveWrite32(core, va, static_cast<u32>(value)); break;
        case 8: ok = self->exclusive_monitor->ExclusiveWrite64(core, va, value); break;
        default: HostStore(user, va, size, value); return 0;
        }
        return ok ? 0u : 1u;
    }

    /// LDXP/LDAXP. The 64-bit pair takes a real 128-bit reservation; the
    /// 32-bit pair is a 64-bit reservation whose two words are the registers,
    /// which is what the architecture specifies rather than a shortcut.
    static void HostExclusiveLoadPair(void* user, u64 va, u32 size, u64* lo, u64* hi) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            *lo = HostLoad(user, va, size);
            *hi = HostLoad(user, va + size, size);
            return;
        }
        const auto core = self->core_index;
        if (size == 8) {
            const u128 v = self->exclusive_monitor->ExclusiveRead128(core, va);
            *lo = v[0];
            *hi = v[1];
        } else {
            const u64 v = self->exclusive_monitor->ExclusiveRead64(core, va);
            *lo = static_cast<u32>(v);
            *hi = static_cast<u32>(v >> 32);
        }
    }

    /// STXP/STLXP. Returns 0 on success, matching STXR's status sense.
    static u32 HostExclusiveStorePair(void* user, u64 va, u32 size, u64 lo, u64 hi) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            HostStore(user, va, size, lo);
            HostStore(user, va + size, size, hi);
            return 0;
        }
        const auto core = self->core_index;
        bool ok = false;
        if (size == 8) {
            ok = self->exclusive_monitor->ExclusiveWrite128(core, va, u128{lo, hi});
        } else {
            ok = self->exclusive_monitor->ExclusiveWrite64(
                core, va, static_cast<u32>(lo) | (static_cast<u64>(static_cast<u32>(hi)) << 32));
        }
        return ok ? 0u : 1u;
    }

    /// Re-read the page table description into the bridge.
    ///
    /// Called on entry to a run rather than once at construction: the table
    /// belongs to the process, and the pointer is not valid until one exists.
    /// A stale pointer here would have generated code reading another address
    /// space, so it is refreshed rather than cached forever.
    void RefreshPageTable() {
        const auto view = system.ApplicationMemory().GetPageTableView();
        bridge.page_entries = view.entries;
        bridge.page_entry_stride = view.entry_stride;
        bridge.page_bits = view.page_bits;
        bridge.pointer_mask = view.pointer_mask;
        bridge.address_space_max = view.address_space_max;
        if (kSlowPathAbove != 0 && kSlowPathAbove < bridge.address_space_max &&
            TotalStaticBlocks() >= kSlowPathAfterBlocks) {
            bridge.address_space_max = kSlowPathAbove;
        }
    }

    /// The same source DynarmicCallbacks64::GetCNTPCT uses, so a guest thread
    /// that migrates between engines sees one monotonic clock.
    static u64 HostReadCntpct(void* user) {
        return static_cast<Impl*>(user)->system.CoreTiming().GetClockTicks();
    }

    static void HostClearExclusive(void* user) {
        auto* self = static_cast<Impl*>(user);
        if (self->exclusive_monitor) {
            self->exclusive_monitor->ClearExclusive(self->core_index);
        }
    }

    /// Report a static-executed access to an address the guest has not mapped.
    ///
    /// The emitted code resolves a mapped access through the page table inline,
    /// so only an unmapped one reaches these callbacks at all. Such an access
    /// is not a guest bug: it means some earlier translated instruction put a
    /// value in a register that the real hardware would never have produced.
    /// The PC trail is the shortest route from the bad address back to the
    /// instruction that computed it, and it is already maintained for misses.
    static void ReportUnmapped(Impl* self, u64 va, u32 size, const char* what) {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) >= 8) {
            return;
        }
        std::string trail;
        const size_t count = std::min<size_t>(self->trail_pos, Impl::kTrail);
        for (size_t i = 0; i < count; ++i) {
            trail += fmt::format("{:#x} ",
                                 self->trail[(self->trail_pos - count + i) & (Impl::kTrail - 1)]);
        }
        LOG_ERROR(Core_ARM, "recomp unmapped {}{} @ {:#x} from pc={:#x} after {} blocks", what,
                  size * 8, va, self->ctx.pc, TotalStaticBlocks());
        LOG_ERROR(Core_ARM, "recomp unmapped PC trail (oldest first): {}", trail);
        for (size_t r = 0; r < 32; r += 4) {
            LOG_ERROR(Core_ARM,
                      "recomp unmapped regs x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x} "
                      "x{:<2}={:#018x}",
                      r, self->ctx.x[r], r + 1, self->ctx.x[r + 1], r + 2, self->ctx.x[r + 2],
                      r + 3, self->ctx.x[r + 3]);
        }
    }

    static u64 HostLoad(void* user, u64 va, u32 size) {
        auto* self = static_cast<Impl*>(user);
        auto& memory = self->system.ApplicationMemory();
        if (size != 0 && !memory.IsValidVirtualAddress(va)) {
            ReportUnmapped(self, va, size, "read");
        }
        switch (size) {
        case 0: // Negotiated guard-v2 checked instruction read; zero is a valid word.
            if (va > ~u64{0} - 3) return 0;
            for (u64 offset = 0; offset < 4; ++offset) {
                if (!memory.IsValidVirtualAddress(va + offset)) return 0;
            }
            return (u64{1} << 32) | memory.Read32(va);
        case 1: return memory.Read8(va);
        case 2: return memory.Read16(va);
        case 4: return memory.Read32(va);
        default: return memory.Read64(va);
        }
    }

    static void HostStore(void* user, u64 va, u32 size, u64 value) {
        if (kTrapStoreLo != 0 && va >= kTrapStoreLo && va < kTrapStoreHi) {
            auto* self = static_cast<Impl*>(user);
            std::string trail;
            const size_t count = std::min<size_t>(self->trail_pos, Impl::kTrail);
            for (size_t i = 0; i < count; ++i) {
                trail += fmt::format("{:#x} ",
                                     self->trail[(self->trail_pos - count + i) & (Impl::kTrail - 1)]);
            }
            LOG_ERROR(Core_ARM,
                      "recomp TRAP store {:#x} size={} value={:#x} from block pc={:#x} blocks={} "
                      "x19={:#x} x0={:#x} x1={:#x} x8={:#x} lr={:#x} trail={}",
                      va, size, value, self->ctx.pc, TotalStaticBlocks(), self->ctx.x[19],
                      self->ctx.x[0], self->ctx.x[1], self->ctx.x[8], self->ctx.x[30], trail);
        }
        auto* self = static_cast<Impl*>(user);
        auto& memory = self->system.ApplicationMemory();
        if (!memory.IsValidVirtualAddress(va)) {
            ReportUnmapped(self, va, size, "write");
        }
        switch (size) {
        case 1: memory.Write8(va, static_cast<u8>(value)); break;
        case 2: memory.Write16(va, static_cast<u16>(value)); break;
        case 4: memory.Write32(va, static_cast<u32>(value)); break;
        default: memory.Write64(va, value); break;
        }
    }

    /// Base address of the module containing `pc`, so an address can be turned
    /// into the module-relative offset a recompiled image is keyed by. The
    /// module list is fixed once the process is running, so it is read once.
    u64 ModuleBaseFor(Kernel::KThread* thread, u64 pc) {
        if (!modules_read) {
            modules_read = true;
            if (auto* process = thread->GetOwnerProcess()) {
                modules = FindModules(process);
                g_counters.RecordModules(modules);
                // Now that the loader has placed everything, tell each image
                // where its own module went.
                if (const auto setter = g_recomp_base_setter.load(std::memory_order_acquire)) {
                    // modules is keyed by base, so iteration is load order.
                    size_t index = 0;
                    for (const auto& [module_base, name] : modules) {
                        setter(index++, name.c_str(), module_base);
                    }
                }
            }
        }
        u64 base = 0;
        for (const auto& [module_base, name] : modules) {
            if (pc >= module_base && module_base >= base) {
                base = module_base;
            }
        }
        return base;
    }

    // Is `pc` inside the main module? There is no module literally named
    // "main" - Smash's is `cross2_Release.nss` - so it is identified by load
    // order instead: the NSO layout is rtld, main, subsdk*, sdk, and `modules`
    // is keyed by base, so main is the second entry. The next base bounds it.
    bool PcInMainModule(u64 pc) const {
        if (modules.size() < 2) {
            return false;
        }
        auto it = std::next(modules.begin());
        const auto next = std::next(it);
        const u64 end = (next != modules.end()) ? next->first : it->first + 0x10000000ULL;
        return pc >= it->first && pc < end;
    }

    struct DynInfo {
        u64 mod_base = 0;
        u64 rela_va = 0, rela_sz = 0, rela_ent = 24, rela_sz_va = 0;
        u64 jmprel_va = 0, jmprel_sz = 0, jmprel_ent = 24, jmprel_sz_va = 0;
        u64 symtab_va = 0, strtab_va = 0;
        // Address of a harmless "return 0" stub inside this module's own text,
        // used as the target for every import that could not be resolved.
        u64 trap_va = 0;
        std::string name;
    };

    // Find an existing `mov x0, #0; ret` (or failing that, a bare `ret`) in a
    // module's text and hand back its address.
    //
    // Unresolved imports have to point somewhere, and the address has to be one
    // BOTH execution engines can survive: the recompiled dispatcher can
    // special-case a magic sentinel, but the dynarmic fallback cannot - it just
    // tries to translate the address and dies with "cannot execute instruction
    // at unmapped address". Reusing a real instruction pair the module already
    // contains sidesteps that entirely: it is mapped, executable, and returns to
    // the caller under any engine, with no per-engine handling and nothing
    // written into guest memory. Most entries in a large import table exist for
    // code paths a given boot never takes, so failing soft here is what keeps a
    // partially-resolvable module bootable at all.
    u64 FindReturnStub(u64 mod_base) {
        auto& mem = system.ApplicationMemory();
        constexpr u32 kMovX0Zero = 0xD2800000, kRet = 0xD65F03C0;
        // Text sits at the front of every NSO; a module without a matching pair
        // in its first megabyte does not have one worth scanning further for.
        constexpr u64 kScanLimit = 0x100000;
        u64 bare_ret = 0;
        for (u64 off = 0; off < kScanLimit; off += 4) {
            const u32 insn = mem.Read32(mod_base + off);
            if (insn == kRet) {
                if (!bare_ret) bare_ret = mod_base + off;
            } else if (insn == kMovX0Zero && mem.Read32(mod_base + off + 4) == kRet) {
                return mod_base + off;
            }
        }
        return bare_ret;
    }

    // Locate a module's MOD0 header and parse its .dynamic section. Returns
    // false if this module has no MOD0 (nothing to relocate).
    bool ParseDynamic(u64 mod_base, DynInfo& out) {
        auto& mem = system.ApplicationMemory();
        // MOD0 magic "MOD0" = 0x30444F4D. It sits at the start of rodata
        // (typically mod+0x2000 for rtld), but the actual location is pointed
        // to by a 4-byte offset at mod+4 (per NSO ABI). Scan the first few KB.
        u64 mod0_va = 0;
        for (u64 off = 0; off < 0x4000; off += 4) {
            if (mem.Read32(mod_base + off) == 0x30444F4Du) {
                mod0_va = mod_base + off;
                break;
            }
        }
        if (!mod0_va) return false;

        // MOD0 layout: magic(4), dyn_offset(4), bss_start(4), bss_end(4)
        // dyn_offset is relative to the MOD0 header itself.
        const u32 dyn_rel_off = mem.Read32(mod0_va + 4);
        const u64 dyn_va = mod0_va + dyn_rel_off;

        constexpr u32 DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
                       DT_RELASZ = 8, DT_RELAENT = 9, DT_PLTREL = 20, DT_JMPREL = 23,
                       DT_REL_TAG = 17;
        out.mod_base = mod_base;
        u64 pltrel_kind = DT_RELA; // default per AArch64 ABI (RELA, not REL)
        for (u64 p = dyn_va; ; p += 16) {
            const u64 tag = mem.Read64(p);
            const u64 val = mem.Read64(p + 8);
            if (tag == DT_NULL) break;
            if (tag == DT_RELA)     out.rela_va    = mod_base + val;
            if (tag == DT_RELASZ)   { out.rela_sz = val; out.rela_sz_va = p + 8; }
            if (tag == DT_RELAENT)  out.rela_ent   = val;
            if (tag == DT_JMPREL)   out.jmprel_va  = mod_base + val;
            if (tag == DT_PLTRELSZ) { out.jmprel_sz = val; out.jmprel_sz_va = p + 8; }
            if (tag == DT_PLTREL)   pltrel_kind    = val;
            if (tag == DT_SYMTAB)   out.symtab_va  = mod_base + val;
            if (tag == DT_STRTAB)   out.strtab_va  = mod_base + val;
            if (p - dyn_va > 0x1000) break; // safety
        }
        // DT_PLTREL says whether JMPREL uses 16-byte REL entries (no addend)
        // instead of 24-byte RELA - vanishingly rare on AArch64, but assuming
        // RELA unconditionally would silently misalign every read if a
        // module did use it. Checked after the loop since DT_PLTREL can
        // appear either before or after the entries it describes.
        out.jmprel_ent = (pltrel_kind == DT_REL_TAG) ? 16 : 24;
        return true;
    }

    // Read a symbol's name (from .dynstr) and value for GLOB_DAT/JUMP_SLOT
    // resolution. Elf64_Sym: st_name(4) st_info(1) st_other(1) st_shndx(2)
    // st_value(8) st_size(8) = 24 bytes.
    struct SymInfo {
        std::string name;
        u64 value = 0;
        bool defined = false;
        bool weak = false;
    };
    SymInfo ReadSymbol(const DynInfo& d, u32 index) {
        auto& mem = system.ApplicationMemory();
        SymInfo s;
        if (!d.symtab_va) return s;
        const u64 sym_va = d.symtab_va + static_cast<u64>(index) * 24;
        const u32 name_off = mem.Read32(sym_va);
        // st_shndx is a 2-byte field at offset 6 (st_name(4) st_info(1)
        // st_other(1) st_shndx(2) st_value(8) st_size(8)) - reading 4 bytes
        // here previously spilled into st_value's low bytes, corrupting the
        // defined/undefined check for essentially every symbol whose value
        // had nonzero low 16 bits.
        const u16 shndx = mem.Read16(sym_va + 6);
        s.value = mem.Read64(sym_va + 8);
        s.defined = shndx != 0; // SHN_UNDEF == 0
        // st_info is the byte at +4; the binding is its high nibble.
        // STB_WEAK == 2. An undefined weak symbol must resolve to 0, which is
        // how the guest's own rtld leaves it - see the fallthrough below.
        s.weak = (static_cast<u8>(mem.Read8(sym_va + 4)) >> 4) == 2;
        if (d.strtab_va) {
            std::string name;
            for (u64 i = 0; i < 512; ++i) {
                const u8 c = static_cast<u8>(mem.Read8(d.strtab_va + name_off + i));
                if (!c) break;
                name.push_back(static_cast<char>(c));
            }
            s.name = std::move(name);
        }
        return s;
    }

    // Every module's exported (defined) symbols, keyed by name, so
    // GLOB_DAT/JUMP_SLOT relocations that reference another module's symbol
    // (e.g. main calling into sdk, or rtld exporting to everything) can be
    // resolved. Built once, across every module, before any relocation
    // actually writes anything - a relocation processed before its target
    // module's exports are indexed would silently resolve to nothing.
    void IndexExports(const DynInfo& d, std::unordered_map<std::string, u64>& out) {
        if (!d.symtab_va || !d.strtab_va) return;
        // No count is stored in .dynamic for a plain DT_SYMTAB (that's normally
        // DT_HASH/DT_GNU_HASH territory), but .dynsym and .dynstr are laid out
        // back to back in every Switch module observed so far, so the gap
        // between them is a reliable entry count - far more so than guessing
        // from name-offset values, which was cutting exports short before
        // rtld's own required symbols were reached (18 unresolved externals
        // for rtld itself were enough to trigger its self-abort).
        u32 max_index = 8192;
        if (d.strtab_va > d.symtab_va) {
            const u64 span = d.strtab_va - d.symtab_va;
            max_index = static_cast<u32>(std::min<u64>(span / 24, 65536));
        }
        for (u32 i = 1; i < max_index; ++i) { // index 0 is always the null symbol
            const auto sym = ReadSymbol(d, i);
            if (sym.defined && !sym.name.empty()) {
                out.emplace(sym.name, d.mod_base + sym.value);
            }
        }
    }

    void ApplyRelocTable(const DynInfo& d, u64 table_va, u64 table_sz, u64 entry_sz,
                          const std::unordered_map<std::string, u64>& exports, u32& applied,
                          u32& unresolved) {
        auto& mem = system.ApplicationMemory();
        constexpr u32 R_AARCH64_ABS64 = 0x101, R_AARCH64_RELATIVE = 0x403,
                       R_AARCH64_GLOB_DAT = 0x401, R_AARCH64_JUMP_SLOT = 0x402,
                       R_AARCH64_IRELATIVE = 0x408;
        for (u64 p = table_va; p < table_va + table_sz; p += entry_sz) {
            const u64 r_offset = mem.Read64(p);
            const u64 r_info   = mem.Read64(p + 8);
            const u64 r_addend = mem.Read64(p + 16);
            const u32 r_type = static_cast<u32>(r_info & 0xFFFFFFFF);
            const u32 r_sym  = static_cast<u32>(r_info >> 32);
            if (r_type == R_AARCH64_RELATIVE) {
                mem.Write64(d.mod_base + r_offset, d.mod_base + r_addend);
                ++applied;
            } else if (r_type == R_AARCH64_IRELATIVE) {
                // IRELATIVE (ifunc): r_addend is a RESOLVER function's address,
                // not the final target - the correct behaviour is to call it
                // (no args, AAPCS64) and store whatever it returns. Actually
                // invoking guest code from inside relocation application would
                // need a full nested-call machinery this backend doesn't have,
                // so - same as a genuinely-unresolved import - patch to the
                // trap sentinel instead of leaving the GOT slot as raw
                // pre-relocation file content. Previously this relocation type
                // matched none of the branches below and was silently skipped
                // entirely, which is exactly how a BLR through this slot ended
                // up jumping to a small leftover file value (e.g. 0xe7ff0)
                // instead of either a real function or a diagnosable trap.
                LOG_ERROR(Core_ARM,
                          "recomp: IRELATIVE relocation at module base={:#x} offset={:#x} not "
                          "invoked (resolver call unsupported); trapped instead",
                          d.mod_base, r_offset);
                mem.Write64(d.mod_base + r_offset, d.trap_va ? d.trap_va : kUnresolvedImportTrap);
            } else if (r_type == R_AARCH64_GLOB_DAT || r_type == R_AARCH64_JUMP_SLOT ||
                       r_type == R_AARCH64_ABS64) {
                const auto sym = ReadSymbol(d, r_sym);
                // ABS64 is S + A, unlike GLOB_DAT/JUMP_SLOT which are plain S.
                // It is by far the most common relocation in a C++ module's
                // .data.rel.ro - every vtable slot, every typeinfo pointer, every
                // static function-pointer table is one - and skipping it left
                // those slots holding the raw module-relative symbol value the
                // linker wrote. A virtual call through such a vtable branches to
                // that small offset instead of base+offset, which is unmapped:
                // that is the whole "cannot execute instruction at unmapped
                // address 0xe7ff0" family of boot crashes.
                const u64 addend = (r_type == R_AARCH64_ABS64) ? r_addend : 0;
                // Linker-synthesized section-boundary symbols (__got_start,
                // __rela_dyn_end, __tbss_align_abs, __EX_start, etc.) describe
                // the CURRENT module's own layout - they're self-referential,
                // not imports - but the minimal Switch toolchain often leaves
                // them marked SHN_UNDEF anyway despite carrying a correct
                // st_value. A nonzero value on an otherwise-"undefined"
                // symbol is a strong signal it's one of these, not a genuine
                // external import (those are left at value 0 with nothing to
                // point to), so trust it ahead of both the defined check and
                // cross-module export lookup.
                u64 synthetic = 0;
                bool is_synthetic = true;
                // These describe THIS module's own relocation sections - the
                // linker leaves them SHN_UNDEF/value-0 in the dynamic symbol
                // table expecting the loader to patch them in directly from
                // its own knowledge of where it placed .rela.dyn/.rela.plt,
                // rather than resolving them like a normal import. We already
                // parsed those bounds for our own use.
                if (sym.name == "__rela_dyn_start" || sym.name == "__rel_dyn_start") {
                    synthetic = d.rela_va;
                } else if (sym.name == "__rela_dyn_end" || sym.name == "__rel_dyn_end") {
                    synthetic = d.rela_va + d.rela_sz;
                } else if (sym.name == "__rela_plt_start" || sym.name == "__rel_plt_start") {
                    synthetic = d.jmprel_va;
                } else if (sym.name == "__rela_plt_end" || sym.name == "__rel_plt_end") {
                    synthetic = d.jmprel_va + d.jmprel_sz;
                } else {
                    is_synthetic = false;
                }
                if (is_synthetic && synthetic) {
                    mem.Write64(d.mod_base + r_offset, synthetic + addend);
                    ++applied;
                } else if (sym.defined || sym.value != 0) {
                    mem.Write64(d.mod_base + r_offset, d.mod_base + sym.value + addend);
                    ++applied;
                } else if (auto it = exports.find(sym.name); it != exports.end()) {
                    mem.Write64(d.mod_base + r_offset, it->second + addend);
                    ++applied;
                } else if (r_type == R_AARCH64_ABS64 && r_sym == 0) {
                    // STN_UNDEF ABS64: S is 0 by definition, so the result is
                    // the addend alone - a plain absolute constant, not a
                    // failed import. Never trap these.
                    mem.Write64(d.mod_base + r_offset, addend);
                    ++applied;
                } else {
                    ++unresolved;
                    if (unresolved <= 30) {
                        LOG_ERROR(Core_ARM, "recomp: unresolved GOT/PLT symbol '{}' for module base={:#x}",
                                  sym.name.empty() ? "<no name>" : sym.name, d.mod_base);
                    }
                    // An undefined *weak* symbol resolves to 0 by ABI, and that
                    // is what rtld leaves under dynarmic. Patching it to a
                    // callable stub instead breaks the `if (&weak) weak(...)`
                    // idiom: the null check passes and the guest calls a
                    // function that only returns 0. Smash guards
                    // nu::VirtualAllocHook/VirtualFreeHook that way at 5489
                    // sites on its allocator path, so every allocation took the
                    // hook branch and got nothing back.
                    // Strong symbols keep the trap sentinel.
                    const u64 stub =
                        sym.weak ? 0 : (d.trap_va ? d.trap_va : kUnresolvedImportTrap);
                    mem.Write64(d.mod_base + r_offset, stub);
                    // The trap counter cannot report this case - it is only
                    // written when a thread actually reaches kUnresolvedImportTrap,
                    // and FindReturnStub always succeeds, so the guest calls a
                    // real `mov x0,#0; ret` and nothing is ever counted. Record
                    // the slot address instead, so a later poll can say whether
                    // anything (nn::ro binding an NRO, say) ever wrote over it.
                    if (!kTrackSlotPrefix.empty() &&
                        sym.name.rfind(kTrackSlotPrefix, 0) == 0) {
                        std::scoped_lock lk{g_tracked_lock};
                        g_tracked_slots.push_back({d.mod_base + r_offset, stub, sym.name});
                    }
                }
            }
        }
    }

    // Pre-apply relocations for every loaded module. Under dynarmic, rtld
    // runs its own self-relocation loop correctly; under ArmRecomp the
    // recompiled loop exits early, leaving most relocations un-applied and
    // corrupting both data reads (R_AARCH64_RELATIVE, e.g. vtables, GOT
    // pointers to local data) and indirect calls through the GOT/PLT
    // (R_AARCH64_GLOB_DAT / R_AARCH64_JUMP_SLOT - unresolved, these are the
    // null/garbage function pointers that were previously observed crashing
    // rtld's module bootstrap). All module bases are already known by the
    // time this runs (the loader maps every NSO up front), so cross-module
    // symbol resolution just needs every module's exports indexed first.
    void ApplyAllRelocations(const std::map<u64, std::string>& all_modules) {
        auto& mem = system.ApplicationMemory();
        std::vector<DynInfo> dyns;
        std::unordered_map<std::string, u64> exports;
        for (const auto& [module_base, name] : all_modules) {
            DynInfo d;
            if (ParseDynamic(module_base, d)) {
                d.trap_va = FindReturnStub(module_base);
                d.name = name;
                IndexExports(d, exports);
                dyns.push_back(d);
            }
        }
        for (const auto& d : dyns) {
            u32 applied = 0, unresolved = 0;
            const bool is_rtld = d.name.find("rtld") != std::string::npos;
            if (kRelaPolicy == "rtld-only" && !is_rtld) {
                // Left for rtld. Touching neither the entries nor the sizes is
                // the whole point: rtld has to see this module exactly as it
                // would under dynarmic, including whatever it records about
                // which imports are still deferred, or nn::ro cannot bind them
                // when an NRO turns up later.
                LOG_INFO(Core_ARM, "recomp: leaving {} base={:#x} to rtld", d.name, d.mod_base);
                continue;
            }
            if (d.rela_va && d.rela_sz) {
                ApplyRelocTable(d, d.rela_va, d.rela_sz, d.rela_ent, exports, applied, unresolved);
            }
            if (d.jmprel_va && d.jmprel_sz) {
                ApplyRelocTable(d, d.jmprel_va, d.jmprel_sz, d.jmprel_ent, exports, applied,
                                 unresolved);
            }
            // Zero DT_RELASZ/DT_PLTRELSZ so rtld's own self-relocator sees
            // nothing left to do and skips both tables - it runs its own
            // GLOB_DAT/JUMP_SLOT resolution loop with a load-bias consistency
            // check that assumes it's relocating fresh, unresolved entries;
            // finding them already resolved by us trips that check and it
            // calls svcBreak, which is what was hanging every recompiled
            // game at boot despite relocations succeeding.
            //
            // That hang is rtld double-relocating *itself*: its hand-written
            // bootstrap loop runs before its C++ machinery and is not
            // idempotent. Every other module is relocated by the C++ pass,
            // which recomputes each slot from the rela entry rather than
            // accumulating into it, so re-running it over our results is a
            // no-op. Zeroing those modules too is what leaves nn::ro unable to
            // bind main's deferred imports when an NRO shows up later.
            if (kRelaPolicy == "zero-all" || kRelaPolicy == "restore-on-main" ||
                ((kRelaPolicy == "zero-rtld" || kRelaPolicy == "rtld-only") && is_rtld)) {
                if (d.rela_sz_va) mem.Write64(d.rela_sz_va, 0);
                if (d.jmprel_sz_va) mem.Write64(d.jmprel_sz_va, 0);
            }
            LOG_INFO(Core_ARM,
                     "recomp: pre-applied {} relocations ({} unresolved external symbols) for "
                     "module base={:#x}",
                     applied, unresolved, d.mod_base);
        }
        // Only the first Impl through here observes the real sizes; see
        // g_rela_restore.
        {
            std::scoped_lock lk{g_rela_restore_lock};
            if (g_rela_restore.empty()) {
                for (const auto& d : dyns) {
                    if (d.rela_sz_va && d.rela_sz) {
                        g_rela_restore.emplace_back(d.rela_sz_va, d.rela_sz);
                    }
                    if (d.jmprel_sz_va && d.jmprel_sz) {
                        g_rela_restore.emplace_back(d.jmprel_sz_va, d.jmprel_sz);
                    }
                    LOG_INFO(Core_ARM, "recomp: saved rela sizes for {} base={:#x} relasz={} pltrelsz={}",
                             d.name, d.mod_base, d.rela_sz, d.jmprel_sz);
                }
            }
        }
    }

    // Put DT_RELASZ/DT_PLTRELSZ back for every module. Only meaningful under
    // the restore-on-main policy; the trigger owns proving rtld is done.
    void RestoreRelocSizes() {
        auto& mem = system.ApplicationMemory();
        std::scoped_lock lk{g_rela_restore_lock};
        for (const auto& [va, size] : g_rela_restore) {
            mem.Write64(va, size);
        }
        LOG_INFO(Core_ARM, "recomp: restored {} rela size fields", g_rela_restore.size());
    }

    System& system;
    RecompLookupFn lookup{};
    GuestContextView ctx{};
    RecompHostMem bridge{};
    std::atomic<bool> interrupted{false};
    Loader::AppLoader::Modules modules{};
    bool modules_read{false};
    bool rela_applied{false};
    static constexpr size_t kTrail = 32;
    u64 trail[kTrail]{};
    size_t trail_pos{0};

    // Interpreter fallback for PCs the static pass never covered. Built on the
    // first miss rather than up front: most runs never need it, and a JIT per
    // core costs a code cache each.
    Kernel::KProcess* owner_process{};
    // The base interface, not dynarmic's implementation of it: the recompiled
    // path only ever calls the virtual methods, and holding the concrete type
    // here is what forced the library into a build that never runs a JIT.
    ExclusiveMonitor* exclusive_monitor{};
    std::size_t core_index{};
    bool uses_wall_clock{};
#ifndef SUYU_NO_JIT
    std::unique_ptr<ArmDynarmic64> fallback{};
#endif
    bool in_fallback{false};
    bool fallback_unavailable{false};
};

ArmRecomp::ArmRecomp(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                     Kernel::KProcess* process, ExclusiveMonitor* exclusive_monitor,
                     std::size_t core_index)
    : ArmInterface{uses_wall_clock}, impl{std::make_unique<Impl>(system, lookup)} {
    impl->owner_process = process;
    impl->exclusive_monitor = exclusive_monitor;
    impl->core_index = core_index;
    impl->uses_wall_clock = uses_wall_clock;
    // One instance is built per core; the last one torn down prints the run's
    // execution-coverage report.
    g_live_instances.fetch_add(1, std::memory_order_relaxed);
}

ArmRecomp::~ArmRecomp() {
    // Report on the *first* instance torn down, not the last. Waiting for the
    // last one means the report is lost whenever anything still holds a
    // reference at shutdown - which happens, and silently costs the whole run's
    // measurement. All per-core instances go down together, so the first is
    // just as complete.
    g_live_instances.fetch_sub(1, std::memory_order_acq_rel);
    static std::once_flag reported;
    std::call_once(reported, [] { ReportRecompCoverage(); });
}

bool ArmRecomp::EnterFallback() {
    if (impl->fallback_unavailable) {
        return false;
    }
    if (StrictNoFallback()) {
        // Latched, so the caller's own critical log naming the PC is what gets
        // read, and the second thread to arrive does not repeat this one.
        impl->fallback_unavailable = true;
        LOG_CRITICAL(Core_ARM, "recomp: strict mode - refusing to fall back to the JIT");
        return false;
    }
#ifdef SUYU_NO_JIT
    // Built without a dynamic recompiler at all. There is nothing to fall back
    // to, by construction rather than by configuration.
    impl->fallback_unavailable = true;
    LOG_CRITICAL(Core_ARM, "recomp: built without a JIT; uncovered code cannot run");
    return false;
#else
    if (!impl->fallback) {
        if (!impl->owner_process || !impl->exclusive_monitor) {
            impl->fallback_unavailable = true;
            return false;
        }
        impl->fallback = std::make_unique<ArmDynarmic64>(
            impl->system, impl->uses_wall_clock, impl->owner_process,
            static_cast<DynarmicExclusiveMonitor&>(*impl->exclusive_monitor), impl->core_index);
        LOG_WARNING(Core_ARM, "recomp: created JIT fallback for uncovered code");
    }
    impl->in_fallback = true;
    return true;
#endif
}

HaltReason ArmRecomp::RunFallback(Kernel::KThread* thread) {
#ifdef SUYU_NO_JIT
    // Unreachable: EnterFallback never succeeds in this build. Kept so the one
    // call site needs no guard of its own.
    (void)thread;
    return HaltReason::PrefetchAbort;
#else
    // The recompiled context is the single source of truth; the JIT is loaded
    // from it on the way in and drained back on the way out, so every accessor
    // on this interface (SVC arguments, thread context save/restore) keeps
    // working unchanged no matter which engine actually ran.
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;
    impl->interrupted.store(false, std::memory_order_relaxed);

    Kernel::Svc::ThreadContext tctx{};
    this->GetContext(tctx);
    impl->fallback->SetContext(tctx);
    impl->fallback->SetTpidrroEl0(impl->ctx.tpidrro_el0);

    const HaltReason hr = impl->fallback->RunThread(thread);

    impl->fallback->GetContext(tctx);
    this->SetContext(tctx);
    if (True(hr & HaltReason::SupervisorCall)) {
        impl->ctx.pending_svc = impl->fallback->GetSvcNumber();
    }

    if (True(hr & HaltReason::SupervisorCall)) {
        g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
    }

    // Return to recompiled execution as soon as the PC is covered again, so a
    // single uncovered function costs only the time spent inside it.
    if (impl->lookup && impl->lookup(impl->ctx.pc)) {
        impl->in_fallback = false;
        g_counters.jit_to_static.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
#endif
}

HaltReason ArmRecomp::RunThread(Kernel::KThread* thread) {
    // The monitor is keyed by core, not by guest thread, so a reservation left
    // behind by a preempted thread survives into whichever thread is scheduled
    // onto this core next. That next thread's STXR can then succeed against a
    // reservation it never took. nn::os releases a mutex with
    // ldaxr / test waiters / stlxr, so a stale success skips the wake branch
    // and the waiters are never signalled. Dynarmic clears here for the same
    // reason - see ArmDynarmic64::RunThread.
    if (impl->exclusive_monitor) {
        impl->exclusive_monitor->ClearExclusive(impl->core_index);
    }
    // Logged once so it is obvious from a log whether the backend was ever
    // entered at all. A run with no errors is otherwise indistinguishable from
    // a run where the guest thread was never scheduled onto it.
    static bool announced = false;
    if (!announced) {
        announced = true;
        LOG_INFO(Core_ARM, "ArmRecomp::RunThread entered, pc={:#x}", impl->ctx.pc);
    }
    // Kept so the coverage report can name what every guest thread is doing at
    // the end of a run. "The guest is stalled" is an observation about one
    // thread; which of the others exited, and which are parked in a wait that
    // will never be signalled, is the part that says why.
    if (g_report_process.load(std::memory_order_relaxed) == nullptr) {
        g_report_process.store(thread->GetOwnerProcess(), std::memory_order_relaxed);
        StartStateWatcher(thread->GetOwnerProcess());
    }
    if (!impl->lookup) {
        LOG_ERROR(Core_ARM, "No recompiled code registered; cannot run thread");
        return HaltReason::BreakLoop;
    }

    impl->RefreshPageTable();

    if (!impl->rela_applied) {
        // Every CPU Impl needs its own complete module map and relocation pass.
        // Collapsing this to once per KProcess left later Impls incompletely
        // initialized and changed MK8's execution despite zero lookup misses.
        // Serialize the shared guest-memory writes to retain the original
        // per-Impl behavior without the original multicore race.
        std::scoped_lock lock{g_process_init_lock};
        if (!impl->rela_applied) {
            impl->ModuleBaseFor(thread, impl->ctx.pc);
            impl->ApplyAllRelocations(impl->modules);
            impl->rela_applied = true;
            StartSlotWatcher();
        }
    }

    if (kRelaPolicy == "restore-on-main" && !g_rela_restored.load(std::memory_order_acquire) &&
        impl->PcInMainModule(impl->ctx.pc)) {
        // Ordering this relies on: rtld is the process entrypoint, and the NSO
        // boot sequence has it self-relocate and then relocate every other
        // module before it branches to main's entry. So the first guest PC
        // inside main is after rtld is finished with both tables, and long
        // before nn::ro loads any NRO (Smash's first LoadNro is ~33s in).
        std::scoped_lock lock{g_process_init_lock};
        if (!g_rela_restored.exchange(true, std::memory_order_acq_rel)) {
            LOG_INFO(Core_ARM, "recomp: first execution in main at pc={:#x}; restoring rela sizes",
                     impl->ctx.pc);
            impl->RestoreRelocSizes();
        }
    }

    // A previous miss handed this thread to the JIT; keep running there until
    // the PC lands back inside recompiled code. The trap sentinel has to be
    // caught before that hand-off as well as inside the dispatch loop below -
    // a thread already in the JIT that calls an unresolved import would
    // otherwise be handed the sentinel address to execute, which is unmapped.
    if (impl->ctx.pc == kUnresolvedImportTrap) {
        impl->ctx.x[0] = 0;
        impl->ctx.pc = impl->ctx.x[30];
        impl->in_fallback = false;
    }
    if (impl->in_fallback) {
        return RunFallback(thread);
    }

    impl->interrupted.store(false, std::memory_order_relaxed);
    impl->ctx.halted = 0;

    while (!impl->ctx.halted) {
        if (impl->interrupted.load(std::memory_order_relaxed)) {
            return HaltReason::BreakLoop;
        }

        // An SVC parked us last time round; the kernel has now serviced it and
        // resumed, so clear it before continuing.
        if (impl->ctx.pending_svc != kNoPendingSvc) {
            impl->ctx.pending_svc = kNoPendingSvc;
        }

        // A recompiled image is keyed by each block's offset within its own
        // module, because that is all the static pass can know: an NSO's
        // segment header carries the offset inside the module, not the address
        // the loader will map it to, and that address changes per run anyway.
        // The host-side dispatcher (suyu's chained lookup) owns picking which
        // image the PC belongs to and reducing to that image's offset before
        // calling into it - a second offset-based retry here used to guess
        // which image based only on pc-base, but two images can both define a
        // block at the same offset (every module has one at offset 0), so a
        // guess made without knowing which image owns the address silently
        // ran the wrong module's code with no error. Ask with the absolute PC
        // and let the dispatcher own the reduction.
        // Rolling trail of the last few PCs. A wild indirect branch reports
        // only the address it landed on, which says nothing about which block
        // computed it; without the predecessors there is no way to tell a bad
        // GOT read from a bad emitted branch.
        impl->trail[impl->trail_pos++ & (Impl::kTrail - 1)] = impl->ctx.pc;

        if (impl->ctx.pc == kUnresolvedImportTrap) {
            // Landed here via a BLR through a GOT/JUMP_SLOT slot patched by
            // ApplyRelocTable because no definition was found anywhere - most
            // such imports exist for code paths this particular boot never
            // takes, so treat the call as a no-op: return to the caller with
            // a zeroed result register rather than crash the thread. x30 is
            // the guest's own return address (BLR sets it before the branch),
            // exactly as if this were a real function that did nothing.
            static std::atomic<int> trap_count{0};
            if (trap_count.fetch_add(1, std::memory_order_relaxed) < 16) {
                g_counters.unresolved_import_traps.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR(Core_ARM, "recomp: called through unresolved import (returning to caller {:#x})",
                          impl->ctx.x[30]);
            }
            impl->ctx.x[0] = 0;
            impl->ctx.pc = impl->ctx.x[30];
            continue;
        }

        RecompBlockFn block = impl->lookup(impl->ctx.pc);
        // Test hook: forces every lookup past the Nth to miss, so the JIT
        // fallback below can be exercised on a title that would otherwise never
        // hit a gap. Unset in normal runs.
        {
            static const char* const force_miss = std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER");
            static const char* const force_miss_static_blocks =
                std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER_STATIC_BLOCKS");
            static std::atomic<int> blocks_run{0};
            if (force_miss) {
                const int n = blocks_run.fetch_add(1, std::memory_order_relaxed);
                const int limit = std::atoi(force_miss);
                // Name the blocks either side of the cutoff. Bisecting on the
                // cutoff tells you which index first breaks the run; this turns
                // that index into the actual guest address to look at.
                if (n >= limit - 4 && n <= limit + 4) {
                    LOG_ERROR(Core_ARM, "recomp: block #{} pc={:#x}", n, impl->ctx.pc);
                }
                if (n >= limit) {
                    block = nullptr;
                }
            }
            // Same hook keyed on a module-relative guest PC. Block counts and
            // wall clocks both bracket a fault loosely; this hands over at one
            // named instruction, which is what turns a bracket into a bisect.
            // Module bases are picked by the loader and differ per run, so the
            // offset is matched against every loaded base rather than an
            // absolute address the caller could not know in advance.
            static const char* const force_miss_at_offset =
                std::getenv("SUYU_RECOMP_FORCE_MISS_AT_MODULE_OFFSET");
            if (force_miss_at_offset) {
                static const u64 target = std::strtoull(force_miss_at_offset, nullptr, 0);
                // Snapshotted rather than read under the counters' lock on every
                // block, which would cost more than the whole dispatch loop.
                // Retried while empty because the first guest blocks can run
                // before the loader has finished registering module bases.
                static std::vector<u64> bases;
                if (bases.empty()) {
                    std::scoped_lock lk{g_counters.hist_lock};
                    for (const auto& [base, name] : g_counters.modules) {
                        bases.push_back(base);
                    }
                }
                for (const u64 base : bases) {
                    if (impl->ctx.pc - base == target) {
                        u64 unset = 0;
                        if (g_forced_cutoff_pc.compare_exchange_strong(
                                unset, impl->ctx.pc, std::memory_order_relaxed)) {
                            g_forced_cutoff_blocks.store(TotalStaticBlocks(),
                                                         std::memory_order_relaxed);
                            LOG_ERROR(Core_ARM,
                                      "recomp: forced offset cutoff {:#x} reached pc={:#x} "
                                      "after {} blocks",
                                      target, impl->ctx.pc,
                                      g_forced_cutoff_blocks.load(std::memory_order_relaxed));
                        }
                        block = nullptr;
                        break;
                    }
                }
            }
            // Same hook keyed on wall time instead of block count. A stalled
            // title stops retiring blocks, so a block-count cutoff placed
            // inside the stall is never reached; seconds get there regardless.
            static const char* const force_miss_after_sec =
                std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER_SEC");
            if (force_miss_after_sec) {
                const double limit = std::atof(force_miss_after_sec);
                const double elapsed = std::chrono::duration<double>(
                                           std::chrono::steady_clock::now() - kRecompStart)
                                           .count();
                if (elapsed >= limit) {
                    u64 unset = 0;
                    if (g_forced_cutoff_pc.compare_exchange_strong(
                            unset, impl->ctx.pc, std::memory_order_relaxed)) {
                        g_forced_cutoff_blocks.store(TotalStaticBlocks(),
                                                     std::memory_order_relaxed);
                        LOG_ERROR(Core_ARM, "recomp: forced time cutoff {}s reached pc={:#x}",
                                  limit, impl->ctx.pc);
                    }
                    block = nullptr;
                }
            }
            if (force_miss_static_blocks) {
                const u64 limit = std::strtoull(force_miss_static_blocks, nullptr, 10);
                const u64 executed = TotalStaticBlocks();
                if (executed >= limit) {
                    u64 unset = 0;
                    if (g_forced_cutoff_pc.compare_exchange_strong(
                            unset, impl->ctx.pc, std::memory_order_relaxed)) {
                        g_forced_cutoff_blocks.store(executed, std::memory_order_relaxed);
                    }
                    static std::atomic<int> static_cutoff_logs{0};
                    if (static_cutoff_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
                        LOG_ERROR(Core_ARM,
                                  "recomp: forced static-block cutoff {} reached at {} pc={:#x}",
                                  limit, executed, impl->ctx.pc);
                    }
                    block = nullptr;
                }
            }
        }
        // A miss is now recoverable, so it can happen many times per second;
        // the full diagnostic dump is kept for the first few only, where it is
        // still useful for finding which indirect call went uncovered.
        static std::atomic<int> miss_count{0};
        const int miss_index = block ? 0 : miss_count.fetch_add(1, std::memory_order_relaxed);
        if (!block && miss_index < 8) {
            std::string trail;
            const size_t count = std::min<size_t>(impl->trail_pos, Impl::kTrail);
            for (size_t i = 0; i < count; ++i) {
                const u64 p = impl->trail[(impl->trail_pos - count + i) & (Impl::kTrail - 1)];
                trail += fmt::format("{:#x} ", p);
            }
            LOG_ERROR(Core_ARM, "recomp PC trail (oldest first): {}", trail);
            LOG_ERROR(Core_ARM, "recomp regs x16={:#x} x17={:#x} x30={:#x} sp={:#x}",
                      impl->ctx.x[16], impl->ctx.x[17], impl->ctx.x[30], impl->ctx.x[31]);
            LOG_ERROR(Core_ARM, "recomp regs x0={:#x} x15={:#x} x18={:#x} x19={:#x}",
                      impl->ctx.x[0], impl->ctx.x[15], impl->ctx.x[18], impl->ctx.x[19]);
            // The whole file, four per line. A miss is almost always a bad value
            // in some register the previous block computed, and guessing which
            // one to print in advance costs a rebuild per guess.
            for (size_t r = 0; r < 32; r += 4) {
                LOG_ERROR(Core_ARM, "recomp regs x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x}",
                          r, impl->ctx.x[r], r + 1, impl->ctx.x[r + 1], r + 2, impl->ctx.x[r + 2],
                          r + 3, impl->ctx.x[r + 3]);
            }
            // Dump guest memory around the registers that look like pointers.
            // A miss caused by a bad *value* and one caused by the wrong data
            // being mapped at the right address look identical from the
            // register file alone.
            for (u32 r : {8u, 22u, 25u}) {
                const u64 p = impl->ctx.x[r];
                if (p < 0x1000 || p > 0x0000'FFFF'FFFF'FFFFULL) {
                    continue;
                }
                std::string dump;
                for (s64 d = -0x20; d < 0x30; d += 4) {
                    dump += fmt::format("{:08x} ", (u32)Impl::HostLoad(impl.get(), p + d, 4));
                }
                LOG_ERROR(Core_ARM, "recomp mem @x{} ({:#x}) [-0x20..+0x30): {}", r, p, dump);
            }
            {
                const u64 mbase = impl->modules.empty() ? 0 : impl->modules.begin()->first;
                // A wide window through .rodata, to diff against the exporter's
                // extracted copy: if the two disagree, the recompiled code is
                // computing correct addresses into memory that holds something
                // other than what was recompiled against.
                for (u64 w = 0x3c00; w < 0x3d80; w += 0x40) {
                    std::string dump;
                    for (u64 i = 0; i < 0x40; i += 4) {
                        dump += fmt::format("{:08x} ", (u32)Impl::HostLoad(impl.get(), mbase + w + i, 4));
                    }
                    LOG_ERROR(Core_ARM, "recomp rodata mod+{:#x}: {}", w, dump);
                }
                for (u64 seg : {0x0ULL, 0x2000ULL, 0x3000ULL}) {
                    std::string dump;
                    for (u64 i = 0; i < 0x40; i += 4) {
                        dump += fmt::format("{:08x} ", Impl::HostLoad(impl.get(), mbase + seg + i, 4));
                    }
                    LOG_ERROR(Core_ARM, "recomp mem mod+{:#x} (base {:#x}): {}", seg, mbase, dump);
                }
            }
        }
        if (!block) {
            // No recompiled block covers this address: an indirect branch into
            // code the static pass never reached. The guest's own instructions
            // are still mapped in guest memory, so hand the thread to a JIT and
            // keep going instead of returning PrefetchAbort - that halt reason
            // makes the kernel suspend the thread for a debugger that is not
            // attached, which is a permanent, silent black-screen hang.
            if (miss_index < 64) {
                LOG_ERROR(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            } else {
                LOG_DEBUG(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            }
            g_counters.fallback_from_miss.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordMiss(impl->ctx.pc);
            if (!EnterFallback()) {
                g_counters.no_fallback_available.fetch_add(1, std::memory_order_relaxed);
                LOG_CRITICAL(Core_ARM,
                             "recomp: no JIT fallback available at PC {:#x}; thread cannot "
                             "continue",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        // Dump periodically: teardown is not guaranteed to run (the emulated
        // process can outlive shutdown), and a run with no report is a run with
        // no measurement. One compare per block against a power-of-two mask.
        const u64 seen = t_blocks.slot->n.load(std::memory_order_relaxed) + 1;
        t_blocks.slot->n.store(seen, std::memory_order_relaxed);
        if ((seen & 0x3FFFFULL) == 0x3FFFFULL) {
            WriteRecompCoverageFile(FormatRecompCoverage());
        }
        // Generated code calls a direct branch's target itself rather than
        // coming back here, so one call below can run a whole chain of blocks.
        // The budget bounds that chain, and what is left of it afterwards says
        // how many blocks actually ran - without which every count here would
        // report chains rather than blocks.
        const int chain_budget = RecompChainBudget();
        impl->ctx.chain_budget = chain_budget;
        block(&impl->ctx);
        {
            const int spent = chain_budget - impl->ctx.chain_budget;
            u64 after = seen;
            if (spent > 0) {
                // The first block is already in `seen`. A decrement to zero
                // parks the next PC without entering it, so that final attempt
                // contributes no executed block.
                after += static_cast<u64>(spent - (impl->ctx.chain_budget == 0 ? 1 : 0));
                t_blocks.slot->n.store(after, std::memory_order_relaxed);
            }
            if (kSamplePc && (seen >> kSamplePcShift) != (after >> kSamplePcShift) &&
                SampleWindowOpen()) {
                g_counters.RecordSample(impl->ctx.pc);
            }
        }

        if (impl->ctx.halted == kHaltIcIvau) {
            if (!g_code_guard_ready.load(std::memory_order_acquire)) {
                LOG_CRITICAL(Core_ARM, "recomp: IC IVAU requires guard-v2 host and all guarded modules");
                std::abort();
            }
            const u64 address = impl->ctx.pending_svc;
            impl->ctx.pending_svc = kNoPendingSvc;
            for (size_t core = 0; core < Hardware::NUM_CPU_CORES; ++core) {
                if (auto* cpu = thread->GetOwnerProcess()->GetArmInterface(core)) cpu->InvalidateCacheRange(address, 64);
            }
            impl->ctx.pc += 4;
            impl->ctx.halted = 0;
            continue;
        }
        if (impl->ctx.halted == kHaltBreakpoint) {
            return HaltReason::InstructionBreakpoint;
        }

        // The block stopped on an instruction the decoder has no translation
        // for, having parked the PC on that instruction. Running it on the JIT
        // instead keeps guest state exact: the alternative the generated code
        // used to take - step over it and zero x0 - silently produced a
        // plausible-looking null that only surfaced as a crash much later, in
        // whatever code eventually dereferenced it.
        if (impl->ctx.halted == kHaltUnhandled) {
            if ((static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4)) & 0xffffffe0U) == 0xd50b7520U) {
                LOG_CRITICAL(Core_ARM, "recomp: legacy unguarded IC IVAU refused at {:#x}", impl->ctx.pc);
                std::abort();
            }
            impl->ctx.halted = 0;
            // The generated code knows the encoding but cannot pass it back
            // through the halt contract, so read it out of guest memory - the
            // PC is parked exactly on the offending instruction. This is what
            // ranks the missing opcodes by execution rather than by how often
            // they appear in the image.
            g_counters.fallback_from_unhandled.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordUnhandled(
                static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4)));
            static std::atomic<int> unhandled_count{0};
            if (unhandled_count.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(Core_ARM, "recomp: unimplemented opcode at {:#x}; running on JIT",
                            impl->ctx.pc);
            }
            if (!EnterFallback()) {
                g_counters.no_fallback_available.fetch_add(1, std::memory_order_relaxed);
                LOG_CRITICAL(Core_ARM, "recomp: unimplemented opcode at {:#x} and no JIT fallback",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        if (impl->ctx.pending_svc != kNoPendingSvc) {
            // Log every SVC call from rtld (first few hundred only to avoid spam)
            LOG_TRACE(Core_ARM, "recomp SVC {} at pc={:#x} x0={:#x} x1={:#x} x2={:#x} x3={:#x}",
                      impl->ctx.pending_svc, impl->ctx.pc, impl->ctx.x[0], impl->ctx.x[1],
                      impl->ctx.x[2], impl->ctx.x[3]);
            g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordSvc(static_cast<u32>(impl->ctx.pending_svc));
            return HaltReason::SupervisorCall;
        }
    }

    return HaltReason::BreakLoop;
}

HaltReason ArmRecomp::StepThread(Kernel::KThread* thread) {
    // Same reservation hand-off as RunThread.
    if (impl->exclusive_monitor) {
        impl->exclusive_monitor->ClearExclusive(impl->core_index);
    }
    // Block granularity is the finest this backend can step: recompiled blocks
    // are straight-line C with no per-instruction re-entry point.
    if (!impl->lookup) {
        return HaltReason::BreakLoop;
    }
    const RecompBlockFn block = impl->lookup(impl->ctx.pc);
    if (!block) {
        return HaltReason::PrefetchAbort;
    }
    impl->ctx.halted = 0;
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.chain_budget = 1;
    block(&impl->ctx);
    if (impl->ctx.halted == kHaltIcIvau) {
        if (!g_code_guard_ready.load(std::memory_order_acquire)) {
            LOG_CRITICAL(Core_ARM, "recomp: IC IVAU requires guard-v2 host and all guarded modules");
            std::abort();
        }
        const u64 address = impl->ctx.pending_svc;
        impl->ctx.pending_svc = kNoPendingSvc;
        for (size_t core = 0; core < Hardware::NUM_CPU_CORES; ++core) {
            if (auto* cpu = thread->GetOwnerProcess()->GetArmInterface(core)) cpu->InvalidateCacheRange(address, 64);
        }
        impl->ctx.pc += 4;
        impl->ctx.halted = 0;
        return HaltReason::StepThread;
    }
    if (impl->ctx.halted == kHaltBreakpoint) {
        return HaltReason::InstructionBreakpoint;
    }
    if (impl->ctx.halted == kHaltUnhandled &&
        (static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4)) & 0xffffffe0U) == 0xd50b7520U) {
        LOG_CRITICAL(Core_ARM, "recomp: legacy unguarded IC IVAU refused while stepping at {:#x}", impl->ctx.pc);
        std::abort();
    }
    if (impl->ctx.pending_svc != kNoPendingSvc) {
        return HaltReason::SupervisorCall;
    }
    return HaltReason::StepThread;
}

void ArmRecomp::ClearInstructionCache() {
#ifndef SUYU_NO_JIT
    if (impl->fallback) impl->fallback->ClearInstructionCache();
#endif
    // Compiled blocks validate their bytes on every entry, including chains.
}

void ArmRecomp::InvalidateCacheRange(u64 addr, std::size_t size) {
#ifndef SUYU_NO_JIT
    if (impl->fallback) impl->fallback->InvalidateCacheRange(addr, size);
#endif
}

void ArmRecomp::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    std::memset(&ctx, 0, sizeof(ctx));
    for (size_t i = 0; i < 29; ++i) {
        ctx.r[i] = impl->ctx.x[i];
    }
    ctx.fp = impl->ctx.x[29];
    ctx.lr = impl->ctx.x[30];
    ctx.sp = impl->ctx.x[31];
    ctx.pc = impl->ctx.pc;
    ctx.pstate = (static_cast<u32>(impl->ctx.n) << 31) |
                 (static_cast<u32>(impl->ctx.z) << 30) |
                 (static_cast<u32>(impl->ctx.c) << 29) |
                 (static_cast<u32>(impl->ctx.v) << 28);
    // u128 here is a pair of 64-bit halves, matching how the generated
    // context stores each vector register.
    for (size_t i = 0; i < 32; ++i) {
        ctx.v[i][0] = impl->ctx.vreg[i][0];
        ctx.v[i][1] = impl->ctx.vreg[i][1];
    }
    ctx.fpcr = static_cast<u32>(impl->ctx.fpcr);
    ctx.fpsr = static_cast<u32>(impl->ctx.fpsr);
    ctx.tpidr = impl->ctx.tpidr_el0;
}

void ArmRecomp::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; ++i) {
        impl->ctx.x[i] = ctx.r[i];
    }
    impl->ctx.x[29] = ctx.fp;
    impl->ctx.x[30] = ctx.lr;
    impl->ctx.x[31] = ctx.sp;
    impl->ctx.pc = ctx.pc;
    impl->ctx.n = static_cast<u8>((ctx.pstate >> 31) & 1);
    impl->ctx.z = static_cast<u8>((ctx.pstate >> 30) & 1);
    impl->ctx.c = static_cast<u8>((ctx.pstate >> 29) & 1);
    impl->ctx.v = static_cast<u8>((ctx.pstate >> 28) & 1);
    for (size_t i = 0; i < 32; ++i) {
        impl->ctx.vreg[i][0] = ctx.v[i][0];
        impl->ctx.vreg[i][1] = ctx.v[i][1];
    }
    impl->ctx.fpcr = ctx.fpcr;
    impl->ctx.fpsr = ctx.fpsr;
    // Only the guest-owned thread pointer travels in ThreadContext. The
    // read-only one is republished separately by PhysicalCore::LoadContext
    // on every switch-in, so writing it from here would overwrite the
    // kernel's TLS pointer with the guest's.
    impl->ctx.tpidr_el0 = ctx.tpidr;
}

void ArmRecomp::SetTpidrroEl0(u64 value) {
    // The emitted MRS handler for TPIDRRO_EL0 reads this out of the guest
    // context, so it has to land there and nowhere else: writing it into
    // tpidr_el0 (as this used to) destroyed the guest's own thread pointer on
    // every context switch and made the guest emit its IPC header outside the
    // TLS region the kernel parses it from.
    impl->ctx.tpidrro_el0 = value;
}

void ArmRecomp::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; ++i) {
        args[i] = impl->ctx.x[i];
    }
}

void ArmRecomp::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; ++i) {
        impl->ctx.x[i] = args[i];
    }
}

u32 ArmRecomp::GetSvcNumber() const {
    return static_cast<u32>(impl->ctx.pending_svc);
}

void ArmRecomp::SignalInterrupt(Kernel::KThread* thread) {
    impl->interrupted.store(true, std::memory_order_relaxed);
    // While the JIT is running this thread it is the one that has to be woken;
    // the flag above is only read by the recompiled dispatch loop.
#ifndef SUYU_NO_JIT
    if (impl->fallback) {
        impl->fallback->SignalInterrupt(thread);
    }
#else
    (void)thread;
#endif
}

const Kernel::DebugWatchpoint* ArmRecomp::HaltedWatchpoint() const {
    return nullptr;
}

void ArmRecomp::RewindBreakpointInstruction() {
    // No breakpoint patching in statically recompiled code.
}

} // namespace Core
