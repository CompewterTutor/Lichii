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
};

struct Translated {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    TranslateEnvironment env;
    Shader::Native::Translator translator{env, block};

    Translated() = default;

    explicit Translated(const Instruction& instruction) {
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

TEST_CASE("Opcode and form gaps fail instead of guessing", "[video_core][native]") {
    Translated program;
    // ATOM GLOBAL CAS has no IR translation yet.
    CHECK_FALSE(program.translator.Translate(Instruction{{0x000093a9, 0, 0, 0}}));
    CHECK(program.translator.Error() == "unsupported native opcode");

    // FFMA form 6 takes a uniform source, which becomes an SSA value only in 1.7.3.
    CHECK_FALSE(program.translator.Translate(
        Instruction{{0x00077c23, 0x00000004, 0x00010009, 0x000fe200}}));
    CHECK(program.translator.Error() == "native uniform source");
}
