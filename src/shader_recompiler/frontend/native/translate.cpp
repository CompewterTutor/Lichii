// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/translate.h"

#include <array>
#include <cstdio>
#include <cstdlib>
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

IR::U32 Translator::ReadUint(u32 which, bool check_modifiers) {
    const auto [source, error] = DecodeSource(current, which);
    if (!error.empty()) {
        throw Unsupported{error};
    }
    if (check_modifiers && (source.absolute || source.negate)) {
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

void Translator::EmitSelectValue() {
    // SEL / FSEL: choose between two sources on a predicate. The predicate index is bits 87-89 with
    // a negation flag in bit 90 (confirmed with nvdisasm), and the sources are the ordinary ALU
    // sources, so every form reuses the generic decode.
    const u32 predicate = current.Bits(87, 3);
    if (predicate == 7) {
        throw Unsupported{"native select predicate"};
    }
    const IR::U1 condition{
        ir.GetPred(static_cast<IR::Pred>(predicate), current.Bits(90, 1) != 0)};
    if ((current.Bits(0, 12) & 0x1ff) == 8) {
        WriteFloat(IR::F32{ir.Select(condition, ReadFloat(0), ReadFloat(1))});
        return;
    }
    WriteUint(IR::U32{ir.Select(condition, ReadUint(0), ReadUint(1))});
}

void Translator::EmitPopulationCount() {
    // POPC: the population count of a register, with bit 63 counting the complement (confirmed with
    // nvdisasm). Only the register form has been seen in a capture.
    if (current.Bits(9, 3) != 1) {
        throw Unsupported{"native population count form"};
    }
    IR::U32 value = Gpr(current.Bits(32, 8));
    if (current.Bits(63, 1)) {
        value = IR::U32{ir.BitwiseNot(value)};
    }
    WriteUint(ir.BitCount(value));
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
        // Bits 72 and 73 are the instruction's own EX and signed flags on ISETP, not source
        // modifiers, so source 0 is read without the modifier check.
        return Maxwell::IntegerCompare(ir, ReadUint(0, false), ReadUint(1),
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
    const u32 opcode = current.Bits(0, 12);
    if (opcode == 0xc24 || opcode == 0xe24) {
        // The uniform file supplies the multiplier (0xc24) or the addend (0xe24), indexed by bits
        // 32-39. On 0xe24 bit 63 negates the addend. The field positions were confirmed with
        // nvdisasm: changing bits 32-39 renames the UR operand, and bits 64-71 is Rb in both forms.
        const bool negate_uniform = opcode == 0xe24 && current.Bits(63, 1) != 0;
        if ((current.words[2] & ~0x080002ffU) != 0x078e0000U ||
            (current.words[1] & ~(0xffU | (negate_uniform ? 0x80000000U : 0U))) != 0 ||
            current.Bits(32, 8) >= NUM_UNIFORM_REGISTERS) {
            throw Unsupported{"native uniform multiply mode"};
        }
        const IR::U32 uniform = Uniform(current.Bits(32, 8));
        if (opcode == 0xc24) {
            WriteUint(IR::U32{ir.IAdd(ir.IMul(Gpr(current.Bits(24, 8)), uniform),
                                    Gpr(current.Bits(64, 8)))});
            return;
        }
        const IR::U32 product = ir.IMul(Gpr(current.Bits(24, 8)), Gpr(current.Bits(64, 8)));
        const IR::U32 addend = negate_uniform ? IR::U32{ir.INeg(uniform)} : uniform;
        WriteUint(IR::U32{ir.IAdd(product, addend)});
        return;
    }
    const bool negate_c = current.Bits(75, 1) != 0;
    if ((current.words[2] & ~0x2ffU) != 0x078e0000U) {
        throw Unsupported{"native IMAD carry mode"};
    }
    const IR::U32 a = Gpr(current.Bits(24, 8));
    // The multiplier and addend swap places between the forms: 0x824 takes the multiplier as an
    // immediate, 0x224 as a register with the addend in bits 64-71, and 0x424 the other way round
    // (multiplier in bits 64-71, immediate addend in word 1). Confirmed with nvdisasm.
    const IR::U32 multiplier{[&]() -> IR::U32 {
        switch (opcode) {
        case 0x824:
            return ir.Imm32(current.words[1]);
        case 0x224:
            return Gpr(current.Bits(32, 8));
        case 0xa24:
            return ReadUint(1); // constant buffer multiplier
        default:
            return Gpr(current.Bits(64, 8));
        }
    }()};
    const IR::U32 product = ir.IMul(a, multiplier);
    IR::U32 addend{[&]() -> IR::U32 {
        if (opcode == 0x424) {
            return ir.Imm32(current.words[1]);
        }
        if (opcode == 0x624) {
            return ReadUint(1); // constant buffer addend
        }
        return Gpr(current.Bits(64, 8));
    }()};
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
    // IADD3.X: the chained form. It adds two carry predicates into the sum, so a 64-bit add whose
    // low half produced a carry count of 0, 1 or 2 continues correctly. The two sources are bits
    // 77-79 and 87-89, each negatable at 80 and 90, the same fields the lowering adds.
    if (current.Bits(74, 1) != 0) {
        if (current.Bits(81, 6) != 0x3f) {
            throw Unsupported{"native extended add carry mode"};
        }
        const auto carry_in = [&](u32 field, u32 negate) -> IR::U32 {
            const u32 index = current.Bits(field, 3);
            const bool negated = current.Bits(negate, 1) != 0;
            const IR::U1 condition{index == 7
                                       ? ir.Imm1(!negated)
                                       : ir.GetPred(static_cast<IR::Pred>(index), negated)};
            return IR::U32{ir.Select(condition, ir.Imm32(1), ir.Imm32(0))};
        };
        const IR::U32 a = Gpr(current.Bits(24, 8));
        const IR::U32 b = Gpr(current.Bits(32, 8));
        const IR::U32 c{current.Bits(64, 8) == 255 ? ir.Imm32(0) : Gpr(current.Bits(64, 8))};
        const IR::U32 first = carry_in(77, 80);
        const IR::U32 second = carry_in(87, 90);
        const auto widen = [&](const IR::U32& value) {
            return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(value, ir.Imm32(0)))};
        };
        const IR::U64 sum{ir.IAdd(IR::U64{ir.IAdd(IR::U64{ir.IAdd(widen(a), widen(b))}, widen(c))},
                                  IR::U64{ir.IAdd(widen(first), widen(second))})};
        const IR::Value halves{ir.UnpackUint2x32(sum)};
        WriteUint(IR::U32{ir.CompositeExtract(halves, 0)});
        return;
    }

    // IADD3: a + b + c, with up to two carry outputs. The lowering derives them from the 64-bit
    // sum: the first predicate is "high word >= 1" (a carry out), the second "high word >= 2"
    // (a carry out of both additions).
    const u32 carry_low = current.Bits(81, 3);
    const u32 carry_high = current.Bits(84, 3);
    const u32 opcode = current.Bits(0, 12);
    const u32 mode = current.words[2] & ~0xffU;
    // Bit 91 marks the uniform-file form; otherwise the shape is the register one.
    const bool allowed = mode == 0x07ffe000U || mode == 0x0f91e000U || mode == 0x0791e000U ||
                         (opcode == 0xc10 && mode == 0x0fffe000U);
    if (!allowed) {
        throw Unsupported{"native add carry mode"};
    }
    const IR::U32 a = Gpr(current.Bits(24, 8));
    IR::U32 b{[&]() -> IR::U32 {
        switch (opcode) {
        case 0x810:
            return ir.Imm32(current.words[1]);
        case 0xc10:
            return Uniform(current.Bits(32, 8));
        case 0xa10: {
            const u32 bank = ConstantBufferBank(current.Bits(54, 5));
            if (bank >= 18 || (current.Bits(38, 16) % 4) != 0) {
                throw Unsupported{"native constant buffer address"};
            }
            return ir.GetCbuf(ir.Imm32(bank), ir.Imm32(current.Bits(38, 16)));
        }
        default:
            if (current.Bits(63, 1)) {
                return ir.INeg(Gpr(current.Bits(32, 8)));
            }
            return Gpr(current.Bits(32, 8));
        }
    }()};
    const bool no_third = current.Bits(64, 8) == 255;
    if (no_third && carry_low == 7 && carry_high == 7) {
        // The common two-input add needs no 64-bit intermediate at all.
        WriteUint(IR::U32{ir.IAdd(a, b)});
        return;
    }
    const IR::U32 c{no_third ? ir.Imm32(0) : Gpr(current.Bits(64, 8))};
    const IR::U64 wide_a{ir.PackUint2x32(ir.CompositeConstruct(a, ir.Imm32(0)))};
    const IR::U64 wide_b{ir.PackUint2x32(ir.CompositeConstruct(b, ir.Imm32(0)))};
    const IR::U64 wide_c{ir.PackUint2x32(ir.CompositeConstruct(c, ir.Imm32(0)))};
    const IR::Value wide{ir.IAdd(IR::U64{ir.IAdd(wide_a, wide_b)}, wide_c)};
    const IR::Value halves{ir.UnpackUint2x32(IR::U64{wide})};
    const IR::U32 high{ir.CompositeExtract(halves, 1)};
    WriteUint(IR::U32{ir.CompositeExtract(halves, 0)});

    const Guard guard = DecodeGuard(current);
    const auto write_carry = [&](u32 target, const IR::U1& value) {
        if (target == 7) {
            return;
        }
        IR::U1 stored{value};
        if (guard) {
            stored = IR::U1{ir.Select(
                ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated), stored,
                ir.GetPred(static_cast<IR::Pred>(target)))};
        }
        ir.SetPred(static_cast<IR::Pred>(target), stored);
    };
    if (carry_low != 7) {
        write_carry(carry_low, IR::U1{ir.INotEqual(high, ir.Imm32(0))});
    }
    if (carry_high != 7) {
        write_carry(carry_high, IR::U1{ir.IGreaterThanEqual(high, ir.Imm32(2), false)});
    }
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
    // Three addressing forms: a plain 64-bit register pair (0x381), a uniform base pair plus an
    // optional register offset, or a cache descriptor in the uniform pair with the whole address in
    // a register pair (desc[URn][Ra.64]).
    if (current.Bits(0, 12) == 0x381) {
        if ((current.words[2] & ~0x2e00U) != 0x001e8100U || (address & 1) != 0 || address >= 246) {
            throw Unsupported{"native global load register address pair"};
        }
        const IR::U64 pointer{ir.PackUint2x32(
            ir.CompositeConstruct(Gpr(address), Gpr(address + 1)))};
        const u32 raw_offset = current.Bits(40, 24);
        const s32 offset = static_cast<s32>(raw_offset << 8) >> 8;
        const IR::U64 address64{
            ir.IAdd(pointer, OffsetU64(static_cast<u32>(offset), offset < 0))};
        const u32 dest_register = current.Bits(16, 8);
        const Guard instruction_guard = DecodeGuard(current);
        const auto store = [&](u32 index, const IR::U32& value) {
            if (instruction_guard) {
                const IR::U1 condition{ir.GetPred(
                    static_cast<IR::Pred>(instruction_guard.index), instruction_guard.negated)};
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
            store(dest_register, ir.LoadGlobalU8(address64));
            return;
        case 1:
            store(dest_register, ir.LoadGlobalS8(address64));
            return;
        case 2:
            store(dest_register, ir.LoadGlobalU16(address64));
            return;
        case 3:
            store(dest_register, ir.LoadGlobalS16(address64));
            return;
        case 4:
            store(dest_register, ir.LoadGlobal32(address64));
            return;
        case 5: {
            const IR::Value pair{ir.LoadGlobal64(address64)};
            store(dest_register, IR::U32{ir.CompositeExtract(pair, 0)});
            store(dest_register + 1, IR::U32{ir.CompositeExtract(pair, 1)});
            return;
        }
        case 6: {
            const IR::Value quad{ir.LoadGlobal128(address64)};
            for (u32 i = 0; i < 4; ++i) {
                store(dest_register + i, IR::U32{ir.CompositeExtract(quad, i)});
            }
            return;
        }
        default:
            throw Unsupported{"native global load size"};
        }
    }
    // Two addressing forms: a uniform base pair plus an optional register offset, or a cache
    // descriptor in the uniform pair and the whole address in a register pair (desc[URn][Ra.64]).
    const bool descriptor = (current.words[2] & ~(0x3fU | 0xe00U)) == 0x0c1e1100U ||
                            current.words[2] == 0x0c1ebd00U;
    // Bits 75-77 carry the memory ordering (.STRONG.SM, .CONSTANT ...); the address is the same.
    // A strongly ordered load becomes a coherent one so the host does not cache it either.
    const bool coherent = (current.words[2] & 0x2000U) != 0;
    if (!descriptor &&
        ((current.words[2] & ~(0x04000000U | 0x3fU | 0xe00U | 0x2c00U)) != 0x081e8100U ||
         current.Bits(38, 2) != 0)) {
        throw Unsupported{"native global load uniform addressing mode"};
    }
    if (descriptor && ((address & 1) != 0 || address >= 246)) {
        throw Unsupported{"native global load register address pair"};
    }
    IR::U64 pointer{[&]() -> IR::U64 {
        if (descriptor) {
            return IR::U64{ir.PackUint2x32(
                ir.CompositeConstruct(Gpr(address), Gpr(address + 1)))};
        }
        const u32 uniform = current.Bits(32, 6);
        return IR::U64{ir.PackUint2x32(
            ir.CompositeConstruct(Uniform(uniform), Uniform(uniform + 1)))};
    }()};
    if (!descriptor && address != 255) {
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
        store(dest, ir.LoadGlobalU8(pointer, coherent));
        return;
    case 1:
        store(dest, ir.LoadGlobalS8(pointer, coherent));
        return;
    case 2:
        store(dest, ir.LoadGlobalU16(pointer, coherent));
        return;
    case 3:
        store(dest, ir.LoadGlobalS16(pointer, coherent));
        return;
    case 4:
        store(dest, ir.LoadGlobal32(pointer, coherent));
        return;
    case 5: {
        const IR::Value pair{ir.LoadGlobal64(pointer, coherent)};
        store(dest, IR::U32{ir.CompositeExtract(pair, 0)});
        store(dest + 1, IR::U32{ir.CompositeExtract(pair, 1)});
        return;
    }
    case 6: {
        const IR::Value quad{ir.LoadGlobal128(pointer, coherent)};
        for (u32 i = 0; i < 4; ++i) {
            store(dest + i, IR::U32{ir.CompositeExtract(quad, i)});
        }
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
    case 5: {
        // A 64-bit store takes an aligned register pair.
        if (data == 255 || (data % 2) != 0) {
            throw Unsupported{"native wide global store"};
        }
        ir.WriteGlobal64(pointer, ir.CompositeConstruct(Gpr(data), Gpr(data + 1)));
        return;
    }
    case 6: {
        if (data == 255 || (data % 4) != 0) {
            throw Unsupported{"native wide global store"};
        }
        ir.WriteGlobal128(pointer, ir.CompositeConstruct(Gpr(data), Gpr(data + 1), Gpr(data + 2),
                                                         Gpr(data + 3)));
        return;
    }
    default:
        throw Unsupported{"native wide global store"};
    }
}

void Translator::EmitCopySpecialToRegisterPair() {
    // CS2R: a special register copied into a register pair. Only the zero idiom is confirmed:
    // `00027805 00000000 0001ff00 000fe200` decodes as `CS2R R2, SRZ`, with 255 where the special
    // register goes and no source word. The special-register field is not decoded yet, so every
    // other special register stays refused rather than guessed.
    const u32 dest = current.Bits(16, 8);
    if (current.Bits(72, 8) != 255 || current.words[1] != 0 ||
        (current.words[2] & ~0x0001ff00U) != 0) {
        throw Unsupported{"native special register pair"};
    }
    if (dest >= 245) {
        throw Unsupported{"native special register pair destination"};
    }
    WriteUint(ir.Imm32(0));
    ir.SetReg(Register(dest + 1), ir.Imm32(0));
}

void Translator::EmitLocalMemory() {
    // STL / LDL: local memory is addressed by byte and the IR takes a word offset, so the address is
    // shifted down by two.
    const u32 opcode = current.Bits(0, 12);
    const u32 size = current.Bits(73, 3);
    const bool uniform_address = current.Bits(91, 1) != 0;
    const u32 offset = current.Bits(40, 24);
    const u32 address = current.Bits(24, 8);
    const u32 allowed = (opcode == 0x983 ? 0x200e00U : 0xe00U) |
                        (uniform_address ? 0x08000000U | (opcode == 0x983 ? 0U : 0x3fU) : 0U);
    if ((address != 255 && address >= 246) || (current.words[2] & ~allowed) != 0x00100000U ||
        size != 4) {
        throw Unsupported{"native local memory mode"};
    }
    IR::U32 pointer{ir.Imm32(offset)};
    if (address != 255) {
        pointer = IR::U32{ir.IAdd(Gpr(address), pointer)};
    }
    if (uniform_address) {
        pointer = IR::U32{ir.IAdd(pointer, Uniform(current.Bits(64, 8)))};
    }
    const IR::U32 word{ir.ShiftRightLogical(pointer, ir.Imm32(2))};
    if (opcode == 0x983) {
        WriteUint(ir.LoadLocal(word));
        return;
    }
    const IR::U32 stored = Gpr(current.Bits(32, 8));
    ir.WriteLocal(word, stored);
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

void Translator::EmitUniformLogicLut() {
    // ULOP3.LUT: the uniform-file version of LOP3, into a uniform register. Bit 91 marks the form,
    // and index 63 of the uniform file is the zero register.
    const u32 target = current.Bits(16, 8);
    if (target >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform LUT destination"};
    }
    const auto uniform_or_zero = [&](u32 index) -> IR::U32 {
        if (index == 63) {
            return ir.Imm32(0);
        }
        return Uniform(index);
    };
    const IR::U32 a = uniform_or_zero(current.Bits(24, 8));
    const IR::U32 b{current.Bits(0, 12) == 0x892 ? ir.Imm32(current.words[1])
                                                 : uniform_or_zero(current.Bits(32, 8))};
    const IR::U32 c = uniform_or_zero(current.Bits(64, 8));
    IR::U32 value = BitwiseLut3(current.Bits(72, 8), a, b, c);
    const u32 guard = current.Bits(12, 4);
    if ((guard & 7) != 7) {
        IR::U1 enabled = UniformPredicate(guard & 7);
        if ((guard & 8) != 0) {
            enabled = IR::U1{ir.LogicalNot(enabled)};
        }
        value = IR::U32{ir.Select(enabled, value, Uniform(target))};
    }
    ir.SetReg(UniformRegister(target), value);
}

void Translator::EmitUniformMultiplyAdd() {
    // UIMAD: a 32-bit uniform multiply-add.
    const u32 target = current.Bits(16, 8);
    if (target >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform multiply mode"};
    }
    const IR::U32 product{ir.IMul(Uniform(current.Bits(24, 8)), ir.Imm32(current.words[1]))};
    const u32 addend = current.Bits(64, 8);
    IR::U32 value{ir.IAdd(product, addend == 255 ? ir.Imm32(0) : Uniform(addend))};
    const u32 guard = current.Bits(12, 4);
    if ((guard & 7) != 7) {
        IR::U1 enabled = UniformPredicate(guard & 7);
        if ((guard & 8) != 0) {
            enabled = IR::U1{ir.LogicalNot(enabled)};
        }
        value = IR::U32{ir.Select(enabled, value, Uniform(target))};
    }
    ir.SetReg(UniformRegister(target), value);
}

void Translator::EmitUniformWideMultiplyAdd() {
    // UIMAD.WIDE.U32: a 64-bit multiply-add into a uniform register pair.
    const u32 target = current.Bits(16, 8);
    const u32 addend = current.Bits(64, 8);
    if (target + 1 >= NUM_UNIFORM_REGISTERS || addend + 1 >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform wide multiply mode"};
    }
    // IR has no 64-bit multiply at all (IMul is 32-bit only), but the multiplier is a constant, so
    // the wide product is the sum of the shifted multiplicand for each set bit.
    const u32 multiplier = current.words[1];
    const IR::U64 a{ir.UConvert(64, Uniform(current.Bits(24, 8)))};
    IR::U64 product{ir.Imm64(u64{0})};
    for (u32 bit = 0; bit < 32; ++bit) {
        if (((multiplier >> bit) & 1) == 0) {
            continue;
        }
        product = IR::U64{
            ir.IAdd(product, IR::U64{ir.ShiftLeftLogical(a, ir.Imm32(bit))})};
    }
    const IR::U64 pair = ir.PackUint2x32(
        ir.CompositeConstruct(Uniform(addend), Uniform(addend + 1)));
    const IR::Value sum{ir.IAdd(product, pair)};
    const IR::Value halves{ir.UnpackUint2x32(IR::U64{sum})};
    ir.SetReg(UniformRegister(target), IR::U32{ir.CompositeExtract(halves, 0)});
    ir.SetReg(UniformRegister(target + 1), IR::U32{ir.CompositeExtract(halves, 1)});
}

void Translator::EmitUniformSelect() {
    // USEL: pick between two uniform sources on a uniform predicate.
    const u32 target = current.Bits(16, 8);
    const u32 condition = current.Bits(87, 3);
    if (target >= NUM_UNIFORM_REGISTERS || condition >= NUM_UNIFORM_PREDICATES ||
        (current.words[2] & ~0x07800000U) != 0x08000000U) {
        throw Unsupported{"native uniform select mode"};
    }
    const IR::U32 a = Uniform(current.Bits(24, 8));
    const IR::U32 b{current.Bits(0, 12) == 0x887 ? ir.Imm32(current.words[1])
                                                 : Uniform(current.Bits(32, 8))};
    IR::U1 select = UniformPredicate(condition);
    if (current.Bits(90, 1) != 0) {
        select = IR::U1{ir.LogicalNot(select)};
    }
    IR::U32 value{ir.Select(select, a, b)};
    const u32 guard = current.Bits(12, 4);
    if ((guard & 7) != 7) {
        IR::U1 enabled = UniformPredicate(guard & 7);
        if ((guard & 8) != 0) {
            enabled = IR::U1{ir.LogicalNot(enabled)};
        }
        value = IR::U32{ir.Select(enabled, value, Uniform(target))};
    }
    ir.SetReg(UniformRegister(target), value);
}

void Translator::EmitUniformAdd() {
    // UIADD3 with an immediate addend: the uniform-file version of IADD3.
    const u32 target = current.Bits(16, 8);
    if (target >= NUM_UNIFORM_REGISTERS || (current.words[2] & ~0xffU) != 0x0fffe000U) {
        throw Unsupported{"native uniform add mode"};
    }
    IR::U32 value{ir.IAdd(Uniform(current.Bits(24, 8)), ir.Imm32(current.words[1]))};
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

void Translator::EmitUniformSetPredicate() {
    // UISETP: compare two uniform registers and combine the result with a uniform predicate.
    const u32 output = current.Bits(81, 3);
    const u32 combine = current.Bits(87, 3);
    const u32 combine_op = current.Bits(74, 2);
    if (output >= NUM_UNIFORM_PREDICATES || current.Bits(84, 3) != 7 || current.Bits(72, 1) ||
        combine_op > 2) {
        throw Unsupported{"native uniform comparison mode"};
    }
    const IR::U32 a = Uniform(current.Bits(24, 8));
    const IR::U32 b{current.Bits(0, 12) == 0x88c ? ir.Imm32(current.words[1])
                                                 : Uniform(current.Bits(32, 8))};
    const IR::U1 comparison = Maxwell::IntegerCompare(
        ir, a, b, static_cast<Maxwell::CompareOp>(current.Bits(76, 3)),
        current.Bits(73, 1) != 0);
    const IR::U1 accumulated{combine == 7
                                 ? ir.Imm1(true)
                                 : UniformPredicate(combine)};
    IR::U1 result = Maxwell::PredicateCombine(
        ir, comparison, accumulated, static_cast<Maxwell::BooleanOp>(combine_op));

    // A uniform instruction is guarded by a uniform predicate.
    const u32 guard = current.Bits(12, 4);
    if ((guard & 7) != 7) {
        IR::U1 condition = UniformPredicate(guard & 7);
        if ((guard & 8) != 0) {
            condition = IR::U1{ir.LogicalNot(condition)};
        }
        result = IR::U1{ir.Select(condition, result, UniformPredicate(output))};
    }
    SetUniformPredicate(output, result);
}

void Translator::EmitUniformShift() {
    // USHF on uniform registers: the same word forms as the immediate SHF, read from the uniform
    // file. The shift amount is the immediate in word 1.
    const u32 target = current.Bits(16, 8);
    const u32 low = current.Bits(24, 8);
    const u32 high = current.Bits(64, 8);
    const u32 shift = current.words[1];
    if (target >= NUM_UNIFORM_REGISTERS || shift > 32) {
        throw Unsupported{"native uniform shift mode"};
    }
    const IR::U32 amount = ir.Imm32(shift);
    const IR::U32 word{[&]() -> IR::U32 {
        // Right shift of the high word: USHF.R.U32.HI URd, URZ, n, URb.
        if (current.words[2] == (0x08011600U | high) && low == 63) {
            return Uniform(high);
        }
        // Right shift of one register: USHF.R.U32 URd, URa, n, URZ.
        if (current.words[2] == 0x0800163fU) {
            return Uniform(low);
        }
        // Left shift: USHF.L.U32 URd, URa, n, URZ.
        if (current.words[2] == 0x0800063fU && high == 63) {
            return Uniform(low);
        }
        throw Unsupported{"native uniform shift mode"};
    }()};
    const bool left = current.words[2] == 0x0800063fU;
    ir.SetReg(UniformRegister(target),
              left ? IR::U32{ir.ShiftLeftLogical(word, amount)}
                   : IR::U32{ir.ShiftRightLogical(word, amount)});
}

void Translator::EmitRegisterToUniform() {
    // R2UR: broadcast the value of the first lane that satisfies the execution predicate into a
    // uniform register.
    const u32 target = current.Bits(16, 8);
    if (target >= NUM_UNIFORM_REGISTERS || current.words[1] != 0 ||
        current.words[2] != 0x000e0000U) {
        throw Unsupported{"native register-to-uniform mode"};
    }
    const Guard guard = DecodeGuard(current);
    const IR::U1 condition{guard ? ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated)
                                 : ir.Imm1(true)};
    const IR::U32 ballot = ir.SubgroupBallot(condition);
    // The lowest set lane of the ballot, isolated so a single-bit value can be measured with
    // FindUMsb: for that value the highest bit *is* the lane index.
    const IR::U32 lowest{ir.BitwiseAnd(ballot, IR::U32{ir.INeg(ballot)})};
    const IR::U32 lane = ir.FindUMsb(lowest);
    const IR::U32 value = Gpr(current.Bits(24, 8));
    ir.SetReg(UniformRegister(target),
              ir.ShuffleIndex(value, lane, ir.Imm32(31), ir.Imm32(0)));
}

void Translator::EmitEffectiveAddress() {
    // LEA: Rd = Rb + (Ra << s). LEA.HI takes the high word of the 64-bit shift instead, and its
    // optional carry predicate (.X) adds the carry of the low half.
    const u32 opcode = current.Bits(0, 12);
    const u32 shift = current.Bits(75, 5);
    const u32 high = current.Bits(64, 8);
    const bool extended = current.Bits(74, 1) != 0;
    if (current.Bits(72, 1) != 0 || (high != 255 && current.Bits(73, 1)) != 0) {
        throw Unsupported{"native effective address mode"};
    }
    const IR::U32 a = Gpr(current.Bits(24, 8));
    const IR::U32 b{[&]() -> IR::U32 {
        switch (opcode) {
        case 0x811:
            return ir.Imm32(current.words[1]);
        case 0xa11: {
            const u32 bank = ConstantBufferBank(current.Bits(54, 5));
            if (bank >= 18 || (current.Bits(38, 16) % 4) != 0) {
                throw Unsupported{"native constant buffer address"};
            }
            return ir.GetCbuf(ir.Imm32(bank), ir.Imm32(current.Bits(38, 16)));
        }
        case 0xc11:
            if (current.words[1] >= NUM_UNIFORM_REGISTERS) {
                throw Unsupported{"native uniform LEA register"};
            }
            return Uniform(current.words[1]);
        default:
            return Gpr(current.Bits(32, 8));
        }
    }()};

    if (current.Bits(80, 1) == 0) {
        // Plain LEA: a scaled addend, optionally with a carry out into bits 81-83 (the low half of
        // a 64-bit address; the next IADD3.X consumes it).
        // Bits 84-86 name a second output that the lowering ignores, and its programs render
        // correctly with that field dropped, so the native path drops it too rather than inventing
        // a meaning for it.
        const bool uniform_form = opcode == 0xc11;
        if (current.Bits(63, 1) || current.Bits(62, 1) ||
            (current.Bits(91, 1) != 0 && !uniform_form)) {
            throw Unsupported{"native effective address mode"};
        }
        const IR::U64 wide_b{ir.PackUint2x32(ir.CompositeConstruct(b, ir.Imm32(0)))};
        const IR::U64 wide_a{ir.PackUint2x32(ir.CompositeConstruct(a, ir.Imm32(0)))};
        const IR::U64 sum{ir.IAdd(wide_b, IR::U64{ir.ShiftLeftLogical(wide_a, ir.Imm32(shift))})};
        const IR::Value halves{ir.UnpackUint2x32(sum)};
        WriteUint(IR::U32{ir.CompositeExtract(halves, 0)});
        const u32 carry = current.Bits(81, 3);
        if (carry != 7) {
            IR::U1 condition{ir.INotEqual(IR::U32{ir.CompositeExtract(halves, 1)}, ir.Imm32(0))};
            const Guard guard = DecodeGuard(current);
            if (guard) {
                condition = IR::U1{ir.Select(
                    ir.GetPred(static_cast<IR::Pred>(guard.index), guard.negated), condition,
                    ir.GetPred(static_cast<IR::Pred>(carry)))};
            }
            ir.SetPred(static_cast<IR::Pred>(carry), condition);
        }
        return;
    }
    // LEA.HI: the high word of the 64-bit product-shift.
    if (high != 255) {
        // A funnel shift with a third register; not captured yet.
        throw Unsupported{"native high effective address mode"};
    }
    const IR::U64 wide = ir.PackUint2x32(ir.CompositeConstruct(a, ir.Imm32(0)));
    const IR::U64 shifted{ir.ShiftLeftLogical(wide, ir.Imm32(shift))};
    const IR::U32 high_word{ir.CompositeExtract(ir.UnpackUint2x32(shifted), 1)};
    const IR::U32 addend{[&]() -> IR::U32 {
        if (!extended) {
            return b;
        }
        const u32 carry = current.Bits(87, 3);
        const IR::U1 condition{carry == 7 ? ir.Imm1(true)
                                          : ir.GetPred(static_cast<IR::Pred>(carry),
                                                       current.Bits(90, 1) != 0)};
        return IR::U32{ir.Select(condition, ir.Imm32(1), ir.Imm32(0))};
    }()};
    WriteUint(IR::U32{ir.IAdd(b, IR::U32{ir.IAdd(high_word, addend)})});
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

void Translator::EmitIntegerMinMax() {
    // IMNMX: min or max of two 32-bit sources, chosen by the predicate in bits 87-90 with the
    // negation flag in bit 90, exactly like FMNMX. Bit 73 selects the signed comparison; for this
    // instruction bits 72-73 are the instruction's own flags, not source modifiers.
    if ((current.words[2] & ~0x0f800200U) != 0) {
        throw Unsupported{"native integer min/max mode"};
    }
    const IR::U32 a = ReadUint(0, false);
    const IR::U32 b{current.Bits(0, 12) == 0x817 ? ir.Imm32(current.words[1]) : ReadUint(1)};
    if (current.Bits(0, 12) == 0xa17 && (current.words[2] & ~0x0f800200U) != 0) {
        throw Unsupported{"native integer min/max mode"};
    }
    const bool signed_order = current.Bits(73, 1) != 0;
    const IR::U32 minimum{signed_order ? IR::U32{ir.SMin(a, b)} : IR::U32{ir.UMin(a, b)}};
    const IR::U32 maximum{signed_order ? IR::U32{ir.SMax(a, b)} : IR::U32{ir.UMax(a, b)}};
    const u32 predicate = current.Bits(87, 3);
    const bool negated = current.Bits(90, 1) != 0;
    const IR::U1 condition{predicate == 7 ? ir.Imm1(!negated)
                                          : ir.GetPred(static_cast<IR::Pred>(predicate), negated)};
    WriteUint(IR::U32{ir.Select(condition, negated ? maximum : minimum,
                                 negated ? minimum : maximum)});
}

void Translator::EmitFindLeadingOne() {
    // FLO.U32: the index of the most significant bit, or all ones when the value is zero. The .SH
    // form XORs the result with 31 first so it can be used as a normalisation shift, which is what
    // the Maxwell FLO.SH the shipping path emits does.
    if (current.Bits(9, 3) != 1 || (current.words[2] & ~0x000e0600U) != 0 ||
        (current.words[1] & 0xffffff00U) != 0) {
        throw Unsupported{"native leading-one mode"};
    }
    const IR::U32 source = Gpr(current.Bits(32, 8));
    IR::U32 result{current.Bits(73, 1) ? ir.FindSMsb(source) : ir.FindUMsb(source)};
    if (current.Bits(74, 1)) {
        const IR::U1 not_found{ir.IEqual(result, ir.Imm32(0xffffffffU))};
        result = IR::U32{ir.Select(not_found, result, IR::U32{ir.BitwiseXor(result, ir.Imm32(31))})};
    }
    WriteUint(result);
}

void Translator::EmitBitReverse() {
    // BREV: the bit order of the source reversed.
    if (current.Bits(9, 3) != 1 || (current.words[2] & ~0xffU) != 0 ||
        (current.words[1] & 0xffffff00U) != 0) {
        throw Unsupported{"native bit reverse mode"};
    }
    WriteUint(ir.BitReverse(Gpr(current.Bits(32, 8))));
}

void Translator::EmitPermuteBytes() {
    // PRMT: a byte permute of two sources. The selector is the immediate in bits 32-63 and the
    // second source is bits 64-71, where 255 means RZ. A selector nibble's bit 3 replicates that
    // byte's sign bit instead of selecting it, as in the Maxwell instruction.
    if ((current.words[2] & ~0xffU) != 0) {
        throw Unsupported{"native byte permute mode"};
    }
    const IR::U32 selector = ir.Imm32(current.words[1]);
    const IR::U32 first = ReadUint(0, false);
    const u32 second_index = current.Bits(64, 8);
    const IR::U32 second{second_index == 255 ? ir.Imm32(0) : Gpr(second_index)};
    IR::U32 result{ir.Imm32(0)};
    for (u32 byte = 0; byte < 4; ++byte) {
        const IR::U32 nibble{ir.BitFieldExtract(selector, ir.Imm32(byte * 4), ir.Imm32(4))};
        const IR::U32 index{ir.BitwiseAnd(nibble, ir.Imm32(7))};
        const IR::U32 source{ir.Select(ir.ILessThan(index, ir.Imm32(4), false), first, second)};
        const IR::U32 offset{ir.ShiftLeftLogical(ir.BitwiseAnd(index, ir.Imm32(3)), ir.Imm32(3))};
        const IR::U32 value{ir.BitFieldExtract(source, offset, ir.Imm32(8))};
        const IR::U32 sign{
            ir.BitFieldExtract(source, ir.IAdd(offset, ir.Imm32(7)), ir.Imm32(1), true)};
        const IR::U1 replicate{ir.INotEqual(ir.BitwiseAnd(nibble, ir.Imm32(8)), ir.Imm32(0))};
        const IR::U32 picked{ir.Select(replicate, sign, value)};
        result = ir.BitFieldInsert(result, picked, ir.Imm32(byte * 8), ir.Imm32(8));
    }
    WriteUint(result);
}

void Translator::EmitFunnelShift() {
    // SHF.L.U32.HI: the high word of {zero, Rb} shifted left, with the shift amount in bits 32-63.
    // The .W flag wraps that amount to five bits; without it the amount saturates at 32, which is
    // what the shipping lowering does, so the 0xffffffff idiom is a plain register pass.
    if (current.Bits(24, 8) != 255 || (current.words[2] & ~0x8ffU) != 0x00010600U ||
        (!current.Bits(75, 1) && (current.words[3] & 0xc00001ffU) != 0)) {
        throw Unsupported{"native funnel shift mode"};
    }
    const u32 amount = current.Bits(75, 1) ? (current.words[1] & 31)
                                           : std::min(current.words[1], 32U);
    const IR::U64 wide{ir.PackUint2x32(ir.CompositeConstruct(Gpr(current.Bits(64, 8)), ir.Imm32(0)))};
    const IR::Value halves{ir.UnpackUint2x32(IR::U64{ir.ShiftLeftLogical(wide, ir.Imm32(amount))})};
    WriteUint(IR::U32{ir.CompositeExtract(halves, 1)});
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
    const u32 opcode = current.Bits(0, 12);
    if (opcode != 0x306 && opcode != 0xd06) {
        // I2FP.F32: a 32-bit source converted to float. Bit 74 picks the signed form, bits 78-79 the
        // rounding mode.
        if ((current.words[2] & ~(0x400U | 0xc000U | (opcode == 0xc45 ? 0x08000000U : 0U))) !=
            0x00201000U) {
            throw Unsupported{"native integer-to-float format"};
        }
        const IR::FpControl control{
            .no_contraction = false,
            .rounding = ROUNDING[current.Bits(78, 2)],
            .fmz_mode = IR::FmzMode::DontCare,
        };
        WriteFloat(IR::F32{ir.ConvertIToF(32, 32, current.Bits(74, 1) != 0, ReadUint(1), control)});
        return;
    }
    // I2F.U8 / I2F.S8: a byte selected from the source, converted to float. Bit 74 picks the
    // signed form.
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

void Translator::EmitFloatToInteger() {
    // F2I word form: round a float and convert it to a 32-bit integer, saturating at the ends and
    // folding NaN to zero. Bits 78-79 pick the rounding (the Maxwell rounding enumeration), bit 72
    // the signedness and bit 80 the denormal mode; the source modifiers are bits 62-63.
    if ((current.words[2] & ~0x0001c100U) != 0x00203000U) {
        throw Unsupported{"native float-to-integer conversion mode"};
    }
    const IR::FpControl control{
        .no_contraction = true,
        .rounding = IR::FpRounding::DontCare,
        .fmz_mode = current.Bits(80, 1) ? IR::FmzMode::FTZ : IR::FmzMode::None,
    };
    const IR::F32 source = ReadFloat(1);
    const IR::F32 rounded{[&]() -> IR::F32 {
        switch (current.Bits(78, 2)) {
        case 0:
            return IR::F32{ir.FPRoundEven(source, control)};
        case 1:
            return IR::F32{ir.FPFloor(source, control)};
        case 2:
            return IR::F32{ir.FPCeil(source, control)};
        default:
            return IR::F32{ir.FPTrunc(source, control)};
        }
    }()};
    const bool is_signed = current.Bits(72, 1) != 0;
    const IR::F32 low = ir.Imm32(is_signed ? -2147483648.0f : 0.0f);
    const IR::F32 high = ir.Imm32(is_signed ? 2147483520.0f : 4294967296.0f);
    const IR::F32 clamped{ir.FPClamp(rounded, low, high)};
    const IR::U32 converted{is_signed ? IR::U32{ir.ConvertFToS(32, clamped)}
                                      : IR::U32{ir.ConvertFToU(32, clamped)}};
    WriteUint(IR::U32{ir.Select(ir.FPIsNan(source), ir.Imm32(0), converted)});
}

void Translator::EmitUniformMultiplyAddRegister() {
    // UIMAD with a uniform multiplier: URd = URa * URb + URc. The 0x2a5 form writes the 64-bit
    // product plus a register pair, so the multiply is expanded into four 16-bit partial products:
    // IR has no 64-bit multiply and a 32-bit one would truncate the cross terms.
    const u32 target = current.Bits(16, 8);
    const u32 addend = current.Bits(64, 8);
    const bool wide = current.Bits(0, 12) == 0x2a5;
    if (target >= NUM_UNIFORM_REGISTERS || current.Bits(24, 8) >= NUM_UNIFORM_REGISTERS ||
        current.Bits(32, 8) >= NUM_UNIFORM_REGISTERS ||
        (current.words[1] & ~0xffU) != 0 ||
        (current.words[2] & ~(wide ? 0xffU : 0x080002ffU)) != (wide ? 0x0f8e0000U : 0x078e0000U)) {
        throw Unsupported{"native uniform multiply mode"};
    }
    const IR::U32 a = Uniform(current.Bits(24, 8));
    const IR::U32 b = Uniform(current.Bits(32, 8));
    if (!wide) {
        if (addend >= NUM_UNIFORM_REGISTERS) {
            throw Unsupported{"native uniform multiply mode"};
        }
        const IR::U32 c{addend == 255 ? ir.Imm32(0) : Uniform(addend)};
        IR::U32 value{ir.IAdd(IR::U32{ir.IMul(a, b)}, c)};
        const u32 guard = current.Bits(12, 4);
        if ((guard & 7) != 7) {
            IR::U1 enabled = UniformPredicate(guard & 7);
            if ((guard & 8) != 0) {
                enabled = IR::U1{ir.LogicalNot(enabled)};
            }
            value = IR::U32{ir.Select(enabled, value, Uniform(target))};
        }
        ir.SetReg(UniformRegister(target), value);
        return;
    }
    if (target + 1 >= NUM_UNIFORM_REGISTERS || (target % 2) != 0 ||
        (addend != 255 && (addend % 2) != 0) || addend + 1 >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform wide multiply mode"};
    }
    const auto high = [&](const IR::U32& value) {
        return IR::U32{ir.ShiftRightLogical(value, ir.Imm32(16))};
    };
    const auto low = [&](const IR::U32& value) {
        return IR::U32{ir.BitwiseAnd(value, ir.Imm32(0xffffU))};
    };
    const auto widen = [&](const IR::U32& value) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(value, ir.Imm32(0)))};
    };
    const IR::U64 product{ir.IAdd(
        ir.IAdd(widen(IR::U32{ir.IMul(low(a), low(b))}),
                IR::U64{ir.ShiftLeftLogical(
                    widen(IR::U32{ir.IMul(low(a), high(b))}), ir.Imm32(16))}),
        ir.IAdd(IR::U64{ir.ShiftLeftLogical(widen(IR::U32{ir.IMul(high(a), low(b))}),
                                            ir.Imm32(16))},
                IR::U64{ir.ShiftLeftLogical(widen(IR::U32{ir.IMul(high(a), high(b))}),
                                            ir.Imm32(32))}))};
    const IR::U64 pair = ir.PackUint2x32(ir.CompositeConstruct(
        addend == 255 ? ir.Imm32(0) : Uniform(addend),
        addend == 255 ? ir.Imm32(0) : Uniform(addend + 1)));
    const IR::Value halves{ir.UnpackUint2x32(IR::U64{ir.IAdd(product, pair)})};
    ir.SetReg(UniformRegister(target), IR::U32{ir.CompositeExtract(halves, 0)});
    ir.SetReg(UniformRegister(target + 1), IR::U32{ir.CompositeExtract(halves, 1)});
}

void Translator::EmitSignExtend() {
    // SGXT.U32 / SGXT.S32: extend from the given bit, which is a logical or arithmetic shift by that
    // amount - the same SHR the shipping lowering emits, with bit 73 selecting the signed shift.
    const u32 shift = current.words[1];
    if ((current.words[2] & ~0x200U) != 0 || shift > 32 || current.Bits(16, 8) >= 255) {
        throw Unsupported{"native sign extend mode"};
    }
    const IR::U32 source = Gpr(current.Bits(24, 8));
    WriteUint(current.Bits(73, 1) ? IR::U32{ir.ShiftRightArithmetic(source, ir.Imm32(shift))}
                                  : IR::U32{ir.ShiftRightLogical(source, ir.Imm32(shift))});
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

void Translator::EmitVote() {
    // VOTE: the warp's ballot into a register plus a predicate result. Bits 72-73 pick ALL, ANY or
    // EQ (whether every active lane matches this one), the input predicate is bits 87-90 and the
    // output predicate bits 81-83, where 7 means discard.
    if (current.words[1] != 0 || current.Bits(72, 2) > 2 || (current.words[2] & ~0x078e0300U) != 0) {
        throw Unsupported{"native vote mode"};
    }
    const u32 input = current.Bits(87, 3);
    const IR::U1 condition{input == 7 ? ir.Imm1(true)
                                      : ir.GetPred(static_cast<IR::Pred>(input),
                                                   current.Bits(90, 1) != 0)};
    const u32 dest = current.Bits(16, 8);
    if (dest == 63) {
        throw Unsupported{"native vote destination"};
    }
    WriteUint(ir.SubgroupBallot(condition));
    const u32 output = current.Bits(81, 3);
    if (output != 7) {
        ir.SetPred(static_cast<IR::Pred>(output), VoteResult(current.Bits(72, 2), condition));
    }
}

void Translator::EmitUniformVote() {
    // VOTEU: the same vote, but the ballot goes to a uniform register (63 means discard) and the
    // result to a uniform predicate.
    if (current.Bits(24, 8) != 0 || current.words[1] != 0 ||
        (current.words[2] & ~0x078e0300U) != 0 || current.Bits(72, 2) > 2 ||
        current.Bits(12, 4) != 7) {
        throw Unsupported{"native uniform vote mode"};
    }
    const u32 input = current.Bits(87, 3);
    const IR::U1 condition{input == 7 ? ir.Imm1(true)
                                      : ir.GetPred(static_cast<IR::Pred>(input),
                                                   current.Bits(90, 1) != 0)};
    const u32 dest = current.Bits(16, 8);
    if (dest != 63) {
        if (dest >= NUM_UNIFORM_REGISTERS) {
            throw Unsupported{"native uniform vote destination"};
        }
        ir.SetReg(UniformRegister(dest), ir.SubgroupBallot(condition));
    }
    const u32 output = current.Bits(81, 3);
    if (output != 7) {
        if (output >= NUM_UNIFORM_PREDICATES) {
            throw Unsupported{"native uniform vote predicate"};
        }
        SetUniformPredicate(output, VoteResult(current.Bits(72, 2), condition));
    }
}

IR::U1 Translator::VoteResult(u32 mode, const IR::U1& condition) {
    switch (mode) {
    case 0:
        return ir.VoteAll(condition);
    case 1:
        return ir.VoteAny(condition);
    case 2:
        return ir.VoteEqual(condition);
    default:
        throw Unsupported{"native vote mode"};
    }
}

void Translator::EmitGlobalReduction() {
    // REDG: a reduction applied to global memory with no result. Type selects the element (0 unsigned
    // 32-bit, 1 signed 32-bit, 3 float) and the operation in bits 87-89 is the same enumeration the
    // Maxwell RED uses. The address is a register pair, or a uniform register pair when the register
    // field is 255.
    const u32 type = current.Bits(73, 3);
    const u32 operation = current.Bits(87, 3);
    const bool uniform_address = current.Bits(24, 8) == 255;
    const u32 variable = 0x03800e00U | (uniform_address ? 0x3fU : 0U);
    const u32 mode = uniform_address ? 0x0c10e100U : 0x0010e100U;
    if ((current.words[2] & ~variable) != mode ||
        (type != 0 && type != 1 && type != 3) || (type == 3 && operation != 0)) {
        throw Unsupported{"native global reduction mode"};
    }
    const IR::U64 pointer = AtomicPointer(current.Bits(40, 24));
    const IR::U32 value = Gpr(current.Bits(32, 8));
    static_cast<void>(ApplyAtomic(type, operation, pointer, value, false));
}

void Translator::EmitGlobalAtomic() {
    // ATOMG: the same operations as REDG, but the old value is returned into the destination
    // register. 0x3a8 addresses through a register pair and 0x9a8 through a uniform register pair.
    const u32 operation = current.Bits(87, 3);
    const bool uniform_pointer = current.Bits(0, 12) == 0x9a8;
    if (uniform_pointer ? ((current.words[2] & ~0x3fU) != 0x081ee140U ||
                           current.Bits(24, 8) != 255)
                        : ((current.words[2] & ~0x07800000U) != 0x001ee100U ||
                           current.Bits(24, 8) >= 246)) {
        throw Unsupported{"native global atomic mode"};
    }
    const IR::U64 pointer = AtomicPointer(current.Bits(40, 24));
    const IR::U32 value = Gpr(current.Bits(32, 8));
    WriteUint(IR::U32{ApplyAtomic(0, operation, pointer, value, true)});
}

IR::U64 Translator::AtomicPointer(u32 raw_offset) {
    // A signed 20-bit byte offset added to the pair. 0x9a8 always addresses through a uniform pair,
    // 0x98e and 0x3a8 through one when the register field is 255, and 0x3a8 through a GPR pair.
    const u32 opcode = current.Bits(0, 12);
    const bool uniform_pair = opcode == 0x9a8 || (opcode == 0x98e && current.Bits(24, 8) == 255);
    const s32 offset = static_cast<s32>(raw_offset << 8) >> 8;
    if (offset < -0x80000 || offset > 0x7ffff) {
        throw Unsupported{"native global atomic offset"};
    }
    const IR::U64 step = OffsetU64(static_cast<u32>(offset), offset < 0);
    if (uniform_pair) {
        const u32 index = current.Bits(64, 6);
        if (index + 1 >= NUM_UNIFORM_REGISTERS) {
            throw Unsupported{"native global atomic address"};
        }
        const IR::U64 base = ir.PackUint2x32(
            ir.CompositeConstruct(Uniform(index), Uniform(index + 1)));
        return ir.IAdd(base, step);
    }
    const u32 index = current.Bits(24, 8);
    if ((index % 2) != 0 || index + 1 >= 246) {
        throw Unsupported{"native global atomic address"};
    }
    const IR::U64 base = ir.PackUint2x32(ir.CompositeConstruct(Gpr(index), Gpr(index + 1)));
    return ir.IAdd(base, step);
}

IR::Value Translator::ApplyAtomic(u32 type, u32 operation, const IR::U64& pointer,
                                  const IR::U32& value, bool) {
    if (type == 3) {
        const IR::F32 stored{ir.BitCast<IR::F32>(value)};
        const IR::FpControl control{
            .no_contraction = false,
            .rounding = IR::FpRounding::RN,
            .fmz_mode = IR::FmzMode::FTZ,
        };
        return ir.GlobalAtomicF32Add(pointer, stored, control);
    }
    const bool is_signed = type == 1;
    switch (operation) {
    case 0:
        return ir.GlobalAtomicIAdd(pointer, value);
    case 1:
        return ir.GlobalAtomicIMin(pointer, value, is_signed);
    case 2:
        return ir.GlobalAtomicIMax(pointer, value, is_signed);
    case 5:
        return ir.GlobalAtomicAnd(pointer, value);
    case 6:
        return ir.GlobalAtomicOr(pointer, value);
    case 7:
        return ir.GlobalAtomicXor(pointer, value);
    default:
        throw Unsupported{"native global atomic operation"};
    }
}

void Translator::EmitUniformAddThree() {
    // UIADD3 over the uniform file. The plain form sums three uniform sources (or an immediate) and
    // can write one or two carry predicates, which the 64-bit sum decides; the .X form adds two
    // carry-in predicates instead. Both mirror the register IADD3 exactly.
    const u32 target = current.Bits(16, 8);
    const u32 opcode = current.Bits(0, 12);
    if (target >= NUM_UNIFORM_REGISTERS) {
        throw Unsupported{"native uniform add mode"};
    }
    const auto read = [&](u32 index) -> IR::U32 {
        if (index == 63) {
            return ir.Imm32(0);
        }
        if (index >= NUM_UNIFORM_REGISTERS) {
            throw Unsupported{"native uniform add register"};
        }
        return Uniform(index);
    };
    const auto carry_in = [&](u32 field, u32 negate) -> IR::U32 {
        const u32 index = current.Bits(field, 3);
        const bool negated = current.Bits(negate, 1) != 0;
        const IR::U1 condition{index == 7 ? ir.Imm1(!negated) : UniformPredicate(index)};
        return IR::U32{ir.Select(condition, ir.Imm32(1), ir.Imm32(0))};
    };
    const IR::U32 a = read(current.Bits(24, 8));
    const IR::U32 b{opcode == 0x890 ? ir.Imm32(current.words[1]) : read(current.Bits(32, 8))};
    const IR::U32 c = read(current.Bits(64, 8));
    // A uniform guard keeps the target's old value when the predicate is false.
    const u32 guard = current.Bits(12, 4);
    const auto guarded = [&](const IR::U32& value) -> IR::U32 {
        if ((guard & 7) == 7) {
            return value;
        }
        IR::U1 enabled = UniformPredicate(guard & 7);
        if ((guard & 8) != 0) {
            enabled = IR::U1{ir.LogicalNot(enabled)};
        }
        return IR::U32{ir.Select(enabled, value, Uniform(target))};
    };

    if (current.Bits(74, 1) != 0) {
        // .X: the carry chain continues a previous add.
        if (current.Bits(81, 6) != 0x3f) {
            throw Unsupported{"native uniform add carry mode"};
        }
        const IR::U32 first = carry_in(77, 80);
        const IR::U32 second = carry_in(87, 90);
        const auto widen = [&](const IR::U32& value) {
            return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(value, ir.Imm32(0)))};
        };
        const IR::U64 sum{ir.IAdd(
            IR::U64{ir.IAdd(IR::U64{ir.IAdd(widen(a), widen(b))}, widen(c))},
            IR::U64{ir.IAdd(widen(first), widen(second))})};
        const IR::Value halves{ir.UnpackUint2x32(sum)};
        ir.SetReg(UniformRegister(target),
                  guarded(IR::U32{ir.CompositeExtract(halves, 0)}));
        return;
    }

    const auto widen = [&](const IR::U32& value) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(value, ir.Imm32(0)))};
    };
    const IR::U64 sum{ir.IAdd(IR::U64{ir.IAdd(widen(a), widen(b))}, widen(c))};
    const IR::Value halves{ir.UnpackUint2x32(sum)};
    ir.SetReg(UniformRegister(target), guarded(IR::U32{ir.CompositeExtract(halves, 0)}));
    const auto carry_out = [&](u32 field, u32 threshold) {
        const u32 destination = current.Bits(field, 3);
        if (destination == 7) {
            return;
        }
        if (destination >= NUM_UNIFORM_PREDICATES) {
            throw Unsupported{"native uniform add carry destination"};
        }
        const IR::U32 high{ir.CompositeExtract(halves, 1)};
        SetUniformPredicate(destination,
                            IR::U1{ir.INotEqual(high, ir.Imm32(threshold))});
    };
    carry_out(81, 1);
    carry_out(84, 2);
}

