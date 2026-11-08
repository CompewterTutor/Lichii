// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/translate.h"

#include <array>
#include <bit>

#include "shader_recompiler/frontend/maxwell/translate/impl/common_funcs.h"

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

IR::Reg Translator::UniformRegister(u32 index) const {
    if (index >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform register index"};
    }
    return static_cast<IR::Reg>(UNIFORM_REGISTER_BASE + index);
}

IR::U32 Translator::Uniform(u32 index) {
    return ir.GetReg(UniformRegister(index));
}

IR::U1 Translator::UniformPredicate(u32 index) {
    if (index >= NUM_UNIFORM_PREDICATES) {
        throw Unsupported{"native uniform predicate index"};
    }
    // The stored mask is 0 or all ones, which is how the hardware keeps it in R239.
    return IR::U1{ir.INotEqual(
        ir.GetReg(static_cast<IR::Reg>(UNIFORM_PREDICATE_BASE + index)), ir.Imm32(0))};
}

void Translator::SetUniformPredicate(u32 index, const IR::U1& value) {
    if (index >= NUM_UNIFORM_PREDICATES) {
        throw Unsupported{"native uniform predicate index"};
    }
    const IR::U32 mask{ir.Select(value, ir.Imm32(0xffffffffU), ir.Imm32(0))};
    ir.SetReg(static_cast<IR::Reg>(UNIFORM_PREDICATE_BASE + index), mask);
}

IR::U1 Translator::PredicateOperand(u32 value, bool uniform) {
    const u32 index = value & 7;
    const bool negated = (value & 8) != 0;
    if (index == 7) {
        return ir.Imm1(!negated);
    }
    IR::U1 predicate{[&] {
        if (!uniform) {
            return ir.GetPred(static_cast<IR::Pred>(index), negated);
        }
        IR::U1 result = UniformPredicate(index);
        if (negated) {
            result = IR::U1{ir.LogicalNot(result)};
        }
        return result;
    }()};
    return predicate;
}

