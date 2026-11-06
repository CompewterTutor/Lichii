// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_server.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/frontend/framebuffer_layout.h"
#include "hid_core/debug_input.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/renderer_base.h"

namespace Core::Nx2Debug {

using nlohmann::json;
using namespace std::chrono_literals;

/// Lines the log ring keeps for log_tail.
constexpr size_t RING_LINES = 20000;

struct ToolState {
    explicit ToolState(System& system_, std::string title_, u64 program_id_,
                       std::filesystem::path dump_directory_)
        : system{system_}, title{std::move(title_)}, program_id{program_id_},
          dump_directory{std::move(dump_directory_)} {}

    System& system;
    std::string title;
    u64 program_id;
    std::filesystem::path dump_directory;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    /// Names this session's captures apart from earlier ones in the same dump directory.
    s64 session{std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()};
    std::string log_filter{Settings::values.log_filter.GetValue()};
    u64 captures{};
    u64 screenshots{};
    u64 memory_id{};
    std::atomic<bool> stopping{};
    /// Serialises pause, resume and step between clients, so two of them cannot fight over the
    /// emulator's suspend state.
    std::mutex control_mutex;
    /// Replays an injected input sequence, timed by presents. Joined when the service stops.
    std::thread input_thread;
    std::atomic<bool> input_stop{};

