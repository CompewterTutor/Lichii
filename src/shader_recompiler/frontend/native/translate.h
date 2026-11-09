// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/native/decode.h"

namespace Shader::Native {

/// Native uniform registers UR0-UR63 live in their own IR register bank above the 256 general
/// registers. Nothing is remapped or copied, which is what makes the lowering's R216-R245
/// reservation, its shadow copies and its 118-opcode pressure table unnecessary here.
constexpr u32 NUM_UNIFORM_REGISTERS{64};
constexpr u32 UNIFORM_REGISTER_BASE{512};

/// IR has no uniform predicate file, so UP0-UP6 are ordinary registers holding the hardware's own
/// representation: 0 for false and all ones for true (the bits R239 carries on Maxwell).
constexpr u32 NUM_UNIFORM_PREDICATES{7};
constexpr u32 UNIFORM_PREDICATE_BASE{UNIFORM_REGISTER_BASE + NUM_UNIFORM_REGISTERS};

/// Translates a native NVN2 instruction to IR directly, without rewriting it into Maxwell encoding
/// first. Registers are ordinary IR registers and the SSA rewrite pass treats them exactly as it
/// does for Maxwell programs, so no shadow copies or reserved register bases are involved.
///
/// Anything not translated yet must fail rather than guess: an unknown operand layout that is
/// misinterpreted renders a wrong image instead of falling back.
class Translator {
public:
    Translator(Environment& env, IR::Block& block) : env{env}, ir{block} {}

    /// Translate one instruction. Returns false and sets Error() when it has no translation yet.
    /// An instruction that fails may have emitted part of its IR, so a caller that falls back must
    /// discard the block.
    [[nodiscard]] bool Translate(const Ampere::Instruction& instruction);

    [[nodiscard]] std::string_view Error() const noexcept {
        return error;
    }

    /// Which operand forms the current opcode still cannot take. Used by the coverage report to say
    /// why a program is not fully translated.
    [[nodiscard]] Ampere::Instruction Current() const noexcept {
        return current;
    }

private:
    struct Unsupported {
        std::string_view reason;
    };

    void Emit();

    [[nodiscard]] IR::Reg Register(u32 index) const;
    [[nodiscard]] IR::Reg UniformRegister(u32 index) const;
    [[nodiscard]] IR::U32 Gpr(u32 index);
    [[nodiscard]] IR::U32 Uniform(u32 index);
    [[nodiscard]] IR::U1 UniformPredicate(u32 index);
    void SetUniformPredicate(u32 index, const IR::U1& value);
    [[nodiscard]] IR::U1 PredicateOperand(u32 value, bool uniform);
    [[nodiscard]] IR::U1 Lut3(u32 lut, const IR::U1& a, const IR::U1& b, const IR::U1& c);
    [[nodiscard]] IR::U32 BitwiseLut3(u32 lut, const IR::U32& a, const IR::U32& b,
                                      const IR::U32& c);
    [[nodiscard]] IR::F32 ReadFloat(u32 which);
    [[nodiscard]] IR::U32 ReadUint(u32 which);
    [[nodiscard]] IR::FpControl FloatControl() const;
    void WriteFloat(const IR::F32& value);
    void WriteUint(const IR::U32& value);

    void EmitMove();
    void EmitSystemRegisterRead();
    void EmitSetPredicate();
    void EmitIntegerMultiplyAdd();
    void EmitLogicLut();
    void EmitIntegerAdd();
    void EmitPredicateToRegister();
    void EmitMultiFunction();
    void EmitGlobalLoad();
    void EmitGlobalStore();
    void EmitSharedMemory();
    void EmitLocalMemory();
    void EmitCopySpecialToRegisterPair();
    void EmitShuffleRegister();
    void EmitFloatMinMax();
    void EmitShift();
    void EmitEffectiveAddress();
    void EmitRegisterToUniform();
    void EmitUniformShift();
    void EmitUniformSetPredicate();
    void EmitUniformAdd();
    void EmitUniformSelect();
    void EmitUniformWideMultiplyAdd();
    void EmitUniformMultiplyAdd();
    void EmitUniformLogicLut();
    void EmitIntegerToFloat();
    void EmitUniformConstantLoad();
    [[nodiscard]] IR::U64 OffsetU64(u32 value, bool negative);
    void EmitUniformMove();
    void EmitPredicateLut();
    void EmitFloatAdd();
    void EmitFloatMultiply();
    void EmitFloatFusedMultiplyAdd();
    void EmitImmediateShuffle();

    Environment& env;
    IR::IREmitter ir;
    Ampere::Instruction current{};
    std::string_view error;
    std::string detailed_error; ///< Backs Error() when the message needs the opcode number.
};

/// Translate a linear run of instructions into one block. Stops at the first instruction with no
/// translation; `error_index` is then that instruction's index.
struct TranslationResult {
    u32 error_index{~0U};
    std::string_view error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

[[nodiscard]] TranslationResult TranslateBlock(Environment& env, IR::Block& block,
                                               std::span<const Ampere::Instruction> code);

} // namespace Shader::Native
