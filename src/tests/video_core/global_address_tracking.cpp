// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
class TestEnvironment final : public Shader::Environment {
public:
    bool IsNativeProgram() const override {
        return native;
    }
    bool native{};

    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};
}

TEST_CASE("Global address tracking stops at a memory load", "[shader_recompiler][global_memory]") {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto low = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x110));
    const auto high = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x114));
    const auto source = ir.PackUint2x32(ir.CompositeConstruct(low, high));
    const auto loaded = ir.LoadGlobal64(source);
    const auto destination = ir.PackUint2x32(ir.CompositeConstruct(
        ir.CompositeExtract(loaded, 0), ir.CompositeExtract(loaded, 1)));
    ir.WriteGlobal32(destination, ir.Imm32(1));

    IR::Program program;
    program.post_order_blocks.push_back(&block);
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    TestEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    const auto has = [&](IR::Opcode opcode) {
        return std::ranges::any_of(block.Instructions(), [=](const IR::Inst& inst) {
            return inst.GetOpcode() == opcode;
        });
    };
    REQUIRE(has(IR::Opcode::LoadStorage64));
    REQUIRE(has(IR::Opcode::WriteGlobal32));
    REQUIRE_FALSE(has(IR::Opcode::WriteStorage32));
}

namespace {
/// The address shape the native frontend builds for a load with a uniform base, a register
/// offset and an immediate offset, and the shape the Maxwell lowering builds for the same load.
enum class LoadShape {
    ImmediateAdd, // PackUint2x32(pair) + Imm64, what the lowering emits
    SumLow,       // low word added with IAdd32, high word untouched
    SumLowCarry,  // low word added with IAdd32 and the carry added to the high word
};

bool TracksStorageBuffer(LoadShape shape, u32 immediate) {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto low = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x110));
    const auto high = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x114));
    const auto base = ir.PackUint2x32(ir.CompositeConstruct(low, high));
    const auto offset = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x120));
    const auto pointer = [&]() -> IR::U64 {
        switch (shape) {
        case LoadShape::ImmediateAdd:
            return ir.IAdd(base, ir.Imm64(u64{immediate}));
        case LoadShape::SumLow: {
            const auto pair = ir.PackUint2x32(
                ir.CompositeConstruct(ir.IAdd(low, offset), high));
            return ir.IAdd(pair, ir.Imm64(u64{immediate}));
        }
        case LoadShape::SumLowCarry: {
            const auto sum64 = ir.IAdd(
                ir.PackUint2x32(ir.CompositeConstruct(low, ir.Imm32(0))),
                ir.PackUint2x32(ir.CompositeConstruct(offset, ir.Imm32(0))));
            const auto words = ir.UnpackUint2x32(sum64);
            const auto pair = ir.PackUint2x32(ir.CompositeConstruct(
                ir.IAdd(low, offset), ir.IAdd(high, IR::U32{ir.CompositeExtract(words, 1)})));
            return ir.IAdd(pair, ir.Imm64(u64{immediate}));
        }
        }
        return base;
    }();
    const IR::U32 loaded = ir.LoadGlobal32(pointer);
    (void)loaded;

    IR::Program program;
    program.post_order_blocks.push_back(&block);
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    TestEnvironment env;
    env.native = true;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    return std::ranges::any_of(block.Instructions(), [](const IR::Inst& inst) {
        return inst.GetOpcode() == IR::Opcode::LoadStorage32;
    });
}
} // Anonymous namespace

TEST_CASE("A native register offset keeps the load on a storage buffer",
          "[shader_recompiler][global_memory]") {
    for (const u32 immediate : {0U, 0x40U}) {
        INFO("immediate " << immediate);
        CHECK(TracksStorageBuffer(LoadShape::ImmediateAdd, immediate));
        CHECK(TracksStorageBuffer(LoadShape::SumLow, immediate));
        CHECK(TracksStorageBuffer(LoadShape::SumLowCarry, immediate));
    }
}
