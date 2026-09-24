// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/arm/recomp/recomp_guard_gen.h"

#include <algorithm>
#include <mutex>

namespace Core::RecompGuardGen {

namespace detail {
std::atomic<bool> g_watching{false};
}

namespace {

// The generated code reads and writes these words with 32/64-bit atomics of its
// own; the host goes through std::atomic views of the same objects.
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t));
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

struct ModuleState {
    Module m;
    bool sticky = false;
    std::uint64_t active_base = 0; // base at activation
    Runs pa_runs;                  // [pa, pa + size) backing the guarded range
};

struct MapRecord {
    const void* table;
    std::uint64_t va, size, pa;
};

struct State {
    std::mutex lock;
    std::vector<ModuleState> modules;
    bool enabled = false;
    bool active = false;
    const void* active_table = nullptr;
    std::atomic<std::uint64_t> active_key{0};
    // Monotonic for the host process lifetime; 0 is never used, so a seen word
    // still at its zero initialiser never matches.
    std::uint32_t counter = 0;
    bool exhausted = false;
    std::uint64_t seq = 0; // every hook moves it; activation retries if it moved
    std::vector<MapRecord> map_log;
    bool map_log_overflow = false;
    std::uint64_t bumps[static_cast<unsigned>(Reason::Count)]{};
};

State& S() {
    static State state;
    return state;
}

std::uint64_t LoadBase(const Module& m) {
    return reinterpret_cast<const std::atomic<std::uint64_t>*>(m.base)->load(
        std::memory_order_relaxed);
}

void StoreWord(const Module& m, std::uint32_t value) {
    reinterpret_cast<std::atomic<std::uint32_t>*>(m.word)->store(value, std::memory_order_release);
}

bool Overlaps(std::uint64_t a, std::uint64_t a_size, std::uint64_t b, std::uint64_t b_size) {
    return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

bool OverlapsModuleVa(const ModuleState& s, std::uint64_t va, std::uint64_t size) {
    const std::uint64_t base = LoadBase(s.m);
    return Overlaps(va, size, base + s.m.code_lo, s.m.code_end - s.m.code_lo);
}

bool OverlapsModulePa(const ModuleState& s, std::uint64_t pa, std::uint64_t size) {
    return std::any_of(s.pa_runs.begin(), s.pa_runs.end(), [&](const auto& run) {
        return Overlaps(pa, size, run.first, run.second);
    });
}

// Caller holds the lock. Stores the new generation into every module word, or
// kVerifyAlways for a module that is not provably stable. Serialised by the
// lock, so each word's modification order follows bump order.
void Bump(State& s, Reason reason) {
    ++s.bumps[static_cast<unsigned>(reason)];
    if (!s.enabled) {
        return;
    }
    if (s.counter >= kVerifyAlways - 1) {
        s.exhausted = true;
    } else {
        ++s.counter;
    }
    for (auto& module : s.modules) {
        if (s.active && LoadBase(module.m) != module.active_base) {
            module.sticky = true; // rebased since the probe
        }
        const bool verify_always = !s.active || s.exhausted || module.sticky;
        StoreWord(module.m, verify_always ? kVerifyAlways : s.counter);
    }
}

// Caller holds the lock. Makes every module `hit` selects sticky and, if there
// was one, bumps so that its word drops to kVerifyAlways now.
template <typename Hit>
void StickyWhere(State& s, Reason reason, Hit&& hit) {
    bool any = false;
    for (auto& module : s.modules) {
        if (hit(module)) {
            module.sticky = true;
            any = true;
        }
    }
    if (any) {
        Bump(s, reason);
    }
}

void TrimLog(State& s, const void* table, std::uint64_t va, std::uint64_t size) {
    std::vector<MapRecord> kept;
    kept.reserve(s.map_log.size());
    const std::uint64_t end = va + size;
    for (const auto& rec : s.map_log) {
        if (rec.table != table || !Overlaps(rec.va, rec.size, va, size)) {
            kept.push_back(rec);
            continue;
        }
        const std::uint64_t rec_end = rec.va + rec.size;
        if (rec.va < va) {
            kept.push_back({rec.table, rec.va, va - rec.va, rec.pa});
        }
        if (end < rec_end) {
            kept.push_back({rec.table, end, rec_end - end, rec.pa + (end - rec.va)});
        }
    }
    s.map_log.swap(kept);
}

} // namespace

void SetModules(std::vector<Module> modules, bool enabled) {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    s.modules.clear();
    for (const auto& m : modules) {
        StoreWord(m, kVerifyAlways);
        s.modules.push_back(ModuleState{m});
    }
    s.enabled = enabled && !s.modules.empty();
    s.active = false;
    s.active_table = nullptr;
    s.active_key.store(0, std::memory_order_release);
    s.map_log.clear();
    s.map_log_overflow = false;
    detail::g_watching.store(s.enabled, std::memory_order_seq_cst);
}

void Forget() {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    detail::g_watching.store(false, std::memory_order_seq_cst);
    s.modules.clear();
    s.enabled = false;
    s.active = false;
    s.active_table = nullptr;
    s.active_key.store(0, std::memory_order_release);
    s.map_log.clear();
    s.map_log_overflow = false;
}

bool IsActive(std::uint64_t key) {
    return S().active_key.load(std::memory_order_acquire) == key;
}