void Translator::EmitUniformEffectiveAddress() {
    // ULEA: LEA over the uniform file - a scaled addend plus an immediate (0x891) or a uniform
    // register (0x291), with an optional carry out. The shipping lowering accepts this shape without
    // checking more than the high-word and carry-bit flags, and programs render correctly with it,
    // so the same fields are used here.
    const u32 target = current.Bits(16, 8);
    const u32 shift = current.Bits(75, 5);
    const bool register_addend = current.Bits(0, 12) == 0x291;
    if (target >= NUM_UNIFORM_REGISTERS || current.Bits(80, 1) != 0 ||
        current.Bits(74, 1) != 0 || current.Bits(91, 1) == 0) {
        throw Unsupported{"native uniform effective address mode"};
    }
    const auto read = [&](u32 index) -> IR::U32 {
        if (index == 63) {
            return ir.Imm32(0);
        }
        if (index >= NUM_UNIFORM_REGISTERS) {
            throw Unsupported{"native uniform effective address register"};
        }
        return Uniform(index);
    };
    const u32 addend = current.Bits(32, 8);
    const IR::U32 a = read(current.Bits(24, 8));
    const IR::U32 b{register_addend ? read(addend) : ir.Imm32(current.words[1])};
    const IR::U64 wide_a{ir.PackUint2x32(ir.CompositeConstruct(a, ir.Imm32(0)))};
    const IR::U64 sum{ir.IAdd(
        IR::U64{ir.PackUint2x32(ir.CompositeConstruct(b, ir.Imm32(0)))},
        IR::U64{ir.ShiftLeftLogical(wide_a, ir.Imm32(shift))})};
    const IR::Value halves{ir.UnpackUint2x32(sum)};
    ir.SetReg(UniformRegister(target), IR::U32{ir.CompositeExtract(halves, 0)});
    const u32 carry = current.Bits(81, 3);
    if (carry != 7) {
        if (carry >= NUM_UNIFORM_PREDICATES) {
            throw Unsupported{"native uniform effective address carry"};
        }
        SetUniformPredicate(
            carry, IR::U1{ir.INotEqual(IR::U32{ir.CompositeExtract(halves, 1)}, ir.Imm32(0))});
    }
}