IR::U1 Translator::Lut3(u32 lut, const IR::U1& a, const IR::U1& b, const IR::U1& c) {
    // NVIDIA's LUT index is a | b << 1 | c << 2, with operands in assembly order.
    const auto bit = [&](u32 index) { return ir.Imm1(((lut >> index) & 1) != 0); };
    const IR::U1 low_a{ir.Select(a, bit(1), bit(0))};
    const IR::U1 low_b{ir.Select(a, bit(3), bit(2))};
    const IR::U1 high_a{ir.Select(a, bit(5), bit(4))};
    const IR::U1 high_b{ir.Select(a, bit(7), bit(6))};
    const IR::U1 low{ir.Select(b, low_b, low_a)};
    const IR::U1 high{ir.Select(b, high_b, high_a)};
    return IR::U1{ir.Select(c, high, low)};
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
            return IR::F32{ir.BitCast<IR::F32>(Uniform(source.index))};
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
        return Uniform(source.index);
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

void Translator::EmitMove() {
    // MOV: register, immediate, uniform register or constant buffer into a general register.
    const u32 opcode = current.Bits(0, 12);
    if (current.Bits(72, 4) != 15 ||
        (current.Bits(76, 16) & (opcode == 0xc02 ? 0U : 0x8000U)) != 0) {
        throw Unsupported{"native MOV modifiers"};
    }
    IR::U32 value{[&]() -> IR::U32 {
        switch (opcode) {
        case 0x202:
            return Gpr(current.Bits(32, 8));
        case 0x802:
            return ir.Imm32(current.words[1]);
        case 0xc02:
            if (current.words[1] >= NUM_UNIFORM_REGISTERS) {
                throw Unsupported{"native uniform move source"};
            }
            return Uniform(current.words[1]);
        case 0xa02: {
            const u32 bank = ConstantBufferBank(current.Bits(54, 5));
            if (bank >= 18 || (current.Bits(38, 16) % 4) != 0) {
                throw Unsupported{"native constant buffer address"};
            }
            return ir.GetCbuf(ir.Imm32(bank), ir.Imm32(current.Bits(38, 16)));
        }
        }
        throw Unsupported{"native MOV source"};
    }()};
    WriteUint(value);
}

void Translator::EmitSystemRegisterRead() {
    // S2R / S2UR. The special register number is the high byte of word 2, and word 2 carries
    // nothing else, which is what makes the encoding unambiguous.
    const u32 special = current.Bits(72, 8);
    if (current.words[1] != 0 || current.words[2] != (special << 8)) {
        throw Unsupported{"native special register mode"};
    }
    const bool uniform = current.Bits(0, 12) == 0x9c3;
    IR::U32 value{[&]() -> IR::U32 {
        switch (special) {
        case 0: // SR_LANEID
            return ir.LaneId();
        case 17: // SR_INVOCATION_ID
            return ir.InvocationId();
        case 18: // SR_Y_DIRECTION
            return IR::U32{ir.BitCast<IR::U32>(ir.YDirection())};
        case 19: // SR_THREAD_KILL
            return IR::U32{ir.Select(ir.IsHelperInvocation(), ir.Imm32(-1), ir.Imm32(0))};
        case 29: // SR_INVOCATION_INFO
            return ir.InvocationInfo();
        case 30: // SR_WSCALEFACTOR_XY
            return ir.SR_WScaleFactorXY();
        case 31: // SR_WSCALEFACTOR_Z
            return ir.SR_WScaleFactorZ();
        case 32: { // SR_TID
            const IR::Value tid{ir.LocalInvocationId()};
            return ir.BitFieldInsert(
                ir.BitFieldInsert(IR::U32{ir.CompositeExtract(tid, 0)},
                                  IR::U32{ir.CompositeExtract(tid, 1)}, ir.Imm32(16),
                                  ir.Imm32(8)),
                IR::U32{ir.CompositeExtract(tid, 2)}, ir.Imm32(26), ir.Imm32(6));
        }
        case 33:
            return ir.LocalInvocationIdX();
        case 34:
            return ir.LocalInvocationIdY();
        case 35:
            return ir.LocalInvocationIdZ();
        case 37:
            return ir.WorkgroupIdX();
        case 38:
            return ir.WorkgroupIdY();
        case 39:
            return ir.WorkgroupIdZ();
        case 56: // SR_EQMASK
            return ir.SubgroupEqMask();
        case 57:
            return ir.SubgroupLtMask();
        case 58:
            return ir.SubgroupLeMask();
        case 59:
            return ir.SubgroupGtMask();
        case 60:
            return ir.SubgroupGeMask();
        case 132: // SR_VARIABLE_RATE: without variable-rate shading every fragment is 1x1.
            return ir.Imm32(0x00010100);
        default:
            throw Unsupported{"native special register"};
        }
    }()};
    if (!uniform) {
        WriteUint(value);
        return;
    }
    const u32 target = current.Bits(16, 8);
    if (target >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform special destination"};
    }
    ir.SetReg(UniformRegister(target), value);
}

void Translator::EmitSetPredicate() {
    // FSETP and ISETP: compare, then combine the result with the accumulated predicate.
    const u32 opcode = current.Bits(0, 12);
    const bool floating = (opcode & 0x1ff) == 0xb;
    const u32 combine_bits = current.Bits(74, 2);
    if (combine_bits > 2) {
        throw Unsupported{"native predicate combination"};
    }
    const auto combine = static_cast<Maxwell::BooleanOp>(combine_bits);
    const u32 accumulate_index = current.Bits(87, 3);
    const IR::U1 accumulate{
        accumulate_index == 7
            ? ir.Imm1(true)
            : ir.GetPred(static_cast<IR::Pred>(accumulate_index), current.Bits(90, 1) != 0)};

    IR::U1 comparison{[&]() -> IR::U1 {
        if (floating) {
            const IR::FpControl control{
                .no_contraction = false,
                .rounding = IR::FpRounding::DontCare,
                .fmz_mode = current.Bits(80, 1) ? IR::FmzMode::FTZ : IR::FmzMode::None,
            };
            return Maxwell::FloatingPointCompare(
                ir, ReadFloat(0), ReadFloat(1),
                static_cast<Maxwell::FPCompareOp>(current.Bits(76, 4)), control);
        }
        if (current.Bits(72, 1)) {
            throw Unsupported{"native extended integer comparison mode"};
        }
        return Maxwell::IntegerCompare(ir, ReadUint(0), ReadUint(1),
                                       static_cast<Maxwell::CompareOp>(current.Bits(76, 3)),
                                       current.Bits(73, 1) != 0);
    }()};

    const u32 dest_a = current.Bits(81, 3);
    const u32 dest_b = current.Bits(84, 3);
    const Guard guard = DecodeGuard(current);
    const IR::U1 condition{guard ? ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated)
                                 : ir.Imm1(true)};
    const auto write = [&](u32 dest, const IR::U1& value) {
        if (dest == 7) {
            return;
        }
        const IR::U1 stored{
            guard ? IR::U1{ir.Select(condition, value, ir.GetPred(static_cast<IR::Pred>(dest)))}
                  : value};
        ir.SetPred(static_cast<IR::Pred>(dest), stored);
    };
    write(dest_a, Maxwell::PredicateCombine(ir, comparison, accumulate, combine));
    write(dest_b, Maxwell::PredicateCombine(ir, IR::U1{ir.LogicalNot(comparison)}, accumulate,
                                            combine));
}

