#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;

class HostBlock {
public:
    HostBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "idle label: cannot allocate a host block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~HostBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    HostBlock(const HostBlock&) = delete;
    HostBlock& operator=(const HostBlock&) = delete;

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

std::uint64_t ImportsMade() {
    std::uint64_t made = 0;
    for (const auto& requester : AgcDriver::Graphics::HostImportCounters().requesters) made += requester.made;
    return made;
}

}

int main() {
    try {
        HostBlock labels;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(labels.Address(), BlockBytes)) {
            std::printf("skipped, the block is not write watched\n");
            return VulkanTestSkipped;
        }
        const auto made = ImportsMade();
        const std::uint32_t value = 0x5a5a0001u;
        const int reason = device->WriteLabelOnGpu(labels.Address() + 8, std::as_bytes(std::span(&value, 1)), 1, 0);
        Require(reason == 0, "idle label: a label in registered memory was refused");
        Require(ImportsMade() == made, "idle label: a label found the recorder idle and still made a host import for a range nothing imported");
        Require(AgcDriver::GuestMemory::Watched(labels.Address(), BlockBytes), "idle label: a label found the recorder idle and unwatched a range nothing imported");
        device->WaitIdle();
        std::puts("idle label tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
