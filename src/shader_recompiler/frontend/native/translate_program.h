// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "shader_recompiler/frontend/ampere/instruction.h"

namespace Shader::Native {

/// One opcode the native frontend cannot translate yet, with how often a program uses it.
struct MissingOpcode {
    u32 opcode{};
    std::size_t count{};
};

/// The native frontend decodes NVN2 instructions straight to IR instead of rewriting them into
/// Maxwell encoding first (see docs/shader-precompile/README.md). It is built one opcode at a time;
/// a program that uses anything unimplemented falls back to the lowering path.
[[nodiscard]] bool IsImplemented(u32 opcode);

/// How many opcodes the frontend can translate.
[[nodiscard]] std::size_t ImplementedCount();

/// The opcodes in `code` that have no IR translation yet, most frequent first. Empty means the
/// whole program is covered.
[[nodiscard]] std::vector<MissingOpcode> Missing(std::span<const Ampere::Instruction> code);

} // namespace Shader::Native