    /// Input recording: every state applied through the debug server, with its present count.
    struct RecordedEvent {
        u64 present{};
        u64 buttons{};
        bool has_sticks{};
        s32 left_x{};
        s32 left_y{};
        s32 right_x{};
        s32 right_y{};
    };
    std::mutex record_mutex;
    bool recording{};
    u64 record_start{};
    std::string record_name;
    std::vector<RecordedEvent> recorded;
    u64 replay_index{};
};

namespace {

std::string Hex(u64 value) {
    return fmt::format("{:016x}", value);
}

json HexList(const std::vector<u64>& values) {    json list = json::array();
    for (const u64 value : values) {
        list.push_back(Hex(value));
    }
    return list;
}

template <typename T>
T Param(const json& params, const char* name, T fallback, T low, T high) {
    const T value = params.value(name, fallback);
    return (std::clamp)(value, low, high);
}

bool IsVulkan() {
    return Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
}

/// Bytes to write, from "bytes_base64". Throws on anything that is not valid base64.
std::vector<u8> DecodeBytes(const json& params) {
    const std::string text = params.value("bytes_base64", std::string{});
    if (text.empty()) {
        throw std::runtime_error{"bytes_base64 is required"};
    }
    std::vector<u8> bytes;
    bytes.reserve(text.size() / 4 * 3);
    u32 chunk{};
    int bits{};
    for (const char character : text) {
        if (character == '=') {
            break;
        }
        u32 value{};
        if (character >= 'A' && character <= 'Z') {
            value = static_cast<u32>(character - 'A');
        } else if (character >= 'a' && character <= 'z') {
            value = static_cast<u32>(character - 'a' + 26);
        } else if (character >= '0' && character <= '9') {
            value = static_cast<u32>(character - '0' + 52);
        } else if (character == '+') {
            value = 62;
        } else if (character == '/') {
            value = 63;
        } else {
            throw std::runtime_error{"bytes_base64 is not valid base64"};
        }
        chunk = (chunk << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<u8>((chunk >> bits) & 0xff));
        }
    }
    return bytes;
}

/// Waits for a future in short slices so a stopping server does not wait out the timeout.
template <typename T>
bool WaitReady(const ToolState& state, std::future<T>& future, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(50ms) != std::future_status::ready) {
        if (state.stopping.load(std::memory_order_relaxed) ||
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    return true;
}

void AppendPng(void* context, void* data, int size) {
    auto& png = *static_cast<std::vector<u8>*>(context);
    const u8* const bytes = static_cast<const u8*>(data);
    png.insert(png.end(), bytes, bytes + size);
}

json Status(ToolState& state, const json& params) {
    const auto window = std::chrono::milliseconds{Param<int>(params, "fps_window_ms", 500, 0, 5000)};
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < window &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    json caches = nullptr;
    if (auto* rasterizer = state.system.GPU().Renderer().ReadRasterizer()) {
        if (const auto stats = rasterizer->GetDebugCacheStats()) {
            caches = {
                {"graphics_pipelines", stats->graphics},
                {"compute_pipelines", stats->compute},
                {"keylist_graphics", stats->keylist_graphics},
                {"keylist_compute", stats->keylist_compute},
                {"built", stats->built},
                {"evicted", stats->evicted},
                {"prefetched", stats->prefetched},
                {"prefetch_failed", stats->prefetch_failed},
            };
        }
    }
    return {
        {"title", state.title},
        {"program_id", fmt::format("{:016X}", state.program_id)},
        {"build", std::string{Common::g_build_fullname}},
        {"scm_rev", std::string{Common::g_scm_rev}},
        {"switch2", Settings::CurrentProgramIsSwitch2()},
        {"renderer", IsVulkan() ? "vulkan" : "other"},
        {"powered_on", state.system.IsPoweredOn()},
        {"paused", state.system.IsPaused()},
        {"presents", after},
        {"fps", seconds > 0.0 ? static_cast<double>(after - before) / seconds : 0.0},
        {"uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start)
                         .count()},
        {"dump_dir", state.dump_directory.generic_string()},
        {"log_filter", state.log_filter},
        {"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())},
        {"caches", caches},
    };
}

void RequireRunning(const ToolState& state) {
    if (!state.system.IsPoweredOn()) {
        throw std::runtime_error{"no game is running"};
    }
}

u64 Presents() {
    return g_nx2_present_count.load(std::memory_order_relaxed);
}

json Pause(ToolState& state, const json&) {
    RequireRunning(state);
    std::scoped_lock lock{state.control_mutex};
    state.system.Pause();
    return {{"paused", state.system.IsPaused()}, {"presents", Presents()}};
}

json Resume(ToolState& state, const json&) {
    RequireRunning(state);
    std::scoped_lock lock{state.control_mutex};
    state.system.Run();
    return {{"paused", state.system.IsPaused()}, {"presents", Presents()}};
}

json Step(ToolState& state, const json& params) {
    RequireRunning(state);
    const u64 frames = Param<u64>(params, "frames", 1, 0, 100000);
    const auto timeout =
        std::chrono::milliseconds{Param<int>(params, "timeout_ms", 30000, 100, 600000)};
    std::scoped_lock lock{state.control_mutex};
    const VideoCore::Nx2Debug::StepResult result = VideoCore::Nx2Debug::StepPresents(
        Presents, [&state](bool paused) {
            if (paused) {
                state.system.Pause();
            } else {
                state.system.Run();
            }
        }, state.stopping, frames, timeout);
    return {
        {"requested", frames},
        {"advanced", result.after - result.before},
        {"presents", result.after},
        {"paused", state.system.IsPaused()},
        {"timed_out", result.timed_out},
    };
}

/// NpadButton bits by name, so a sequence reads like the controller.
u64 ParseButtons(const json& value) {
    static const std::map<std::string_view, u64, std::less<>> names{
        {"a", 1ULL << 0},        {"b", 1ULL << 1},         {"x", 1ULL << 2},
        {"y", 1ULL << 3},        {"stickl", 1ULL << 4},    {"stickr", 1ULL << 5},
        {"l", 1ULL << 6},        {"r", 1ULL << 7},         {"zl", 1ULL << 8},
        {"zr", 1ULL << 9},       {"plus", 1ULL << 10},     {"start", 1ULL << 10},
        {"minus", 1ULL << 11},   {"select", 1ULL << 11},   {"left", 1ULL << 12},
        {"up", 1ULL << 13},      {"right", 1ULL << 14},    {"down", 1ULL << 15},
        {"dpadleft", 1ULL << 12}, {"dpadup", 1ULL << 13},  {"dpadright", 1ULL << 14},
        {"dpaddown", 1ULL << 15}, {"sl", 1ULL << 24},      {"sr", 1ULL << 25},
    };
    if (value.is_number()) {
        return value.get<u64>();
    }
    u64 buttons{};
    for (const json& name : value.is_array() ? value : json::array({value})) {
        const std::string text = name.is_string() ? name.get<std::string>() : std::string{};
        std::string key;
        for (const char character : text) {
            if (character != '_' && character != '-' && character != ' ') {
                key += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
        }
        const auto it = names.find(key);
        if (it == names.end()) {
            throw std::runtime_error{fmt::format("unknown button \"{}\"", text)};
        }
        buttons |= it->second;
    }
    return buttons;
}

/// One injected state: buttons plus optional stick positions in the -1..1 range.
struct InjectedInput {
    u64 buttons{};
    bool has_sticks{};
    s32 left_x{};
    s32 left_y{};
    s32 right_x{};
    s32 right_y{};
};

InjectedInput ParseInput(const json& step) {
    constexpr s32 MaxStick = 32767;
    InjectedInput input{};
    input.buttons = step.contains("buttons") ? ParseButtons(step.at("buttons")) : 0;
    const auto stick = [&](const char* name, s32& x, s32& y) {
        const json& value = step.at(name);
        if (!value.is_array() || value.size() != 2) {
            throw std::runtime_error{fmt::format("{} must be [x, y] in -1..1", name)};
        }
        const double raw_x = std::clamp(value[0].get<double>(), -1.0, 1.0);
        const double raw_y = std::clamp(value[1].get<double>(), -1.0, 1.0);
        x = static_cast<s32>(raw_x * MaxStick);
        y = static_cast<s32>(raw_y * MaxStick);
        input.has_sticks = true;
    };
    if (step.contains("left")) {
        stick("left", input.left_x, input.left_y);
    }
    if (step.contains("right")) {
        stick("right", input.right_x, input.right_y);
    }
    return input;
}

void StopInputThread(ToolState& state);

void ApplyInput(ToolState& state, const InjectedInput& input) {
    if (state.recording) {
        std::scoped_lock lock{state.record_mutex};
        if (state.recording) {
            state.recorded.push_back({.present = Presents(),
                                      .buttons = input.buttons,
                                      .has_sticks = input.has_sticks,
                                      .left_x = input.left_x,
                                      .left_y = input.left_y,
                                      .right_x = input.right_x,
                                      .right_y = input.right_y});
        }
    }
    Core::HID::DebugInput::Set(input.buttons, input.left_x, input.left_y, input.right_x,
                               input.right_y, input.has_sticks);
}

/// Writes the recorded events as one JSON file inside the dump directory.
std::filesystem::path SaveRecording(ToolState& state) {
    const auto directory = state.dump_directory;
    std::filesystem::create_directories(directory);
    const auto path = directory / (state.record_name + ".json");
    json events = json::array();
    for (const ToolState::RecordedEvent& event : state.recorded) {
        json entry = {{"present", event.present}, {"buttons", Hex(event.buttons)}};
        if (event.has_sticks) {
            entry["left"] = json::array({event.left_x, event.left_y});
            entry["right"] = json::array({event.right_x, event.right_y});
        }
        events.push_back(std::move(entry));
    }
    json file = {
        {"version", 1},
        {"title", state.title},
        {"program_id", fmt::format("{:016X}", state.program_id)},
        {"start_present", state.record_start},
        {"frames", state.recorded.empty() ? 0 : state.recorded.back().present - state.record_start},
        {"events", std::move(events)},
    };
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << file.dump(2) << '\n';
    if (!stream) {
        throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
    }
    return path;
}

json InputRecord(ToolState& state, const json& params) {
    if (params.value("stop", false)) {
        std::scoped_lock lock{state.record_mutex};
        if (!state.recording) {
            throw std::runtime_error{"no recording is running"};
        }
        state.recording = false;
        const auto path = SaveRecording(state);
        return {{"path", path.generic_string()},
                {"events", state.recorded.size()},
                {"start_present", state.record_start}};
    }
    std::scoped_lock lock{state.record_mutex};
    state.record_name = params.value("name", fmt::format("input-{}-{:03}", state.session,
                                                         ++state.replay_index));
    state.recorded.clear();
    state.record_start = Presents();
    state.recording = true;
    return {{"recording", true}, {"name", state.record_name}, {"start_present", state.record_start}};
}

json InputPlayback(ToolState& state, const json& params) {
    const std::string name = params.value("name", std::string{});
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
        throw std::runtime_error{"name is a recording in the dump directory, without a path"};
    }
    const auto path = state.dump_directory / (name + ".json");
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error{fmt::format("no recording {} in {}", name,
                                             state.dump_directory.generic_string())};
    }
    json file = json::parse(stream);
    const u64 recorded_start = file.value("start_present", u64{0});
    const bool absolute = params.value("absolute", false);
    json events = file.at("events");
    if (events.empty()) {
        throw std::runtime_error{"the recording has no events"};
    }
    std::vector<ToolState::RecordedEvent> steps;
    for (const json& entry : events) {
        ToolState::RecordedEvent event{};
        event.present = entry.at("present").get<u64>();
        event.buttons = ParseButtons(entry.at("buttons"));
        if (entry.contains("left")) {
            event.has_sticks = true;
            event.left_x = entry.at("left")[0].get<s32>();
            event.left_y = entry.at("left")[1].get<s32>();
            event.right_x = entry.at("right")[0].get<s32>();
            event.right_y = entry.at("right")[1].get<s32>();
        }
        steps.push_back(event);
    }
    const u64 duration = steps.back().present - recorded_start;
    const u64 absolute_start = steps.front().present;

    std::scoped_lock lock{state.control_mutex};
    StopInputThread(state);
    state.input_stop.store(false, std::memory_order_relaxed);
    state.input_thread = std::thread{[&state, steps = std::move(steps), absolute,
                                      recorded_start, absolute_start] {
        // Relative playback keeps the recorded spacing from the moment it starts; absolute
        // playback waits for the frame numbers the recording saw, which only matches in the same
        // boot.
        u64 base{};
        if (absolute) {
            while (Presents() < absolute_start && !state.input_stop.load(std::memory_order_relaxed) &&
                   !state.stopping.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            base = recorded_start;
        } else {
            base = Presents();
        }
        for (const ToolState::RecordedEvent& event : steps) {
            const u64 target = base + (event.present - recorded_start);
            while (Presents() < target && !state.input_stop.load(std::memory_order_relaxed) &&
                   !state.stopping.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (state.input_stop.load(std::memory_order_relaxed) ||
                state.stopping.load(std::memory_order_relaxed)) {
                break;
            }
            Core::HID::DebugInput::Set(event.buttons, event.left_x, event.left_y, event.right_x,
                                       event.right_y, event.has_sticks);
        }
        Core::HID::DebugInput::Clear();
    }};
    return {{"name", name},
            {"events", steps.size()},
            {"frames", duration},
            {"mode", absolute ? "absolute" : "relative"}};
}

void StopInputThread(ToolState& state) {
    state.input_stop.store(true, std::memory_order_relaxed);
    if (state.input_thread.joinable()) {
        state.input_thread.join();
    }
}

json Input(ToolState& state, const json& params) {
    std::scoped_lock lock{state.control_mutex};
    StopInputThread(state);
    if (params.value("clear", false)) {
        Core::HID::DebugInput::Clear();
        return {{"active", false}};
    }
    const InjectedInput input = ParseInput(params);
    ApplyInput(state, input);
    return {{"active", true},
            {"buttons", Hex(input.buttons)},
            {"sticks", input.has_sticks}};
}

json InputSequence(ToolState& state, const json& params) {
    const json steps = params.value("steps", json::array());
    if (!steps.is_array() || steps.empty()) {
        throw std::runtime_error{"steps is a non-empty array of {frames, buttons, left, right}"};
    }
    for (const json& step : steps) {
        if (!step.contains("frames")) {
            throw std::runtime_error{"every step needs frames"};
        }
        ParseInput(step);
    }
    std::scoped_lock lock{state.control_mutex};
    StopInputThread(state);
    state.input_stop.store(false, std::memory_order_relaxed);
    state.input_thread = std::thread{[&state, steps] {
        for (const json& step : steps) {
            if (state.input_stop.load(std::memory_order_relaxed) ||
                state.stopping.load(std::memory_order_relaxed)) {
                break;
            }
            ApplyInput(state, ParseInput(step));
            const u64 frames = step.at("frames").get<u64>();
            const u64 target = Presents() + frames;
            while (Presents() < target && !state.input_stop.load(std::memory_order_relaxed) &&
                   !state.stopping.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        Core::HID::DebugInput::Clear();
    }};
    u64 frames{};
    for (const json& step : steps) {
        frames += step.at("frames").get<u64>();
    }
    return {{"steps", steps.size()}, {"frames", frames}, {"note", "replaying, timed by presents"}};
}

json Screenshot(ToolState& state, const json& params) {    auto& renderer = state.system.GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        throw std::runtime_error{"a screenshot is already pending: no frame was presented since"};
    }
    // Shader toggles for an A/B image: reject these programs only for this capture, then restore
    // the previous set, so two screenshots differ by exactly the listed shaders.
    std::vector<u64> rejected;
    for (const json& hash : params.value("reject", json::array())) {
        rejected.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    if (!rejected.empty() && !IsVulkan()) {
        throw std::runtime_error{"a shader toggle needs the Vulkan renderer"};
    }
    const auto reject_timeout = std::chrono::seconds{Param<int>(params, "reject_timeout_s", 5, 0, 60)};
    const auto apply_rejects = [&](bool reject) {
        if (rejected.empty()) {
            return true;
        }
        const u64 generation = VideoCore::Nx2Debug::SetRejected(rejected, reject);
        return VideoCore::Nx2Debug::WaitRejectApplied(generation, reject_timeout);
    };
    apply_rejects(true);
    SCOPE_EXIT {
        apply_rejects(false);
    };

    const u32 height = Param<u32>(params, "height", 720, 144, 2160);
    const auto layout = Layout::DefaultFrameLayout(height * 16 / 9, height);
    const auto pixels = std::make_shared<std::vector<u8>>(size_t{layout.width} * layout.height * 4);
    const auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    renderer.RequestScreenshot(
        pixels->data(), [pixels, done](bool invert_y) { done->set_value(invert_y); }, layout);
    const int timeout = Param<int>(params, "timeout_s", 5, 1, 30);
    if (!WaitReady(state, future, std::chrono::seconds{timeout})) {
        throw std::runtime_error{
            fmt::format("no frame was presented within {} s (paused or loading?)", timeout)};
    }
    const bool invert_y = future.get();
    std::vector<u8> rgb(size_t{layout.width} * layout.height * 3);
    for (u32 y = 0; y < layout.height; ++y) {
        const u8* source = pixels->data() + size_t{invert_y ? layout.height - 1 - y : y} * layout.width * 4;
        u8* target = rgb.data() + size_t{y} * layout.width * 3;
        for (u32 x = 0; x < layout.width; ++x, source += 4, target += 3) {
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
        }
    }
    std::vector<u8> png;
    if (!stbi_write_png_to_func(AppendPng, &png, static_cast<int>(layout.width),
                                static_cast<int>(layout.height), 3, rgb.data(),
                                static_cast<int>(layout.width * 3))) {
        throw std::runtime_error{"PNG encoding failed"};
    }
    json result = {{"width", layout.width}, {"height", layout.height}};
    if (!rejected.empty()) {
        result["overrides"] = {{"rejected", HexList(rejected)}};
    }
    if (params.value("save", false)) {
        std::filesystem::create_directories(state.dump_directory);
        const auto path =
            state.dump_directory / fmt::format("screenshot-{}-{:04}.png", state.session, ++state.screenshots);
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) {
            throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
        }
        result["path"] = path.generic_string();
    }
    if (params.value("inline", true)) {
        result["png_base64"] = Base64(png);
    }
    return result;
}

json LogTail(ToolState&, const json& params) {
    const u64 after = params.value("after", u64{0});
    const size_t limit = Param<size_t>(params, "limit", 200, 1, 5000);
    const std::string contains = params.value("contains", std::string{});
    const std::vector<Common::Log::RingLine> lines = Common::Log::ReadRing(after);
    std::vector<const Common::Log::RingLine*> matched;
    for (const auto& line : lines) {
        if (contains.empty() || line.text.find(contains) != std::string::npos) {
            matched.push_back(&line);
        }
    }
    // From the start the newest lines; after a sequence number the oldest ones, to page forward.
    const bool more = matched.size() > limit;
    if (more) {
        if (after == 0) {
            matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(limit));
        } else {
            matched.resize(limit);
        }
    }
    json text = json::array();
    for (const auto* line : matched) {
        text.push_back(line->text);
    }
    const u64 next = more && after != 0 ? matched.back()->sequence
                     : lines.empty()    ? after
                                        : lines.back().sequence;
    return {{"lines", std::move(text)}, {"next", next}, {"more", more}};
}

json LogFilter(ToolState& state, const json& params) {
    const std::string filter = params.at("filter").get<std::string>();
    Common::Log::Filter parsed;
    parsed.ParseFilterString(filter);
    Common::Log::SetGlobalFilter(parsed);
    json result = {{"previous", state.log_filter}, {"filter", filter}};
    state.log_filter = filter;
    return result;
}

json CaptureFrame(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"frame capture needs the Vulkan renderer"};
    }
    const u64 id = ++state.captures;
    const auto directory = state.dump_directory / fmt::format("capture-{}-{:03}", state.session, id);
    std::filesystem::create_directories(directory);
    const bool sampled = params.value("sampled", false);
    const int timeout = Param<int>(params, "timeout_s", 120, 1, 600);
    VideoCore::Nx2Debug::RequestCapture({.id = id, .directory = directory, .sampled = sampled});
    const auto finished = VideoCore::Nx2Debug::WaitCapture(id, std::chrono::seconds{timeout});
    if (!finished) {
        VideoCore::Nx2Debug::CancelCapture(id);
        throw std::runtime_error{fmt::format(
            "the capture did not finish within {} s (paused or loading?); if it started, its "
            "files are still being written to {}",
            timeout, directory.generic_string())};
    }
    if (!finished->error.empty()) {
        throw std::runtime_error{finished->error};
    }
    const size_t keep = Param<size_t>(params, "manifest_lines", 200, 0, 5000);
    json entries = json::array();
    json kinds = json::object();
    json frame;
    json summary;
    size_t total = 0;
    std::ifstream manifest{directory / "manifest.jsonl"};
    for (std::string line; std::getline(manifest, line);) {
        json entry = json::parse(line, nullptr, false);
        if (entry.is_discarded()) {
            continue;
        }
        const std::string kind = entry.value("kind", std::string{});
        if (kind == "frame") {
            frame = std::move(entry);
        } else if (kind == "summary") {
            summary = std::move(entry);
        } else {
            kinds[kind] = kinds.value(kind, 0) + 1;
            if (total++ < keep) {
                entries.push_back(std::move(entry));
            }
        }
    }
    return {
        {"directory", directory.generic_string()},
        {"sampled", sampled},
        {"frame", std::move(frame)},
        {"summary", std::move(summary)},
        {"kinds", std::move(kinds)},
        {"entries_total", total},
        {"entries", std::move(entries)},
        {"index_command",
         fmt::format("python3 -I tools/nx2/frame_dump_index.py \"{}\"", directory.generic_string())},
    };
}

json RejectShaders(ToolState&, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"runtime rejection needs the Vulkan renderer"};
    }
    std::vector<u64> hashes;
    for (const json& hash : params.value("hashes", json::array())) {
        hashes.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    const bool reject = params.value("reject", true);
    const u64 generation = VideoCore::Nx2Debug::SetRejected(hashes, reject);
    const int timeout = Param<int>(params, "timeout_s", 5, 0, 60);
    const bool applied = hashes.empty() ||
                         VideoCore::Nx2Debug::WaitRejectApplied(generation, std::chrono::seconds{timeout});
    json result = {{"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())}, {"applied", applied}};
    if (!applied) {
        result["note"] = "no frame was presented yet; the change applies at the next frame end";
    }
    return result;
}

json ReadGuestMemory(ToolState& state, const json& params) {
    const u64 address = Param<u64>(params, "address", 0, 0, 0x7fff'ffff'ffff);
    const int size = Param<int>(params, "size", 256, 1, 1 << 20);
    auto& memory = state.system.ApplicationMemory();
    if (!memory.IsValidVirtualAddressRange(address, size)) {
        throw std::runtime_error{fmt::format(
            "range {:#x}..{:#x} is not mapped in application memory", address, address + size)};
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!memory.ReadBlock(address, bytes.data(), bytes.size())) {
        throw std::runtime_error{"read failed: the range became unmapped"};
    }
    return {
        {"address", address},
        {"size", bytes.size()},
        {"bytes_base64", Base64(bytes)},
    };
}

json WriteGuestMemory(ToolState& state, const json& params) {
    const u64 address = Param<u64>(params, "address", 0, 0, 0x7fff'ffff'ffff);
    if (!params.value("confirm", false)) {
        throw std::runtime_error{"writing guest memory needs \"confirm\": true"};
    }
    const std::vector<u8> bytes = DecodeBytes(params);
    auto& memory = state.system.ApplicationMemory();
    if (!memory.IsValidVirtualAddressRange(address, bytes.size())) {
        throw std::runtime_error{fmt::format(
            "range {:#x}..{:#x} is not mapped in application memory", address,
            address + bytes.size())};
    }
    if (!memory.WriteBlock(address, bytes.data(), bytes.size())) {
        throw std::runtime_error{"write failed: the range became unmapped"};
    }
    return {{"address", address}, {"written", bytes.size()}};
}

json ReadGpuMemory(ToolState& state, const json& params) {
    const u64 address = Param<u64>(params, "address", 0, 0, 0xffff'ffff'ffff);
    const int size = Param<int>(params, "size", 256, 1, 1 << 20);
    const auto timeout = std::chrono::milliseconds{Param<int>(params, "timeout_ms", 5000, 100, 60000)};
    VideoCore::Nx2Debug::MemoryAccess access{};
    access.id = ++state.memory_id;
    access.address = address;
    access.size = static_cast<std::size_t>(size);
    access.write = false;
    if (!VideoCore::Nx2Debug::RequestMemoryAccess(access)) {
        throw std::runtime_error{"another GPU memory access is pending; retry"};
    }
    const auto done = VideoCore::Nx2Debug::WaitMemoryAccess(access.id, timeout);
    if (!done) {
        throw std::runtime_error{
            "no frame end arrived within the timeout; the rasterizer takes the access there"};
    }
    if (!done->error.empty()) {
        throw std::runtime_error{done->error};
    }
    return {
        {"address", address},
        {"size", done->bytes.size()},
        {"bytes_base64", Base64(done->bytes)},
    };
}

json WriteGpuMemory(ToolState& state, const json& params) {
    const u64 address = Param<u64>(params, "address", 0, 0, 0xffff'ffff'ffff);
    if (!params.value("confirm", false)) {
        throw std::runtime_error{"writing GPU memory needs \"confirm\": true"};
    }
    const std::vector<u8> bytes = DecodeBytes(params);
    const auto timeout = std::chrono::milliseconds{Param<int>(params, "timeout_ms", 5000, 100, 60000)};
    VideoCore::Nx2Debug::MemoryAccess access{};
    access.id = ++state.memory_id;
    access.address = address;
    access.size = bytes.size();
    access.write = true;
    access.bytes = bytes;
    if (!VideoCore::Nx2Debug::RequestMemoryAccess(access)) {
        throw std::runtime_error{"another GPU memory access is pending; retry"};
    }
    const auto done = VideoCore::Nx2Debug::WaitMemoryAccess(access.id, timeout);
    if (!done) {
        throw std::runtime_error{
            "no frame end arrived within the timeout; the rasterizer takes the access there"};
    }
    if (!done->error.empty()) {
        throw std::runtime_error{done->error};
    }
    return {{"address", address}, {"written", done->size}};
}

json ScanShaders(ToolState& state, const json& params) {
    // Native Ampere programs end with this address map (see Shader::Ampere::NativeAddressMapMagic).
    constexpr u64 NativeAddressMapMagic = 0x50414d4e4544454eULL;
    constexpr u64 PageSize = 4096;
    const int max_hits = Param<int>(params, "max_hits", 512, 1, 100000);
    const u64 before_bytes = static_cast<u64>(Param<int>(params, "before_kb", 512, 0, 16384)) * 1024;
    const u64 after_bytes = static_cast<u64>(Param<int>(params, "after_kb", 64, 0, 16384)) * 1024;
    u64 magic = NativeAddressMapMagic;
    int width = 8;
    if (const std::string text = params.value("magic", std::string{}); !text.empty()) {
        const std::string digits = text.starts_with("0x") ? text.substr(2) : text;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), magic, 16);
        if (error != std::errc{} || end != digits.data() + digits.size() || width < 1) {
            throw std::runtime_error{"magic must be a hex value"};
        }
        width = Param<int>(params, "width", 4, 1, 8);
    }

    struct Hit {
        int source{}; // 0 = GPU address space, 1 = application memory
        void* owner{};
        GPUVAddr address{};
        bool operator<(const Hit& rhs) const noexcept {
            return address < rhs.address;
        }
        bool operator==(const Hit& rhs) const noexcept {
            return source == rhs.source && owner == rhs.owner && address == rhs.address;
        }
    };
    std::vector<Hit> hits;
    size_t pages{};
    size_t instances{};
    bool full = false;
    const auto scan = [&](int source, void* owner, GPUVAddr addr, const u8* data,
                          std::size_t size) {
        ++pages;
        if (full) {
            return;
        }
        for (std::size_t offset = 0; offset + width <= size; offset += width) {
            u64 value{};
            std::memcpy(&value, data + offset, width);
            if (value == magic) {
                hits.push_back({source, owner, addr + offset});
            }
            if (hits.size() >= static_cast<std::size_t>(max_hits)) {
                full = true;
                return;
            }
        }
    };
    for (Tegra::MemoryManager* manager : Tegra::MemoryManager::Instances()) {
        ++instances;
        if (manager != nullptr) {
            manager->VisitMappedPages([&, manager](GPUVAddr addr, const u8* data,
                                                   std::size_t size) {
                scan(0, manager, addr, data, size);
            });
        }
    }
    auto& application_memory = state.system.ApplicationMemory();
    application_memory.VisitMappedPages(
        [&](VAddr addr, const u8* data, std::size_t size) {
            scan(1, &application_memory, addr, data, size);
        });
    std::sort(hits.begin(), hits.end(),
              [](const Hit& lhs, const Hit& rhs) { return lhs.address < rhs.address; });
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [](const Hit& lhs, const Hit& rhs) {
                               return lhs.source == rhs.source && lhs.owner == rhs.owner &&
                                      lhs.address == rhs.address;
                           }),
               hits.end());

    const auto read_page = [](const Hit& hit, u64 page, u8* out) -> bool {
        if (hit.source == 0) {
            auto* manager = static_cast<Tegra::MemoryManager*>(hit.owner);
            if (manager == nullptr || !manager->IsFullyMappedRange(page, PageSize)) {
                return false;
            }
            const u8* const data = manager->GetSpan(page, PageSize);
            if (data == nullptr) {
                return false;
            }
            std::memcpy(out, data, PageSize);
            return true;
        }
        auto* memory = static_cast<Core::Memory::Memory*>(hit.owner);
        const u8* const data = memory != nullptr ? memory->GetSpan(page, PageSize) : nullptr;
        if (data == nullptr) {
            return false;
        }
        std::memcpy(out, data, PageSize);
        return true;
    };

    const auto directory =
        state.dump_directory / fmt::format("shader-scan-{}", state.session);
    std::filesystem::create_directories(directory);
    std::ofstream manifest{directory / "manifest.jsonl", std::ios::app};
    std::vector<std::pair<u64, u64>> covered;
    size_t dumped{};
    u64 bytes{};
    for (const Hit& hit : hits) {
        const u64 start = (hit.address > before_bytes ? hit.address - before_bytes : 0) &
                          ~(PageSize - 1);
        const u64 end = (hit.address + after_bytes + PageSize - 1) & ~(PageSize - 1);
        const bool already = std::ranges::any_of(covered, [start, end](const auto& range) {
            return start >= range.first && end <= range.second;
        });
        if (already) {
            continue;
        }
        covered.emplace_back(start, end);
        std::vector<u8> window(end - start, 0);
        for (u64 page = start; page < end; page += PageSize) {
            read_page(hit, page, window.data() + (page - start));
        }
        const auto file = directory / fmt::format("region-{:012x}.bin", start);
        std::ofstream stream{file, std::ios::binary};
        stream.write(reinterpret_cast<const char*>(window.data()),
                     static_cast<std::streamsize>(window.size()));
        if (!stream) {
            continue;
        }
        ++dumped;
        bytes += window.size();
        manifest << fmt::format(
                        "{{\"hit\":\"0x{:x}\",\"start\":\"0x{:x}\",\"size\":{},\"file\":\"{}\"}}\n",
                        hit.address, start, window.size(), file.filename().string());
        manifest.flush();
    }
    return {
        {"hits", hits.size()},
        {"pages", pages},
        {"instances", instances},
        {"regions", dumped},
        {"bytes", bytes},
        {"directory", directory.generic_string()},
        {"note", "windows end after the address map; find the program base offline"},
    };
}

