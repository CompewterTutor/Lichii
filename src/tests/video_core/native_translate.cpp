// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Every instruction here is captured from Ocarina of Time's compute program and its expected
// meaning was cross-checked with nvdisasm. The native ISA is undocumented, so a translation is only
// trusted once a captured word decodes to the same thing nvdisasm prints.

#include <array>
#include <optional>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/native/translate.h"

namespace {

namespace IR = Shader::IR;
using Shader::Ampere::Instruction;

class TranslateEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override {
        throw std::logic_error("Unexpected instruction read");
    }
    u32 ReadCbufValue(u32, u32) override {
        throw std::logic_error("Unexpected cbuf read");
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

    void SetStage(Shader::Stage value) {
        stage = value;
    }
};

struct Translated {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    TranslateEnvironment env;
    Shader::Native::Translator translator{env, block};

    Translated() = default;

    explicit Translated(const Instruction& instruction,
                        Shader::Stage stage = Shader::Stage::VertexB) {
        env.SetStage(stage);
        REQUIRE(translator.Translate(instruction));
    }

    [[nodiscard]] size_t Count(IR::Opcode opcode) const {
        size_t count{};
        for (const IR::Inst& inst : block) {
            if (inst.GetOpcode() == opcode) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] const IR::Inst* Find(IR::Opcode opcode) const {
        for (const IR::Inst& inst : block) {
            if (inst.GetOpcode() == opcode) {
                return &inst;
            }
        }
        return nullptr;
    }
};

/// FFMA.FTZ R7, R0, R7, R9
constexpr Instruction Ffma{{0x00077223, 0x00000007, 0x00010009, 0x060fe200}};
/// FFMA.FTZ R5, R5, R14, -1  (immediate addend)
constexpr Instruction FfmaImmediate{{0x05057423, 0xbf800000, 0x0001000e, 0x000fe200}};
/// FMUL.FTZ R13, R38, R33
constexpr Instruction Fmul{{0x260d7220, 0x00000021, 0x00410000, 0x000fe200}};
/// FMUL.FTZ R15, R17, c[0x3][0x4d4]
constexpr Instruction FmulCbuf{{0x110f7a20, 0x00c13500, 0x00410000, 0x000fe200}};
/// @P4 FADD.FTZ R16, R16, R39
constexpr Instruction FaddGuarded{{0x10104221, 0x00000027, 0x00010000, 0x000fe200}};
/// FADD.FTZ R17, -R17, c[0x3][0x44]
constexpr Instruction FaddCbufNegated{{0x11117621, 0x00c01100, 0x00010100, 0x000fc600}};
/// SHFL.IDX PT, R39, R18, 0x1, 0x1f
constexpr Instruction Shuffle{{0x12277f89, 0x00201f00, 0x000e0000, 0x000ea200}};
/// S2R R2, SR_TID.X
constexpr Instruction ReadTidX{{0x00027919, 0x00000000, 0x00002100, 0x000e2800}};
/// FSETP.GT.FTZ.AND P0, PT, R46, RZ, PT
constexpr Instruction FloatSetPredicate{{0x2e00720b, 0x000000ff, 0x03f14000, 0x001fe400}};
/// ISETP.GE.U32.AND P2, PT, R50, R4, PT
constexpr Instruction IntSetPredicate{{0x3200720c, 0x00000004, 0x03f46070, 0x000fc800}};
/// IMAD.U32 R5, R3, 0x60, R2
constexpr Instruction Imad{{0x03057824, 0x00000060, 0x078e0002, 0x001fc800}};
/// LOP3.LUT R0, R3, 0xfffc, RZ, 0xc0
constexpr Instruction LogicLut{{0x03007812, 0x0000fffc, 0x078ec0ff, 0x044fe400}};
/// MUFU.SIN R5, R39
constexpr Instruction MultiFunction{{0x00057308, 0x00000027, 0x00000400, 0x0003e200}};
/// P2R R3, PR, RZ, 0x4
constexpr Instruction PredicateToRegister{{0xff037803, 0x00000004, 0x00000000, 0x000fca00}};
/// IADD3 R30, R29, R39, RZ
constexpr Instruction IntegerAdd{{0x1d1e7210, 0x00000027, 0x07ffe0ff, 0x000fc800}};
/// LDG.E.CONSTANT R0, [R0.U32+UR4]
constexpr Instruction GlobalLoad{{0x00007981, 0x00000004, 0x081e8900, 0x000ea200}};
/// ULDC.64 UR4, c[0x0][0x50]
constexpr Instruction UniformConstantLoad{{0x00047ab9, 0x00001400, 0x00000a00, 0x000fe400}};
/// POPC R21, R17
constexpr Instruction PopulationCount{{0x00157309, 0x00000011, 0x00000000, 0x000e2200}};
/// SEL R38, RZ, 0x1, P3
constexpr Instruction Select{{0xff267807, 0x00000001, 0x01800000, 0x000fc800}};
/// FSEL R42, R42, 0.9f, P6
constexpr Instruction FloatSelect{{0x2a2a7808, 0x3f666666, 0x03000000, 0x000fe400}};
/// FMNMX.FTZ R11, R11, 30, PT
constexpr Instruction FloatMinMaxImmediate{{0x0b0b7809, 0x41f00000, 0x03810000, 0x000fe200}};
/// FMNMX R1, R6, R7 with bits 87-90 = 0xf: PT plus the min/max swap bit
constexpr Instruction FloatMinMaxSwapped{{0x06017209, 0x40000007, 0x07810200, 0x000fc800}};
/// IMNMX.U32 R29, R30, R29, PT
constexpr Instruction IntegerMinMax{{0x1e1d7217, 0x0000001d, 0x03800000, 0x004fc800}};
/// IMNMX.U32 R33, R17, 0xa000, PT
constexpr Instruction IntegerMinMaxImmediate{{0x11217817, 0x0000a000, 0x03800000, 0x040fe400}};
/// FLO.U32 R17, R17
constexpr Instruction FindLeadingOne{{0x00117300, 0x00000011, 0x000e0000, 0x000fe400}};
/// BREV R39, R38
constexpr Instruction BitReverse{{0x00277301, 0x00000026, 0x00000000, 0x000e6200}};
/// PRMT R17, R41, 0x5410, R42
constexpr Instruction PermuteBytes{{0x29117816, 0x00005410, 0x0000002a, 0x001fe200}};
/// SHF.L.U32.HI R0, RZ, R50, 0xffffffff  (shift saturates at 32: a register pass)
constexpr Instruction FunnelShift{{0xff007419, 0xffffffff, 0x00010632, 0x000fe200}};
/// IMAD R10, R50, R10, UR11
constexpr Instruction ImadUniformAddend{{0x320a7e24, 0x0000000b, 0x0f8e020a, 0x000fca00}};
/// IADD3 R40, R17, UR4, RZ
constexpr Instruction IntegerAddUniform{{0x11287c10, 0x00000004, 0x0fffe0ff, 0x000fe400}};
/// STG.E.64.STRONG.SM [R39.U32+UR10], R6
constexpr Instruction GlobalStore64{{0x27007986, 0x00000006, 0x0810ab0a, 0x0001e200}};
/// CS2R R2, SRZ
constexpr Instruction CopySpecialToPair{{0x00027805, 0x00000000, 0x0001ff00, 0x000fe200}};
/// STL [RZ], R3
constexpr Instruction StoreLocal{{0xff007387, 0x00000003, 0x00100800, 0x0001e800}};
/// F2I.FTZ.U32.NTZ R13, R13 (default rounding)
constexpr Instruction FloatToInteger{{0x000d7305, 0x0000000d, 0x00213000, 0x000e2200}};
/// F2I.FTZ.TRUNC.NTZ R11, R28
constexpr Instruction FloatToIntegerTrunc{{0x000b7305, 0x0000001c, 0x0021f100, 0x000e2200}};
/// UIMAD UR4, UR4, UR13, URZ
constexpr Instruction UniformMultiplyAdd{{0x040472a4, 0x0000000d, 0x0f8e023f, 0x000fc800}};
/// UIMAD.WIDE.U32 UR10, UR4, UR12, UR10
constexpr Instruction UniformWideMultiplyAdd{{0x040a72a5, 0x0000000c, 0x0f8e000a, 0x000fc400}};
/// VOTE.ALL R38, PT, PT
constexpr Instruction Vote{{0x00267806, 0x00000000, 0x038e0000, 0x000fe200}};
/// VOTEU.ANY UP4, P6
constexpr Instruction UniformVote{{0x003f7886, 0x00000000, 0x03080100, 0x000fca00}};
/// REDG.E.MAX.STRONG.GPU [R10+-0xc], R29
constexpr Instruction GlobalReduction{{0x0a00798e, 0xfffff41d, 0x0110e100, 0x0001e400}};
/// ATOMG.E.ADD.STRONG.GPU PT, R39, [R10], R29
constexpr Instruction GlobalAtomic{{0x0a2773a8, 0x0000001d, 0x001ee100, 0x001ea200}};
/// @UP3 UIADD3 UR4, UR15, -0x1, URZ
constexpr Instruction UniformAddGuarded{{0x0f043890, 0xffffffff, 0x0fffe03f, 0x000fc800}};
/// ULEA UR4, UR16, 0x82, 0x6
constexpr Instruction UniformEffectiveAddress{{0x10043891, 0x00000082, 0x0f8e303f, 0x000fe200}};
/// LDC.U8 R2, c[0x5][R2+0x7]
constexpr Instruction IndexedConstantLoad{{0x02027b82, 0x014001c0, 0x00000000, 0x000e2200}};
/// REDUX.MAX UR14, R21
constexpr Instruction WarpReduction{{0x150e73c4, 0x00000000, 0x00014000, 0x000e2600}};
/// TEX.SCR.LZ R1, R0, R9, 0x0, 0xc, 2D, 0x1
constexpr Instruction TextureFetch{{0x00017b60, 0x30000c09, 0x009e01ff, 0x000f4400}};
/// @P4 LDC.64 R2, c[0x4][R10]
constexpr Instruction IndexedConstantLoad64{{0x0a024b82, 0x01000000, 0x00000a00, 0x000e2400}};
/// TLD.SCR.LZ R10, R12, R12, R17, 2D, 0x7
constexpr Instruction TexelFetch{{0x0c0c7b66, 0x30002411, 0x009e070a, 0x000f4200}};
/// TXQ RZ, R24, R24, TEX_HEADER_DIMENSION, 0x3
constexpr Instruction TextureQuery{{0x18187b6f, 0x00008400, 0x000003ff, 0x000f4200}};
/// F2I.FTZ.U16.NTZ R38, R11
constexpr Instruction FloatToSmallInteger{{0x00267305, 0x0000000b, 0x00212800, 0x000e6400}};
/// FRND.FLOOR R2, R11
constexpr Instruction FloatRound{{0x00027307, 0x0000000b, 0x00205000, 0x000e2200}};
/// I2F.U16 R5, R5.H1
constexpr Instruction IntegerToFloatHalf{{0x00057306, 0x10000005, 0x00101000, 0x000e2400}};
/// FMNMX.FTZ R20, R20, c[0x0][0x40], !PT
constexpr Instruction FloatMinMaxBuffer{{0x14147a09, 0x00001000, 0x07810000, 0x000fe200}};
/// UBREV UR4, UR6
constexpr Instruction UniformBitReverse{{0x000472be, 0x00000006, 0x08000000, 0x000fe200}};
/// SGXT.U32 R5, R5, 0x1
constexpr Instruction SignExtend{{0x0505781a, 0x00000001, 0x00000000, 0x004fe400}};
/// IMAD.WIDE.U32 R10, R11, 0x10, R2
constexpr Instruction WideMultiplyAdd{{0x0b0a7825, 0x00000010, 0x078e0002, 0x000fca00}};
/// IMAD.HI.U32 R11, R31, 0xb21642c9, RZ
constexpr Instruction HighMultiply{{0x1f0b7827, 0xb21642c9, 0x078e00ff, 0x040fe200}};
/// IMAD R3, R1, c[0x3][0x1c], RZ
constexpr Instruction MultiplyConstantBuffer{{0x01037a24, 0x00c00700, 0x078e02ff, 0x000fc800}};
/// IMNMX R7, R6, c[0x0][0x18], PT
constexpr Instruction IntegerMinMaxBuffer{{0x06077a17, 0x00000600, 0x03800200, 0x000fe200}};
/// AST R0, a[0x70], four components: the whole position
constexpr Instruction AttributeStorePosition{{0xff007322, 0x00007000, 0x00000cff, 0x000fe200}};
/// AST R4, a[0xe0] (generic 6 x)
constexpr Instruction AttributeStoreGeneric{{0xff007322, 0x0000e004, 0x000000ff, 0x0001e200}};
/// AST RZ, a[0x80]: RZ stores zero
constexpr Instruction AttributeStoreZero{{0xff008322, 0x000080ff, 0x000000ff, 0x000fe200}};
/// AST R3, a[0x5c]: the one-component per-primitive hint
constexpr Instruction AttributeStoreHint{{0xff007322, 0x00005c03, 0x000000ff, 0x000fe200}};
/// ALD.96 R0, a[0x80]
constexpr Instruction AttributeLoad{{0xff007321, 0x000080ff, 0x00000800, 0x000e2200}};
/// ALD R12, a[0x140]
constexpr Instruction AttributeLoadSingle{{0xff0c7321, 0x000140ff, 0x00000000, 0x000e6200}};
/// IPA.PASS R38, a[0x7c]
constexpr Instruction InterpolatePositionW{{0x00267326, 0x000000ff, 0x000e001f, 0x040e0200}};
/// IPA.PASS R1, a[0xd0]
constexpr Instruction InterpolateGeneric{{0x00017326, 0x000000ff, 0x000e0034, 0x000fe200}};
/// ISBERD.ATTR.32 R2, [RZ]
constexpr Instruction StageBufferRead{{0xff027923, 0x00000000, 0x00003800, 0x000e2800}};
/// ISBEWR.O.32 [0x3], R5
constexpr Instruction StageBufferMapWrite{{0xff008927, 0x00000305, 0x00008800, 0x0001e200}};
/// ISBEWR.O.ATTR.32 [R42+0x380], R53
constexpr Instruction StageBufferAttributeWrite{{0x2a007927, 0x00038035, 0x0000f800, 0x000fe800}};

} // Anonymous namespace

TEST_CASE("FFMA translates to a fused multiply-add with FTZ", "[video_core][native]") {
    const Translated lowered{Ffma};
    const IR::Inst* fma = lowered.Find(IR::Opcode::FPFma32);
    REQUIRE(fma != nullptr);
    const IR::FpControl control = fma->Flags<IR::FpControl>();
    CHECK(control.no_contraction);
    CHECK(control.fmz_mode == IR::FmzMode::FTZ);
    CHECK(control.rounding == IR::FpRounding::RN);

    const IR::Inst* store = lowered.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R7);
    CHECK(lowered.Count(IR::Opcode::GetRegister) == 3); // R0, R7, R9
}

