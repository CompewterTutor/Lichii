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
    CHECK(Shader::Native::IsImplemented(0x94d));  // EXIT
    CHECK_FALSE(Shader::Native::IsImplemented(0x223));

    const std::array code{WithOpcode(0x223), WithOpcode(0x223), WithOpcode(0x94d),
                          WithOpcode(0x221)};
    const auto missing = Shader::Native::Missing(code);
    REQUIRE(missing.size() == 2);
    CHECK(missing[0].opcode == 0x223); // most frequent first
    CHECK(missing[0].count == 2);
    CHECK(missing[1].opcode == 0x221);
    CHECK(missing[1].count == 1);
}

TEST_CASE("A program of implemented opcodes is covered", "[video_core][native]") {
    const std::array code{WithOpcode(0x918), WithOpcode(0x94d)};
    CHECK(Shader::Native::Missing(code).empty());
    CHECK(Shader::Native::ImplementedCount() == 2);
}
