#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::byte Kept{0x40};
constexpr std::uint32_t BaseVertexLocation = 0x8c;

alignas(256) constexpr std::array<std::uint32_t, 20> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060208u, 0x7e0802f2u, 0x7e0a02ffu, 0x44800000u, 0xf80000d1u, 0x00000005u, 0xf80008cfu, 0x04030201u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};

alignas(8192) std::array<std::byte, 131072> TargetStorage{};

std::span<std::byte> TargetSpan() {
    const auto base = reinterpret_cast<std::uintptr_t>(TargetStorage.data());
    const auto aligned = (base + 65535u) & ~std::uintptr_t{65535u};
    return {TargetStorage.data() + (aligned - base), 65536};
}

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void AppendState(std::vector<std::uint32_t>& commands) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0x01200000}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x906c000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u}, {0x00c, std::bit_cast<std::uint32_t>(1.0f)},
    };
    Append(commands, 0x79, {0x242, 4});
    Append(commands, 0x79, {0x24a, 0});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
}

void AppendTarget(std::vector<std::uint32_t>& commands, std::span<std::byte> memory, std::uint32_t userWord) {
    const auto color = reinterpret_cast<std::uintptr_t>(memory.data());
    Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(color >> 8u)});
    Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(color >> 40u)});
    Append(commands, 0x76, {BaseVertexLocation, userWord});
}

std::size_t Covered(std::span<std::byte> memory) {
    return static_cast<std::size_t>(std::count_if(memory.begin(), memory.end(), [](std::byte value) { return value != Kept; }));
}

void Flush(std::span<std::byte> memory) {
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(memory.data()), memory.size());
}

void Finish() {
    auto idle = std::async(std::launch::async, [] { AgcDriverWaitIdle_nid_postfix(); });
    if (idle.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
        std::fputs("cold draw skip: the draw pipeline deadlocked while its pipeline build was gated\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    idle.get();
}

void SubmitDraw(std::span<std::byte> memory) {
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendTarget(commands, memory, 0);
    Append(commands, 0x2d, {9, 2});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
}

struct Blocker {
    std::shared_ptr<std::atomic<bool>> release = std::make_shared<std::atomic<bool>>(false);
    void Enqueue() {
        DrawPipeline::Queue0().Enqueue([release = release] {
            while (!release->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            { std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex()); }
        }, {});
    }
};

struct Opener {
    std::promise<void>& gate;
    bool opened = false;
    void Open() {
        if (!opened) gate.set_value();
        opened = true;
    }
    ~Opener() { Open(); }
};

void ColdDrawIsSkippedThenDrawnOnceTheBuildIsResident() {
    std::promise<void> gate;
    Opener opener{gate};
    const auto opened = gate.get_future().share();
    AgcDriver::Graphics::SetPrebuildGate([opened] { opened.wait(); });
    std::fill(TargetSpan().begin(), TargetSpan().end(), Kept);
    Flush(TargetSpan());
    const auto skippedBefore = AgcDriver::Graphics::ColdDrawSkippedTotal();
    Blocker blocker;
    blocker.Enqueue();
    SubmitDraw(TargetSpan());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    blocker.release->store(true);
    const auto waitStart = std::chrono::steady_clock::now();
    while (AgcDriver::Graphics::ColdDrawSkippedTotal() == skippedBefore || DrawPipeline::Queue0().Outstanding() != 0) {
        Require(std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(30), "the draw behind a gated pipeline build was not skipped (skipped " + std::to_string(AgcDriver::Graphics::ColdDrawSkippedTotal() - skippedBefore) + ", outstanding " + std::to_string(DrawPipeline::Queue0().Outstanding()) + ")");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Flush(TargetSpan());
    Require(Covered(TargetSpan()) == 0, "a skipped draw changed its target");
    opener.Open();
    AgcDriver::Graphics::SetPrebuildGate({});
    const auto start = std::chrono::steady_clock::now();
    while (AgcDriver::Graphics::ColdDrawRecoveredTotal() == 0) {
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(60), "the draw was never recorded after the build was released");
        Blocker again;
        again.Enqueue();
        SubmitDraw(TargetSpan());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        again.release->store(true);
        Finish();
    }
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_PREBUILD_DEDUPE", "1");
    _putenv_s("APS5_SKIP_COLD_DRAWS", "1");
    _putenv_s("APS5_SKIP_COLD_DRAWS_WAIT_MS", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_PREBUILD_DEDUPE", "1", 1);
    setenv("APS5_SKIP_COLD_DRAWS", "1", 1);
    setenv("APS5_SKIP_COLD_DRAWS_WAIT_MS", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        ColdDrawIsSkippedThenDrawnOnceTheBuildIsResident();
        AgcDriverShutdown_nid_postfix();
        std::puts("cold draw skip draw tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
