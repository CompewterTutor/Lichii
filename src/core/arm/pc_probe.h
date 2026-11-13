// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

// EDEN_NX2_PC_PROBE=1: keeps the guest PC each core last executed a block at, so a guest that
// spins can be located from the debug server (`core_pc`) instead of guessed at. One relaxed store
// per block, and nothing at all unless the variable is set.
namespace Core::PcProbe {

inline const bool g_enabled = std::getenv("EDEN_NX2_PC_PROBE") != nullptr;

inline std::atomic<u64> g_pc[4]{};

inline void Store(size_t core, u64 pc) {
    if (core < std::size(g_pc)) {
        g_pc[core].store(pc, std::memory_order_relaxed);
    }
}

inline u64 Load(size_t core) {
    return core < std::size(g_pc) ? g_pc[core].load(std::memory_order_relaxed) : 0;
}

} // namespace Core::PcProbe