void Translator::EmitIndexedConstantLoad() {
    // LDC with an indexed byte offset: a byte or word out of a constant bank at register + offset.
    // Mode 2 is the segmented form, where the register holds the flattened constant-buffer address
    // and the bank moves into the high half.
    const u32 width = current.Bits(73, 3);
    const u32 mode_bits = current.Bits(78, 2);
    const u32 bank = ConstantBufferBank(current.Bits(54, 5));
    const u32 dest = current.Bits(16, 8);
    if (width > 5 || (mode_bits != 0 && mode_bits != 2) || bank >= 18 ||
        (current.words[2] & ~0xce00U) != 0 || (current.words[1] & 0xf800003fU) != 0 ||
        (width == 5 && dest != 255 && (dest & 1) != 0)) {
        throw Unsupported{"native indexed constant load mode"};
    }
    const IR::U32 offset = ir.IAdd(Gpr(current.Bits(24, 8)), ir.Imm32(current.Bits(38, 16)));
    IR::U32 index = ir.Imm32(bank);
    IR::U32 address = offset;
    if (mode_bits == 2) {
        index = IR::U32{ir.IAdd(ir.BitFieldExtract(offset, ir.Imm32(16), ir.Imm32(16)),
                                ir.Imm32(bank))};
        address = IR::U32{ir.BitFieldExtract(offset, ir.Imm32(0), ir.Imm32(16))};
    }
    switch (width) {
    case 0:
        WriteUint(IR::U32{ir.GetCbuf(index, address, 8, false)});
        return;
    case 1:
        WriteUint(IR::U32{ir.GetCbuf(index, address, 8, true)});
        return;
    case 2:
        WriteUint(IR::U32{ir.GetCbuf(index, address, 16, false)});
        return;
    case 3:
        WriteUint(IR::U32{ir.GetCbuf(index, address, 16, true)});
        return;
    case 4:
        WriteUint(IR::U32{ir.GetCbuf(index, address, 32, false)});
        return;
    default: {
        // A 64-bit constant pair.
        const IR::Value pair{ir.GetCbuf(index, address, 64, false)};
        if (dest == 255) {
            return;
        }
        if (dest + 1 >= 246) {
            throw Unsupported{"native indexed constant load destination"};
        }
        ir.SetReg(Register(dest), IR::U32{ir.CompositeExtract(pair, 0)});
        ir.SetReg(Register(dest + 1), IR::U32{ir.CompositeExtract(pair, 1)});
        return;
    }
    }
}