void Translator::EmitUniformMove() {
    // UMOV: a uniform register from a uniform register or an immediate. These execute uniformly, so
    // their guard is a uniform predicate.
    const u32 target = current.Bits(16, 8);
    if (target >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform destination register"};
    }
    IR::U32 value{[&]() -> IR::U32 {
        if (current.Bits(0, 12) == 0x882) {
            if (current.words[2] != 0) {
                throw Unsupported{"native uniform immediate move mode"};
            }
            return ir.Imm32(current.words[1]);
        }
        if (current.words[2] != 0x08000000 || current.words[1] >= NUM_UNIFORM_REGISTERS) {
            throw Unsupported{"native uniform move mode"};
        }
        return Uniform(current.words[1]);
    }()};
    const u32 guard = current.Bits(12, 4);
    if ((guard & 7) != 7) {
        IR::U1 condition = UniformPredicate(guard & 7);
        if ((guard & 8) != 0) {
            condition = IR::U1{ir.LogicalNot(condition)};
        }
        value = IR::U32{ir.Select(condition, value, Uniform(target))};
    }
    ir.SetReg(UniformRegister(target), value);
}

void Translator::EmitPredicateLut() {
    // PLOP3.LUT: a three-input predicate lookup table. Bit 67 makes the third source a uniform
    // predicate, and bits 84-86 name a second destination when they are not PT.
    const u32 target = current.Bits(81, 3);
    const u32 second_target = current.Bits(84, 3);
    const u32 lut = current.Bits(64, 3) | (current.Bits(72, 5) << 3);
    if (second_target != 7) {
        throw Unsupported{"native dual predicate LUT mode"};
    }
    if (target == 7 || current.Bits(16, 8) != 0 || current.words[1] != 0) {
        throw Unsupported{"native predicate LUT mode"};
    }
    const IR::U1 a = PredicateOperand(current.Bits(87, 4), false);
    const IR::U1 b = PredicateOperand(current.Bits(77, 4), false);
    const IR::U1 c = PredicateOperand(current.Bits(68, 4), current.Bits(67, 1) != 0);
    IR::U1 result = Lut3(lut, a, b, c);
    const Guard guard = DecodeGuard(current);
    if (guard) {
        const IR::U1 condition = ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated);
        result = IR::U1{ir.Select(condition, result, ir.GetPred(static_cast<IR::Pred>(target)))};
    }
    ir.SetPred(static_cast<IR::Pred>(target), result);
}

IR::U32 Translator::BitwiseLut3(u32 lut, const IR::U32& a, const IR::U32& b, const IR::U32& c) {
    // A bitwise lookup table: each output bit depends on the same bit of the three sources, so the
    // Shannon expansion is a bitwise select. NVIDIA indexes the table as a | b << 1 | c << 2.
    const auto bit = [&](u32 index) {
        return ((lut >> index) & 1) != 0 ? ir.Imm32(0xffffffffU) : ir.Imm32(0);
    };
    const auto choose = [&](const IR::U32& selector, const IR::U32& when_set,
                            const IR::U32& when_clear) {
        return IR::U32{ir.BitwiseOr(ir.BitwiseAnd(selector, when_set),
                                    ir.BitwiseAnd(ir.BitwiseNot(selector), when_clear))};
    };
    const IR::U32 low_b = choose(c, bit(6), bit(2));
    const IR::U32 low_a = choose(c, bit(4), bit(0));
    const IR::U32 high_b = choose(c, bit(7), bit(3));
    const IR::U32 high_a = choose(c, bit(5), bit(1));
    const IR::U32 low = choose(b, low_b, low_a);
    const IR::U32 high = choose(b, high_b, high_a);
    return choose(a, high, low);
}

