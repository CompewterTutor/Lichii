// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

/// Input injected by the debug server (EDEN_NX2_DEBUG_SERVER). It is ORed over the player's
/// controller state, so the physical controller keeps working and the player's mapping is never
/// changed. Nothing is injected until the debug server sets it, and settings are never touched.
namespace Core::HID::DebugInput {

/// Overrides the buttons and sticks of every emulated controller while active.
void Set(u64 buttons, s32 left_x, s32 left_y, s32 right_x, s32 right_y, bool has_sticks);
/// Replaces the injected buttons and keeps the current sticks.
void SetButtons(u64 buttons);
/// Stops injecting. The physical controller is in effect on the next read.
void Clear();

[[nodiscard]] bool Active();
[[nodiscard]] u64 Buttons();
[[nodiscard]] bool Sticks(s32& left_x, s32& left_y, s32& right_x, s32& right_y);

} // namespace Core::HID::DebugInput