void Translator::EmitWarpReduction() {
    // REDUX: a reduction over the warp's active lanes into a uniform register. Inactive lanes
    // contribute the identity (zero, or all ones for a minimum), which is what the shipping lowering
    // does, and the butterfly reduction leaves every lane holding the same value.
    const u32 operation = current.words[2] & ~0x200U;
    const bool maximum = operation == 0x14000U;
    const bool minimum = operation == 0x10000U;
    const bool logical_or = current.words[2] == 0x4000U;
    const bool logical_xor = current.words[2] == 0x8000U;
    const bool signed_order = current.Bits(73, 1) != 0;
    const u32 target = current.Bits(16, 8);
    if (target >= NUM_UNIFORM_REGISTERS || current.words[1] != 0 ||
        (current.words[2] != 0xc000U && !maximum && !minimum && !logical_or && !logical_xor) ||
        (signed_order && !maximum && !minimum)) {
        throw Unsupported{"native warp reduction mode"};
    }
    const IR::U32 active = ir.SubgroupBallot(ir.Imm1(true));
    const IR::U32 own = ir.SubgroupEqMask();
    const IR::U1 enabled{ir.INotEqual(IR::U32{ir.BitwiseAnd(active, own)}, ir.Imm32(0))};
    const IR::U32 source = Gpr(current.Bits(24, 8));
    IR::U32 value{ir.Select(enabled, source, ir.Imm32(minimum ? 0xffffffffU : 0))};
    const auto combine = [&](const IR::U32& a, const IR::U32& b) -> IR::U32 {
        if (logical_or) {
            return IR::U32{ir.BitwiseOr(a, b)};
        }
        if (logical_xor) {
            return IR::U32{ir.BitwiseXor(a, b)};
        }
        if (minimum) {
            return signed_order ? IR::U32{ir.SMin(a, b)} : IR::U32{ir.UMin(a, b)};
        }
        if (maximum) {
            return signed_order ? IR::U32{ir.SMax(a, b)} : IR::U32{ir.UMax(a, b)};
        }
        return IR::U32{ir.IAdd(a, b)};
    };
    for (u32 offset = 16; offset > 0; offset >>= 1) {
        const IR::U32 other = ir.ShuffleButterfly(value, ir.Imm32(offset), ir.Imm32(31),
                                                  ir.Imm32(0));
        value = combine(value, other);
    }
    ir.SetReg(UniformRegister(target), value);
}

