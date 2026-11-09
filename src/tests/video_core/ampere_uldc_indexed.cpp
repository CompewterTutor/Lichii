// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Ocarina of Time's compute shaders read a byte and a halfword through the indexed uniform load
// (ULDC.IS). These are captured instructions from that title's dump.

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/load_constant.h"

namespace {

using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
/// ULDC.U16 UR4, c[5][UR17+0]: the indexed halfword load the lowering refused.
constexpr Instruction CapturedHalfword{{0x11047abb, 0x01400000, 0x08000400, 0x000fe400}};
/// ULDC.U8 UR4, c[5][UR13+3]: the indexed byte load next to it.
constexpr Instruction CapturedByte{{0x0d047abb, 0x014000c0, 0x08000000, 0x000fe400}};

void SetBits(Instruction& instruction, u32 bit, u32 width, u32 value) {
    const u32 mask = ((1U << width) - 1) << (bit % 32);
    instruction.words[bit / 32] =
        (instruction.words[bit / 32] & ~mask) | ((value << (bit % 32)) & mask);
}

Shader::Maxwell::LDC::Encoding LoweredLoad(const Instruction& instruction) {
    const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 2);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions.front()) ==
            Shader::Maxwell::Opcode::LDC);
    Shader::Maxwell::LDC::Encoding encoding{};
    encoding.raw = lowered.instructions.front();
    return encoding;
}

} // Anonymous namespace

TEST_CASE("Indexed ULDC lowers subword widths through Maxwell LDC", "[video_core][ampere][uldc]") {
    const auto halfword = LoweredLoad(CapturedHalfword);
    CHECK(halfword.size == Shader::Maxwell::LDC::Size::U16);
    CHECK(halfword.mode == Shader::Maxwell::LDC::Mode::Default);
    CHECK(halfword.index == 5);
    CHECK(halfword.src_reg == static_cast<Shader::IR::Reg>(233));  // UR17 -> R233
    CHECK(halfword.dest_reg == static_cast<Shader::IR::Reg>(220)); // UR4 -> R220
    CHECK(halfword.offset == 0);

    const auto byte = LoweredLoad(CapturedByte);
    CHECK(byte.size == Shader::Maxwell::LDC::Size::U8);
    CHECK(byte.mode == Shader::Maxwell::LDC::Mode::Default);
    CHECK(byte.index == 5);
    CHECK(byte.src_reg == static_cast<Shader::IR::Reg>(229));  // UR13 -> R229
    CHECK(byte.dest_reg == static_cast<Shader::IR::Reg>(220)); // UR4 -> R220
    CHECK(byte.offset == 3);
}

TEST_CASE("Indexed ULDC keeps the bank, width and mode checks", "[video_core][ampere][uldc]") {
    // An unbound bank, an unknown width and an unbound destination all stay rejected, so
    // accepting subword sizes did not loosen the rest of the encoding. An offset the index
    // register can align is accepted: the host reads the aligned word either way.
    for (const auto change : std::array{
             std::array{54U, 5U, 18U}, std::array{73U, 3U, 1U},
             std::array{73U, 3U, 3U}, std::array{16U, 8U, 63U}}) {
        auto instruction = CapturedHalfword;
        SetBits(instruction, change[0], change[1], change[2]);
        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
        CHECK_FALSE(lowered);
    }
    auto odd = CapturedHalfword;
    SetBits(odd, 38, 16, 1);
    CHECK(LowerToMaxwell(std::array{odd, Exit}));
    auto mode = CapturedHalfword;
    SetBits(mode, 0, 12, 0xab9);
    SetBits(mode, 38, 16, 2);
    SetBits(mode, 24, 8, 63);
    CHECK_FALSE(LowerToMaxwell(std::array{mode, Exit}));
}
