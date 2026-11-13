// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

// EDEN_NX2_PC_PROBE=1: keeps where each core last ran a block - PC, stack pointer, frame pointer
// and link register - so a guest that spins can be located, and its caller read, from the debug
// server (`core_pc`) instead of guessed at. Relaxed stores, and nothing at all unless the variable
// is set.
namespace Core::PcProbe {

inline const bool g_enabled = std::getenv("EDEN_NX2_PC_PROBE") != nullptr;

struct Sample {
    u64 pc{};
    u64 sp{};
    u64 fp{};
    u64 lr{};
};

inline std::array<std::atomic<u64>, 4> g_pc{};
inline std::array<std::atomic<u64>, 4> g_sp{};
inline std::array<std::atomic<u64>, 4> g_fp{};
inline std::array<std::atomic<u64>, 4> g_lr{};

inline void Store(size_t core, const Sample& sample) {
    if (core >= std::size(g_pc)) {
        return;
    }
    g_pc[core].store(sample.pc, std::memory_order_relaxed);
    g_sp[core].store(sample.sp, std::memory_order_relaxed);
    g_fp[core].store(sample.fp, std::memory_order_relaxed);
    g_lr[core].store(sample.lr, std::memory_order_relaxed);
}

inline Sample Load(size_t core) {
    if (core >= std::size(g_pc)) {
        return {};
    }
    return Sample{.pc = g_pc[core].load(std::memory_order_relaxed),
                  .sp = g_sp[core].load(std::memory_order_relaxed),
                  .fp = g_fp[core].load(std::memory_order_relaxed),
                  .lr = g_lr[core].load(std::memory_order_relaxed)};
}

} // namespace Core::PcProbe