void Translator::EmitTextureFetch() {
    // TEX (0xb60): a sample with the descriptor in a constant buffer and the operands packed in
    // register pairs. The operand list is the array index first, then the coordinates, then the
    // LOD/bias, the offsets and the depth reference, the same packing the shipping lowering
    // unpacks into Maxwell TEX's coordinate and meta registers.
    constexpr std::array<u32, 8> COORDINATE_COUNT{1, 2, 3, 3, 2, 3, 0, 4};
    constexpr std::array<TextureType, 8> TEXTURE_TYPE{
        TextureType::Color1D,      TextureType::Color2D,
        TextureType::Color3D,      TextureType::ColorCube,
        TextureType::ColorArray1D, TextureType::ColorArray2D,
        TextureType::Color3D,      TextureType::ColorArrayCube,
    };
    const u32 dest = current.Bits(16, 8);
    const u32 second_dest = current.Bits(64, 8);
    const u32 source_a = current.Bits(24, 8);
    const u32 source_b = current.Bits(32, 8);
    const u32 dimension = current.Bits(61, 3);
    const u32 mask = current.Bits(72, 4);
    const u32 lod_mode = current.Bits(87, 3);
    const bool depth_compare = current.Bits(78, 1) != 0;
    const bool offsets = current.Bits(76, 1) != 0;
    const bool split = second_dest != 255;
    if (dimension == 6 || COORDINATE_COUNT[dimension] == 0) {
        throw Unsupported{"native texture dimension"};
    }
    if (current.Bits(79, 1) != 0) {
        throw Unsupported{"native half-precision texture result"};
    }
    if (current.Bits(81, 3) != 7 || current.Bits(77, 1) != 0 || current.Bits(59, 1) != 0 ||
        current.Bits(60, 1) != 1 || current.Bits(40, 14) > 0x1fff || mask == 0 ||
        (lod_mode == 4 || lod_mode == 5) || (depth_compare && mask != 1)) {
        throw Unsupported{"native texture mode"};
    }
    const auto check_destination = [&](u32 target, u32 width) {
        if (target != 255 && target + width > 245) {
            throw Unsupported{"native texture destination"};
        }
    };
    if (split && mask == 15) {
        // All four components split into two pairs, one per destination register.
        if ((second_dest & 1) != 0 || second_dest + 1 > 245) {
            throw Unsupported{"native texture split destination"};
        }
        check_destination(dest, 1);
    } else if (split) {
        check_destination(dest, 1);
        check_destination(second_dest, 1);
    } else {
        check_destination(dest, 3);
    }

    const u32 coordinates = COORDINATE_COUNT[dimension];
    const bool has_lod = lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7;
    const u32 count = coordinates + (has_lod ? 1 : 0) + (offsets ? 1 : 0) +
                      (depth_compare ? 1 : 0);
    const u32 in_a = count <= 4 ? (count + 1) / 2 : 4;
    const auto slot = [&](u32 i) -> u32 {
        if (i < in_a) {
            return source_a == 255 ? 255U : source_a + i;
        }
        return source_b == 255 ? 255U : source_b + (i - in_a);
    };
    const auto uint = [&](u32 index) -> IR::U32 {
        return index == 255 ? ir.Imm32(0) : Gpr(index);
    };
    const auto floating = [&](u32 index) -> IR::F32 {
        return IR::F32{ir.BitCast<IR::F32>(uint(index))};
    };
    const auto array_index = [&](u32 index) -> IR::F32 {
        const IR::U32 value{uint(index)};
        return IR::F32{ir.ConvertUToF(32, 16, IR::U32{ir.BitFieldExtract(value, ir.Imm32(0),
                                                                        ir.Imm32(16))})};
    };

    IR::Value coords;
    switch (dimension) {
    case 0: // 1D
        coords = floating(slot(0));
        break;
    case 1: // 2D
        coords = ir.CompositeConstruct(floating(slot(0)), floating(slot(1)));
        break;
    case 2: // 3D
    case 3: // CUBE
        coords = ir.CompositeConstruct(floating(slot(0)), floating(slot(1)), floating(slot(2)));
        break;
    case 4: // ARRAY_1D: the array index comes first in the operand list
        coords = ir.CompositeConstruct(floating(slot(1)), array_index(slot(0)));
        break;
    case 5: // ARRAY_2D
        coords = ir.CompositeConstruct(floating(slot(1)), floating(slot(2)),
                                       array_index(slot(0)));
        break;
    default: // ARRAY_CUBE
        coords = ir.CompositeConstruct(floating(slot(1)), floating(slot(2)), floating(slot(3)),
                                       array_index(slot(0)));
        break;
    }

    u32 next = coordinates;
    const IR::F32 bias{has_lod && (lod_mode == 2 || lod_mode == 6) ? floating(slot(next++))
                                                                  : ir.Imm32(0.0f)};
    const IR::F32 explicit_lod{has_lod && (lod_mode == 3 || lod_mode == 7) ? floating(slot(next++))
                                                                           : ir.Imm32(0.0f)};
    IR::Value offset;
    if (offsets) {
        const IR::U32 packed{uint(slot(next++))};
        const auto extract = [&](u32 bit) -> IR::U32 {
            return IR::U32{ir.BitFieldExtract(packed, ir.Imm32(bit), ir.Imm32(4), true)};
        };
        switch (dimension) {
        case 0:
        case 4:
            offset = extract(0);
            break;
        case 1:
        case 5:
            offset = ir.CompositeConstruct(extract(0), extract(4));
            break;
        case 2:
            offset = ir.CompositeConstruct(extract(0), extract(4), extract(8));
            break;
        default:
            throw Unsupported{"native texture offset on a cube map"};
        }
    }
    const IR::F32 dref{depth_compare ? floating(slot(next++)) : ir.Imm32(0.0f)};

    IR::TextureInstInfo info{};
    info.type.Assign(TEXTURE_TYPE[dimension]);
    info.is_depth.Assign(depth_compare ? 1 : 0);
    info.has_bias.Assign(lod_mode == 2 || lod_mode == 6 ? 1 : 0);
    info.has_lod_clamp.Assign(0);
    info.ndv_is_active.Assign(0);

    const IR::Value handle{ir.Imm32(current.Bits(40, 13) * 4)};
    const IR::Value sample{[&]() -> IR::Value {
        if (depth_compare) {
            if (lod_mode == 1 || lod_mode == 3 || lod_mode == 7) {
                return ir.ImageSampleDrefExplicitLod(handle, coords, dref, explicit_lod, offset,
                                                     info);
            }
            return ir.ImageSampleDrefImplicitLod(handle, coords, dref, bias, offset,
                                                 ir.Imm32(0.0f), info);
        }
        if (lod_mode == 1) {
            return ir.ImageSampleExplicitLod(handle, coords, ir.Imm32(0.0f), offset, info);
        }
        if (lod_mode == 3 || lod_mode == 7) {
            return ir.ImageSampleExplicitLod(handle, coords, explicit_lod, offset, info);
        }
        return ir.ImageSampleImplicitLod(handle, coords, bias, offset, ir.Imm32(0.0f), info);
    }()};

    for (u32 element = 0; element < 4; ++element) {
        if (((mask >> element) & 1) == 0) {
            continue;
        }
        const IR::F32 value{depth_compare
                                ? (element < 3 ? IR::F32{sample} : ir.Imm32(1.0f))
                                : IR::F32{ir.CompositeExtract(sample, element)}};
        const u32 target = !split ? (dest == 255 ? 255U : dest + element)
                                  : (element < 2 ? (dest == 255 ? 255U : dest + element)
                                                 : second_dest + (element - 2));
        if (target != 255) {
            if (target >= 246) {
                throw Unsupported{"native texture destination"};
            }
            ir.SetReg(Register(target), ir.BitCast<IR::U32>(value));
        }
    }
}