TEST_CASE("FFMA takes an immediate addend", "[video_core][native]") {
    const Translated lowered{FfmaImmediate};
    REQUIRE(lowered.Find(IR::Opcode::FPFma32) != nullptr);
    const IR::Inst* store = lowered.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R5);

    bool saw_minus_one{};
    for (const IR::Inst& inst : lowered.block) {
        if (inst.GetOpcode() == IR::Opcode::FPFma32) {
            // The third operand is the immediate word 0xbf800000, which is -1.0f.
            const IR::Value addend = inst.Arg(2);
            if (addend.IsImmediate()) {
                saw_minus_one = std::bit_cast<f32>(addend.U32()) == -1.0f;
            }
        }
    }
    CHECK(saw_minus_one);
}

TEST_CASE("FMUL translates, including a constant buffer source", "[video_core][native]") {
    const Translated registers{Fmul};
    REQUIRE(registers.Find(IR::Opcode::FPMul32) != nullptr);
    CHECK(registers.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R13);
    CHECK(registers.Find(IR::Opcode::GetCbufU32) == nullptr);

    const Translated buffer{FmulCbuf};
    REQUIRE(buffer.Find(IR::Opcode::FPMul32) != nullptr);
    const IR::Inst* cbuf = buffer.Find(IR::Opcode::GetCbufF32);
    REQUIRE(cbuf != nullptr);
    CHECK(cbuf->Arg(0).U32() == 3);      // bank
    CHECK(cbuf->Arg(1).U32() == 0x4d4);  // byte offset
}

