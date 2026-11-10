// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The native program path has to produce a usable IR::Program, not just a syntax list: this runs
// the whole pipeline the shader cache runs, on a two-instruction program.

#include <array>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/native/program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {

using Shader::Ampere::Instruction;

class ProgramEnvironment final : public Shader::Environment {
public:
    explicit ProgramEnvironment(Shader::Stage stage_) {
        stage = stage_;
    }

    u64 ReadInstruction(u32) override {
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
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
    bool HasNativeInstructions() const override {
        return true;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

constexpr Instruction Nop{{0x00007918, 0, 0, 0x000fc000}};
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0x000fea00}};

} // Anonymous namespace

TEST_CASE("The native frontend builds a whole program", "[video_core][native]") {
    const std::array code{Nop, Nop, Exit};
    ProgramEnvironment env{Shader::Stage::Compute};

    Shader::ObjectPool<Shader::IR::Inst> inst_pool{256};
    Shader::ObjectPool<Shader::IR::Block> block_pool{64};
    const Shader::HostTranslateInfo host_info{};
    const Shader::IR::Program program{Shader::Native::TranslateProgram(
        inst_pool, block_pool, env, code, host_info)};

    REQUIRE(!program.blocks.empty());
    CHECK(program.stage == Shader::Stage::Compute);
    // The program must end in a return, which is what the epilogue block is for.
    CHECK(program.syntax_list.back().type == Shader::IR::AbstractSyntaxNode::Type::Return);
}

TEST_CASE("A vertex program keeps its attribute stores", "[video_core][native]") {
    // AST R0, a[0x70] writes the whole position; the store has to survive the whole pipeline the
    // shader cache runs, or the backend has nothing to emit for gl_Position.
    const std::array code{Instruction{{0xff007322, 0x00007000, 0x00000cff, 0x000fe200}}, Exit};
    ProgramEnvironment env{Shader::Stage::VertexB};

    Shader::ObjectPool<Shader::IR::Inst> inst_pool{256};
    Shader::ObjectPool<Shader::IR::Block> block_pool{64};
    const Shader::HostTranslateInfo host_info{};
    const Shader::IR::Program program{Shader::Native::TranslateProgram(
        inst_pool, block_pool, env, code, host_info)};

    size_t stores{};
    for (const Shader::IR::Block* block : program.blocks) {
        for (const Shader::IR::Inst& inst : *block) {
            stores += inst.GetOpcode() == Shader::IR::Opcode::SetAttribute ? 1 : 0;
        }
    }
    CHECK(stores == 4);
}

TEST_CASE("An untranslatable program throws so the caller can fall back",
          "[video_core][native]") {
    // ATOM GLOBAL CAS has no translation, so the whole-program path must fail loudly rather than
    // return a half-built program.
    const std::array code{Instruction{{0x000093a9, 0, 0, 0}}, Exit};
    ProgramEnvironment env{Shader::Stage::Compute};

    Shader::ObjectPool<Shader::IR::Inst> inst_pool{256};
    Shader::ObjectPool<Shader::IR::Block> block_pool{64};
    const Shader::HostTranslateInfo host_info{};
    CHECK_THROWS(Shader::Native::TranslateProgram(inst_pool, block_pool, env, code, host_info));
}
