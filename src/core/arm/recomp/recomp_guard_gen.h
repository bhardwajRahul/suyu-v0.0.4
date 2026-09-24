// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

// Host side of the ABI 6 generation code guard (GG1).
//
// A GG1 block skips its instruction-word check while its seen word equals its
// module's generation word. This owns those generation words. It moves them
// forward after every event that can change a guarded instruction word or its
// mapping, and holds a module at kVerifyAlways whenever it cannot prove that
// module's code stable. The analysis of which events those are is
// diagnostics/suyu-guard-generation-20260923/DESIGN.md; each hook below names
// its caller.
//
// Deliberately free of emulator types, so tests/recompiler_smoke can drive it
// directly against a generated module.
namespace Core::RecompGuardGen {

/// Never stored into a block's seen word, so a module holding it verifies on
/// every entry, exactly as ABI 5 does.
inline constexpr std::uint32_t kVerifyAlways = 0xFFFFFFFFu;
/// Passed to recomp_image_guard_gen_v1.
inline constexpr std::uint32_t kHostVersion = 1;
/// Live mappings remembered (for permissions and the alias check) before
/// everything goes sticky.
inline constexpr std::size_t kMaxMapLog = 16384;

/// What recomp_image_guard_gen_v1 hands the host.
struct Module {
    std::uint32_t* word;       ///< the module's generation word
    const std::uint64_t* base; ///< the module's load base (g_module_base)
    std::uint64_t code_lo;     ///< module-relative span of guarded words
    std::uint64_t code_end;
};

enum class Reason : unsigned {
    Activate,
    Map,
    Unmap,
    Protect,
    DeviceMap,
    Invalidate,
    InvalidateAll,
    PageTableSwap,
    Count,
};

struct Stats {
    bool enabled;
    bool active;
    std::uint32_t generation;
    std::uint32_t modules;
    std::uint32_t sticky;
    std::uint64_t map_log;
    bool map_log_overflow;
    std::uint64_t bumps[static_cast<unsigned>(Reason::Count)];
};

namespace detail {
extern std::atomic<bool> g_watching;
}

/// Cheap test for the hooks: false unless GG1 modules are registered and enabled.
inline bool Watching() {
    return detail::g_watching.load(std::memory_order_seq_cst);
}

/// Loader, after SetRecompLookup and before the process is created. Every word
/// is set to kVerifyAlways. With enabled false (no guard-v2, or
/// SUYU_RECOMP_GUARD_GEN=0) it stays there and nothing else happens.
void SetModules(std::vector<Module> modules, bool enabled);
/// Drops the modules without touching them: their images may be unloaded.
void Forget();

/// True once Activate has completed for this process key. Acquire, so a core
/// that sees it also sees the generation words that activation stored.
bool IsActive(std::uint64_t key);
/// First run of a process: decide which modules are provably stable, then move
/// the generation. `table` identifies the process page table the hooks report
/// (Common::PageTable::entries.data()). Everything is decided from what the
/// hooks recorded since that table was created: a module is stable only if
/// its whole guarded span is mapped in `table` without write permission and
/// no other live mapping, in any table, shares its physical pages. Takes no
/// lock but its own, so it is safe inside ArmInterface::RunThread.
void Activate(std::uint64_t key, const void* table);

// Hooks. `table` is the page table being changed, same identity as above.
/// Core::Memory::MapMemoryRegion, any process.
void OnMap(const void* table, std::uint64_t va, std::uint64_t size, std::uint64_t pa,
           bool writable);
/// Core::Memory::UnmapRegion, any process.
void OnUnmap(const void* table, std::uint64_t va, std::uint64_t size);
/// Core::Memory::ProtectRegion, any process.
void OnProtect(const void* table, std::uint64_t va, std::uint64_t size, bool writable);
/// DeviceMemoryManager::Map of a process VA range.
void OnDeviceMap(const void* table, std::uint64_t va, std::uint64_t size);
/// Instruction-cache invalidation of a range (IC IVAU, kernel, gdbstub, cheats).
void OnInvalidate(std::uint64_t va, std::uint64_t size);
/// Whole instruction-cache invalidation.
void OnInvalidateAll();
/// Core::Memory::SetCurrentPageTable: a process and its table were created.
void OnPageTableSwap(const void* table);

Stats GetStats();

} // namespace Core::RecompGuardGen
