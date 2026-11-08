// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/program.h"

#include <stdexcept>
#include <string>

#include "shader_recompiler/exception.h"
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
    program.post_order_blocks = PostOrder(program.syntax_list.front());

    Maxwell::FinalizeProgram(program, env, host_info);
    return program;
}

} // namespace Shader::Native
