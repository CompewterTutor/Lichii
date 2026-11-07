// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/translate_program.h"

#include <algorithm>
#include <array>
#include <map>

namespace Shader::Native {

namespace {

/// Opcodes whose IR translation is implemented. Every entry needs a test with a captured
/// instruction: the ISA is undocumented, so "it looks like the Maxwell opcode" is not evidence.
/// Currently only the opcodes with no operands at all, which keeps this honest until the first
/// arithmetic family lands.
constexpr std::array<u32, 2> Implemented{
    0x918, // NOP
    0x94d, // EXIT
};

} // Anonymous namespace

bool IsImplemented(u32 opcode) {
    return std::ranges::find(Implemented, opcode) != Implemented.end();
}

std::size_t ImplementedCount() {
    return Implemented.size();
}

std::vector<MissingOpcode> Missing(std::span<const Ampere::Instruction> code) {
    std::map<u32, std::size_t> counts;
    for (const Ampere::Instruction& instruction : code) {
        const u32 opcode = instruction.Bits(0, 12);
        // Zero words are padding after a program's end in a raw allocation, not instructions.
        if (opcode == 0 && instruction.words[1] == 0 && instruction.words[2] == 0 &&
            instruction.words[3] == 0) {
            continue;
        }
        if (!IsImplemented(opcode)) {
            ++counts[opcode];
        }
    }
    std::vector<MissingOpcode> missing;
    missing.reserve(counts.size());
    for (const auto& [opcode, count] : counts) {
        missing.push_back({.opcode = opcode, .count = count});
    }
    std::ranges::stable_sort(missing, [](const MissingOpcode& a, const MissingOpcode& b) {
        return a.count != b.count ? a.count > b.count : a.opcode < b.opcode;
    });
    return missing;
}

} // namespace Shader::Native