TEST_CASE("FADD with a guard selects, and source modifiers reach the IR", "[video_core][native]") {
    const Translated guarded{FaddGuarded};
    REQUIRE(guarded.Find(IR::Opcode::FPAdd32) != nullptr);
    // A guarded instruction keeps its old destination when the guard is false.
    CHECK(guarded.Count(IR::Opcode::SelectU32) == 1);
    const IR::Inst* pred = guarded.Find(IR::Opcode::GetPred);
    REQUIRE(pred != nullptr);
    CHECK(pred->Arg(0).Pred() == IR::Pred::P4);

    const Translated negated{FaddCbufNegated};
    REQUIRE(negated.Find(IR::Opcode::FPAdd32) != nullptr);
    // An unpredicated instruction must not turn into a select.
    CHECK(negated.Count(IR::Opcode::SelectU32) == 0);
    const IR::Inst* cbuf = negated.Find(IR::Opcode::GetCbufF32);
    REQUIRE(cbuf != nullptr);
    CHECK(cbuf->Arg(0).U32() == 3);
    CHECK(cbuf->Arg(1).U32() == 0x44);
    // The negate on R17 becomes a sign flip on the first source.
    CHECK(negated.Count(IR::Opcode::FPNeg32) == 1);
}

TEST_CASE("SHFL with an immediate lane translates to a shuffle", "[video_core][native]") {
    const Translated shuffle{Shuffle};
    REQUIRE(shuffle.Find(IR::Opcode::ShuffleIndex) != nullptr);
    const IR::Inst* store = shuffle.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R39);
}