void Translator::EmitIntegerMultiplyAdd() {
    // IMAD: Ra * immediate + Rc, with the addend optionally negated. The low 32 bits of the product
    // are the same signed or unsigned, so only the carry forms need their own treatment.
    const bool negate_c = current.Bits(75, 1) != 0;
    if (current.words[1] == 1 || (current.words[2] & ~0x2ffU) != 0x078e0000U) {
        throw Unsupported{"native IMAD carry mode"};
    }
    const IR::U32 a = Gpr(current.Bits(24, 8));
    const IR::U32 product = ir.IMul(a, ir.Imm32(current.words[1]));
    IR::U32 addend = Gpr(current.Bits(64, 8));
    if (negate_c) {
        addend = ir.INeg(addend);
    }
    WriteUint(IR::U32{ir.IAdd(product, addend)});
}

void Translator::EmitLogicLut() {
    // LOP3: any three-input bitwise table, with an optional predicate set to (result != 0).
    const u32 opcode = current.Bits(0, 12);
    const u32 lut = current.Bits(72, 8);
    const u32 predicate = current.Bits(81, 3);
    if (current.Bits(80, 1) || current.Bits(87, 4) != 15 ||
        (opcode == 0x212 && current.Bits(40, 24)) || (opcode == 0xa12 && current.Bits(62, 2))) {
        throw Unsupported{"native LUT/predicate mode"};
    }
    const IR::U32 a = Gpr(current.Bits(24, 8));
    const IR::U32 b{[&]() -> IR::U32 {
        switch (opcode) {
        case 0x212:
            return Gpr(current.Bits(32, 8));
        case 0x812:
            return ir.Imm32(current.words[1]);
        case 0xa12: {
            const u32 bank = ConstantBufferBank(current.Bits(54, 5));
            if (bank >= 18 || (current.Bits(38, 16) % 4) != 0) {
                throw Unsupported{"native constant buffer address"};
            }
            return ir.GetCbuf(ir.Imm32(bank), ir.Imm32(current.Bits(38, 16)));
        }
        default:
            if (current.words[1] >= NUM_UNIFORM_REGISTERS) {
                throw Unsupported{"native uniform logic source"};
            }
            return Uniform(current.words[1]);
        }
    }()};
    const IR::U32 c = Gpr(current.Bits(64, 8));
    const IR::U32 result = BitwiseLut3(lut, a, b, c);
    WriteUint(result);
    if (predicate != 7) {
        IR::U1 nonzero{ir.INotEqual(result, ir.Imm32(0))};
        const Guard guard = DecodeGuard(current);
        if (guard) {
            const IR::U1 condition{ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated)};
            nonzero = IR::U1{ir.Select(
                condition, nonzero, ir.GetPred(static_cast<IR::Pred>(predicate)))};
        }
        ir.SetPred(static_cast<IR::Pred>(predicate), nonzero);
    }
}

void Translator::EmitIntegerAdd() {
    // IADD3 with a zero third source and no carry outputs: the plain three-input add.
    if ((current.words[2] & ~0xffU) != 0x07ffe000U) {
        throw Unsupported{"native add carry mode"};
    }
    const u32 opcode = current.Bits(0, 12);
    const IR::U32 a = Gpr(current.Bits(24, 8));
    IR::U32 b{[&]() -> IR::U32 {
        switch (opcode) {
        case 0x810:
            return ir.Imm32(current.words[1]);
        case 0xa10: {
            const u32 bank = ConstantBufferBank(current.Bits(54, 5));
            if (bank >= 18 || (current.Bits(38, 16) % 4) != 0) {
                throw Unsupported{"native constant buffer address"};
            }
            return ir.GetCbuf(ir.Imm32(bank), ir.Imm32(current.Bits(38, 16)));
        }
        case 0xc10:
            return Uniform(current.Bits(32, 8));
        default:
            if (current.Bits(63, 1)) {
                return ir.INeg(Gpr(current.Bits(32, 8)));
            }
            return Gpr(current.Bits(32, 8));
        }
    }()};
    WriteUint(IR::U32{ir.IAdd(a, b)});
}

void Translator::EmitPredicateToRegister() {
    // P2R: insert the seven predicates into the destination's low bits where the mask says so,
    // keeping every other bit from the merge source.
    if (current.Bits(76, 2) != 0) {
        throw Unsupported{"native predicate byte select"};
    }
    const u32 mask = current.words[1];
    if ((mask & ~0x7fU) != 0) {
        throw Unsupported{"native predicate mask"};
    }
    const IR::U32 source = Gpr(current.Bits(24, 8));
    IR::U32 bits = ir.Imm32(0);
    for (u32 index = 0; index < 7; ++index) {
        if (((mask >> index) & 1) == 0) {
            continue;
        }
        const IR::U1 predicate{ir.GetPred(static_cast<IR::Pred>(index))};
        const IR::U32 value{ir.Select(predicate, ir.Imm32(1U << index), ir.Imm32(0))};
        bits = IR::U32{ir.BitwiseOr(bits, value)};
    }
    const IR::U32 masked{ir.Imm32(mask)};
    const IR::U32 kept{ir.BitwiseAnd(source, ir.BitwiseNot(masked))};
    WriteUint(IR::U32{ir.BitwiseOr(kept, ir.BitwiseAnd(bits, masked))});
}

