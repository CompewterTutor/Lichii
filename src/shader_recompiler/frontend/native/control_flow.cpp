// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/native/control_flow.h"

#include <fmt/format.h>

namespace Shader::Native {

namespace {

/// Branch target in units of 16-byte instructions, relative to the end of the branch. The fields
/// and the 14-bit sign extension are the ones the lowering already uses.
[[nodiscard]] u32 BranchTarget(const Ampere::Instruction& insn, u32 index) {
    const u64 raw = (insn.words[1] & ~3U) | (u64{insn.words[2] & 0x3ffff} << 32);
    const s64 relative = static_cast<s64>(raw << 14) >> 14;
    const s64 position = s64{index} * 16 + 16 + relative;
    return static_cast<u32>(position / 16);
}

[[nodiscard]] IR::Condition DecodeCondition(const Ampere::Instruction& insn) {
    const u32 value = insn.Bits(12, 4);
    const u32 index = value & 7;
    const bool negated = (value & 8) != 0;
    if (index == 7) {
        return IR::Condition(!negated);
    }
    return IR::Condition(static_cast<IR::Pred>(index), negated);
}

} // Anonymous namespace

Structurer::Structurer(Environment& env_, std::span<const Ampere::Instruction> code_,
                       ObjectPool<IR::Inst>& inst_pool_, ObjectPool<IR::Block>& block_pool_)
    : env{env_}, code{code_}, inst_pool{inst_pool_}, block_pool{block_pool_} {}

IR::Block* Structurer::NewBlock() {
    return block_pool.Create(inst_pool);
}

void Structurer::BranchTo(IR::Block* from, IR::Block* to) {
    if (to == nullptr) {
        return;
    }
    for (IR::Block* successor : from->ImmSuccessors()) {
        if (successor == to) {
            return;
        }
    }
    from->AddBranch(to);
}

void Structurer::AppendBlockNode(IR::Block* block) {
    auto& node = syntax_list.emplace_back();
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = block;
}

IR::U1 Structurer::ConditionValue(IR::Block& block, const ControlBlock& control) {
    IR::IREmitter emitter{block};
    const auto value = [&](const IR::Condition& cond) {
        const auto [pred, negated] = cond.GetPred();
        if (pred == IR::Pred::PT) {
            return emitter.Imm1(!negated);
        }
        return emitter.GetPred(pred, negated);
    };
    IR::U1 result{value(control.cond)};
    if (control.extra.GetPred().first != IR::Pred::PT) {
        result = IR::U1{emitter.LogicalAnd(result, value(control.extra))};
    }
    return result;
}

void Structurer::TranslateRange(IR::Block* block, u32 begin, u32 end) {
    Translator translator{env, *block};
    for (u32 index = begin; index < end; ++index) {
        bool translated{};
        try {
            translated = translator.Translate(code[index]);
        } catch (const std::exception& exception) {
            // A type or consistency failure inside IR construction: name the instruction so the
            // report points at the word that caused it, not at the whole program.
            error = fmt::format("instruction {} ({:#05x}): {}", index, code[index].Bits(0, 12),
                                exception.what());
            throw UnsupportedInstruction{};
        }
        if (!translated) {
            error = translator.Error();
            throw UnsupportedInstruction{};
        }
    }
}

void Structurer::EmitBreak(IR::Block* block, const IR::U1& cond, IR::Block* merge,
                           IR::Block* skip) {
    block->AddBranch(merge);
    block->AddBranch(skip);
    IR::IREmitter emitter{*block};
    auto& node = syntax_list.emplace_back();
    node.type = IR::AbstractSyntaxNode::Type::Break;
    node.data.break_node.cond = emitter.ConditionRef(cond);
    node.data.break_node.merge = merge;
    node.data.break_node.skip = skip;
}

bool Structurer::Split() {
    if (code.empty()) {
        error = "native program is empty";
        return false;
    }
    std::vector<bool> boundary(code.size() + 1);
    boundary[0] = true;
    boundary[code.size()] = true;
    for (u32 index = 0; index < code.size(); ++index) {
        const u32 opcode = code[index].Bits(0, 12);
        if (opcode == 0x947) { // BRA
            if (code[index].Bits(32, 2) == 2) {
                continue; // .DIV never branches on the host
            }
            const u32 target = BranchTarget(code[index], index);
            if (target > code.size()) {
                error = "native branch leaves the captured program";
                return false;
            }
            boundary[target] = true;
            boundary[index + 1] = true;
        } else if (opcode == 0x945) { // BSSY: a block starts at the reconvergence point
            const u32 target = BranchTarget(code[index], index);
            if (target > code.size()) {
                error = "native reconvergence point leaves the captured program";
                return false;
            }
            boundary[target] = true;
        } else if (opcode == 0x94d) { // EXIT
            boundary[index + 1] = true;
        }
    }
    std::vector<u32> starts;
    for (u32 index = 0; index <= code.size(); ++index) {
        if (boundary[index]) {
            starts.push_back(index);
        }
    }
    for (size_t s = 0; s + 1 < starts.size(); ++s) {
        const u32 begin = starts[s];
        const u32 limit = starts[s + 1];
        ControlBlock block{};
        block.begin = begin;
        block.end = limit;
        block.kind = ControlBlock::End::FallThrough;
        bool terminated{};
        for (u32 index = begin; index < limit; ++index) {
            const u32 opcode = code[index].Bits(0, 12);
            if (opcode == 0x94d) {
                block.end = index;
                block.terminator = index;
                block.kind = ControlBlock::End::Exit;
                block.cond = DecodeCondition(code[index]);
                terminated = true;
                break;
            }
            if (opcode != 0x947) {
                continue;
            }
            const u32 mode = code[index].Bits(32, 2);
            const IR::Condition cond = DecodeCondition(code[index]);
            block.end = index;
            block.terminator = index;
            if (mode == 2) {
                // .DIV never branches, so the run continues past it.
                block.begin = index + 1;
                continue;
            }
            const u32 extra = code[index].Bits(87, 4);
            const IR::Condition extra_cond = extra == 7
                                                 ? IR::Condition(true)
                                                 : IR::Condition(static_cast<IR::Pred>(extra & 7),
                                                                 (extra & 8) != 0);
            const bool unconditional =
                mode == 3 ||
                (cond.GetPred().first == IR::Pred::PT &&
                 extra_cond.GetPred().first == IR::Pred::PT);
            block.kind =
                unconditional ? ControlBlock::End::Branch : ControlBlock::End::Conditional;
            block.cond = cond;
            block.extra = extra_cond;
            block.target = BranchTarget(code[index], index);
            terminated = true;
            break;
        }
        if (!terminated) {
            block.end = limit;
        }
        blocks.push_back(block);
    }
    entries.assign(blocks.size(), nullptr);
    appended.assign(blocks.size(), false);
    for (size_t index = 0; index < blocks.size(); ++index) {
        entries[index] = NewBlock();
    }
    return true;
}

u32 Structurer::BlockContaining(u32 instruction) const {
    u32 low = 0;
    u32 high = static_cast<u32>(blocks.size());
    while (low < high) {
        const u32 middle = (low + high) / 2;
        if (blocks[middle].begin <= instruction) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low == 0 ? 0 : low - 1;
}

IR::Block* Structurer::EmitRange(u32 first, u32 last, IR::Block* merge) {
    IR::Block* current{};
    u32 index = first;
    while (index < last) {
        const ControlBlock& block = blocks[index];
        IR::Block* ir = entries[index];
        if (!appended[index]) {
            AppendBlockNode(ir);
            appended[index] = true;
        }
        if (current != nullptr) {
            BranchTo(current, ir);
        }
        TranslateRange(ir, block.begin, block.end);
        current = ir;
        const IR::U1 taken{[&] {
            return block.kind == ControlBlock::End::Conditional ? ConditionValue(*ir, block)
                                                                : IR::U1{};
        }()};

        switch (block.kind) {
        case ControlBlock::End::FallThrough:
            ++index;
            continue;

        case ControlBlock::End::Exit: {
            const auto [pred, negated] = block.cond.GetPred();
            const auto [extra_pred, extra_negated] = block.extra.GetPred();
            if (pred == IR::Pred::PT && !negated && extra_pred == IR::Pred::PT) {
                BranchTo(ir, return_block);
                return nullptr;
            }
            IR::Block* skip = NewBlock();
            EmitBreak(ir, ConditionValue(*ir, block), return_block, skip);
            AppendBlockNode(skip);
            current = skip;
            ++index;
            continue;
        }

        case ControlBlock::End::Branch: {
            if (block.target == block.terminator) {
                // A computed jump closes the program with a branch to itself.
                BranchTo(ir, return_block);
                return nullptr;
            }
            const u32 target_block = BlockContaining(block.target);
            if (target_block >= last) {
                BranchTo(ir, merge);
                return nullptr;
            }
            error = "native branch into the middle of a region";
            throw UnsupportedInstruction{};
        }

        case ControlBlock::End::Conditional: {
            const u32 target_block = BlockContaining(block.target);
            if (target_block >= last) {
                // The guard leaves the region.
                IR::Block* skip = NewBlock();
                EmitBreak(ir, taken, merge, skip);
                AppendBlockNode(skip);
                current = skip;
                ++index;
                continue;
            }
            // A diamond: taking the branch skips the body, so the fallthrough is the body. The
            // block just before the target decides where both paths meet again.
            u32 merge_block = target_block;
            if (target_block > index + 1) {
                const ControlBlock& previous = blocks[target_block - 1];
                if (previous.kind == ControlBlock::End::Branch) {
                    const u32 previous_target = BlockContaining(previous.target);
                    if (previous_target > target_block && previous_target <= last) {
                        merge_block = previous_target;
                    }
                }
            }
            IR::Block* const diamond_merge = entries[merge_block];

            const size_t if_index = syntax_list.size();
            syntax_list.emplace_back();
            const size_t body_index = syntax_list.size();

            static_cast<void>(EmitRange(index + 1, target_block, diamond_merge));

            IR::Block* const body_entry{syntax_list.at(body_index).data.block};
            IR::IREmitter emitter{*ir};
            const IR::U1 condition = emitter.ConditionRef(IR::U1{emitter.LogicalNot(taken)});
            BranchTo(ir, body_entry);
            BranchTo(ir, diamond_merge);

            auto& if_node = syntax_list[if_index];
            if_node.type = IR::AbstractSyntaxNode::Type::If;
            if_node.data.if_node.cond = condition;
            if_node.data.if_node.body = body_entry;
            if_node.data.if_node.merge = diamond_merge;

            auto& endif_node = syntax_list.emplace_back();
            endif_node.type = IR::AbstractSyntaxNode::Type::EndIf;
            endif_node.data.end_if.merge = diamond_merge;

            // The else part follows the EndIf; the loop continues there and stops at the merge.
            index = target_block;
            continue;
        }
        }
    }
    if (current != nullptr) {
        BranchTo(current, merge);
    }
    return merge;
}

bool Structurer::BuildImpl() {
    return_block = NewBlock();
    IR::IREmitter{*return_block}.Epilogue();
    static_cast<void>(EmitRange(0, static_cast<u32>(blocks.size()), return_block));
    if (!appended.back()) {
        // Fall off the end of the program into the epilogue.
    }
    AppendBlockNode(return_block);
    syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
    return true;
}

bool Structurer::Build() {
    try {
        if (!Split()) {
            return false;
        }
        return BuildImpl();
    } catch (const UnsupportedInstruction&) {
        return false;
    }
}

} // namespace Shader::Native