TEST_CASE("S2R reads a special register", "[video_core][native]") {
    const Translated tid{ReadTidX};
    // SR_TID_X is the local invocation id, which is not an instruction argument but a read.
    const IR::Inst* store = tid.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R2);
    CHECK(tid.Find(IR::Opcode::LocalInvocationId) != nullptr);
}

TEST_CASE("FSETP and ISETP set predicates", "[video_core][native]") {
    const Translated floating{FloatSetPredicate};
    const IR::Inst* fp = floating.Find(IR::Opcode::SetPred);
    REQUIRE(fp != nullptr);
    CHECK(fp->Arg(0).Pred() == IR::Pred::P0);
    // FPCompareOp GT becomes a greater-than comparison.
    CHECK(floating.Find(IR::Opcode::FPOrdGreaterThan32) != nullptr);

    const Translated integer{IntSetPredicate};
    const IR::Inst* ip = integer.Find(IR::Opcode::SetPred);
    REQUIRE(ip != nullptr);
    CHECK(ip->Arg(0).Pred() == IR::Pred::P2);
    // GE.U32 becomes a greater-than-or-equal comparison with an unsigned source.
    CHECK(integer.Find(IR::Opcode::UGreaterThanEqual) != nullptr);
}

TEST_CASE("IMAD multiplies and adds", "[video_core][native]") {
    const Translated imad{Imad};
    REQUIRE(imad.Find(IR::Opcode::IMul32) != nullptr);
    REQUIRE(imad.Find(IR::Opcode::IAdd32) != nullptr);
    const IR::Inst* store = imad.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R5);
}

TEST_CASE("LOP3 with the AND table reduces to a bitwise and", "[video_core][native]") {
    // LUT 0xc0 is a & b, so the generic expansion has to end up as the and of the two sources.
    // The table is indexed with a as the most significant selector (a * 4 + b * 2 + c); indexing it
    // the other way round turns this table into a constant zero and makes the door of Ocarina's Deku
    // Tree house disappear.
    const Translated lut{LogicLut};
    const IR::Inst* store = lut.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R0);
    CHECK(lut.Count(IR::Opcode::BitwiseAnd32) >= 1);
    CHECK(lut.Count(IR::Opcode::BitwiseNot32) >= 1); // the ~c terms of the expansion
}

TEST_CASE("MUFU scales turns to radians for sine", "[video_core][native]") {
    const Translated sine{MultiFunction};
    REQUIRE(sine.Find(IR::Opcode::FPSin) != nullptr);
    // SM70+ takes the angle in turns, so a multiply by 2pi has to sit in front of the sine.
    REQUIRE(sine.Find(IR::Opcode::FPMul32) != nullptr);
    const IR::Inst* store = sine.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R5);
}

TEST_CASE("P2R collects predicates into register bits", "[video_core][native]") {
    const Translated p2r{PredicateToRegister};
    const IR::Inst* store = p2r.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R3);
    // Mask bit 2 is the only one set, so exactly one predicate is collected.
    CHECK(p2r.Count(IR::Opcode::SelectU32) == 1);
    CHECK(p2r.Find(IR::Opcode::GetPred) != nullptr);
}

TEST_CASE("IADD3 adds two registers", "[video_core][native]") {
    const Translated add{IntegerAdd};
    REQUIRE(add.Find(IR::Opcode::IAdd32) != nullptr);
    const IR::Inst* store = add.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R30);
}

TEST_CASE("LDG loads through a uniform base plus a register offset", "[video_core][native]") {
    const Translated load{GlobalLoad};
    REQUIRE(load.Find(IR::Opcode::LoadGlobal32) != nullptr);
    // The address is assembled as a 64-bit pair, so the base and the offset are packed.
    CHECK(load.Count(IR::Opcode::PackUint2x32) >= 1);
    const IR::Inst* store = load.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R0);
}

TEST_CASE("ULDC loads a constant buffer pair into uniform registers", "[video_core][native]") {
    const Translated load{UniformConstantLoad};
    CHECK(load.Count(IR::Opcode::GetCbufU32) == 2);
    size_t uniform_writes{};
    for (const IR::Inst& inst : load.block) {
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            uniform_writes |= static_cast<u32>(inst.Arg(0).Reg()) ==
                              Shader::Native::UNIFORM_REGISTER_BASE + 4;
        }
    }
    CHECK(uniform_writes);
}

