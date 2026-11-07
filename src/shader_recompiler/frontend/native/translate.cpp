// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/translate.h"

#include <array>
#include <bit>

namespace Shader::Native {

namespace {

constexpr std::array<IR::FpRounding, 4> ROUNDING{
    IR::FpRounding::RN,
    IR::FpRounding::RM,
    IR::FpRounding::RP,
    IR::FpRounding::RZ,
};

// NVN2 multipliers are encoded as 1 = /8, 2 = /4, 3 = /2, 4 = 1, 5 = *2, 6 = *4, 7 = *8.
constexpr std::array<f32, 8> SCALE{1.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f,
                                   1.0f, 2.0f,       4.0f,       8.0f};

/// The environment only exposes Maxwell's 18 constant buffer banks. Bank 24 is read by NVN2
/// fullscreen vertex shaders without the game ever binding it; the console reads zeros from it, and
/// so does the unused last bank Maxwell is pointed at here.
u32 ConstantBufferBank(u32 bank) {
    return bank == 24 ? 17U : bank;
}

} // Anonymous namespace

IR::Reg Translator::Register(u32 index) const {
    if (index > 255) {
        throw Unsupported{"native register index"};
    }
    if (index == 255) {
        return IR::Reg::RZ;
    }
    return IR::Reg::R0 + static_cast<int>(index);
}

IR::U32 Translator::Gpr(u32 index) {
    return ir.GetReg(Register(index));
}

IR::F32 Translator::ReadFloat(u32 which) {
    const auto [source, error] = DecodeSource(current, which);
    if (!error.empty()) {
        throw Unsupported{error};
    }
    const IR::F32 value{[&]() -> IR::F32 {
        switch (source.kind) {
        case SourceKind::Register:
            return IR::F32{ir.BitCast<IR::F32>(Gpr(source.index))};
        case SourceKind::Immediate:
            return ir.Imm32(std::bit_cast<f32>(source.immediate));
        case SourceKind::ConstantBuffer: {
            const u32 bank = ConstantBufferBank(source.index);
            if (bank >= 18) {
                throw Unsupported{"native constant buffer bank"};
            }
            return ir.GetFloatCbuf(ir.Imm32(bank), ir.Imm32(source.offset));
        }
        case SourceKind::Uniform:
            // Uniform registers become ordinary SSA values in 1.7.3; until then the lowering
            // handles them.
            throw Unsupported{"native uniform source"};
        }
        throw Unsupported{"native ALU source"};
    }()};
    return ir.FPAbsNeg(value, source.absolute, source.negate);
}

IR::U32 Translator::ReadUint(u32 which) {
    const auto [source, error] = DecodeSource(current, which);
    if (!error.empty()) {
        throw Unsupported{error};
    }
    if (source.absolute || source.negate) {
        throw Unsupported{"native integer source modifier"};
    }
    switch (source.kind) {
    case SourceKind::Register:
        return Gpr(source.index);
    case SourceKind::Immediate:
        return ir.Imm32(source.immediate);
    case SourceKind::ConstantBuffer: {
        const u32 bank = ConstantBufferBank(source.index);
        if (bank >= 18) {
            throw Unsupported{"native constant buffer bank"};
        }
        return ir.GetCbuf(ir.Imm32(bank), ir.Imm32(source.offset));
    }
    case SourceKind::Uniform:
        throw Unsupported{"native uniform source"};
    }
    throw Unsupported{"native ALU source"};
}

IR::FpControl Translator::FloatControl() const {
    return IR::FpControl{
        .no_contraction = true,
        .rounding = ROUNDING[current.Bits(78, 2)],
        .fmz_mode = current.Bits(80, 1) ? IR::FmzMode::FTZ : IR::FmzMode::None,
    };
}

void Translator::WriteFloat(const IR::F32& value) {
    const u32 dest = current.Bits(16, 8);
    const Guard guard = DecodeGuard(current);
    if (!guard) {
        if (dest != 255) {
            ir.SetReg(Register(dest), ir.BitCast<IR::U32>(value));
        }
        return;
    }
    // A guarded instruction keeps the previous destination value when the guard is false. That is
    // exact for a register destination and needs no control flow of its own.
    if (dest == 255) {
        return;
    }
    const IR::U1 condition = ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated);
    const IR::U32 stored{ir.BitCast<IR::U32>(value)};
    ir.SetReg(Register(dest), IR::U32{ir.Select(condition, stored, Gpr(dest))});
}

void Translator::WriteUint(const IR::U32& value) {
    const u32 dest = current.Bits(16, 8);
    const Guard guard = DecodeGuard(current);
    if (!guard) {
        if (dest != 255) {
            ir.SetReg(Register(dest), value);
        }
        return;
    }
    if (dest == 255) {
        return;
    }
    const IR::U1 condition = ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated);
    ir.SetReg(Register(dest), IR::U32{ir.Select(condition, value, Gpr(dest))});
}

void Translator::EmitFloatAdd() {
    if (current.Bits(81, 10) || current.Bits(91, 1)) {
        throw Unsupported{"native FADD mode"};
    }
    const IR::F32 a = ReadFloat(0);
    // The second source is the one the form selects: 1, or 2 for d/cbuf/immediate first sources.
    const u32 form = current.Bits(9, 3);
    const IR::F32 b = ReadFloat(form == 2 || form == 3 || form == 7 ? 2 : 1);
    IR::F32 value = ir.FPAdd(a, b, FloatControl());
    if (current.Bits(77, 1)) {
        value = ir.FPSaturate(value);
    }
    WriteFloat(value);
}

