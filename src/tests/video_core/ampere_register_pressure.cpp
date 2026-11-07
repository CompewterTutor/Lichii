// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A native instruction that touches a high guest register is re-expanded with low shadows
// (PrepareRegisterPressure). These opcodes appear in Ocarina of Time's shaders and used to fall
// through to the "unsupported" default, so the whole program was rejected.

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/register_pressure.h"

namespace {

using Shader::Ampere::Instruction;
using Shader::Ampere::PrepareRegisterPressure;

void SetBits(Instruction& instruction, u32 bit, u32 width, u32 value) {
    const u32 mask = ((1U << width) - 1) << (bit % 32);
    instruction.words[bit / 32] =
        (instruction.words[bit / 32] & ~mask) | ((value << (bit % 32)) & mask);
}

/// Every guest register the instruction names has to move below the reserved base.
void CheckShadowed(const Instruction& instruction) {
    const auto pressure = PrepareRegisterPressure(instruction, 216);
    INFO("opcode 0x" << std::hex << instruction.Bits(0, 12));
    REQUIRE(pressure.valid);
    if (pressure.prefix.empty()) {
        return; // no high register in a position this opcode reads
    }
    REQUIRE(pressure.prefix.size() == pressure.suffix.size());
    CHECK(pressure.instruction.Bits(16, 8) < 216);
}

} // Anonymous namespace

TEST_CASE("Register shadows cover the OoT pressure opcodes", "[video_core][ampere][pressure]") {
    // Captured instructions: LEA with an immediate addend, FMUL with a uniform source, a register
    // shift and an FFMA with a uniform addend, each naming a register at or above R216.
    for (const Instruction captured : {
             Instruction{0xcdd93811, 0x00008000, 0x078e80ff, 0x000fe400}, // 0x811
             Instruction{0x24e37c20, 0x00000004, 0x08410000, 0x001fe200}, // 0xc20
             Instruction{0x18257c23, 0x00000005, 0x080100e3, 0x002fe200}, // 0xc23
         }) {
        CheckShadowed(captured);
    }

    Instruction shift{0xff017219, 0x000000dc, 0x00011e6d, 0x000fc600}; // 0x219
    CheckShadowed(shift);
}

TEST_CASE("Register shadows cover IMNMX, its uniform form and R2UR", "[video_core][ampere][pressure]") {
    for (const u32 opcode : {0x217U, 0xa17U}) {
        Instruction minmax{0x00000000, 0x00000000, 0x03800000, 0x000fc400};
        SetBits(minmax, 0, 12, opcode);
        SetBits(minmax, 16, 8, 220);
        SetBits(minmax, 24, 8, 200);
        SetBits(minmax, 32, 8, 230);
        SetBits(minmax, 12, 4, 7);
        CheckShadowed(minmax);
    }
    // R2UR writes a uniform register, so only its guest source is a shadow candidate.
    Instruction r2ur{0x000e0000, 0x00000000, 0x000e0000, 0x000fc400};
    SetBits(r2ur, 0, 12, 0x3c2);
    SetBits(r2ur, 16, 8, 30);
    SetBits(r2ur, 24, 8, 220);
    SetBits(r2ur, 12, 4, 7);
    CheckShadowed(r2ur);
}

TEST_CASE("An opcode with unknown operands still refuses to be shadowed",
          "[video_core][ampere][pressure]") {
    Instruction unknown{0, 0, 0, 0};
    SetBits(unknown, 0, 12, 0x999);
    SetBits(unknown, 16, 8, 220);
    CHECK_FALSE(PrepareRegisterPressure(unknown, 216).valid);
}