TEST_CASE("Opcode gaps fail instead of guessing", "[video_core][native]") {
    Translated program;
    // ATOM GLOBAL CAS has no IR translation yet.
    CHECK_FALSE(program.translator.Translate(Instruction{{0x000093a9, 0, 0, 0}}));
    CHECK(program.translator.Error().starts_with("unsupported native opcode"));
}

TEST_CASE("Uniform registers are their own SSA values", "[video_core][native]") {
    // FMUL.FTZ R7, R38, UR15: the uniform index is the low byte of word 1.
    const Translated uniform{{0x26077c20, 0x0000000f, 0x08410000, 0x008fe200}};
    REQUIRE(uniform.Find(IR::Opcode::FPMul32) != nullptr);
    bool read_uniform{};
    for (const IR::Inst& inst : uniform.block) {
        if (inst.GetOpcode() == IR::Opcode::GetRegister) {
            const auto reg = inst.Arg(0).Reg();
            read_uniform |= static_cast<u32>(reg) ==
                            Shader::Native::UNIFORM_REGISTER_BASE + 15;
        }
    }
    CHECK(read_uniform);
    // No shadow copy and no reserved base: the uniform register is read directly.
    CHECK(uniform.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R7);
}

TEST_CASE("MOV reaches the uniform register file and constant buffer", "[video_core][native]") {
    // MOV R8, RZ
    const Translated zero{{0x00087202, 0x000000ff, 0x00000f00, 0x00afe200}};
    CHECK(zero.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R8);

    // MOV R6, UR4
    const Translated from_uniform{{0x00060c02, 0x00000004, 0x00000f00, 0x0000e200}};
    const IR::Inst* store = from_uniform.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R6);
}

TEST_CASE("Uniform predicates fold into a predicate LUT", "[video_core][native]") {
    // PLOP3.LUT P0, PT, PT, PT, UP0, 0x80, 0x0 copies UP0 into P0 through the lookup table.
    const Translated lut{{0x0000781c, 0x00000000, 0x03f0f008, 0x000fda00}};
    const IR::Inst* store = lut.Find(IR::Opcode::SetPred);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Pred() == IR::Pred::P0);
    // The table is expanded as seven selects over the eight truth-table entries.
    CHECK(lut.Count(IR::Opcode::SelectU1) == 7);
    // The uniform predicate is read out of its own bank, not a copied register.
    bool read_uniform_predicate{};
    for (const IR::Inst& inst : lut.block) {
        if (inst.GetOpcode() == IR::Opcode::GetRegister) {
            read_uniform_predicate |= static_cast<u32>(inst.Arg(0).Reg()) ==
                                      Shader::Native::UNIFORM_PREDICATE_BASE;
        }
    }
    CHECK(read_uniform_predicate);
}


TEST_CASE("POPC, SEL and FSEL translate", "[video_core][native]") {
    const Translated popcount{PopulationCount};
    REQUIRE(popcount.Find(IR::Opcode::BitCount32) != nullptr);
    CHECK(popcount.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R21);

    const Translated select{Select};
    const IR::Inst* chosen = select.Find(IR::Opcode::SelectU32);
    REQUIRE(chosen != nullptr);
    const IR::Inst* condition = select.Find(IR::Opcode::GetPred);
    REQUIRE(condition != nullptr);
    CHECK(condition->Arg(0).Pred() == IR::Pred::P3);
    CHECK(select.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R38);

    const Translated floating{FloatSelect};
    REQUIRE(floating.Find(IR::Opcode::SelectF32) != nullptr);
}

TEST_CASE("FMNMX swap bit selects the maximum", "[video_core][native]") {
    // The negation bit that makes the lowered Maxwell instruction a maximum swaps the two results;
    // it does not negate the predicate. Getting this backwards turns the cube-map coordinate
    // normalisation into a minimum, and the sky and terrain render black.
    const Translated minmax{FloatMinMaxSwapped};
    const IR::Inst* select = minmax.Find(IR::Opcode::SelectF32);
    REQUIRE(select != nullptr);
    const IR::Inst* maximum = minmax.Find(IR::Opcode::FPMax32);
    REQUIRE(maximum != nullptr);
    CHECK(minmax.Count(IR::Opcode::FPMin32) == 1);
    CHECK(select->Arg(1).Inst() == maximum);
    CHECK(minmax.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R1);
}

TEST_CASE("FMNMX and IMNMX translate", "[video_core][native]") {
    const Translated floating{FloatMinMaxImmediate};
    REQUIRE(floating.Find(IR::Opcode::SelectF32) != nullptr);
    CHECK(floating.Count(IR::Opcode::FPMin32) == 1);
    CHECK(floating.Count(IR::Opcode::FPMax32) == 1);
    CHECK(floating.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R11);

    const Translated integer{IntegerMinMax};
    REQUIRE(integer.Find(IR::Opcode::UMin32) != nullptr);
    REQUIRE(integer.Find(IR::Opcode::UMax32) != nullptr);
    CHECK(integer.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R29);

    const Translated immediate{IntegerMinMaxImmediate};
    CHECK(immediate.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R33);
}

TEST_CASE("FLO and BREV translate", "[video_core][native]") {
    const Translated leading{FindLeadingOne};
    REQUIRE(leading.Find(IR::Opcode::FindUMsb32) != nullptr);
    CHECK(leading.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R17);

    const Translated reverse{BitReverse};
    REQUIRE(reverse.Find(IR::Opcode::BitReverse32) != nullptr);
    CHECK(reverse.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R39);
}

