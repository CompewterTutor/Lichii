// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/program.h"

#include <array>
#include <set>
#include <stdexcept>
#include <string>

#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/post_order.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"

namespace Shader::Native {

namespace {

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    IR::BlockList blocks;
    u32 order{};
    for (const auto& node : syntax_list) {
        if (node.type != IR::AbstractSyntaxNode::Type::Block) {
            continue;
        }
        blocks.push_back(node.data.block);
        blocks.back()->SetOrder(order++);
    }
    return blocks;
}

/// Every block a syntax node names must also appear as a block node: the SPIR-V backend labels only
/// the emitted blocks, so a merge, branch target or loop header that is missing would be emitted
/// with an unset label and take the process down. The structurer is supposed to keep this invariant;
/// check it here so an oversight falls back to the lowering instead of crashing.
void ValidateSyntax(const IR::AbstractSyntaxList& syntax_list, const IR::BlockList& blocks) {
    std::set<const IR::Block*> emitted;
    for (const IR::Block* block : blocks) {
        emitted.insert(block);
    }
    using Type = IR::AbstractSyntaxNode::Type;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        std::array<const IR::Block*, 3> referenced{};
        size_t count{};
        switch (node.type) {
        case Type::Block:
            break;
        case Type::If:
            referenced[count++] = node.data.if_node.body;
            referenced[count++] = node.data.if_node.merge;
            break;
        case Type::EndIf:
            referenced[count++] = node.data.end_if.merge;
            break;
        case Type::Loop:
            referenced[count++] = node.data.loop.body;
            referenced[count++] = node.data.loop.continue_block;
            referenced[count++] = node.data.loop.merge;
            break;
        case Type::Repeat:
            referenced[count++] = node.data.repeat.loop_header;
            referenced[count++] = node.data.repeat.merge;
            break;
        case Type::Break:
            referenced[count++] = node.data.break_node.merge;
            referenced[count++] = node.data.break_node.skip;
            break;
        case Type::Return:
        case Type::Unreachable:
            break;
        }
        for (size_t index = 0; index < count; ++index) {
            if (referenced[index] != nullptr && !emitted.contains(referenced[index])) {
                throw NotImplementedException(
                    "Native frontend: the syntax list names a block that is never emitted");
            }
        }
    }
}

} // Anonymous namespace

IR::Program TranslateProgram(ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
                             Environment& env, std::span<const Ampere::Instruction> code,
                             const HostTranslateInfo& host_info) {
    Structurer structurer{env, code, inst_pool, block_pool};
    if (!structurer.Build()) {
        throw NotImplementedException("Native frontend: {}", structurer.Error());
    }

    IR::Program program;
    program.syntax_list = structurer.Syntax();
    program.blocks = GenerateBlocks(program.syntax_list);
    if (program.syntax_list.empty()) {
        throw NotImplementedException("Native frontend produced no syntax");
    }
    ValidateSyntax(program.syntax_list, program.blocks);
    program.post_order_blocks = PostOrder(program.syntax_list.front());

    // The Maxwell frontend starts every program with a prologue, which the SPIR-V backend turns
    // into the default gl_Position (0, 0, 0, 1) for a vertex stage. NVN2 vertex programs write
    // only the components they compute, so without it Z and W stay undefined and the geometry
    // lands outside the view.
    IR::Block& first_block{*program.syntax_list.front().data.block};
    IR::IREmitter{first_block, first_block.begin()}.Prologue();

    Maxwell::FinalizeProgram(program, env, host_info);
    return program;
}

} // namespace Shader::Native