void Translator::EmitMultiFunction() {
    // MUFU. Screenspace sine and cosine take their angle in turns on SM70 and later, while the IR
    // takes radians, so the source is scaled by 2pi first.
    const u32 operation = current.Bits(74, 6);
    if (current.Bits(80, 12) != 0) {
        throw Unsupported{"native MUFU mode"};
    }
    const bool turns = operation == 0 || operation == 1;
    IR::F32 source = ReadFloat(1);
    if (turns) {
        source = ir.FPMul(source, ir.Imm32(6.283185307179586476925286766559f));
    }
    const IR::F32 value{[&]() -> IR::F32 {
        switch (operation) {
        case 0:
            return ir.FPCos(source);
        case 1:
            return ir.FPSin(source);
        case 2:
            return ir.FPExp2(source);
        case 3:
            return ir.FPLog2(source);
        case 4:
            return ir.FPRecip(source);
        case 5:
            return ir.FPRecipSqrt(source);
        case 8:
            return ir.FPSqrt(source);
        case 9:
            return ir.FPTanh(source);
        default:
            throw Unsupported{"native MUFU operation"};
        }
    }()};
    WriteFloat(value);
}

IR::U64 Translator::OffsetU64(u32 value, bool negative) {
    return IR::U64{ir.PackUint2x32(
        ir.CompositeConstruct(ir.Imm32(value), ir.Imm32(negative ? 0xffffffffU : 0)))};
}

void Translator::EmitGlobalLoad() {
    // LDG: a 64-bit address built from a uniform base pair, an optional register offset and an
    // immediate byte offset, then a load of 8 to 128 bits.
    const u32 size = current.Bits(73, 3);
    const u32 address = current.Bits(24, 8);
    if (size > 6) {
        throw Unsupported{"native global load size"};
    }
    if ((current.words[2] & ~(0x04000000U | 0x3fU | 0xe00U)) != 0x081e8100U ||
        current.Bits(38, 2) != 0) {
        throw Unsupported{"native global load uniform addressing mode"};
    }
    const u32 uniform = current.Bits(32, 6);
    const IR::U64 base = ir.PackUint2x32(
        ir.CompositeConstruct(Uniform(uniform), Uniform(uniform + 1)));
    IR::U64 pointer = base;
    if (address != 255) {
        // Bit 90 makes the register offset a 64-bit pair; otherwise it is zero-extended.
        const IR::U64 offset =
            current.Bits(90, 1)
                ? IR::U64{ir.PackUint2x32(ir.CompositeConstruct(Gpr(address), Gpr(address + 1)))}
                : IR::U64{ir.PackUint2x32(ir.CompositeConstruct(Gpr(address), ir.Imm32(0)))};
        pointer = ir.IAdd(pointer, offset);
    }
    const u32 raw = current.Bits(40, 24);
    const s32 immediate = static_cast<s32>(raw << 8) >> 8;
    pointer = ir.IAdd(pointer, OffsetU64(static_cast<u32>(immediate), immediate < 0));

    const u32 dest = current.Bits(16, 8);
    const auto store = [&](u32 index, const IR::U32& value) {
        const Guard guard = DecodeGuard(current);
        if (guard) {
            const IR::U1 condition{ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated)};
            const IR::Reg reg{Register(index)};
            ir.SetReg(reg, IR::U32{ir.Select(condition, value, ir.GetReg(reg))});
            return;
        }
        if (index != 255) {
            ir.SetReg(Register(index), value);
        }
    };
    switch (size) {
    case 0:
        store(dest, ir.LoadGlobalU8(pointer));
        return;
    case 1:
        store(dest, ir.LoadGlobalS8(pointer));
        return;
    case 2:
        store(dest, ir.LoadGlobalU16(pointer));
        return;
    case 3:
        store(dest, ir.LoadGlobalS16(pointer));
        return;
    case 4:
        store(dest, ir.LoadGlobal32(pointer));
        return;
    case 5: {
        const IR::Value pair{ir.LoadGlobal64(pointer)};
        store(dest, IR::U32{ir.CompositeExtract(pair, 0)});
        store(dest + 1, IR::U32{ir.CompositeExtract(pair, 1)});
        return;
    }
    default:
        throw Unsupported{"native global load size"};
    }
}

