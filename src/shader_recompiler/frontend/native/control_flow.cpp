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

bool ReferencesBlock(const IR::AbstractSyntaxList& syntax_list, const IR::Block* block) {
    using Type = IR::AbstractSyntaxNode::Type;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case Type::Block:
            if (node.data.block == block) {
                return true;
            }
            break;
        case Type::If:
            if (node.data.if_node.body == block || node.data.if_node.merge == block) {
                return true;
            }
            break;
        case Type::EndIf:
            if (node.data.end_if.merge == block) {
                return true;
            }
            break;
        case Type::Loop:
            if (node.data.loop.body == block || node.data.loop.continue_block == block ||
                node.data.loop.merge == block) {
                return true;
            }
            break;
        case Type::Repeat:
            if (node.data.repeat.loop_header == block || node.data.repeat.merge == block) {
                return true;
            }
            break;
        case Type::Break:
            if (node.data.break_node.merge == block || node.data.break_node.skip == block) {
                return true;
            }
            break;
        case Type::Return:
        case Type::Unreachable:
            break;
        }
    }
    return false;
}

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
        std::string failure;
        try {
            translated = translator.Translate(code[index]);
        } catch (const std::exception& exception) {
            // A type or consistency failure inside IR construction.
            failure = exception.what();
        }
        if (translated) {
            continue;
        }
        if (failure.empty()) {
            failure = translator.Error();
        }
        // Name the instruction: "unsupported native opcode" alone does not say where it is.
        if (tolerant) {
            issues.push_back(
                {index, fmt::format("instruction {} ({:#05x}): {}", index, code[index].Bits(0, 12),
                                    failure)});
            continue;
        }
        error = fmt::format("instruction {} ({:#05x}): {}", index, code[index].Bits(0, 12), failure);
        throw UnsupportedInstruction{};
    }
}

void Structurer::EmitFragmentExit(IR::Block& block) {
    if (env.ShaderStage() != Stage::Fragment) {
        return;
    }
    // The Maxwell frontend's EXIT does this for every fragment program: NVN2 leaves the color in
    // R0 and up and the pixel shader header says which render target and components those registers
    // belong to. Without it a native fragment program writes no color at all.
    IR::IREmitter ir{block};
    const ProgramHeader& sph{env.SPH()};
    IR::Reg source{IR::Reg::R0};
    for (u32 render_target = 0; render_target < 8; ++render_target) {
        if (!sph.ps.HasOutputComponents(render_target)) {
            continue;
        }
        const std::array<bool, 4> mask{sph.ps.EnabledOutputComponents(render_target)};
        for (u32 component = 0; component < 4; ++component) {
            if (!mask[component]) {
                ++source;
                continue;
            }
            ir.SetFragColor(render_target, component, ir.BitCast<IR::F32>(ir.GetReg(source)));
            ++source;
        }
    }
    if (sph.ps.omap.sample_mask != 0) {
        ir.SetSampleMask(ir.GetReg(source));
    }
}

void Structurer::EmitBreak(IR::Block* block, const IR::U1& cond, IR::Block* merge,
                           IR::Block* skip) {    block->AddBranch(merge);
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
    FindLoops();
    if (!error.empty()) {
        return false;
    }
    for (size_t index = 0; index < blocks.size(); ++index) {
        entries[index] = NewBlock();
    }
    return true;
}

