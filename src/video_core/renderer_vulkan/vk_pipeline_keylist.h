// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace Vulkan {

/// One graphics pipeline state without any game code: the shader programs are identified by the
/// content hash of their native instructions, and the rest is fixed pipeline state. Portable
/// between machines and drivers, unlike a driver pipeline cache.
struct PipelineListGraphicsKey {
    std::array<u64, 6> unique_hashes{};
    FixedPipelineState state{};

    bool operator==(const PipelineListGraphicsKey&) const noexcept;
};

struct PipelineListComputeKey {
    u64 unique_hash{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};

    bool operator==(const PipelineListComputeKey&) const noexcept;
};

/// Per-title list of the pipelines a game has used. It holds identifiers only (shader content
/// hashes and register state, never code), so a list can be shared between players of the same
/// title and build. `Siblings` returns the other states recorded for one exact set of shaders,
/// which the pipeline cache builds on background workers before the game asks for them.
class PipelineKeyList {
public:
    /// A list can be switched off with EDEN_NX2_KEYLIST=0.
    [[nodiscard]] static bool Enabled();

    /// Reads the rows of `file`, ignoring anything that does not verify. Existing rows are kept,
    /// and the rows read here are not written back as a contribution.
    void Load(const std::filesystem::path& file);

    /// Reads a contribution this machine has not merged into a shared list yet. Its rows stay
    /// pending, so a session that adds nothing still writes them out again.
    void LoadContribution(const std::filesystem::path& file);

    /// Writes every known row to `file`, replacing it. Rows are ordered by key hash.
    bool Save(const std::filesystem::path& file) const;

    /// Writes only the rows that are in no loaded list, in the same format, so a contribution can
    /// be merged into a shared list instead of replacing it. Nothing is written, and a stale file
    /// is removed, when there is no such row. Rows are ordered by key hash.
    bool SaveContribution(const std::filesystem::path& file) const;

    /// Adds a key unless it is already known. The driver id and version are cleared so a list is
    /// valid across driver updates; the cache restores them before a pipeline is built.
    void RecordGraphics(PipelineListGraphicsKey key);

    void RecordCompute(PipelineListComputeKey key);

    /// Every recorded graphics key that uses exactly these shaders, except `key` itself.
    [[nodiscard]] std::vector<PipelineListGraphicsKey> Siblings(
        const PipelineListGraphicsKey& key) const;

    [[nodiscard]] bool Contains(const PipelineListGraphicsKey& key) const;

    [[nodiscard]] size_t GraphicsCount() const;

    [[nodiscard]] size_t ComputeCount() const;

    /// Rows in no loaded list: exactly what `SaveContribution` would write.
    [[nodiscard]] size_t ContributionRows() const;

private:
    void LoadImpl(const std::filesystem::path& file, bool mark_loaded);

    mutable std::mutex mutex;
    std::unordered_map<u64, PipelineListGraphicsKey> graphics;
    std::unordered_map<u64, PipelineListComputeKey> compute;
    /// Shader set hash -> graphics key hashes, to find state variants of one set quickly.
    std::unordered_map<u64, std::vector<u64>> by_shaders;
    /// Hashes that came from a loaded list, so a session only contributes what it really added.
    std::unordered_set<u64> loaded_graphics;
    std::unordered_set<u64> loaded_compute;
};

} // namespace Vulkan
