// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/translate_program.h"

#include <algorithm>
#include <array>
#include <map>

namespace Shader::Native {

namespace {

/// Opcodes whose IR translation is implemented. Every entry needs a test with a captured
/// instruction: the ISA is undocumented, so "it looks like the Maxwell opcode" is not evidence.
constexpr std::array<u32, 142> Implemented{
    0x918,                                               // NOP
    0x220, 0x820, 0xa20, 0xc20,                          // FMUL
    0x221, 0x421, 0x621, 0x821, 0xa21, 0xc21, 0xe21,     // FADD
    0x223, 0x423, 0x623, 0x823, 0xa23, 0xc23, 0xe23,     // FFMA
    0xf89,                                               // SHFL with an immediate lane
    0x202, 0x802, 0xa02, 0xc02,                          // MOV register/immediate/cbuf/uniform
    0x882, 0xc82,                                        // UMOV immediate/uniform
    0x81c,                                               // PLOP3.LUT
    0x224, 0x424, 0x824, 0xc24, 0xe24,                   // IMAD (all forms)
    0x225, 0x825, 0x827,                                 // IMAD.WIDE, IMAD.HI
    0x81a,                                               // SGXT
    0xa24, 0x624,                                        // IMAD constant buffer forms
    0xa17,                                               // IMNMX constant buffer
    0x212, 0x812, 0xa12, 0xc12,                          // LOP3
    0x210,                                               // IADD3
    0x209,                                               // FMNMX
    0x819,                                               // SHF with an immediate shift
    0x211, 0x811, 0xa11, 0xc11,                          // LEA
    0x3c2,                                               // R2UR
    0x899,                                               // USHF
    0x28c, 0x88c,                                        // UISETP
    0x890,                                               // UIADD3 with an immediate
    0x287, 0x887,                                        // USEL
    0x8a5,                                               // UIMAD.WIDE
    0x8a4,                                               // UIMAD
    0x292, 0x892,                                        // ULOP3
    0x306, 0xd06,                                        // I2F
    0x305,                                               // F2I (word form)
    0x2a4, 0x2a5,                                        // UIMAD with a uniform multiplier
    0x245, 0xa45, 0xc45,                                 // I2FP 32-bit
    0x388,                                               // STS with a register offset
    0x387, 0x983,                                        // STL, LDL
    0x805,                                               // CS2R (zero idiom)
    0x803,                                               // P2R
    0x308,                                               // MUFU
    0x381, 0x981,                                        // LDG
    0x986,                                               // STG
    0x984, 0x988,                                        // LDS, STS
    0x589,                                               // SHFL with a register lane
    0x810, 0xa10, 0xc10,                                 // IADD3 immediate/cbuf/uniform
    0xab9, 0xabb,                                        // ULDC
    0x947,                                               // BRA, handled by the structurer
    0x94d,                                               // EXIT, handled by the structurer
    0x941, 0x942, 0x945, 0x948,                          // BSYNC, BREAK, BSSY, WARPSYNC
    0x919, 0x9c3,                                        // S2R / S2UR
    0x20b, 0x80b, 0xa0b, 0xc0b,                          // FSETP
    0x207, 0x807, 0xa07, 0x208, 0x808, 0xa08,            // SEL, FSEL
    0x309,                                               // POPC
    0x806, 0x886,                                        // VOTE, VOTEU
    0x98e, 0x3a8, 0x9a8,                                 // REDG, ATOMG
    0x290, 0x891, 0x291,                                 // UIADD3 register, ULEA
    0xb60, 0xb66, 0xb6f,                                 // TEX, TLD, TXQ
    0x91a,                                               // DEPBAR
    0xd00,                                               // FLO (uniform source)
    0x307,                                               // FRND
    0xd09,                                               // POPC (uniform source)
    0xa09,                                               // FMNMX (constant buffer source)
    0x2be,                                               // UBREV
    0xb82,                                               // LDC with an indexed offset
    0x3c4,                                               // REDUX
    0x217, 0x817,                                        // IMNMX
    0x300, 0x301,                                        // FLO, BREV
    0x816,                                               // PRMT
    0x419,                                               // SHF.L.U32.HI
    0x809, 0xb08, 0xd08,                                 // FMNMX/MUFU extra forms
    0x20c, 0x80c, 0xa0c, 0xc0c,                          // ISETP
    0x321, 0x322,                                        // ALD, AST
    0x326,                                               // IPA
    0x923, 0x927,                                        // ISBERD, ISBEWR
};

} // Anonymous namespace

bool IsImplemented(u32 opcode) {
    return std::ranges::find(Implemented, opcode) != Implemented.end();
}

std::size_t ImplementedCount() {
    return Implemented.size();
}

std::vector<MissingOpcode> Missing(std::span<const Ampere::Instruction> code) {
    std::map<u32, std::size_t> counts;
    for (const Ampere::Instruction& instruction : code) {
        const u32 opcode = instruction.Bits(0, 12);
        // Zero words are padding after a program's end in a raw allocation, not instructions.
        if (opcode == 0 && instruction.words[1] == 0 && instruction.words[2] == 0 &&
            instruction.words[3] == 0) {
            continue;
        }
        if (!IsImplemented(opcode)) {
            ++counts[opcode];
        }
    }
    std::vector<MissingOpcode> missing;
    missing.reserve(counts.size());
    for (const auto& [opcode, count] : counts) {
        missing.push_back({.opcode = opcode, .count = count});
    }
    std::ranges::stable_sort(missing, [](const MissingOpcode& a, const MissingOpcode& b) {
        return a.count != b.count ? a.count > b.count : a.opcode < b.opcode;
    });
    return missing;
}

} // namespace Shader::Native