void Translator::EmitUniformConstantLoad() {
    // ULDC: a constant buffer word or pair into the uniform register file. The indexed form adds a
    // uniform register to the byte offset, in bytes: the Maxwell LDC it lowers to does exactly that
    // (`ir.IAdd(reg, imm_offset)` in load_constant.cpp), so the units are settled by that code.
    const u32 opcode = current.Bits(0, 12);
    const u32 dest = current.Bits(16, 8);
    const u32 width = current.Bits(73, 3);
    const u32 offset = current.Bits(38, 16);
    const u32 bank = ConstantBufferBank(current.Bits(54, 5));
    const bool indexed = opcode == 0xabb;
    if (indexed) {
        if ((current.words[2] & ~0xe00U) != 0x08000000U) {
            throw Unsupported{"native indexed uniform constant load mode"};
        }
    } else if (current.words[2] & ~0xe00U) {
        throw Unsupported{"native uniform constant load mode"};
    }
    if (bank >= 18 || dest >= NUM_UNIFORM_REGISTERS ||
        (width != 0 && width != 2 && width != 4 && width != 5)) {
        throw Unsupported{"native uniform constant load mode"};
    }
    const u32 alignment = width == 0 ? 1U : width == 2 ? 2U : 4U;
    if (offset % alignment != 0 || (width == 5 && dest + 1 >= NUM_UNIFORM_REGISTERS)) {
        throw Unsupported{"native uniform constant load alignment"};
    }
    IR::U32 address{ir.Imm32(offset)};
    if (indexed) {
        address = IR::U32{ir.IAdd(Uniform(current.Bits(24, 8)), address)};
    }
    const IR::U32 binding = ir.Imm32(bank);
    if (width == 5) {
        ir.SetReg(UniformRegister(dest), IR::U32{ir.GetCbuf(binding, address, 32, false)});
        ir.SetReg(UniformRegister(dest + 1),
                  IR::U32{ir.GetCbuf(binding, IR::U32{ir.IAdd(address, ir.Imm32(4))}, 32, false)});
        return;
    }
    ir.SetReg(UniformRegister(dest),
              IR::U32{ir.GetCbuf(binding, address, width == 0 ? 8 : width == 2 ? 16 : 32, false)});
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

void Translator::EmitWideMultiplyAdd() {
    // IMAD.WIDE.U32: a 32x32 product plus a register pair, written as a pair. IR has no 64-bit
    // multiply, so the product is four 16-bit partial products; the signed form corrects the high
    // word the way the shipping lowering does (subtract b if a is negative, add a if b is), which
    // makes the low word identical either way.
    const u32 opcode = current.Bits(0, 12);
    const u32 dest = current.Bits(16, 8);
    const u32 addend = current.Bits(64, 8);
    const bool high_only = opcode == 0x827;
    const bool signed_product = current.Bits(73, 1) != 0;
    if ((current.words[2] & ~0x000e02ffU) != 0x07800000U || dest == 255 ||
        (!high_only && ((dest % 2) != 0 || (addend != 255 && (addend % 2) != 0))) ||
        (signed_product && current.Bits(81, 3) != 7)) {
        throw Unsupported{"native high multiply mode"};
    }
    const IR::U32 a = Gpr(current.Bits(24, 8));
    const IR::U32 b{opcode == 0x825 ? ir.Imm32(current.words[1]) : Gpr(current.Bits(32, 8))};
    const auto high = [&](const IR::U32& value) {
        return IR::U32{ir.ShiftRightLogical(value, ir.Imm32(16))};
    };
    const auto low = [&](const IR::U32& value) {
        return IR::U32{ir.BitwiseAnd(value, ir.Imm32(0xffffU))};
    };
    const auto widen = [&](const IR::U32& value) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(value, ir.Imm32(0)))};
    };
    IR::U64 product{ir.IAdd(
        ir.IAdd(widen(IR::U32{ir.IMul(low(a), low(b))}),
                IR::U64{ir.ShiftLeftLogical(widen(IR::U32{ir.IMul(low(a), high(b))}),
                                            ir.Imm32(16))}),
        ir.IAdd(IR::U64{ir.ShiftLeftLogical(widen(IR::U32{ir.IMul(high(a), low(b))}),
                                            ir.Imm32(16))},
                IR::U64{ir.ShiftLeftLogical(widen(IR::U32{ir.IMul(high(a), high(b))}),
                                            ir.Imm32(32))}))};
    if (signed_product) {
        const IR::U32 signed_a{ir.ShiftRightArithmetic(a, ir.Imm32(31))};
        const IR::U32 signed_b{ir.ShiftRightArithmetic(b, ir.Imm32(31))};
        const IR::U64 correction{ir.IAdd(
            IR::U64{ir.ShiftLeftLogical(widen(IR::U32{ir.BitwiseAnd(signed_b, a)}), ir.Imm32(32))},
            IR::U64{ir.ShiftLeftLogical(widen(IR::U32{ir.BitwiseAnd(signed_a, b)}), ir.Imm32(32))})};
        product = ir.IAdd(product, correction);
    }
    if (high_only) {
        // IMAD.HI: only the high word of the product, plus a single addend register.
        const IR::U32 high_word{ir.CompositeExtract(ir.UnpackUint2x32(product), 1)};
        const IR::U32 extra{addend == 255 ? ir.Imm32(0) : Gpr(addend)};
        WriteUint(IR::U32{ir.IAdd(high_word, extra)});
        return;
    }
    const IR::U64 pair = addend == 255
                             ? ir.Imm64(u64{0})
                             : IR::U64{ir.PackUint2x32(
                                   ir.CompositeConstruct(Gpr(addend), Gpr(addend + 1)))};
    const IR::Value halves{ir.UnpackUint2x32(IR::U64{ir.IAdd(product, pair)})};
    ir.SetReg(Register(dest), IR::U32{ir.CompositeExtract(halves, 0)});
    ir.SetReg(Register(dest + 1), IR::U32{ir.CompositeExtract(halves, 1)});
}