Handlers MakeHandlers(const std::shared_ptr<ToolState>& state) {
    const auto handler = [&state](json (*tool)(ToolState&, const json&)) -> Handler {
        return [state, tool](const json& params) { return tool(*state, params); };
    };
    return {
        {"status", handler(Status)},
        {"pause", handler(Pause)},
        {"resume", handler(Resume)},
        {"step", handler(Step)},
        {"screenshot", handler(Screenshot)},
        {"log_tail", handler(LogTail)},
        {"log_filter", handler(LogFilter)},
        {"capture_frame", handler(CaptureFrame)},
        {"reject_shaders", handler(RejectShaders)},
        {"scan_shaders", handler(ScanShaders)},
        {"read_guest_memory", handler(ReadGuestMemory)},
        {"write_guest_memory", handler(WriteGuestMemory)},
        {"input", handler(Input)},
        {"input_sequence", handler(InputSequence)},
        {"input_record", handler(InputRecord)},
        {"input_playback", handler(InputPlayback)},
        {"read_gpu_memory", handler(ReadGpuMemory)},
        {"write_gpu_memory", handler(WriteGpuMemory)},
    };
}

} // Anonymous namespace

u64 ParseShaderHash(std::string_view text) {
    const std::string_view digits =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    u64 value = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (digits.size() != 16 || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format(
            "bad hash \"{}\": want all 16 hex digits of a native program key (shader dump file "
            "name) or shader hash (frame dump \"shaders\"); a shorter one would match nothing",
            text)};
    }
    return value;
}