TEST_CASE("PRMT translates to a byte permute", "[video_core][native]") {
    const Translated permute{PermuteBytes};
    CHECK(permute.Count(IR::Opcode::BitFieldInsert) == 4);
    CHECK(permute.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R17);
}

TEST_CASE("SHF.L.U32.HI saturates its shift", "[video_core][native]") {
    const Translated funnel{FunnelShift};
    // A saturated shift of 32 leaves the low word in the high half, so the value passes through.
    const IR::Inst* store = funnel.Find(IR::Opcode::SetRegister);
    REQUIRE(store != nullptr);
    CHECK(store->Arg(0).Reg() == IR::Reg::R0);
    REQUIRE(funnel.Find(IR::Opcode::GetRegister) != nullptr);
    CHECK(funnel.Find(IR::Opcode::GetRegister)->Arg(0).Reg() == IR::Reg::R50);
}

TEST_CASE("IMAD and IADD3 take a uniform source", "[video_core][native]") {
    const Translated multiply{ImadUniformAddend};
    // UR11 sits at the uniform bank base plus eleven, so a read of it is a GetRegister with that
    // index; the product itself is an ordinary 32-bit multiply.
    bool saw_uniform_read{};
    for (const IR::Inst& inst : multiply.block) {
        if (inst.GetOpcode() == IR::Opcode::GetRegister &&
            static_cast<u32>(inst.Arg(0).Reg()) == 512 + 11) {
            saw_uniform_read = true;
        }
    }
    CHECK(saw_uniform_read);
    CHECK(multiply.Find(IR::Opcode::IMul32) != nullptr);
    CHECK(multiply.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R10);

    const Translated add{IntegerAddUniform};
    CHECK(add.Find(IR::Opcode::IAdd32) != nullptr);
    CHECK(add.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R40);
}

TEST_CASE("STG.E.64 writes a register pair", "[video_core][native]") {
    const Translated store{GlobalStore64};
    const IR::Inst* wide = store.Find(IR::Opcode::WriteGlobal64);
    REQUIRE(wide != nullptr);
    CHECK(store.Count(IR::Opcode::WriteGlobal32) == 0);
}

TEST_CASE("CS2R zero idiom and STL translate", "[video_core][native]") {
    const Translated copy{CopySpecialToPair};
    CHECK(copy.Count(IR::Opcode::SetRegister) == 2);
    const IR::Inst* first = copy.Find(IR::Opcode::SetRegister);
    REQUIRE(first != nullptr);
    CHECK(first->Arg(0).Reg() == IR::Reg::R2);
    CHECK(first->Arg(1).IsImmediate());

    const Translated local{StoreLocal};
    REQUIRE(local.Find(IR::Opcode::WriteLocal) != nullptr);
}

TEST_CASE("F2I rounds, clamps and converts", "[video_core][native]") {
    // Bits 78-79 carry the rounds-to-even/floor/ceil/truncate selection.
    const Translated rounded{FloatToInteger};
    REQUIRE(rounded.Find(IR::Opcode::FPRoundEven32) != nullptr);
    REQUIRE(rounded.Find(IR::Opcode::FPClamp32) != nullptr);
    REQUIRE(rounded.Find(IR::Opcode::ConvertU32F32) != nullptr);
    CHECK(rounded.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R13);

    const Translated truncated{FloatToIntegerTrunc};
    REQUIRE(truncated.Find(IR::Opcode::FPTrunc32) != nullptr);
}

TEST_CASE("UIMAD multiplies uniform registers", "[video_core][native]") {
    const Translated narrow{UniformMultiplyAdd};
    REQUIRE(narrow.Find(IR::Opcode::IMul32) != nullptr);
    REQUIRE(narrow.Find(IR::Opcode::IAdd32) != nullptr);
    CHECK(static_cast<u32>(narrow.Find(IR::Opcode::SetRegister)->Arg(0).Reg()) == 512 + 4);

    const Translated wide{UniformWideMultiplyAdd};
    // Four 16-bit partial products, then the 64-bit addend pair.
    CHECK(wide.Count(IR::Opcode::IMul32) == 4);
    CHECK(wide.Count(IR::Opcode::SetRegister) == 2);
}

TEST_CASE("VOTE and VOTEU vote across the warp", "[video_core][native]") {
    // The captured VOTE discards its predicate result (bits 81-83 are PT), so only the ballot
    // reaches the block; VOTEU below writes a result.
    const Translated vote{Vote};
    REQUIRE(vote.Find(IR::Opcode::SubgroupBallot) != nullptr);
    CHECK(vote.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R38);
    CHECK(vote.Find(IR::Opcode::VoteAll) == nullptr);

    const Translated uniform{UniformVote};
    REQUIRE(uniform.Find(IR::Opcode::VoteAny) != nullptr);
    // The vote result lands in uniform predicate UP4, which lives after the uniform registers.
    bool wrote_predicate{};
    for (const IR::Inst& inst : uniform.block) {
        if (inst.GetOpcode() == IR::Opcode::SetRegister &&
            static_cast<u32>(inst.Arg(0).Reg()) == 576 + 4) {
            wrote_predicate = true;
        }
    }
    CHECK(wrote_predicate);
}

TEST_CASE("REDG and ATOMG reach global memory", "[video_core][native]") {
    const Translated reduction{GlobalReduction};
    // MAX over unsigned words.
    REQUIRE(reduction.Find(IR::Opcode::GlobalAtomicUMax32) != nullptr);
    CHECK(reduction.Find(IR::Opcode::SetRegister) == nullptr);

    const Translated atomic{GlobalAtomic};
    REQUIRE(atomic.Find(IR::Opcode::GlobalAtomicIAdd32) != nullptr);
    CHECK(atomic.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R39);
}