void Translator::EmitFloatFusedMultiplyAdd() {
    const u32 opcode = current.Bits(0, 12);
    const bool uniform_form = opcode == 0xc23 || opcode == 0xe23;
    if (current.Bits(76, 1) || current.Bits(81, 10) != 0 ||
        (current.Bits(91, 1) != 0 && !uniform_form)) {
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
    case 0x225: // IMAD.WIDE (register multiplier)
    case 0x827: // IMAD.HI (immediate multiplier)
    case 0x825: // IMAD.WIDE (immediate multiplier)
        EmitWideMultiplyAdd();
        return;
    case 0x81a: // SGXT
        EmitSignExtend();
        return;
    case 0xa24: // IMAD  (constant buffer multiplier)
    case 0x624: // IMAD  (constant buffer addend)
    case 0x224: // IMAD  (register multiplier)
    case 0x424: // IMAD  (constant buffer multiplier)
    case 0xc24: // IMAD  (uniform multiplier)
    case 0xe24: // IMAD  (uniform addend)
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
    case 0x806: // VOTE
        EmitVote();
        return;
    case 0x886: // VOTEU
        EmitUniformVote();
        return;
    case 0x98e: // REDG
        EmitGlobalReduction();
        return;
    case 0x3a8: // ATOMG (register pair)
    case 0x9a8: // ATOMG (uniform pair)
        EmitGlobalAtomic();
        return;
    case 0x890: // UIADD3 (immediate)
    case 0x290: // UIADD3 (register)
        EmitUniformAddThree();
        return;
    case 0x891: // ULEA (immediate)
    case 0x291: // ULEA (register)
        EmitUniformEffectiveAddress();
        return;
    case 0xb60: // TEX
        EmitTextureFetch();
        return;
    case 0xb82: // LDC with an indexed offset
        EmitIndexedConstantLoad();
        return;
    case 0x3c4: // REDUX
        EmitWarpReduction();
        return;
    case 0x305: // F2I (word form)
        EmitFloatToInteger();
        return;
    case 0x2a4: // UIMAD with a uniform multiplier
    case 0x2a5: // UIMAD.WIDE.U32 with a uniform multiplier
        EmitUniformMultiplyAddRegister();
        return;
    case 0x217: // IMNMX (register)
    case 0x817: // IMNMX (immediate)
    case 0xa17: // IMNMX (constant buffer)
        EmitIntegerMinMax();
        return;
    case 0x300: // FLO
        EmitFindLeadingOne();
        return;
    case 0x301: // BREV
        EmitBitReverse();
        return;
    case 0x816: // PRMT (immediate selector)
        EmitPermuteBytes();
        return;
    case 0x419: // SHF.L.U32.HI
        EmitFunnelShift();
        return;
    case 0x809: // FMNMX (immediate)
        EmitFloatMinMax();
        return;
    case 0xb08: // MUFU (constant buffer source)
    case 0xd08: // MUFU (uniform source)
    case 0x308: // MUFU
        EmitMultiFunction();
        return;
    case 0x381: // LDG with a register-pair address
    case 0x981: // LDG
        EmitGlobalLoad();
        return;
    case 0x986: // STG
        EmitGlobalStore();
        return;
    case 0x805: // CS2R
        EmitCopySpecialToRegisterPair();
        return;
    case 0x387: // STL
    case 0x983: // LDL
        EmitLocalMemory();
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
    case 0x292: // ULOP3
    case 0x892: // ULOP3 with an immediate
        EmitUniformLogicLut();
        return;
    case 0x8a4: // UIMAD
        EmitUniformMultiplyAdd();
        return;
    case 0x8a5: // UIMAD.WIDE
        EmitUniformWideMultiplyAdd();
        return;
    case 0x287: // USEL
    case 0x887: // USEL with an immediate
        EmitUniformSelect();
        return;
    case 0x28c: // UISETP
    case 0x88c: // UISETP with an immediate
        EmitUniformSetPredicate();
        return;
    case 0x899: // USHF
        EmitUniformShift();
        return;
    case 0x3c2: // R2UR
        EmitRegisterToUniform();
        return;
    case 0x211: // LEA (register addend)
    case 0x811: // LEA (immediate addend)
    case 0xa11: // LEA (constant buffer addend)
    case 0xc11: // LEA (uniform addend)
        EmitEffectiveAddress();
        return;
    case 0x245: // I2FP 32-bit (register)
    case 0xa45: // I2FP 32-bit (constant buffer)
    case 0xc45: // I2FP 32-bit (uniform)
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
    case 0x207: // SEL  (register)
    case 0x807: // SEL  (immediate)
    case 0xa07: // SEL  (constant buffer)
    case 0x208: // FSEL (register)
    case 0x808: // FSEL (immediate)
    case 0xa08: // FSEL (constant buffer)
        EmitSelectValue();
        return;
    case 0x309: // POPC (register)
        EmitPopulationCount();
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
