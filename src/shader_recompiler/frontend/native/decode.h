// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

#include "shader_recompiler/frontend/ampere/instruction.h"

namespace Shader::Native {

/// Where an ALU source operand comes from.
enum class SourceKind {
    Register,
    Immediate,
    ConstantBuffer,
    Uniform,
};

struct Source {
    SourceKind kind{SourceKind::Register};
    /// Register number, uniform register number or constant buffer bank.
    u32 index{};
    /// Constant buffer byte offset.
    u32 offset{};
    /// Raw immediate word.
    u32 immediate{};
    bool absolute{};
    bool negate{};
};

struct DecodedSource {
    Source source;
    std::string_view error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

/// Decode ALU source `which` (0 to 2). Source 0 is always a register; sources 1 and 2 depend on the
/// form in bits 9-11, which is part of the opcode. The layout is the one the lowering already reads
/// in lower_to_maxwell.h operand(), which real captured programs have exercised.
[[nodiscard]] inline DecodedSource DecodeSource(const Ampere::Instruction& insn, u32 which) {
    DecodedSource result;
    Source& source{result.source};
    if (which == 0) {
        source.kind = SourceKind::Register;
        source.index = insn.Bits(24, 8);
        source.absolute = insn.Bits(73, 1) != 0;
        source.negate = insn.Bits(72, 1) != 0;
        return result;
    }
    const u32 form = insn.Bits(9, 3);
    if (form == 0) {
        result.error = "native ALU source form";
        return result;
    }
    const bool wide = which == 1 ? (form == 4 || form == 5 || form == 6)
                                 : (form == 2 || form == 3 || form == 7);
    if (wide) {
        const u32 kind = (form == 6 || form == 7) ? 3U : (form == 4 || form == 2) ? 1U : 2U;
        if (kind == 1) {
            source.kind = SourceKind::Immediate;
            source.immediate = insn.words[1];
        } else if (kind == 2) {
            if (insn.Bits(91, 1)) {
                result.error = "native constant buffer mode";
                return result;
            }
            source.kind = SourceKind::ConstantBuffer;
            source.index = insn.Bits(54, 5);
            source.offset = insn.Bits(38, 16);
            if ((source.offset % 4) != 0) {
                result.error = "native constant buffer alignment";
                return result;
            }
        } else {
            source.kind = SourceKind::Uniform;
            source.index = insn.Bits(32, 8);
        }
        if (kind == 2 || kind == 3) {
            source.absolute = insn.Bits(62, 1) != 0;
            source.negate = insn.Bits(63, 1) != 0;
        }
        return result;
    }
    const bool upper = which == 1 ? (form == 2 || form == 3 || form == 7) : true;
    source.kind = SourceKind::Register;
    source.index = insn.Bits(upper ? 64 : 32, 8);
    source.absolute = insn.Bits(upper ? 74 : 62, 1) != 0;
    source.negate = insn.Bits(upper ? 75 : 63, 1) != 0;
    return result;
}

/// The guard predicate: index in bits 12-14, negation in bit 15. Index 7 is PT, which means the
/// instruction is unpredicated.
struct Guard {
    u32 index{};
    bool negated{};
    bool valid{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid;
    }
};

[[nodiscard]] inline Guard DecodeGuard(const Ampere::Instruction& insn) {
    const u32 value = insn.Bits(12, 4);
    const u32 index = value & 7;
    return Guard{.index = index, .negated = (value & 8) != 0, .valid = index != 7};
}

} // namespace Shader::Native