void Activate(std::uint64_t key, const void* table, const ProbeFn& probe) {
    auto& s = S();
    constexpr int kAttempts = 3;
    for (int attempt = 0;; ++attempt) {
        std::uint64_t seq_before = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        {
            std::scoped_lock lk{s.lock};
            if (!s.enabled || s.active_key.load(std::memory_order_relaxed) == key) {
                return;
            }
            // Until this completes, every module verifies on every entry.
            s.active = false;
            seq_before = s.seq;
            for (const auto& module : s.modules) {
                StoreWord(module.m, kVerifyAlways);
                ranges.emplace_back(LoadBase(module.m) + module.m.code_lo,
                                    module.m.code_end - module.m.code_lo);
            }
        }
        // The probe takes the kernel's page-table lock, which the hooks run
        // under, so it must run without ours.
        std::vector<bool> stable(ranges.size());
        std::vector<Runs> runs(ranges.size());
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            stable[i] = ranges[i].second != 0 && probe(ranges[i].first, ranges[i].second, runs[i]);
        }
        std::scoped_lock lk{s.lock};
        if (!s.enabled || s.active_key.load(std::memory_order_relaxed) == key ||
            ranges.size() != s.modules.size()) {
            return;
        }
        const bool contended = s.seq != seq_before;
        if (contended && attempt + 1 < kAttempts) {
            continue; // something was mapped meanwhile; probe again
        }
        for (std::size_t i = 0; i < s.modules.size(); ++i) {
            auto& module = s.modules[i];
            module.active_base = ranges[i].first - module.m.code_lo;
            module.pa_runs = std::move(runs[i]);
            module.sticky = contended || s.map_log_overflow || !stable[i];
            if (module.sticky) {
                continue;
            }
            // Any other live mapping of this module's physical pages, in any
            // table, is an alias through which its code could change.
            for (const auto& rec : s.map_log) {
                const bool own = rec.table == table &&
                                 Overlaps(rec.va, rec.size, ranges[i].first, ranges[i].second);
                if (!own && OverlapsModulePa(module, rec.pa, rec.size)) {
                    module.sticky = true;
                    break;
                }
            }
        }
        s.active = true;
        s.active_table = table;
        Bump(s, Reason::Activate);
        s.active_key.store(key, std::memory_order_release);
        return;
    }
}

void OnMap(const void* table, std::uint64_t va, std::uint64_t size, std::uint64_t pa,
           bool writable) {
    (void)writable; // any new mapping over a module is already sticky
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    ++s.seq;
    if (s.map_log.size() < kMaxMapLog) {
        s.map_log.push_back({table, va, size, pa});
    } else {
        s.map_log_overflow = true;
    }
    if (!s.active) {
        return;
    }
    StickyWhere(s, Reason::Map, [&](const ModuleState& module) {
        return (table == s.active_table && OverlapsModuleVa(module, va, size)) ||
               OverlapsModulePa(module, pa, size);
    });
}

void OnUnmap(const void* table, std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    ++s.seq;
    TrimLog(s, table, va, size);
    if (!s.active || table != s.active_table) {
        return;
    }
    StickyWhere(s, Reason::Unmap,
                [&](const ModuleState& module) { return OverlapsModuleVa(module, va, size); });
}

void OnProtect(const void* table, std::uint64_t va, std::uint64_t size, bool writable) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    ++s.seq;
    if (!s.active || table != s.active_table) {
        return;
    }
    bool any = false;
    for (auto& module : s.modules) {
        if (OverlapsModuleVa(module, va, size)) {
            any = true;
            if (writable) {
                module.sticky = true; // guest stores to it are not hooked
            }
        }
    }
    if (any) {
        Bump(s, Reason::Protect);
    }
}

void OnDeviceMap(const void* table, std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    ++s.seq;
    if (!s.active || table != s.active_table) {
        return;
    }
    // Device writes land in the backing without any CPU hook.
    StickyWhere(s, Reason::DeviceMap,
                [&](const ModuleState& module) { return OverlapsModuleVa(module, va, size); });
}

void OnInvalidate(std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    // Outside every module range this cannot concern a guarded word: a module
    // with a writable, aliased or device-mapped page is already sticky.
    const bool hit = std::any_of(s.modules.begin(), s.modules.end(), [&](const auto& module) {
        return OverlapsModuleVa(module, va, size);
    });
    if (hit) {
        Bump(s, Reason::Invalidate);
    }
}

void OnInvalidateAll() {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    Bump(s, Reason::InvalidateAll);
}

void OnPageTableSwap() {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    ++s.seq;
    Bump(s, Reason::PageTableSwap);
}

Stats GetStats() {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    Stats out{};
    out.enabled = s.enabled;
    out.active = s.active;
    out.generation = s.exhausted ? kVerifyAlways : s.counter;
    out.modules = static_cast<std::uint32_t>(s.modules.size());
    out.sticky = static_cast<std::uint32_t>(
        std::count_if(s.modules.begin(), s.modules.end(), [](const auto& m) { return m.sticky; }));
    out.map_log = s.map_log.size();
    out.map_log_overflow = s.map_log_overflow;
    std::copy(std::begin(s.bumps), std::end(s.bumps), std::begin(out.bumps));
    return out;
}

} // namespace Core::RecompGuardGen