void Translator::EmitGlobalStore() {
    // STG: the same address shape as LDG, with the data in bits 32-39.
    const u32 size = current.Bits(73, 3);
    const u32 data = current.Bits(32, 8);
    const u32 address = current.Bits(24, 8);
    if (size > 6 || !current.Bits(72, 1)) {
        throw Unsupported{"native global store size"};
    }
    if ((current.words[2] & ~(0x04000000U | 0x3fU | 0xe00U)) != 0x0810a100U ||
        current.Bits(38, 2) != 0) {
        throw Unsupported{"native global store uniform addressing mode"};
    }
    const u32 uniform = current.Bits(64, 6);
    const IR::U64 base = ir.PackUint2x32(
        ir.CompositeConstruct(Uniform(uniform), Uniform(uniform + 1)));
    IR::U64 pointer = base;
    if (address != 255) {
        const IR::U64 offset =
            current.Bits(90, 1)
                ? IR::U64{ir.PackUint2x32(ir.CompositeConstruct(Gpr(address), Gpr(address + 1)))}
                : IR::U64{ir.PackUint2x32(ir.CompositeConstruct(Gpr(address), ir.Imm32(0)))};
        pointer = ir.IAdd(pointer, offset);
    }
    const u32 raw = current.Bits(40, 24);
    const s32 immediate = static_cast<s32>(raw << 8) >> 8;
    pointer = ir.IAdd(pointer, OffsetU64(static_cast<u32>(immediate), immediate < 0));

    const auto value = [&](u32 index) -> IR::U32 {
        return index == 255 ? ir.Imm32(0) : Gpr(index);
    };
    switch (size) {
    case 0:
        ir.WriteGlobalU8(pointer, value(data));
        return;
    case 1:
        ir.WriteGlobalS8(pointer, value(data));
        return;
    case 2:
        ir.WriteGlobalU16(pointer, value(data));
        return;
    case 3:
        ir.WriteGlobalS16(pointer, value(data));
        return;
    case 4:
        ir.WriteGlobal32(pointer, value(data));
        return;
    default:
        // Wider stores write a register pair or quad from consecutive registers.
        throw Unsupported{"native wide global store"};
    }
}

void Translator::EmitSharedMemory() {
    // LDS / STS: a register base, an optional scale (stride), an optional uniform base and a byte
    // offset. IR takes the whole byte offset, so the pieces are simply added.
    const u32 opcode = current.Bits(0, 12);
    const u32 width = current.Bits(73, 3);
    const u32 stride = current.Bits(78, 2);
    const bool store = opcode != 0x984;
    if (width > 6) {
        throw Unsupported{"native shared memory width"};
    }
    const bool uniform_base = (opcode == 0x984 || opcode == 0x988) && current.Bits(91, 1);
    const u32 ignored = uniform_base ? (opcode == 0x988 ? 0x080000ffU : 0x08000000U) : 0U;
    if ((current.words[2] & ~ignored) != ((width << 9) | (stride << 14)) ||
        (opcode == 0x984 && !uniform_base && current.Bits(32, 8) != 0) ||
        (opcode == 0x988 && !uniform_base)) {
        throw Unsupported{"native shared memory addressing mode"};
    }
    IR::U32 pointer = Gpr(current.Bits(24, 8));
    if (stride != 0) {
        // The register is scaled by the access size: 2^(stride + 1) bytes.
        pointer = ir.ShiftLeftLogical(pointer, ir.Imm32(stride + 1));
    }
    if (uniform_base) {
        pointer = ir.IAdd(pointer, Uniform(current.Bits(store ? 64 : 32, 8)));
    }
    pointer = ir.IAdd(pointer, ir.Imm32(current.Bits(40, 24)));

    const u32 bit_size = width == 0 ? 8 : width == 2 ? 16 : width == 4 ? 32 : 0;
    if (bit_size == 0) {
        throw Unsupported{"native shared memory width"};
    }
    if (store) {
        ir.WriteShared(bit_size, pointer, Gpr(current.Bits(32, 8)));
        return;
    }
    const IR::Value value{ir.LoadShared(bit_size, false, pointer)};
    WriteUint(IR::U32{value});
}