void Translator::EmitFloatMultiply() {
    const u32 scale = current.Bits(84, 3);
    if (current.Bits(76, 1) || scale == 0 || current.Bits(81, 3) || current.Bits(87, 4)) {
        throw Unsupported{"native FMUL scale/mode"};
    }
    // A scale other than 1 needs an explicit rounding mode and denormal handling to be defined.
    if (scale != 4 && (!current.Bits(80, 1) || current.Bits(78, 2))) {
        throw Unsupported{"native FMUL scale/mode"};
    }
    IR::F32 a = ReadFloat(0);
    if (scale != 4) {
        a = ir.FPMul(a, ir.Imm32(SCALE[scale]));
    }
    const IR::F32 b = ReadFloat(1);
    IR::F32 value = ir.FPMul(a, b, FloatControl());
    if (current.Bits(77, 1)) {
        value = ir.FPSaturate(value);
    }
    WriteFloat(value);
}

void Translator::EmitFloatFusedMultiplyAdd() {
    if (current.Bits(76, 1) || current.Bits(81, 11)) {
        throw Unsupported{"native FFMA denormal/source mode"};
    }
    const IR::F32 a = ReadFloat(0);
    const IR::F32 b = ReadFloat(1);
    const IR::F32 c = ReadFloat(2);
    IR::F32 value = ir.FPFma(a, b, c, FloatControl());
    if (current.Bits(77, 1)) {
        value = ir.FPSaturate(value);
    }
    WriteFloat(value);
}

void Translator::EmitImmediateShuffle() {
    // SHFL.IDX with an immediate lane: 53-57 lane, 40-52 mask, 58-59 mode.
    if (current.words[2] != 0x000e0000 || current.Bits(32, 8) || current.Bits(60, 4)) {
        throw Unsupported{"native immediate shuffle mode"};
    }
    const u32 lane = current.Bits(53, 5);
    const u32 mask = current.Bits(40, 13);
    const u32 mode = current.Bits(58, 2);
    const IR::U32 value = Gpr(current.Bits(24, 8));
    const IR::U32 index = ir.Imm32(lane);
    const IR::U32 packed = ir.Imm32(mask);
    const IR::U32 clamp = ir.BitFieldExtract(packed, ir.Imm32(0), ir.Imm32(5));
    const IR::U32 segment = ir.BitFieldExtract(packed, ir.Imm32(8), ir.Imm32(5));
    const IR::U32 result{[&] {
        switch (mode) {
        case 0:
            return ir.ShuffleIndex(value, index, clamp, segment);
        case 1:
            return ir.ShuffleUp(value, index, clamp, segment);
        case 2:
            return ir.ShuffleDown(value, index, clamp, segment);
        case 3:
            return ir.ShuffleButterfly(value, index, clamp, segment);
        default:
            throw Unsupported{"native shuffle mode"};
        }
    }()};
    WriteUint(result);
}

void Translator::Emit() {
    switch (current.Bits(0, 12)) {
    case 0x220: // FMUL  (form 1)
    case 0x820: // FMUL  (form 4, immediate second source)
    case 0xa20: // FMUL  (form 5, constant buffer second source)
    case 0xc20: // FMUL  (form 6, uniform second source: rejected in ReadFloat until 1.7.3)
        EmitFloatMultiply();
        return;
    case 0x221: // FADD  (form 1)
    case 0x421: // FADD  (form 2, immediate second source)
    case 0x621: // FADD  (form 3, constant buffer second source)
    case 0x821: // FADD  (form 4, immediate first source)
    case 0xa21: // FADD  (form 5, constant buffer first source)
    case 0xc21: // FADD  (form 6, uniform second source: rejected in ReadFloat until 1.7.3)
    case 0xe21: // FADD  (form 7, uniform first source: rejected in ReadFloat until 1.7.3)
        EmitFloatAdd();
        return;
    case 0x223: // FFMA  (form 1)
    case 0x423: // FFMA  (form 2, immediate second source)
    case 0x623: // FFMA  (form 3, constant buffer second source)
    case 0x823: // FFMA  (form 4, immediate first source)
    case 0xa23: // FFMA  (form 5, constant buffer first source)
    case 0xc23: // FFMA  (form 6, uniform second source: rejected in ReadFloat until 1.7.3)
    case 0xe23: // FFMA  (form 7, uniform first source: rejected in ReadFloat until 1.7.3)
        EmitFloatFusedMultiplyAdd();
        return;
    case 0xf89: // SHFL.IDX with an immediate lane
        EmitImmediateShuffle();
        return;
    case 0x918: // NOP
        return;
    default:
        throw Unsupported{"unsupported native opcode"};
    }
}

bool Translator::Translate(const Ampere::Instruction& instruction) {
    current = instruction;
    try {
        Emit();
        return true;
    } catch (const Unsupported& unsupported) {
        error = unsupported.reason;
        return false;
    }
}

TranslationResult TranslateBlock(Environment& env, IR::Block& block,
                                 std::span<const Ampere::Instruction> code) {
    Translator translator{env, block};
    for (size_t index = 0; index < code.size(); ++index) {
        if (!translator.Translate(code[index])) {
            return {.error_index = static_cast<u32>(index), .error = translator.Error()};
        }
    }
    return {};
}

} // namespace Shader::Native
