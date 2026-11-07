// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace VideoCore {

/// One entry of the shader inventory the debug server reports: a program the shader cache knows,
/// by guest address, with the hash its pipelines are keyed by and whether lowering refused it.
struct DebugShader {
    u64 address{};
    u64 unique_hash{};
    u64 size_bytes{};
    bool rejected{};
    std::string error;
};

using DebugShaderInventory = std::vector<DebugShader>;

} // namespace VideoCore