void Translator::EmitShift() {
    // SHF with an immediate shift. Only the plain word forms are handled: a right shift reads the
    // high word, a left shift the low word, and the other word must be RZ.
    const u32 direction = current.Bits(76, 1);
    const u32 type = current.Bits(73, 2);
    const u32 shift = current.words[1];
    if (current.Bits(75, 1) || shift > 32) {
        throw Unsupported{"native shift mode"};
    }
    const IR::U32 amount = ir.Imm32(shift);
    if (direction != 0) {
        if (current.Bits(24, 8) != 255 || (type != 3 && type != 2)) {
            throw Unsupported{"native shift mode"};
        }
        const IR::U32 source = Gpr(current.Bits(64, 8));
        WriteUint(type == 2 ? IR::U32{ir.ShiftRightArithmetic(source, amount)}
                            : IR::U32{ir.ShiftRightLogical(source, amount)});
        return;
    }
    if (current.Bits(64, 8) != 255 || type != 3) {
        throw Unsupported{"native shift mode"};
    }
    WriteUint(IR::U32{ir.ShiftLeftLogical(Gpr(current.Bits(24, 8)), amount)});
}

void Translator::EmitFloatMinMax() {
    // FMNMX: min or max of two sources, chosen by the predicate in bits 87-90. A negated predicate
    // swaps the two, which is what turns the same instruction into the other direction.
    if (current.Bits(74, 6) != 0 || current.Bits(82, 5) != 0 || current.Bits(81, 1)) {
        throw Unsupported{"native floating min/max mode"};
    }
    const IR::F32 a = ReadFloat(0);
    const IR::F32 b = ReadFloat(1);
    const IR::FpControl control{
        .no_contraction = false,
        .rounding = IR::FpRounding::DontCare,
        .fmz_mode = current.Bits(80, 1) ? IR::FmzMode::FTZ : IR::FmzMode::None,
    };
    const IR::F32 maximum = ir.FPMax(a, b, control);
    const IR::F32 minimum = ir.FPMin(a, b, control);
    const u32 predicate = current.Bits(87, 3);
    const bool negated = current.Bits(90, 1) != 0;
    const IR::U1 condition{predicate == 7
                               ? ir.Imm1(!negated)
                               : ir.GetPred(static_cast<IR::Pred>(predicate), negated)};
    WriteFloat(IR::F32{ir.Select(condition, negated ? maximum : minimum,
                                 negated ? minimum : maximum)});
}

void Translator::EmitIntegerToFloat() {
    // I2F.U8 / I2F.S8: a byte selected from the source, converted to float. Bit 74 picks the
    // signed form.
    const u32 opcode = current.Bits(0, 12);
    const bool signed_conversion = current.Bits(74, 1) != 0;
    if ((current.words[2] & ~0x400U) != (opcode == 0xd06 ? 0x08001000U : 0x00001000U)) {
        throw Unsupported{"native integer-to-float format"};
    }
    if (current.words[1] & 0xcfffff00U) {
        throw Unsupported{"native integer-to-float source"};
    }
    IR::U32 source{[&]() -> IR::U32 {
        if (opcode == 0xd06) {
            return Uniform(current.Bits(32, 8));
        }
        if (current.Bits(24, 8) != 0) {
            throw Unsupported{"native integer-to-float source"};
        }
        return Gpr(current.Bits(32, 8));
    }()};
    const u32 selector = current.Bits(60, 2);
    // A byte of the source, converted to float. The signed form sign-extends the selected byte.
    const IR::Value value{ir.ConvertIToF(32, 8, signed_conversion,
                                         ir.BitFieldExtract(source, ir.Imm32(selector * 8),
                                                            ir.Imm32(8)))};
    WriteFloat(IR::F32{value});
}

