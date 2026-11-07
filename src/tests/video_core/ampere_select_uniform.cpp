// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// 0xc08 is FSEL with a uniform second source. Ocarina of Time's fragment shaders use it in the
// foliage pass, where it was the last unsupported opcode in the program.

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {

using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
/// FSEL R1, R1, UR4, P1
constexpr Instruction CapturedSelect{{0x01017c08, 0x00000004, 0x08800000, 0x000fe400}};
/// FSEL R43, RZ, UR7, !P4
constexpr Instruction CapturedNegatedSelect{{0xff2b7c08, 0x00000007, 0x0e000000, 0x000fe400}};

void SetBits(Instruction& instruction, u32 bit, u32 width, u32 value) {
    const u32 mask = ((1U << width) - 1) << (bit % 32);
    instruction.words[bit / 32] =
        (instruction.words[bit / 32] & ~mask) | ((value << (bit % 32)) & mask);
}

u64 Expected(u32 dest, u32 source, u32 uniform, u32 predicate, bool negated) {
    return 0x5ca0000000000000ULL | (u64{7} << 16) | dest | (u64{source} << 8) |
           (u64{uniform} << 20) | (u64{predicate} << 39) | (u64{negated} << 42);
}

} // Anonymous namespace

TEST_CASE("FSEL with a uniform source lowers to Maxwell SEL", "[video_core][ampere][fsel]") {
    const auto lowered = LowerToMaxwell(std::array{CapturedSelect, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 2);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions.front()) == Shader::Maxwell::Opcode::SEL_reg);
    // UR4 lives at R220; P1 selects.
    CHECK(lowered.instructions.front() == Expected(1, 1, 220, 1, false));

    const auto negated = LowerToMaxwell(std::array{CapturedNegatedSelect, Exit});
    INFO(negated.error);
    REQUIRE(negated);
    // RZ stays RZ, UR7 is R223, and !P4 is the predicate with the not bit set.
    CHECK(negated.instructions.front() == Expected(43, 255, 223, 4, true));
}

TEST_CASE("FSEL rejects a non-uniform mode word", "[video_core][ampere][fsel]") {
    auto instruction = CapturedSelect;
    SetBits(instruction, 91, 1, 0);
    CHECK_FALSE(LowerToMaxwell(std::array{instruction, Exit}));

    auto other = CapturedSelect;
    SetBits(other, 92, 1, 1);
    CHECK_FALSE(LowerToMaxwell(std::array{other, Exit}));
}
