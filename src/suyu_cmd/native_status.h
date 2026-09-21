// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace Core {
class System;
struct PerfStatsResults;
}

namespace SuyuCmd {

/// What the main game process is really executing on.
///
/// `Pending` exists because registration, backend selection and actual
/// execution are three different states: a recompiled image can be registered
/// while the application process has not yet been handed it. Reporting that as
/// static AOT would be a claim about something that has not happened.
enum class NativeBackendClass {
    Pending,
    StrictAot,
    HybridAot,
    DynarmicJit,
    Nce,
    Unknown,
};

/// One timestamped reading of everything the status display needs.
///
/// Filled by exactly one sampler per process. The title, the F12 panel and any
/// benchmark read the same snapshot; none of them sample independently, because
/// the underlying performance counters are destructive to read.
struct NativeStatusSnapshot {
    std::string game_name;
    std::string display_version;
    std::uint64_t title_id{};

    bool game_running{};

    // Performance is published by whichever sampler owns the counters and
    // expires if that sampler stops reporting.
    bool perf_available{};
    std::uint64_t perf_sample_monotonic_ms{};
    double average_game_fps{};
    double system_fps{};
    double emulation_speed{};
    double frametime_ms{};

    bool recomp_registered{};
    bool backend_active{};
    std::uint64_t static_blocks{};
    bool jit_available{};
    bool strict_requested{};
    std::uint64_t jit_transitions{};

    bool applet_running{};
    std::string applet_name;
    NativeBackendClass applet_backend{NativeBackendClass::Unknown};

    std::uint64_t sample_monotonic_ms{};
};

inline const char* NativeBackendName(NativeBackendClass kind) {
    switch (kind) {
    case NativeBackendClass::Pending:
        return "PENDING";
    case NativeBackendClass::StrictAot:
        return "AOT STRICT";
    case NativeBackendClass::HybridAot:
        return "HYBRID AOT";
    case NativeBackendClass::DynarmicJit:
        return "JIT";
    case NativeBackendClass::Nce:
        return "NCE";
    default:
        return "UNKNOWN";
    }
}

inline NativeBackendClass ClassifyGameBackend(const NativeStatusSnapshot& s) {
    if (!s.game_running) {
        return NativeBackendClass::Pending;
    }
    if (!s.recomp_registered) {
        return NativeBackendClass::Unknown;
    }
    if (!s.backend_active || s.static_blocks == 0) {
        return NativeBackendClass::Pending;
    }
    return s.strict_requested ? NativeBackendClass::StrictAot : NativeBackendClass::HybridAot;
}

/// `strict_requested` is the policy asked for, `jit_available` is what the build
/// can do. A JIT-capable binary running a strict game is not a JIT-free
/// executable, so the two are never folded into one word.
inline std::string GameBackendQualifier(const NativeStatusSnapshot& s) {
    const NativeBackendClass kind = ClassifyGameBackend(s);
    if (kind == NativeBackendClass::StrictAot) {
        return s.jit_available ? " (JIT-capable)" : " (no-JIT build)";
    }
    if (kind == NativeBackendClass::HybridAot) {
        return " (JIT fallback allowed)";
    }
    return {};
}

inline std::string FormatNativeTitle(const NativeStatusSnapshot& s) {
    std::string out;
    out += s.game_name.empty() ? std::string{"Game"} : s.game_name;
    if (!s.display_version.empty()) {
        out += " ";
        out += s.display_version;
    }
    out += " | Game: ";
    out += NativeBackendName(ClassifyGameBackend(s));
    out += GameBackendQualifier(s);

    if (!s.perf_available) {
        out += " | FPS: -- | speed: --";
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), " | %.1f FPS | %.0f%%", s.average_game_fps,
                      s.emulation_speed * 100.0);
        out += buf;
    }

    char jit[96];
    std::snprintf(jit, sizeof(jit), " | JIT transitions: %llu",
                  static_cast<unsigned long long>(s.jit_transitions));
    out += jit;
    out += " | F12 Controls";
    return out;
}

/// Samples the live state. The SDL thread alone calls with `consume_perf=true`.
/// While benchmarking owns the counters, it calls with false and the benchmark
/// sampler publishes performance independently.
NativeStatusSnapshot SampleNativeStatus(Core::System& system, bool consume_perf);
void SetNativeLaunchName(std::string name, bool explicit_name);
void SetNativeLaunchVersion(std::string version);
void StoreNativePerfStats(const Core::PerfStatsResults& results);

void StoreNativeStatusSnapshot(const NativeStatusSnapshot& snapshot);
bool TryGetNativeStatusSnapshot(NativeStatusSnapshot& out);

} // namespace SuyuCmd
