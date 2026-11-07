// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/native/translate_program.h"

namespace {

using Shader::Ampere::Instruction;

Instruction WithOpcode(u32 opcode) {
    return Instruction{{opcode, 0, 0, 0}};
}

} // Anonymous namespace

TEST_CASE("Native frontend reports the opcodes it cannot translate", "[video_core][native]") {
    CHECK(Shader::Native::IsImplemented(0x918));  // NOP
    CHECK(Shader::Native::IsImplemented(0x223));  // FFMA
    CHECK_FALSE(Shader::Native::IsImplemented(0x94d)); // EXIT is control flow, not an opcode yet

    const std::array code{WithOpcode(0x94d), WithOpcode(0x94d), WithOpcode(0x918),
                          WithOpcode(0xf89)};
    const auto missing = Shader::Native::Missing(code);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].opcode == 0x94d); // most frequent first
    CHECK(missing[0].count == 2);
}

TEST_CASE("Zero padding is not an unimplemented opcode", "[video_core][native]") {
    // A raw allocation carries the program followed by zero words; they must not read as opcode 0.
    const std::array code{WithOpcode(0x918), WithOpcode(0x000), WithOpcode(0x000)};
    CHECK(Shader::Native::Missing(code).empty());

    // A zero opcode word with other bits set is not padding, so it is still reported.
    const std::array odd{Instruction{{0x00000000, 0x00000008, 0, 0}}, WithOpcode(0x000)};
    const auto missing = Shader::Native::Missing(odd);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].opcode == 0x0);
}

TEST_CASE("A program of implemented opcodes is covered", "[video_core][native]") {
    const std::array code{WithOpcode(0x918), WithOpcode(0xa23)};
    CHECK(Shader::Native::Missing(code).empty());
    CHECK(Shader::Native::ImplementedCount() == 15);
}
