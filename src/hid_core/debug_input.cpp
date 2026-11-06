// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#include "hid_core/debug_input.h"

namespace Core::HID::DebugInput {
namespace {

std::atomic<bool> active{};
std::atomic<u64> buttons{};
std::atomic<bool> has_sticks{};
std::atomic<s32> left_x{};
std::atomic<s32> left_y{};
std::atomic<s32> right_x{};
std::atomic<s32> right_y{};

} // Anonymous namespace

void Set(u64 buttons_, s32 left_x_, s32 left_y_, s32 right_x_, s32 right_y_, bool has_sticks_) {
    left_x.store(left_x_, std::memory_order_relaxed);
    left_y.store(left_y_, std::memory_order_relaxed);
    right_x.store(right_x_, std::memory_order_relaxed);
    right_y.store(right_y_, std::memory_order_relaxed);
    has_sticks.store(has_sticks_, std::memory_order_relaxed);
    buttons.store(buttons_, std::memory_order_relaxed);
    active.store(true, std::memory_order_relaxed);
}

void SetButtons(u64 buttons_) {
    buttons.store(buttons_, std::memory_order_relaxed);
    active.store(true, std::memory_order_relaxed);
}

void Clear() {
    buttons.store(0, std::memory_order_relaxed);
    has_sticks.store(false, std::memory_order_relaxed);
    active.store(false, std::memory_order_relaxed);
}

bool Active() {
    return active.load(std::memory_order_relaxed);
}

u64 Buttons() {
    return active.load(std::memory_order_relaxed) ? buttons.load(std::memory_order_relaxed) : 0;
}

bool Sticks(s32& left_x_, s32& left_y_, s32& right_x_, s32& right_y_) {
    if (!active.load(std::memory_order_relaxed) ||
        !has_sticks.load(std::memory_order_relaxed)) {
        return false;
    }
    left_x_ = left_x.load(std::memory_order_relaxed);
    left_y_ = left_y.load(std::memory_order_relaxed);
    right_x_ = right_x.load(std::memory_order_relaxed);
    right_y_ = right_y.load(std::memory_order_relaxed);
    return true;
}

} // namespace Core::HID::DebugInput
