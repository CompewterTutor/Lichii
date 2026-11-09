// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

/// Requests from the debug server (EDEN_NX2_DEBUG_SERVER, docs/nx2/debug-mcp.md) to the thread
/// that owns the rasterizer. The server posts them from its own thread; RasterizerVulkan takes
/// them at the end of each presented frame, so every cache they touch is used from its owner.
/// Without the server nothing is posted and the per-frame cost is one relaxed atomic load.
namespace VideoCore::Nx2Debug {

struct CaptureRequest {
    u64 id;
    std::filesystem::path directory;
    bool sampled;
    /// The uniform and storage buffers of every draw and dispatch, and dispatch group counts.
    bool buffers;
};

struct CaptureResult {
    std::string error;
};

struct GpuReadRequest {
    u64 id;
    u64 gpu_address;
    u64 size;
};

struct GpuReadResult {
    std::vector<u8> bytes;
    /// Tegra::MemoryManager::GetID of the address space that maps the whole range.
    u64 address_space;
    std::string error;
};

struct RejectChange {
    u64 generation;
    /// Whether a hash left the list, so programs rejected before must be lowered again.
    bool unrejected;
};

/// Opens a debug server session (clears requests and rejections) or ends it (wakes waiters).
void BeginSession();
void EndSession();

/// Rasterizer thread: whether a capture or a reject change is waiting.
[[nodiscard]] bool HasRequests() noexcept;

/// Server thread: queues a frame capture; the rasterizer takes it at the next frame end.
void RequestCapture(CaptureRequest request);
/// Server thread: withdraws a capture that was not taken yet (it timed out).
void CancelCapture(u64 id);
/// Server thread: waits for the capture's manifest to be complete. Empty on timeout or when the
/// session ends.
[[nodiscard]] std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<CaptureRequest> TakeCapture();
/// Frame dump writer thread, once the manifest summary is written (or the capture failed).
void FinishCapture(u64 id, CaptureResult result);

/// Server thread: queues a read of GPU virtual memory; the rasterizer takes it at the next frame
/// end, where the GPU's own reads of its address spaces run.
void RequestGpuRead(GpuReadRequest request);
/// Server thread: withdraws a read that was not taken yet (it timed out).
void CancelGpuRead(u64 id);
/// Server thread: empty on timeout or when the session ends.
[[nodiscard]] std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<GpuReadRequest> TakeGpuRead();
/// Rasterizer thread.
void FinishGpuRead(u64 id, GpuReadResult result);

/// Server thread: adds or removes native program keys or shader hashes (as EDEN_NX2_REJECT_HASH
/// takes them). Returns the generation to wait for.
u64 SetRejected(std::span<const u64> hashes, bool rejected);[[nodiscard]] std::vector<u64> RejectedHashes();
/// Lowering: whether the debug server rejects this program. Cheap while the list is empty.
[[nodiscard]] bool IsRejected(u64 hash);
[[nodiscard]] bool AnyRejected() noexcept;
/// Rasterizer thread: the pending reject change, once.
[[nodiscard]] std::optional<RejectChange> TakeRejectChange();
/// Rasterizer thread: the cached programs of this generation were dropped.
void NoteRejectApplied(u64 generation);
/// Server thread: waits until the rasterizer applied the generation.
[[nodiscard]] bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout);

/// One read or write of GPU memory, run on the rasterizer thread so the caches it flushes are
/// the ones it owns.
struct MemoryAccess {
    u64 id{};
    GPUVAddr address{};
    std::size_t size{};
    bool write{};
    /// Input bytes for a write; filled with the bytes read for a read.
    std::vector<u8> bytes;
    /// Set by the rasterizer when the range is not fully mapped or the access failed.
    std::string error;
};

/// Server thread: queues one access; only one can be pending at a time. Returns false when one is.
[[nodiscard]] bool RequestMemoryAccess(MemoryAccess request);
/// Server thread: waits for the access with this id. Empty on timeout or when the session ends.
[[nodiscard]] std::optional<MemoryAccess> WaitMemoryAccess(u64 id,
                                                           std::chrono::milliseconds timeout);
/// Rasterizer thread: the pending access, once.
[[nodiscard]] std::optional<MemoryAccess> TakeMemoryAccess();
/// Rasterizer thread: publishes the result and wakes the waiter.
void FinishMemoryAccess(MemoryAccess result);

/// What a present-granular step did. The emulator is paused when this returns.
struct StepResult {
    u64 before{};
    u64 after{};
    bool timed_out{};
};

/// Runs the emulator until `frames` presentations have happened, then pauses it again.
///
/// Stepping has to be driven from the server thread: the emulator's own present path runs on a
/// guest thread, and pausing from there would suspend the thread that is calling. `set_paused`
/// takes the same lock the frontend uses, so the EmuThread is never half-suspended.
[[nodiscard]] StepResult StepPresents(const std::function<u64()>& presents,
                                      const std::function<void(bool paused)>& set_paused,
                                      const std::atomic<bool>& stopping, u64 frames,
                                      std::chrono::milliseconds timeout);

} // namespace VideoCore::Nx2Debug
