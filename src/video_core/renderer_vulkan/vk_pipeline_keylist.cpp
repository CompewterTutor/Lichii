// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_pipeline_keylist.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <utility>

#include "common/cityhash.h"
#include "common/logging.h"

namespace Vulkan {

namespace {

constexpr std::array<char, 8> MagicNumber{'l', 'c', 'h', 'k', 'e', 'y', 'l', 's'};
constexpr u32 FormatVersion = 1;
constexpr u8 KindGraphics = 0;
constexpr u8 KindCompute = 1;
/// A foreign or damaged file must not be able to exhaust memory.
constexpr size_t MaxRows = 1'000'000;

/// The driver id and version describe the machine that recorded a row, not the pipeline. Clearing
/// them lets a list from another machine or another driver build be matched here.
void ClearDriver(PipelineListGraphicsKey& key) noexcept {
    key.state.driver_id = 0;
    key.state.driver_version = 0;
}

u64 ShaderSetHash(const std::array<u64, 6>& unique_hashes) noexcept {
    return Common::CityHash64(reinterpret_cast<const char*>(unique_hashes.data()),
                              sizeof(unique_hashes));
}

u64 GraphicsKeyHash(const PipelineListGraphicsKey& key) noexcept {
    const size_t size{sizeof(PipelineListGraphicsKey::unique_hashes) + key.state.Size()};
    return Common::CityHash64(reinterpret_cast<const char*>(&key), size);
}

u64 ComputeKeyHash(const PipelineListComputeKey& key) noexcept {
    return Common::CityHash64(reinterpret_cast<const char*>(&key), sizeof(key));
}

template <typename T>
bool ReadPod(std::istream& stream, T& value) {
    return static_cast<bool>(
        stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
void WritePod(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

using GraphicsRow = std::pair<u64, const PipelineListGraphicsKey*>;
using ComputeRow = std::pair<u64, const PipelineListComputeKey*>;

/// Writes sorted rows in the list format, replacing `file` through a temporary file.
bool WriteRows(const std::filesystem::path& file, const std::vector<GraphicsRow>& graphics_rows,
               const std::vector<ComputeRow>& compute_rows) {
    const auto temporary = file.string() + ".tmp";
    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
    if (!stream.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open key list for writing: {}", file.string());
        return false;
    }
    stream.write(MagicNumber.data(), MagicNumber.size());
    WritePod(stream, FormatVersion);
    for (const auto& [hash, key] : graphics_rows) {
        const u8 kind = KindGraphics;
        const u8 reserved{};
        const u16 reserved16{};
        const u32 state_size = static_cast<u32>(key->state.Size());
        WritePod(stream, kind);
        WritePod(stream, reserved);
        WritePod(stream, reserved16);
        WritePod(stream, hash);
        for (const u64 unique_hash : key->unique_hashes) {
            WritePod(stream, unique_hash);
        }
        WritePod(stream, state_size);
        stream.write(reinterpret_cast<const char*>(&key->state), state_size);
    }
    for (const auto& [hash, key] : compute_rows) {
        const u8 kind = KindCompute;
        const u8 reserved{};
        const u16 reserved16{};
        WritePod(stream, kind);
        WritePod(stream, reserved);
        WritePod(stream, reserved16);
        WritePod(stream, hash);
        WritePod(stream, key->unique_hash);
        WritePod(stream, key->shared_memory_size);
        for (const u32 size : key->workgroup_size) {
            WritePod(stream, size);
        }
    }
    stream.close();
    if (!stream) {
        LOG_ERROR(Common_Filesystem, "Failed to write key list: {}", file.string());
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to replace key list {}: {}", file.string(),
                  error.message());
        return false;
    }
    return true;
}

} // Anonymous namespace

bool PipelineListGraphicsKey::operator==(const PipelineListGraphicsKey& rhs) const noexcept {
    if (unique_hashes != rhs.unique_hashes) {
        return false;
    }
    return std::memcmp(&state, &rhs.state, state.Size()) == 0;
}

bool PipelineListComputeKey::operator==(const PipelineListComputeKey& rhs) const noexcept {
    return std::memcmp(this, &rhs, sizeof(*this)) == 0;
}

bool PipelineKeyList::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_KEYLIST");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

void PipelineKeyList::Load(const std::filesystem::path& file) {
    LoadImpl(file, true);
}

void PipelineKeyList::LoadContribution(const std::filesystem::path& file) {
    LoadImpl(file, false);
}

void PipelineKeyList::LoadImpl(const std::filesystem::path& file, bool mark_loaded) {
    std::ifstream stream{file, std::ios::binary};
    if (!stream.is_open()) {
        return;
    }
    std::array<char, 8> magic{};
    u32 version{};
    stream.read(magic.data(), magic.size());
    if (!stream || magic != MagicNumber || !ReadPod(stream, version) || version != FormatVersion) {
        LOG_INFO(Render_Vulkan, "Ignoring key list with an unknown format: {}", file.string());
        return;
    }

    size_t added{};
    size_t rejected{};
    std::scoped_lock lock{mutex};
    while (stream.good() && graphics.size() + compute.size() < MaxRows) {
        u8 kind{};
        if (!ReadPod(stream, kind)) {
            break;
        }
        u8 reserved{};
        u16 reserved16{};
        u64 stored_hash{};
        ReadPod(stream, reserved);
        ReadPod(stream, reserved16);
        if (!ReadPod(stream, stored_hash)) {
            ++rejected;
            break;
        }
        if (kind == KindGraphics) {
            PipelineListGraphicsKey key{};
            u32 state_size{};
            bool ok = true;
            for (u64& hash : key.unique_hashes) {
                ok = ok && ReadPod(stream, hash);
            }
            ok = ok && ReadPod(stream, state_size);
            if (!ok || state_size > sizeof(FixedPipelineState)) {
                ++rejected;
                break;
            }
            ok = static_cast<bool>(stream.read(reinterpret_cast<char*>(&key.state), state_size));
            ClearDriver(key);
            // A row whose size or hash does not match this build's state layout is dropped rather
            // than built with the wrong registers.
            if (!ok || state_size != key.state.Size() || GraphicsKeyHash(key) != stored_hash) {
                ++rejected;
                continue;
            }
            const u64 hash = stored_hash;
            if (mark_loaded) {
                loaded_graphics.insert(hash);
            }
            if (graphics.contains(hash)) {
                continue;
            }
            const u64 set = ShaderSetHash(key.unique_hashes);
            graphics.emplace(hash, key);
            by_shaders[set].push_back(hash);
            ++added;
        } else if (kind == KindCompute) {
            PipelineListComputeKey key{};
            bool ok = true;
            ok = ok && ReadPod(stream, key.unique_hash);
            ok = ok && ReadPod(stream, key.shared_memory_size);
            for (u32& size : key.workgroup_size) {
                ok = ok && ReadPod(stream, size);
            }
            if (!ok || ComputeKeyHash(key) != stored_hash) {
                ++rejected;
                if (!ok) {
                    break;
                }
                continue;
            }
            if (mark_loaded) {
                loaded_compute.insert(stored_hash);
            }
            if (compute.emplace(stored_hash, key).second) {
                ++added;
            }
        } else {
            ++rejected;
            break;
        }
    }
    LOG_INFO(Render_Vulkan, "Key list {}: {} graphics, {} compute, {} added, {} rejected",
             file.filename().string(), graphics.size(), compute.size(), added, rejected);
}

bool PipelineKeyList::Save(const std::filesystem::path& file) const {
    std::vector<GraphicsRow> graphics_rows;
    std::vector<ComputeRow> compute_rows;
    {
        std::scoped_lock lock{mutex};
        graphics_rows.reserve(graphics.size());
        for (const auto& [hash, key] : graphics) {
            graphics_rows.emplace_back(hash, &key);
        }
        compute_rows.reserve(compute.size());
        for (const auto& [hash, key] : compute) {
            compute_rows.emplace_back(hash, &key);
        }
    }
    std::ranges::sort(graphics_rows, {}, &GraphicsRow::first);
    std::ranges::sort(compute_rows, {}, &ComputeRow::first);
    return WriteRows(file, graphics_rows, compute_rows);
}

bool PipelineKeyList::SaveContribution(const std::filesystem::path& file) const {
    std::vector<GraphicsRow> graphics_rows;
    std::vector<ComputeRow> compute_rows;
    {
        std::scoped_lock lock{mutex};
        for (const auto& [hash, key] : graphics) {
            if (!loaded_graphics.contains(hash)) {
                graphics_rows.emplace_back(hash, &key);
            }
        }
        for (const auto& [hash, key] : compute) {
            if (!loaded_compute.contains(hash)) {
                compute_rows.emplace_back(hash, &key);
            }
        }
    }
    if (graphics_rows.empty() && compute_rows.empty()) {
        // A stale contribution from an earlier session would be merged as if this one found it.
        std::error_code error;
        std::filesystem::remove(file, error);
        return true;
    }
    std::ranges::sort(graphics_rows, {}, &GraphicsRow::first);
    std::ranges::sort(compute_rows, {}, &ComputeRow::first);
    return WriteRows(file, graphics_rows, compute_rows);
}

void PipelineKeyList::RecordGraphics(PipelineListGraphicsKey key) {
    ClearDriver(key);
    const u64 hash = GraphicsKeyHash(key);
    std::scoped_lock lock{mutex};
    if (graphics.contains(hash)) {
        return;
    }
    const u64 set = ShaderSetHash(key.unique_hashes);
    graphics.emplace(hash, key);
    by_shaders[set].push_back(hash);
}

void PipelineKeyList::RecordCompute(PipelineListComputeKey key) {
    const u64 hash = ComputeKeyHash(key);
    std::scoped_lock lock{mutex};
    compute.emplace(hash, key);
}

std::vector<PipelineListGraphicsKey> PipelineKeyList::Siblings(
    const PipelineListGraphicsKey& key) const {
    PipelineListGraphicsKey normalized = key;
    ClearDriver(normalized);
    const u64 self = GraphicsKeyHash(normalized);
    const u64 set = ShaderSetHash(normalized.unique_hashes);
    std::vector<PipelineListGraphicsKey> siblings;
    std::scoped_lock lock{mutex};
    const auto found = by_shaders.find(set);
    if (found == by_shaders.end()) {
        return siblings;
    }
    for (const u64 hash : found->second) {
        if (hash != self) {
            siblings.push_back(graphics.at(hash));
        }
    }
    return siblings;
}

bool PipelineKeyList::Contains(const PipelineListGraphicsKey& key) const {
    PipelineListGraphicsKey normalized = key;
    ClearDriver(normalized);
    const u64 hash = GraphicsKeyHash(normalized);
    std::scoped_lock lock{mutex};
    return graphics.contains(hash);
}

size_t PipelineKeyList::GraphicsCount() const {
    std::scoped_lock lock{mutex};
    return graphics.size();
}

size_t PipelineKeyList::ComputeCount() const {
    std::scoped_lock lock{mutex};
    return compute.size();
}

size_t PipelineKeyList::ContributionRows() const {
    std::scoped_lock lock{mutex};
    size_t rows{};
    for (const auto& [hash, key] : graphics) {
        if (!loaded_graphics.contains(hash)) {
            ++rows;
        }
    }
    for (const auto& [hash, key] : compute) {
        if (!loaded_compute.contains(hash)) {
            ++rows;
        }
    }
    return rows;
}

} // namespace Vulkan
