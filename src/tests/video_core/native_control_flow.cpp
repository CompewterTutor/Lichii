// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Control flow shapes are checked against captured words: the branch target encoding, the
// reconvergence points and the predicate guards all come from Ocarina of Time's compute program.

#include <array>
#include <optional>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/native/control_flow.h"

namespace {

namespace IR = Shader::IR;
using Shader::Ampere::Instruction;

class FlowEnvironment final : public Shader::Environment {
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

constexpr Instruction Nop{{0x00007918, 0, 0, 0x000fc000}};
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0x000fea00}};
/// A branch at `index` whose target is `target`. Predicate 7 is PT, so a predicated branch needs
/// bits 12-15 replaced rather than or-ed in.
Instruction Branch(u32 index, u32 target, u32 predicate = 7, u32 mode = 0) {
    const s64 relative = s64{target} * 16 - (s64{index} * 16 + 16);
    const u64 raw = static_cast<u64>(relative) & ((u64{1} << 50) - 1);
    // Bits 87-90 are the extra branch condition; 7 means none, the same field captured branches
    // carry.
    return Instruction{{(0x00007947U & ~0xf000U) | (predicate << 12),
                        static_cast<u32>(raw) | mode,
                        0x03800000U | static_cast<u32>(raw >> 32), 0x000fea00}};
}

struct Built {
    Shader::ObjectPool<IR::Inst> inst_pool{256};
    Shader::ObjectPool<IR::Block> block_pool{64};
    FlowEnvironment env;
    std::vector<Instruction> code;
    std::unique_ptr<Shader::Native::Structurer> structurer;

    explicit Built(std::vector<Instruction> program) : code{std::move(program)} {
        structurer = std::make_unique<Shader::Native::Structurer>(env, code, inst_pool, block_pool);
    }

    [[nodiscard]] std::vector<IR::AbstractSyntaxNode::Type> Nodes() const {
        std::vector<IR::AbstractSyntaxNode::Type> result;
        for (const auto& node : structurer->Syntax()) {
            result.push_back(node.type);
        }
        return result;
    }
};

} // Anonymous namespace

TEST_CASE("A predicated branch becomes an if", "[video_core][native][control]") {
    // 0 NOP, 1 @P0 BRA 3, 2 NOP, 3 NOP, 4 EXIT
    Built built{std::vector<Instruction>{Nop, Branch(1, 3, 0), Nop, Nop, Exit}};
    INFO(built.structurer->Error());
    REQUIRE(built.structurer->Build());

    using Type = IR::AbstractSyntaxNode::Type;
    const std::vector<Type> nodes{built.Nodes()};
    REQUIRE(nodes.size() == 7);
    CHECK(nodes[0] == Type::Block);   // 0, which holds the branch
    CHECK(nodes[1] == Type::If);
    CHECK(nodes[2] == Type::Block);   // 2, the body
    CHECK(nodes[3] == Type::EndIf);
    CHECK(nodes[4] == Type::Block);   // the merge
    CHECK(nodes[5] == Type::Block);   // the epilogue
    CHECK(nodes[6] == Type::Return);

    // The if header branches to the body and to the merge; the condition is the fallthrough path,
    // so it is the guard negated.
    const auto& entry = built.structurer->Syntax()[0].data.block;
    REQUIRE(entry->ImmSuccessors().size() == 2);
    CHECK(entry->ImmSuccessors()[0] == built.structurer->Syntax()[2].data.block);
    CHECK(entry->ImmSuccessors()[1] == built.structurer->Syntax()[4].data.block);
    CHECK(built.structurer->Syntax()[1].data.if_node.body ==
          built.structurer->Syntax()[2].data.block);
    CHECK(built.structurer->Syntax()[1].data.if_node.merge ==
          built.structurer->Syntax()[4].data.block);
}

TEST_CASE("An unconditional exit ends the program", "[video_core][native][control]") {
    Built built{std::vector<Instruction>{Nop, Exit}};
    INFO(built.structurer->Error());
    REQUIRE(built.structurer->Build());

    using Type = IR::AbstractSyntaxNode::Type;
    const std::vector<Type> nodes{built.Nodes()};
    REQUIRE(nodes.size() == 3);
    CHECK(nodes[0] == Type::Block);
    CHECK(nodes[1] == Type::Block); // the epilogue never gets its own if
    CHECK(nodes[2] == Type::Return);
}

TEST_CASE("A branch into an earlier block is refused", "[video_core][native][control]") {
    // 0 NOP, 1 NOP, 2 BRA 0, 3 EXIT: a backward branch that is not the closing self-branch.
    Built built{std::vector<Instruction>{Nop, Nop, Branch(2, 0), Exit}};
    CHECK_FALSE(built.structurer->Build());
    CHECK(built.structurer->Error() == "native branch into the middle of a region");
}

TEST_CASE("A closing self-branch ends the program", "[video_core][native][control]") {
    // 0 NOP, 1 BRA 1: a computed jump closes the program with a branch to itself.
    Built built{std::vector<Instruction>{Nop, Branch(1, 1)}};
    INFO(built.structurer->Error());
    REQUIRE(built.structurer->Build());
    CHECK(built.structurer->Blocks().size() == 2);
}
