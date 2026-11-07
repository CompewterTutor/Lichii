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

TEST_CASE("A contribution holds only rows no loaded list has", "[pipeline_keylist]") {
    TempDir temp;
    const auto shared = temp.path / "keylist.bin";
    const auto contribution = temp.path / "keylist.contribution.bin";
    const std::array<u64, 6> hashes{13, 14, 0, 0, 0, 0};

    Vulkan::PipelineKeyList collector;
    collector.RecordGraphics(GraphicsKey(hashes, 0x77));
    REQUIRE(collector.Save(shared));

    Vulkan::PipelineKeyList session;
    session.Load(shared);
    session.RecordGraphics(GraphicsKey(hashes, 0x77));
    CHECK(session.ContributionRows() == 0);
    session.RecordGraphics(GraphicsKey(hashes, 0x78));
    session.RecordCompute({
        .unique_hash = 0xdef,
        .shared_memory_size = 4096,
        .workgroup_size = {16, 16, 1},
    });
    CHECK(session.ContributionRows() == 2);
    REQUIRE(session.SaveContribution(contribution));

    Vulkan::PipelineKeyList merged;
    merged.Load(shared);
    merged.LoadContribution(contribution);
    CHECK(merged.GraphicsCount() == 2);
    CHECK(merged.ComputeCount() == 1);
    CHECK(merged.Contains(GraphicsKey(hashes, 0x78)));
    CHECK(merged.Contains(GraphicsKey(hashes, 0x77)));
}

TEST_CASE("Merging a contribution twice changes nothing", "[pipeline_keylist]") {
    TempDir temp;
    const auto shared = temp.path / "keylist.bin";
    const auto contribution = temp.path / "keylist.contribution.bin";
    const std::array<u64, 6> hashes{15, 16, 0, 0, 0, 0};

    Vulkan::PipelineKeyList session;
    session.RecordGraphics(GraphicsKey(hashes, 0x79));
    REQUIRE(session.Save(shared));
    REQUIRE(session.SaveContribution(contribution));

    Vulkan::PipelineKeyList merged;
    merged.Load(shared);
    merged.Load(contribution);
    const size_t once = merged.GraphicsCount();
    merged.Load(contribution);
    merged.Load(shared);
    CHECK(merged.GraphicsCount() == once);
}

TEST_CASE("An empty contribution removes the stale file", "[pipeline_keylist]") {
    TempDir temp;
    const auto shared = temp.path / "keylist.bin";
    const auto contribution = temp.path / "keylist.contribution.bin";
    const std::array<u64, 6> hashes{17, 18, 0, 0, 0, 0};

    Vulkan::PipelineKeyList first;
    first.RecordGraphics(GraphicsKey(hashes, 0x7a));
    REQUIRE(first.SaveContribution(contribution));
    REQUIRE(std::filesystem::exists(contribution));

    // A later session that loads the shared list adds nothing, so the pending file must not survive
    // as if it were new.
    Vulkan::PipelineKeyList second;
    second.RecordGraphics(GraphicsKey(hashes, 0x7a));
    REQUIRE(second.Save(shared));
    Vulkan::PipelineKeyList third;
    third.Load(shared);
    CHECK(third.ContributionRows() == 0);
    REQUIRE(third.SaveContribution(contribution));
    CHECK_FALSE(std::filesystem::exists(contribution));
}

TEST_CASE("A pending contribution stays pending across sessions", "[pipeline_keylist]") {
    TempDir temp;
    const auto shared = temp.path / "keylist.bin";
    const auto contribution = temp.path / "keylist.contribution.bin";
    const std::array<u64, 6> hashes{19, 20, 0, 0, 0, 0};

    {
        Vulkan::PipelineKeyList first;
        first.RecordGraphics(GraphicsKey(hashes, 0x7b));
        REQUIRE(first.SaveContribution(contribution));
    }
    // Next session: the row is not in the shared list, so it is still worth contributing.
    Vulkan::PipelineKeyList second;
    second.Load(shared);
    second.LoadContribution(contribution);
    CHECK(second.ContributionRows() == 1);
    REQUIRE(second.SaveContribution(contribution));
    CHECK(std::filesystem::exists(contribution));
}