Service::Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                 std::string dump_directory)
    : state{std::make_shared<ToolState>(system, std::move(title), program_id,
                                        std::move(dump_directory))} {
    VideoCore::Nx2Debug::BeginSession();
    Common::Log::SetRingCapacity(RING_LINES);
    try {
        server = std::make_unique<Server>(endpoint, MakeHandlers(state));
    } catch (...) {
        VideoCore::Nx2Debug::EndSession();
        throw;
    }
    LOG_INFO(Debug, "Debug server listening on {}:{}, dump dir {}", endpoint.host, server->Port(),
             state->dump_directory.generic_string());
}

Service::~Service() {
    state->stopping.store(true, std::memory_order_relaxed);
    StopInputThread(*state);
    Core::HID::DebugInput::Clear();
    VideoCore::Nx2Debug::EndSession();
    server.reset();
}

std::unique_ptr<Service> MaybeStartService(System& system, std::string title, u64 program_id) {
    const char* const text = std::getenv("EDEN_NX2_DEBUG_SERVER");
    if (text == nullptr || *text == '\0') {
        return nullptr;
    }
    std::string error;
    const auto endpoint = ParseEndpoint(text, error);
    if (!endpoint) {
        LOG_ERROR(Debug, "EDEN_NX2_DEBUG_SERVER={}: {}; the debug server is off", text, error);
        return nullptr;
    }
    const char* const directory = std::getenv("EDEN_NX2_DEBUG_DIR");
    std::string dump_directory = directory != nullptr && *directory != '\0'
        ? std::string{directory}
        : (Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "debug-server").generic_string();
    try {
        return std::make_unique<Service>(system, std::move(title), program_id, *endpoint,
                                         std::move(dump_directory));
    } catch (const std::exception& failure) {
        LOG_ERROR(Debug, "Debug server cannot listen on {}:{}: {}", endpoint->host, endpoint->port,
                  failure.what());
        return nullptr;
    }
}

} // namespace Core::Nx2Debug
