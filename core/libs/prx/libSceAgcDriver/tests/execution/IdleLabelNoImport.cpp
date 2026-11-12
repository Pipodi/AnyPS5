#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
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

class WatchedBlock {
public:
    WatchedBlock() {
#ifdef _WIN32
        data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        Require(data != nullptr, "idle label: cannot allocate a guest arena block");
        GuestArena::GuestArenaCommit_nid_postfix(data, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        void* raw = mmap(nullptr, 2 * BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "idle label: cannot map a block");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
        if (aligned != begin) munmap(raw, aligned - begin);
        if (aligned + BlockBytes != begin + 2 * BlockBytes) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + 2 * BlockBytes - aligned - BlockBytes);
        data = reinterpret_cast<std::uint8_t*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, BlockBytes);
#endif
        std::memset(data, 0, BlockBytes);
        GuestAllocations::Mutation mutation;
        mutation.Add(data, BlockBytes, true, true, true);
    }

    ~WatchedBlock() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, BlockBytes);
#else
        munmap(data, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, BlockBytes);
#endif
    }

    WatchedBlock(const WatchedBlock&) = delete;
    WatchedBlock& operator=(const WatchedBlock&) = delete;

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data)); }

private:
    std::uint8_t* data = nullptr;
};

std::uint64_t ImportsMade() {
    std::uint64_t made = 0;
    for (const auto& requester : AgcDriver::Graphics::HostImportCounters().requesters) made += requester.made;
    return made;
}

}

int main() {
    try {
        if (!AgcDriver::GuestMemory::WriteWatched()) {
            std::printf("skipped, the guest arena is not write watched\n");
            return VulkanTestSkipped;
        }
        WatchedBlock labels;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Require(AgcDriver::GuestMemory::Watched(labels.Address(), BlockBytes), "idle label: the block is not write watched");
        const auto made = ImportsMade();
        const std::uint32_t value = 0x5a5a0001u;
        const int reason = device->WriteLabelOnGpu(labels.Address() + 8, std::as_bytes(std::span(&value, 1)), 1, 0);
        Require(reason == 1, "idle label: a label found the recorder idle and returned reason " + std::to_string(reason) + " instead of 1");
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