TEST_CASE("UIADD3 and ULEA work on the uniform file", "[video_core][native]") {
    const Translated add{UniformAddGuarded};
    REQUIRE(add.Find(IR::Opcode::IAdd64) != nullptr);
    CHECK(static_cast<u32>(add.Find(IR::Opcode::SetRegister)->Arg(0).Reg()) == 512 + 4);

    const Translated address{UniformEffectiveAddress};
    REQUIRE(address.Find(IR::Opcode::ShiftLeftLogical64) != nullptr);
    CHECK(static_cast<u32>(address.Find(IR::Opcode::SetRegister)->Arg(0).Reg()) == 512 + 4);
}

TEST_CASE("LDC with an indexed offset reads a constant byte", "[video_core][native]") {
    const Translated load{IndexedConstantLoad};
    REQUIRE(load.Find(IR::Opcode::GetCbufU8) != nullptr);
    CHECK(load.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R2);
}

TEST_CASE("REDUX reduces the warp into a uniform register", "[video_core][native]") {
    const Translated reduction{WarpReduction};
    // Five butterfly steps cover the 32 lanes.
    CHECK(reduction.Count(IR::Opcode::ShuffleButterfly) == 5);
    REQUIRE(reduction.Find(IR::Opcode::UMax32) != nullptr);
    CHECK(static_cast<u32>(reduction.Find(IR::Opcode::SetRegister)->Arg(0).Reg()) == 512 + 14);
}

TEST_CASE("SGXT shifts the source", "[video_core][native]") {
    const Translated extend{SignExtend};
    REQUIRE(extend.Find(IR::Opcode::ShiftRightLogical32) != nullptr);
    CHECK(extend.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R5);
}

TEST_CASE("IMAD.WIDE and IMAD.HI produce a 64-bit product", "[video_core][native]") {
    const Translated wide{WideMultiplyAdd};
    CHECK(wide.Count(IR::Opcode::IMul32) == 4);
    CHECK(wide.Count(IR::Opcode::SetRegister) == 2);

    const Translated high{HighMultiply};
    CHECK(high.Count(IR::Opcode::IMul32) == 4);
    CHECK(high.Count(IR::Opcode::SetRegister) == 1);
    CHECK(high.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R11);
}

TEST_CASE("IMAD and IMNMX read a constant buffer source", "[video_core][native]") {
    const Translated imad{MultiplyConstantBuffer};
    REQUIRE(imad.Find(IR::Opcode::GetCbufU32) != nullptr);
    CHECK(imad.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R3);

    const Translated imnmx{IntegerMinMaxBuffer};
    REQUIRE(imnmx.Find(IR::Opcode::GetCbufU32) != nullptr);
    CHECK(imnmx.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R7);
}

TEST_CASE("TEX samples a texture at level zero", "[video_core][native]") {
    const Translated sample{TextureFetch};
    const IR::Inst* const image{sample.Find(IR::Opcode::BoundImageSampleExplicitLod)};
    REQUIRE(image != nullptr);
    // The descriptor is at constant buffer byte offset 12 * 4; the sample is 2D at level zero.
    CHECK(image->Arg(0).U32() == 12U * 4U);
    CHECK(image->Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color2D);
    REQUIRE(sample.Find(IR::Opcode::SetRegister) != nullptr);
    CHECK(sample.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R1);
}

TEST_CASE("LDC.64 reads a constant pair", "[video_core][native]") {
    const Translated load{IndexedConstantLoad64};
    REQUIRE(load.Find(IR::Opcode::GetCbufU32x2) != nullptr);
    CHECK(load.Count(IR::Opcode::SetRegister) == 2);
}

TEST_CASE("TLD fetches a texel", "[video_core][native]") {
    const Translated fetch{TexelFetch};
    const IR::Inst* const image{fetch.Find(IR::Opcode::BoundImageFetch)};
    REQUIRE(image != nullptr);
    CHECK(image->Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color2D);
    // The destination pair is split: two components in R12 and the third in R10.
    CHECK(fetch.Count(IR::Opcode::SetRegister) == 3);
}

TEST_CASE("TXQ queries the texture dimensions", "[video_core][native]") {
    const Translated query{TextureQuery};
    REQUIRE(query.Find(IR::Opcode::BoundImageQueryDimensions) != nullptr);
    // Width and height, into R24 and R25.
    CHECK(query.Count(IR::Opcode::SetRegister) == 2);
}

TEST_CASE("F2I converts into a 16-bit destination", "[video_core][native]") {
    const Translated convert{FloatToSmallInteger};
    REQUIRE(convert.Find(IR::Opcode::FPClamp32) != nullptr);
    REQUIRE(convert.Find(IR::Opcode::ConvertU32F32) != nullptr);
    CHECK(convert.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R38);
}

TEST_CASE("FRND rounds to an integral float", "[video_core][native]") {
    const Translated round{FloatRound};
    REQUIRE(round.Find(IR::Opcode::FPFloor32) != nullptr);
    CHECK(round.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R2);
}

TEST_CASE("I2F converts the selected halfword", "[video_core][native]") {
    const Translated convert{IntegerToFloatHalf};
    REQUIRE(convert.Find(IR::Opcode::ConvertF32U16) != nullptr);
    REQUIRE(convert.Find(IR::Opcode::BitFieldUExtract) != nullptr);
    CHECK(convert.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R5);
}

TEST_CASE("FMNMX reads a constant buffer source", "[video_core][native]") {
    const Translated minmax{FloatMinMaxBuffer};
    REQUIRE(minmax.Find(IR::Opcode::GetCbufF32) != nullptr);
    CHECK(minmax.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R20);
}