void Structurer::FindLoops() {
    // A backward branch is a loop's back edge; its target block is the loop header. Every program in
    // the captured corpus is structured, so a header with two back edges means two continues and is
    // refused rather than guessed.
    loop_latch.assign(blocks.size(), NO_LATCH);
    for (u32 index = 0; index < blocks.size(); ++index) {
        const ControlBlock& block = blocks[index];
        if (block.kind != ControlBlock::End::Conditional && block.kind != ControlBlock::End::Branch) {
            continue;
        }
        if (block.kind == ControlBlock::End::Branch && block.target == block.terminator) {
            continue; // The closing self-branch that ends the program is not a loop.
        }
        const u32 target = BlockContaining(block.target);
        if (target > index) {
            continue;
        }
        if (loop_latch[target] != NO_LATCH) {
            error = "native loop has more than one back edge";
            return;
        }
        loop_latch[target] = index;
    }
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

void Structurer::EmitReturn(IR::Block* from) {
    // A return gets its own epilogue block, emitted right here: the SPIR-V backend branches a block
    // to the next block node, so the return cannot be appended at the end of the program unless the
    // block before it falls through to it.
    IR::Block* const exit_block = NewBlock();
    IR::IREmitter{*exit_block}.Epilogue();
    BranchTo(from, exit_block);
    AppendBlockNode(exit_block);
    syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
}

IR::Block* Structurer::EmitRange(u32 first, u32 last, IR::Block* merge) {
    IR::Block* current{};
    u32 index = first;
    while (index < last) {
        const ControlBlock& block = blocks[index];
        IR::Block* ir = entries[index];
        if (loop_latch[index] != NO_LATCH && !emitting_loops.contains(index)) {
            // A loop gets an empty header block so the Loop node can name a body, a continue block
            // and a merge without the header's own terminator getting in the way.
            const u32 latch = loop_latch[index];
            if (appended[index]) {
                error = "native loop header is also a merge block";
                throw UnsupportedInstruction{};
            }
            if (latch >= last) {
                error = "native loop crosses a region boundary";
                throw UnsupportedInstruction{};
            }
            const bool own_merge = latch + 1 < last;
            IR::Block* const loop_merge = own_merge ? entries[latch + 1] : merge;
            IR::Block* const loop_header = NewBlock();
            if (current != nullptr) {
                BranchTo(current, loop_header);
            }
            AppendBlockNode(loop_header);
            const size_t loop_index = syntax_list.size();
            syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Loop;

            emitting_loops.insert(index);
            loops.push_back({loop_header, loop_merge, latch});
            static_cast<void>(EmitRange(index, latch + 1, loop_merge));
            loops.pop_back();
            emitting_loops.erase(index);

            IR::Block* const body_entry{syntax_list.at(loop_index + 1).data.block};
            BranchTo(loop_header, body_entry);

            auto& loop = syntax_list[loop_index];
            loop.data.loop.body = body_entry;
            loop.data.loop.continue_block = entries[latch];
            loop.data.loop.merge = loop_merge;

            if (own_merge) {
                AppendBlockNode(loop_merge);
                appended[latch + 1] = true;
            }
            index = latch + 1;
            current = nullptr;
            continue;
        }
        if (current != nullptr) {
            BranchTo(current, ir);
        }
        if (!appended[index]) {
            AppendBlockNode(ir);
            appended[index] = true;
        }
        TranslateRange(ir, block.begin, block.end);
        current = ir;
        const IR::U1 taken{[&] {
            return block.kind == ControlBlock::End::Conditional ? ConditionValue(*ir, block)
                                                                : IR::U1{};
        }()};

        if (index == (loops.empty() ? NO_LATCH : loops.back().latch)) {
            // This block closes the loop: its back edge becomes the Repeat condition.
            IR::U1 condition;
            if (block.kind == ControlBlock::End::Conditional) {
                condition = taken;
            } else if (block.kind == ControlBlock::End::Branch) {
                condition = IR::U1{IR::IREmitter{*ir}.Imm1(true)};
            } else {
                error = "native loop back edge is not a branch";
                throw UnsupportedInstruction{};
            }
            const IR::U1 repeat_condition{IR::IREmitter{*ir}.ConditionRef(condition)};
            // The Repeat node emits a conditional branch to the header and to the merge, so the
            // latch's IR successors must be the same two blocks. Without them the merge has no
            // predecessor and a pass is free to drop it while the syntax list still names it.
            BranchTo(ir, loops.back().header);
            BranchTo(ir, loops.back().merge);
            Terminate(ir);
            auto& repeat = syntax_list.emplace_back();
            repeat.type = IR::AbstractSyntaxNode::Type::Repeat;
            repeat.data.repeat.cond = repeat_condition;
            repeat.data.repeat.loop_header = loops.back().header;
            repeat.data.repeat.merge = loops.back().merge;
            current = nullptr;
            ++index;
            continue;
        }

        switch (block.kind) {
        case ControlBlock::End::FallThrough:
            ++index;
            continue;

        case ControlBlock::End::Exit: {
            const auto [pred, negated] = block.cond.GetPred();
            const auto [extra_pred, extra_negated] = block.extra.GetPred();
            if (pred == IR::Pred::PT && !negated && extra_pred == IR::Pred::PT) {
                // An unconditional exit ends the program here.
                EmitFragmentExit(*ir);
                EmitReturn(ir);
                return nullptr;
            }
            // A conditional exit is a selection whose body returns: branching out of a selection to
            // the return block is not structured, so the return is a block inside it.
            IR::Block* const return_here = NewBlock();
            IR::IREmitter{*return_here}.Epilogue();
            EmitFragmentExit(*return_here);
            IR::Block* const skip = NewBlock();
            IR::IREmitter emitter{*ir};
            const IR::U1 condition = emitter.ConditionRef(ConditionValue(*ir, block));

            const size_t if_index = syntax_list.size();
            syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::If;
            AppendBlockNode(return_here);
            syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
            syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::EndIf;
            AppendBlockNode(skip);

            BranchTo(ir, return_here);
            BranchTo(ir, skip);
            Terminate(ir);

            auto& if_node = syntax_list[if_index];
            if_node.type = IR::AbstractSyntaxNode::Type::If;
            if_node.data.if_node.cond = condition;
            if_node.data.if_node.body = return_here;
            if_node.data.if_node.merge = skip;
            syntax_list[if_index + 2].data.end_if.merge = skip;

            current = skip;
            ++index;
            continue;
        }

        case ControlBlock::End::Branch: {
            if (block.target == block.terminator) {
                // A computed jump closes the program with a branch to itself.
                EmitFragmentExit(*ir);
                EmitReturn(ir);
                return nullptr;
            }
            const u32 target_block = BlockContaining(block.target);
            if (target_block >= last) {
                BranchTo(ir, merge);
                return merge;
            }
            error = "native unconditional branch into the middle of a region";
            throw UnsupportedInstruction{};
        }

        case ControlBlock::End::Conditional: {
            const u32 target_block = BlockContaining(block.target);
            if (target_block >= last) {
                // The guard leaves the region; its merge is where this range is meant to continue.
                IR::Block* skip = NewBlock();
                EmitBreak(ir, taken, merge, skip);
                Terminate(ir);
                AppendBlockNode(skip);
                current = skip;
                ++index;
                continue;
            }
            if (target_block <= index) {
                error = "native backward branch (loop) has no structuring yet";
                throw UnsupportedInstruction{};
            }
            if (target_block == index + 1) {
                // Both arms meet at the next block, so the condition changes nothing.
                ++index;
                continue;
            }

            // A diamond: taking the branch skips the body, so the fallthrough is the body and the
            // target is where the arms meet. The SPIR-V backend branches a block to the next block
            // in the syntax list, so the merge is emitted immediately after the EndIf.
            IR::Block* const branch_merge = entries[target_block];
            const size_t if_index = syntax_list.size();
            syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::If;
            const size_t body_index = syntax_list.size();

            static_cast<void>(EmitRange(index + 1, target_block, branch_merge));

            IR::Block* const body_entry{syntax_list.at(body_index).data.block};
            IR::IREmitter emitter{*ir};
            const IR::U1 condition = emitter.ConditionRef(IR::U1{emitter.LogicalNot(taken)});
            BranchTo(ir, body_entry);
            BranchTo(ir, branch_merge);
            Terminate(ir);

            auto& if_node = syntax_list[if_index];
            if_node.type = IR::AbstractSyntaxNode::Type::If;
            if_node.data.if_node.cond = condition;
            if_node.data.if_node.body = body_entry;
            if_node.data.if_node.merge = branch_merge;

            auto& endif_node = syntax_list.emplace_back();
            endif_node.type = IR::AbstractSyntaxNode::Type::EndIf;
            endif_node.data.end_if.merge = branch_merge;

            AppendBlockNode(branch_merge);
            appended[target_block] = true;
            index = target_block;
            current = nullptr;
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
    // A program that ends with an unconditional exit has already emitted its own epilogue; adding
    // another one would leave a block with no predecessor, which the SSA pass removes while the
    // syntax list keeps it.
    //
    // A loop or conditional exit can name the return block as its merge and end the range, in which
    // case EmitRange returns null but the merge still needs a label and an epilogue.
    IR::Block* const emitted = EmitRange(0, static_cast<u32>(blocks.size()), return_block);
    if (emitted != nullptr || ReferencesBlock(syntax_list, return_block)) {
        AppendBlockNode(return_block);
        syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
    }
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
