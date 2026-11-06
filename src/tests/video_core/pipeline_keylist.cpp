// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_pipeline_keylist.h"

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-pipeline-keylist-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

Vulkan::PipelineListGraphicsKey GraphicsKey(std::array<u64, 6> hashes, u32 color_format,
                                            u32 driver_id = 4318) {
    Vulkan::PipelineListGraphicsKey key{};
    key.unique_hashes = hashes;
    key.state.color_formats[0] = static_cast<u8>(color_format);
    key.state.driver_id = driver_id;
    key.state.driver_version = 1;
    return key;
}

} // Anonymous namespace

TEST_CASE("A key list round-trips graphics and compute rows", "[pipeline_keylist]") {
    TempDir temp;
    const auto file = temp.path / "keylist.bin";
    const std::array<u64, 6> hashes{1, 2, 3, 0, 0, 0};
    Vulkan::PipelineKeyList recorded;
    recorded.RecordGraphics(GraphicsKey(hashes, 0x11));
    recorded.RecordGraphics(GraphicsKey(hashes, 0x22));
    recorded.RecordCompute({
        .unique_hash = 0xabc,
        .shared_memory_size = 1024,
        .workgroup_size = {32, 1, 1},
    });
    REQUIRE(recorded.Save(file));

    Vulkan::PipelineKeyList loaded;
    loaded.Load(file);
    CHECK(loaded.GraphicsCount() == 2);
    CHECK(loaded.Contains(GraphicsKey(hashes, 0x11)));
    CHECK(loaded.Contains(GraphicsKey(hashes, 0x22)));
    const auto siblings = loaded.Siblings(GraphicsKey(hashes, 0x11));
    REQUIRE(siblings.size() == 1);
    CHECK(siblings.front().state.color_formats[0] == 0x22);
}

TEST_CASE("A key list ignores another driver's recorded identity", "[pipeline_keylist]") {
    TempDir temp;
    const auto file = temp.path / "keylist.bin";
    const std::array<u64, 6> hashes{4, 5, 6, 0, 0, 0};
    Vulkan::PipelineKeyList recorded;
    recorded.RecordGraphics(GraphicsKey(hashes, 0x33, 4318));
    REQUIRE(recorded.Save(file));

    // Another machine's driver id and version must not stop the row from matching.
    Vulkan::PipelineKeyList loaded;
    loaded.Load(file);
    CHECK(loaded.Contains(GraphicsKey(hashes, 0x33, 12345)));
    CHECK_FALSE(loaded.Contains(GraphicsKey(hashes, 0x34, 12345)));
}

TEST_CASE("Re-recording the same state keeps one row", "[pipeline_keylist]") {
    Vulkan::PipelineKeyList list;
    const std::array<u64, 6> hashes{7, 8, 0, 0, 0, 0};
    list.RecordGraphics(GraphicsKey(hashes, 0x44));
    list.RecordGraphics(GraphicsKey(hashes, 0x44, 9999));
    CHECK(list.GraphicsCount() == 1);
    CHECK(list.Siblings(GraphicsKey(hashes, 0x44)).empty());
}

TEST_CASE("A damaged key list is dropped without touching loaded rows", "[pipeline_keylist]") {
    TempDir temp;
    const auto good = temp.path / "good.bin";
    const auto bad = temp.path / "bad.bin";
    const std::array<u64, 6> hashes{9, 10, 0, 0, 0, 0};
    Vulkan::PipelineKeyList recorded;
    recorded.RecordGraphics(GraphicsKey(hashes, 0x55));
    REQUIRE(recorded.Save(good));

    {
        std::ofstream stream{bad, std::ios::binary};
        stream.write("not a key list", 14);
    }
    Vulkan::PipelineKeyList loaded;
    loaded.Load(good);
    REQUIRE(loaded.GraphicsCount() == 1);
    loaded.Load(bad);
    CHECK(loaded.GraphicsCount() == 1);
    CHECK(loaded.Contains(GraphicsKey(hashes, 0x55)));
}

TEST_CASE("A truncated row does not lose earlier rows", "[pipeline_keylist]") {
    TempDir temp;
    const auto file = temp.path / "keylist.bin";
    const std::array<u64, 6> hashes{11, 12, 0, 0, 0, 0};
    Vulkan::PipelineKeyList recorded;
    recorded.RecordGraphics(GraphicsKey(hashes, 0x66));
    recorded.RecordGraphics(GraphicsKey(hashes, 0x67));
    REQUIRE(recorded.Save(file));
    const auto size = std::filesystem::file_size(file);
    std::filesystem::resize_file(file, size - 40);

    Vulkan::PipelineKeyList loaded;
    loaded.Load(file);
    CHECK(loaded.GraphicsCount() == 1);
}
