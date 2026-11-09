// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/native/translate.h"
#include "shader_recompiler/object_pool.h"

namespace Shader::Native {

/// One straight run of translatable instructions and the control-flow instruction that ends it.
struct ControlBlock {
    enum class End {
        FallThrough,
        Branch,
        Conditional,
        Exit,
    };

    u32 begin{};        ///< First instruction index, included.
    u32 end{};          ///< One past the last instruction of the run.
    u32 terminator{};   ///< Instruction index of the BRA or EXIT that ends the run.
    End kind{};
    IR::Condition cond{};
    /// A second condition in bits 87-90 that the branch also requires; PT when there is none.
    IR::Condition extra{true};
    u32 target{};       ///< Branch target as an instruction index.
};

/// Builds IR blocks and an abstract syntax list from the native instruction stream.
///
/// NVN2 control flow is structured: BSSY names a reconvergence point, BSYNC closes a region, BRA
/// leaves it and EXIT ends the program. Warp reconvergence has no host equivalent and is dropped;
/// only the branches those instructions delimit matter, so blocks are split at branch targets and
/// after terminators and the regions are rebuilt from the branches alone.
class Structurer {
public:
    Structurer(Environment& env, std::span<const Ampere::Instruction> code,
               ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool);

    /// Build the whole program. Returns false and sets Error() when a shape has no translation yet.
    [[nodiscard]] bool Build();

    [[nodiscard]] std::string_view Error() const noexcept {
        return error;
    }

    [[nodiscard]] const IR::AbstractSyntaxList& Syntax() const noexcept {
        return syntax_list;
    }

    [[nodiscard]] std::span<const ControlBlock> Blocks() const noexcept {
        return blocks;
    }

    /// Diagnostic mode: an instruction without a translation is recorded and skipped instead of
    /// failing the build, so one run lists every blocked instruction in the program. The IR a
    /// tolerant build produces is not valid output: coverage reporting only.
    void SetTolerant(bool value) noexcept {
        tolerant = value;
    }

    struct Issue {
        u32 instruction;
        std::string message;
    };

    [[nodiscard]] const std::vector<Issue>& Issues() const noexcept {
        return issues;
    }

private:
    struct UnsupportedInstruction {};

    [[nodiscard]] bool Split();
    [[nodiscard]] bool BuildImpl();

    [[nodiscard]] IR::Block* NewBlock();
    void BranchTo(IR::Block* from, IR::Block* to);
    /// True once a block has its terminator, so a fallthrough does not add a third successor.
    [[nodiscard]] bool Terminated(const IR::Block* block) const {
        return terminated.contains(const_cast<IR::Block*>(block));
    }
    void Terminate(IR::Block* block) {
        terminated.insert(block);
    }
    void AppendBlockNode(IR::Block* block);
    void TranslateRange(IR::Block* block, u32 begin, u32 end);
    void EmitBreak(IR::Block* block, const IR::U1& cond, IR::Block* merge, IR::Block* skip);
    [[nodiscard]] IR::U1 ConditionValue(IR::Block& block, const ControlBlock& control);
    [[nodiscard]] u32 BlockContaining(u32 instruction) const;
    [[nodiscard]] IR::Block* EmitRange(u32 first, u32 last, IR::Block* merge);
    /// SPIR-V allows a block to be the merge block of one header only. Two diamonds that converge on
    /// the same block are such a case, so the second gets an empty block that branches to it.
    [[nodiscard]] IR::Block* UniqueMerge(IR::Block* merge);

    Environment& env;
    std::span<const Ampere::Instruction> code;
    ObjectPool<IR::Inst>& inst_pool;
    ObjectPool<IR::Block>& block_pool;
    std::vector<ControlBlock> blocks;
    std::vector<IR::Block*> entries;
    std::vector<bool> appended;
    IR::AbstractSyntaxList syntax_list;
    IR::Block* return_block{};
    /// The translator builds some of its messages at runtime, so the structurer has to own a copy:
    /// keeping a string_view would dangle as soon as the translator goes out of scope.
    std::string error;
    bool tolerant{};
    std::vector<Issue> issues;
    std::set<IR::Block*> merge_blocks;
    std::set<IR::Block*> terminated;
};

} // namespace Shader::Native