TEST_CASE("UBREV reverses a uniform register", "[video_core][native]") {
    const Translated reverse{UniformBitReverse};
    REQUIRE(reverse.Find(IR::Opcode::BitReverse32) != nullptr);
    CHECK(static_cast<u32>(reverse.Find(IR::Opcode::SetRegister)->Arg(0).Reg()) == 512 + 4);
}

TEST_CASE("ALD loads consecutive attributes into consecutive registers", "[video_core][native]") {
    const Translated wide{AttributeLoad};
    REQUIRE(wide.Count(IR::Opcode::GetAttribute) == 3);
    std::array<IR::Attribute, 3> attributes{};
    std::array<IR::Reg, 3> registers{};
    size_t index{};
    for (const IR::Inst& inst : wide.block) {
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            attributes[index] = inst.Arg(0).Attribute();
            ++index;
        }
    }
    CHECK(attributes[0] == IR::Attribute::Generic0X);
    CHECK(attributes[2] == IR::Attribute::Generic0Z);
    index = 0;
    for (const IR::Inst& inst : wide.block) {
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            registers[index] = inst.Arg(0).Reg();
            ++index;
        }
    }
    REQUIRE(index == 3);
    CHECK(registers[0] == IR::Reg::R0);
    CHECK(registers[2] == IR::Reg::R2);

    const Translated single{AttributeLoadSingle};
    REQUIRE(single.Count(IR::Opcode::GetAttribute) == 1);
    CHECK(single.Find(IR::Opcode::GetAttribute)->Arg(0).Attribute() ==
          IR::Attribute::Generic12X);
    CHECK(single.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R12);
}

TEST_CASE("AST stores attributes for the vertex in bits 64-71", "[video_core][native]") {
    // Four components write the whole position from four consecutive registers.
    const Translated position{AttributeStorePosition};
    REQUIRE(position.Count(IR::Opcode::SetAttribute) == 4);
    std::array<IR::Attribute, 4> attributes{};
    size_t index{};
    for (const IR::Inst& inst : position.block) {
        if (inst.GetOpcode() == IR::Opcode::SetAttribute) {
            attributes[index] = inst.Arg(0).Attribute();
            // Captured vertex programs name RZ, which is vertex 0.
            REQUIRE(inst.Arg(2).Inst() != nullptr);
            CHECK(inst.Arg(2).Inst()->GetOpcode() == IR::Opcode::GetRegister);
            CHECK(inst.Arg(2).Inst()->Arg(0).Reg() == IR::Reg::RZ);
            ++index;
        }
    }
    CHECK(attributes[0] == IR::Attribute::PositionX);
    CHECK(attributes[3] == IR::Attribute::PositionW);

    const Translated generic{AttributeStoreGeneric};
    REQUIRE(generic.Count(IR::Opcode::SetAttribute) == 1);
    CHECK(generic.Find(IR::Opcode::SetAttribute)->Arg(0).Attribute() ==
          IR::Attribute::Generic6X);

    // RZ stores zeros rather than reading a register that does not exist.
    const Translated zero{AttributeStoreZero};
    REQUIRE(zero.Count(IR::Opcode::SetAttribute) == 1);
    CHECK(zero.Find(IR::Opcode::SetAttribute)->Arg(0).Attribute() == IR::Attribute::Generic0X);
}

TEST_CASE("AST drops the one-component per-primitive hint", "[video_core][native]") {
    // a[0x5c] with one component is the shading rate hint: nothing reads it back, and the lowering
    // drops it too.
    CHECK(Translated{AttributeStoreHint}.Count(IR::Opcode::SetAttribute) == 0);
}

TEST_CASE("IPA reads a varying and applies perspective", "[video_core][native]") {
    // PositionW is not generic, so it is read as is.
    const Translated position{InterpolatePositionW};
    REQUIRE(position.Count(IR::Opcode::GetAttribute) == 1);
    CHECK(position.Find(IR::Opcode::GetAttribute)->Arg(0).Attribute() ==
          IR::Attribute::PositionW);
    CHECK(position.Count(IR::Opcode::FPMul32) == 0);
    CHECK(position.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R38);

    // An unused input map entry still means perspective, so the value is scaled by PositionW.
    const Translated generic{InterpolateGeneric};
    REQUIRE(generic.Count(IR::Opcode::FPMul32) == 1);
    bool read_position_w{};
    for (const IR::Inst& inst : generic.block) {
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            read_position_w |= inst.Arg(0).Attribute() == IR::Attribute::PositionW;
        }
    }
    CHECK(read_position_w);
    CHECK(generic.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R1);
}

TEST_CASE("ISBERD and ISBEWR reach the stage buffer", "[video_core][native]") {
    const Translated read{StageBufferRead, Shader::Stage::Mesh};
    REQUIRE(read.Find(IR::Opcode::IsbeRead) != nullptr);
    CHECK(read.Find(IR::Opcode::SetRegister)->Arg(0).Reg() == IR::Reg::R2);

    const Translated map_write{StageBufferMapWrite, Shader::Stage::Task};
    REQUIRE(map_write.Find(IR::Opcode::IsbeWriteMapU32) != nullptr);
    CHECK(map_write.Count(IR::Opcode::IsbeWriteMapU8) == 0);

    const Translated attribute_write{StageBufferAttributeWrite, Shader::Stage::Mesh};
    REQUIRE(attribute_write.Find(IR::Opcode::IsbeWriteAttribute) != nullptr);

    // A stage buffer access is only meaningful in a task or mesh program.
    Translated wrong_stage;
    CHECK_FALSE(wrong_stage.translator.Translate(StageBufferRead));
    CHECK(wrong_stage.translator.Error().find("task and mesh") != std::string_view::npos);
}