void Translator::EmitShuffleRegister() {
    // SHFL with the lane in a register: the same shape as the immediate form.
    if ((current.words[2] & ~0x000e0000U) || current.Bits(53, 5) || current.Bits(60, 4) ||
        (current.words[3] & 0xc00001ffU)) {
        throw Unsupported{"native register shuffle mode"};
    }
    const IR::U32 value = Gpr(current.Bits(24, 8));
    const IR::U32 index = Gpr(current.Bits(32, 8));
    const IR::U32 packed = ir.Imm32(current.Bits(40, 13));
    const IR::U32 clamp = ir.BitFieldExtract(packed, ir.Imm32(0), ir.Imm32(5));
    const IR::U32 segment = ir.BitFieldExtract(packed, ir.Imm32(8), ir.Imm32(5));
    const IR::U32 result{[&] {
        switch (current.Bits(58, 2)) {
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

void Translator::EmitUniformConstantLoad() {
    // ULDC: a constant buffer word or pair loaded into the uniform register file. The indexed form
    // is not handled yet: its index register's units have not been confirmed against captured code.
    const u32 dest = current.Bits(16, 8);
    const u32 width = current.Bits(73, 3);
    const u32 offset = current.Bits(38, 16);
    const u32 bank = ConstantBufferBank(current.Bits(54, 5));
    if (current.Bits(0, 12) != 0xab9) {
        throw Unsupported{"native indexed uniform constant load"};
    }
    if (bank >= 18 || dest >= NUM_UNIFORM_REGISTERS || (width != 4 && width != 5)) {
        throw Unsupported{"native uniform constant load mode"};
    }
    if (offset % (width == 5 ? 8U : 4U) != 0 ||
        (width == 5 && dest + 1 >= NUM_UNIFORM_REGISTERS)) {
        throw Unsupported{"native uniform constant load alignment"};
    }
    ir.SetReg(UniformRegister(dest), ir.GetCbuf(ir.Imm32(bank), ir.Imm32(offset)));
    if (width == 5) {
        ir.SetReg(UniformRegister(dest + 1),
                  ir.GetCbuf(ir.Imm32(bank), ir.Imm32(offset + 4)));
    }
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
    case 0x202: // MOV   (register)
    case 0x802: // MOV   (immediate)
    case 0xa02: // MOV   (constant buffer)
    case 0xc02: // MOV   (uniform register)
        EmitMove();
        return;
    case 0x824: // IMAD
        EmitIntegerMultiplyAdd();
        return;
    case 0x212: // LOP3 (register)
    case 0x812: // LOP3 (immediate)
    case 0xa12: // LOP3 (constant buffer)
    case 0xc12: // LOP3 (uniform register)
        EmitLogicLut();
        return;
    case 0x210: // IADD3 (register)
    case 0x810: // IADD3 (immediate)
    case 0xa10: // IADD3 (constant buffer)
    case 0xc10: // IADD3 (uniform)
        EmitIntegerAdd();
        return;
    case 0x803: // P2R
        EmitPredicateToRegister();
        return;
    case 0x308: // MUFU
        EmitMultiFunction();
        return;
    case 0x981: // LDG
        EmitGlobalLoad();
        return;
    case 0x986: // STG
        EmitGlobalStore();
        return;
    case 0x388: // STS with a register offset
    case 0x984: // LDS
    case 0x988: // STS
        EmitSharedMemory();
        return;
    case 0x209: // FMNMX
        EmitFloatMinMax();
        return;
    case 0x819: // SHF with an immediate shift
        EmitShift();
        return;
    case 0x306: // I2F
    case 0xd06: // I2F (uniform source)
        EmitIntegerToFloat();
        return;
    case 0x589: // SHFL with a register lane
        EmitShuffleRegister();
        return;
    case 0xab9: // ULDC
    case 0xabb: // ULDC indexed
        EmitUniformConstantLoad();
        return;
    case 0x919: // S2R
    case 0x9c3: // S2UR
        EmitSystemRegisterRead();
        return;
    case 0x20b: // FSETP (register)
    case 0x80b: // FSETP (immediate)
    case 0xa0b: // FSETP (constant buffer)
    case 0xc0b: // FSETP (uniform)
    case 0x20c: // ISETP (register)
    case 0x80c: // ISETP (immediate)
    case 0xa0c: // ISETP (constant buffer)
    case 0xc0c: // ISETP (uniform)
        EmitSetPredicate();
        return;
    case 0x882: // UMOV  (immediate)
    case 0xc82: // UMOV  (uniform register)
        EmitUniformMove();
        return;
    case 0x81c: // PLOP3.LUT
        EmitPredicateLut();
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
    case 0x945: // BSSY
    case 0x941: // BSYNC
    case 0x942: // BREAK
    case 0x948: // WARPSYNC
        // Warp reconvergence is a no-op on the host: threads are independent, and the branches
        // these instructions delimit are what the structurer reads.
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
        if (unsupported.reason == "unsupported native opcode") {
            // Name the opcode: the coverage report is otherwise a wall of identical failures.
            detailed_error = fmt::format("unsupported native opcode {:#05x}",
                                         instruction.Bits(0, 12));
            error = detailed_error;
            return false;
        }
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
