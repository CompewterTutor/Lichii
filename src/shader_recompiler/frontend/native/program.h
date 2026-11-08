// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/native/control_flow.h"
#include "shader_recompiler/object_pool.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Native {

/// Translates a captured native program end to end: instructions to IR, control flow to an abstract
/// syntax list, then the same stage setup and optimization pipeline the Maxwell frontend uses.
///
/// Throws on anything the frontend cannot translate yet, so a caller that wants the lowering as a
/// fallback can catch and hand the program back.
[[nodiscard]] IR::Program TranslateProgram(ObjectPool<IR::Inst>& inst_pool,
                                           ObjectPool<IR::Block>& block_pool, Environment& env,
                                           std::span<const Ampere::Instruction> code,
                                           const HostTranslateInfo& host_info);

} // namespace Shader::Native
