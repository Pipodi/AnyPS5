#include "prx/libSceAgcDriver/State/include/Configuration.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/FrameDumpIo.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileWindows.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ResidentCensus.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "SceTypes.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif
#ifdef __linux__
#include "prx/libc/include/specifics/linux/HostMapping.hpp"
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using AgcDriver::FrameCall;
using AgcDriver::FrameCallKind;
using AgcDriver::FrameCaptureReader;
using AgcDriver::FrameMemoryChunk;
using AgcDriver::FrameMemoryKind;
using AgcDriver::FrameRangeAccess;
using AgcDriver::FrameRangeKind;
using Clock = std::chrono::steady_clock;

struct Dump {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::filesystem::path path;
};

struct Options {
    std::filesystem::path directory;
    std::vector<Dump> dumps;
    std::filesystem::path frames;
    bool pace = true;
    bool strict = false;
    bool exact = false;
    std::size_t frameLimit = 0;
    std::filesystem::path checkpointReport;
    std::chrono::milliseconds paceTimeout{10000};
    std::chrono::milliseconds memoryPaceTimeout{10000};
    std::chrono::milliseconds releaseAfter{50};
    std::chrono::milliseconds drainTimeout{10000};
    std::chrono::seconds timeout{900};
    bool release = true;
    bool synthesize = true;
    // One profile window per replayed frame (ProfileWindows), closed at the frame's flip; off
    // (--wall-windows): the lines close on their wall clock as in the game.
    bool frameWindows = true;
    // The bindless heap primer: the capture's BindlessTables record (off: --no-bindless-prime), or
    // --bindless-in <file> (a first replay's --bindless-out, for captures without the record).
    bool bindlessPrime = true;
    std::filesystem::path bindlessIn;
    std::filesystem::path bindlessOut;
    // --dedupe-prologue: skip a prologue ABI resolution that repeats an identical earlier one with no
    // shader registration or memory call in between (its inputs did not change).
    bool dedupePrologue = false;
    // --host-regions: print this process's host regions inside the guest arena and exit.
    bool hostRegions = false;
    std::chrono::milliseconds busyTimeout{600000};
    std::size_t repeat = 1;
    bool shaders = false;
    std::filesystem::path shaderCode;
    std::size_t submits = 0;
    bool traceSerials = false;
    bool coldPipelines = false;
};

// The prologue's provably redundant ABI resolutions (--dedupe-prologue): a resolve call is redundant
// when an identical one (kind, queue, flags, address, size, values and registers) was made since the
// last register-shader or memory call; any other call kind starts a new epoch.
class PrologueDedupe {
public:
    bool Redundant(const FrameCall& call) {
        const bool resolve = call.kind == FrameCallKind::ResolveShaderAbi || call.kind == FrameCallKind::ResolveGraphicsAbi || call.kind == FrameCallKind::ResolveGraphicsStagesAbi;
        if (!resolve) {
            ++epoch;
            return false;
        }
        std::string key;
        const auto put = [&](const void* data, std::size_t bytes) { key.append(static_cast<const char*>(data), bytes); };
        const auto kind = static_cast<std::uint32_t>(call.kind);
        put(&kind, sizeof(kind));
        put(&call.queue, sizeof(call.queue));
        put(&call.flags, sizeof(call.flags));
        put(&call.address, sizeof(call.address));
        put(&call.size, sizeof(call.size));
        const auto count = [&](std::size_t value) { put(&value, sizeof(value)); };
        count(call.values.size());
        if (!call.values.empty()) put(call.values.data(), call.values.size() * sizeof(call.values[0]));
        count(call.context.size());
        for (const auto& reg : call.context) {
            put(&reg.offset, sizeof(reg.offset));
            put(&reg.value, sizeof(reg.value));
        }
        count(call.primitive.size());
        for (const auto& reg : call.primitive) {
            put(&reg.offset, sizeof(reg.offset));
            put(&reg.value, sizeof(reg.value));
        }
        auto [entry, inserted] = seen.try_emplace(std::move(key), epoch);
        if (inserted || entry->second != epoch) {
            entry->second = epoch;
            return false;
        }
        ++skipped;
        return true;
    }
    std::uint64_t Skipped() const { return skipped; }

private:
    std::unordered_map<std::string, std::uint64_t> seen;
    std::uint64_t epoch = 0;
    std::uint64_t skipped = 0;
};

class Frames {
public:
    void SetSize(std::uint32_t displayWidth, std::uint32_t displayHeight) {
        width = displayWidth;
        height = displayHeight;
    }

    void MarkReady() {
        AgcDriverTracePresent_nid_postfix(width, height);
        std::lock_guard lock(mutex);
        ready.push_back(Clock::now());
    }

    void Failed(std::exception_ptr error) {
        std::lock_guard lock(mutex);
        if (!failure) failure = error;
    }

    std::vector<Clock::time_point> Times() const {
        std::lock_guard lock(mutex);
        return ready;
    }

    std::exception_ptr Failure() const {
        std::lock_guard lock(mutex);
        return failure;
    }

private:
    mutable std::mutex mutex;
    std::vector<Clock::time_point> ready;
    std::exception_ptr failure;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class ReplayFlip final : public AgcDriver::IFlipRequest {
public:
    explicit ReplayFlip(Frames& frames) : frames(frames) {}
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override { frames.MarkReady(); }
    void Fail(std::exception_ptr error) noexcept override { frames.Failed(error); }

private:
    Frames& frames;
};

class ReplayRenderingWait final : public AgcDriver::IRenderingWait {
public:
    void Wait() override {}
};

class ReplayOutput final : public AgcDriver::IVideoOutput {
public:
    explicit ReplayOutput(Frames& frames) : frames(frames) {}
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override { return std::make_shared<ReplayFlip>(frames); }
    std::shared_ptr<AgcDriver::IRenderingWait> CaptureRenderingWait(std::uint32_t) override { return std::make_shared<ReplayRenderingWait>(); }
    void Fail(std::exception_ptr error) noexcept override { frames.Failed(error); }

private:
    Frames& frames;
};

[[noreturn]] void hardExit(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
    std::_Exit(code);
}

void startWatchdog(std::chrono::seconds timeout) {
    std::thread([timeout] {
        const auto deadline = Clock::now() + timeout;
        while (timeout.count() <= 0 || Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!AgcDriverFailed_nid_postfix()) continue;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::fprintf(stderr, "agc_frame_replay: the driver failed; if it lost the GPU device (Vulkan result -4), the NVIDIA shader store may have been reset: check the game dictionary before the next game run; exiting without a driver shutdown\n");
            hardExit(6);
        }
        std::fprintf(stderr, "agc_frame_replay: no result after %lld s (--timeout-s); exiting without a driver shutdown\n", static_cast<long long>(timeout.count()));
        hardExit(4);
    }).detach();
}

std::uint64_t headerNumber(const std::string& header, std::string_view key) {
    const auto quoted = "\"" + std::string(key) + "\":";
    const auto at = header.find(quoted);
    if (at == std::string::npos) throw std::runtime_error("frame replay: the capture header has no " + std::string(key));
    return std::strtoull(header.c_str() + at + quoted.size(), nullptr, 10);
}

// Host regions: what this process had allocated inside the guest arena's range before libc reserved it at static
// init (GuestArena.cpp: "_holes"/"_hostRegions": the process heap, early thread stacks, early CRT heaps). Their
// addresses are randomized per process start (high-entropy bottom-up ASLR), so in some starts one lands on a range
// the capture's process used, and the replay cannot map it ("fixed guest mapping ... overlaps a host region").
// HostCollisions lists the captured ranges that overlap one; the replay then starts itself again (main).
using Interval = std::pair<std::uint64_t, std::uint64_t>;

[[maybe_unused]] std::vector<Interval> HostCollisions(const FrameCaptureReader& reader, const std::function<bool(std::uint64_t, std::uint64_t)>& overlaps) {
    std::vector<Interval> found;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind != FrameMemoryKind::Range || chunk.rangeKind == FrameRangeKind::Host) continue;
        if (overlaps(chunk.address, chunk.bytes)) found.emplace_back(chunk.address, chunk.address + chunk.bytes);
    }
    return found;
}

#ifdef _WIN32
// What owns the host allocation at `address`: its region, type and protection, and whether it is a heap of this
// process, a thread's stack or an image.
std::string DescribeHostRegion(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), &info, sizeof(info)) == 0) return "not queryable";
    const auto base = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
    std::string owner;
    if (info.Type == MEM_IMAGE) {
        char name[MAX_PATH]{};
        owner = GetModuleFileNameA(reinterpret_cast<HMODULE>(base), name, MAX_PATH) != 0 ? std::string("image ") + name : "image";
    }
    std::vector<HANDLE> heaps(256);
    const auto count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
    for (DWORD index = 0; index < count && index < heaps.size(); ++index) {
        if (reinterpret_cast<std::uintptr_t>(heaps[index]) == base) owner = std::string("heap ") + (heaps[index] == GetProcessHeap() ? "(the process heap)" : std::to_string(index));
    }
    using Query = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread")));
    if (const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snapshot != INVALID_HANDLE_VALUE && query != nullptr) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (bool more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
            const auto thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr) continue;
            struct { LONG status; void* teb; void* process; void* thread; std::uintptr_t affinity; LONG priority; LONG basePriority; } basic{};
            if (query(thread, 0, &basic, sizeof(basic), nullptr) == 0 && basic.teb != nullptr) {
                const auto* tib = static_cast<const NT_TIB*>(basic.teb);
                MEMORY_BASIC_INFORMATION stack{};
                if (VirtualQuery(tib->StackLimit, &stack, sizeof(stack)) != 0 && stack.AllocationBase == info.AllocationBase) owner = "stack of thread " + std::to_string(entry.th32ThreadID);
                if (reinterpret_cast<std::uintptr_t>(basic.teb) >= base && reinterpret_cast<std::uintptr_t>(basic.teb) < reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) owner = "TEB of thread " + std::to_string(entry.th32ThreadID);
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
    }
    char text[320];
    std::snprintf(text, sizeof(text), "region 0x%llx+0x%llx in allocation 0x%llx, %s %s, protect 0x%lx (allocated 0x%lx)%s%s",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.BaseAddress)), static_cast<unsigned long long>(info.RegionSize), static_cast<unsigned long long>(base),
                  info.State == MEM_COMMIT ? "committed" : info.State == MEM_RESERVE ? "reserved" : "free",
                  info.Type == MEM_IMAGE ? "image" : info.Type == MEM_MAPPED ? "mapped" : "private", info.Protect, info.AllocationProtect,
                  owner.empty() ? "" : ": ", owner.c_str());
    return text;
}

// --host-regions: every host region inside the guest arena of this process, described.
int PrintHostRegions() {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    GuestArena::GuestArenaRange_nid_postfix(&base, &size);
    std::size_t regions = 0;
    for (std::uintptr_t at = base; at < base + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0) break;
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State != MEM_FREE && GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(at, std::min<std::uintptr_t>(next, base + size) - at)) {
            std::printf("host region %s\n", DescribeHostRegion(at).c_str());
            ++regions;
        }
        at = next;
    }
    std::printf("%zu host regions in the guest arena 0x%llx+0x%llx\n", regions, static_cast<unsigned long long>(base), static_cast<unsigned long long>(size));
    return 0;
}
#endif

void restore(const FrameCaptureReader& reader, const FrameMemoryChunk& chunk) {
    reader.Read(chunk, std::span(reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes)));
}

bool committedWritable(std::uint64_t address, std::uint64_t bytes) {
#ifdef _WIN32
    for (auto at = address; at < address + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) return false;
        const auto access = info.Protect & 0xffu;
        if ((info.Protect & PAGE_GUARD) != 0 || (access != PAGE_READWRITE && access != PAGE_EXECUTE_READWRITE)) return false;
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
#elif defined(__linux__)
    auto cursor = address;
    HostMapping::Walk(address, address + bytes, "frame replay", [&](const HostMapping::Mapping& mapping) {
        if (mapping.start > cursor || !mapping.readable || !mapping.writable) return;
        cursor = std::max(cursor, mapping.end);
    });
    return cursor >= address + bytes;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return false;
#endif
}

void commitRange(std::uint64_t address, std::uint64_t length) {
    auto* const pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    const auto bytes = static_cast<std::size_t>(length);
#ifdef __linux__
    const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    const auto begin = address & ~(page - 1);
    const auto end = (address + length + page - 1) & ~(page - 1);
    if (begin != address || end != address + length) {
        GuestArena::GuestArenaSetProtection_nid_postfix(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin), 0x04);
        return;
    }
#endif
    GuestArena::GuestArenaCommit_nid_postfix(pointer, bytes, 0x04, bytes);
}

void commit(const FrameMemoryChunk& chunk) {
    commitRange(chunk.address, chunk.bytes);
}

std::pair<std::uint64_t, std::uint64_t> progress();

struct Skipped {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t bytes = 0;

    void Add(const FrameMemoryChunk& chunk) {
        ranges.emplace_back(chunk.address, chunk.address + chunk.bytes);
        bytes += chunk.bytes;
    }

    bool Overlaps(const FrameMemoryChunk& chunk) const {
        return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) { return chunk.address < range.second && range.first < chunk.address + chunk.bytes; });
    }
};

void mapHostRanges(const FrameCaptureReader& reader, Skipped& skipped) {
    std::vector<const FrameMemoryChunk*> host;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Range && chunk.rangeKind == FrameRangeKind::Host) host.push_back(&chunk);
    }
#ifdef _WIN32
    constexpr std::uint64_t Granularity = 64u * 1024u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> envelopes;
    for (const auto* chunk : host) {
        const auto begin = chunk->address & ~(Granularity - 1);
        const auto end = (chunk->address + chunk->bytes + Granularity - 1) & ~(Granularity - 1);
        if (!envelopes.empty() && begin <= envelopes.back().second) envelopes.back().second = std::max(envelopes.back().second, end);
        else envelopes.emplace_back(begin, end);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> refused;
    for (const auto& [begin, end] : envelopes) {
        if (VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(begin)), static_cast<SIZE_T>(end - begin), MEM_RESERVE, PAGE_NOACCESS) == nullptr) refused.emplace_back(begin, end);
    }
    for (const auto* chunk : host) {
        const bool blocked = std::any_of(refused.begin(), refused.end(), [&](const auto& range) { return chunk->address < range.second && range.first < chunk->address + chunk->bytes; });
        if (!blocked && VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk->address)), static_cast<SIZE_T>(chunk->bytes), MEM_COMMIT, PAGE_READWRITE) != nullptr) continue;
        if ((chunk->access & FrameRangeAccess::Gpu) != 0) throw std::runtime_error("frame replay: cannot map the GPU-visible host range at " + std::to_string(chunk->address));
        skipped.Add(*chunk);
    }
#elif defined(__linux__)
    const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    std::sort(host.begin(), host.end(), [](const auto* left, const auto* right) { return left->address < right->address; });
    std::vector<std::pair<std::uint64_t, std::uint64_t>> envelopes;
    for (const auto* chunk : host) {
        const auto begin = chunk->address & ~(page - 1);
        const auto end = (chunk->address + chunk->bytes + page - 1) & ~(page - 1);
        if (!envelopes.empty() && begin < envelopes.back().second) envelopes.back().second = std::max(envelopes.back().second, end);
        else envelopes.emplace_back(begin, end);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> refused;
    for (const auto& [begin, end] : envelopes) {
        auto* const wanted = reinterpret_cast<void*>(static_cast<std::uintptr_t>(begin));
        const auto bytes = static_cast<std::size_t>(end - begin);
        void* const placed = mmap(wanted, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (placed == wanted) continue;
        if (placed != MAP_FAILED) munmap(placed, bytes);
        refused.emplace_back(begin, end);
    }
    for (const auto* chunk : host) {
        const bool blocked = std::any_of(refused.begin(), refused.end(), [&](const auto& range) { return chunk->address < range.second && range.first < chunk->address + chunk->bytes; });
        if (!blocked) continue;
        if ((chunk->access & FrameRangeAccess::Gpu) != 0) throw std::runtime_error("frame replay: cannot map the GPU-visible host range at " + std::to_string(chunk->address));
        skipped.Add(*chunk);
    }
#else
    for (const auto* chunk : host) skipped.Add(*chunk);
#endif
}

struct AliasedPages {
    std::size_t pageBytes = 0;
    std::map<std::uint64_t, std::uint64_t> groupOf;
};

AliasedPages aliasedPages(const FrameCaptureReader& reader) {
    AliasedPages result;
    for (const auto& call : reader.Calls()) {
        if (call.kind != FrameCallKind::AliasedPages) continue;
        result.pageBytes = static_cast<std::size_t>(call.size);
        for (std::size_t index = 0; index + 1 < call.values.size(); index += 2) result.groupOf[call.values[index]] = call.values[index + 1];
    }
    return result;
}

void commitWithAliases(void* pointer, std::size_t bytes, const AliasedPages& aliases, std::map<std::uint64_t, void*>& sections) {
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    const auto end = begin + bytes;
    auto alias = aliases.groupOf.lower_bound(begin);
    if (alias == aliases.groupOf.end() || alias->first >= end) {
        commitRange(begin, bytes);
        return;
    }
#ifdef _WIN32
    for (auto at = begin; at < end;) {
        if (alias != aliases.groupOf.end() && alias->first == at) {
            auto& section = sections[alias->second];
            if (section == nullptr) {
                section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(aliases.pageBytes), nullptr);
                if (section == nullptr) throw std::runtime_error("frame replay: cannot create the shared backing of an aliased page");
            }
            GuestArena::GuestArenaMapDirect_nid_postfix(reinterpret_cast<void*>(at), aliases.pageBytes, section, 0, PAGE_READWRITE);
            at += aliases.pageBytes;
            ++alias;
            continue;
        }
        const auto next = alias != aliases.groupOf.end() && alias->first < end ? static_cast<std::uintptr_t>(alias->first) : end;
        GuestArena::GuestArenaCommit_nid_postfix(reinterpret_cast<void*>(at), next - at, 0x04, next - at);
        at = next;
    }
#else
    throw std::runtime_error("frame replay: aliased guest pages are replayed on Windows only");
#endif
}

void restoreSnapshot(const FrameCaptureReader& reader, const Skipped& skipped) {
    std::vector<const FrameMemoryChunk*> snapshot;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Snapshot && !skipped.Overlaps(chunk)) snapshot.push_back(&chunk);
    }
    const auto workers = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    for (std::size_t first = 0; first < snapshot.size(); first += workers) {
        std::vector<std::future<void>> pending;
        for (auto index = first; index < std::min(snapshot.size(), first + workers); ++index) {
            pending.push_back(std::async(std::launch::async, [&reader, chunk = snapshot[index]] { restore(reader, *chunk); }));
        }
        for (auto& future : pending) future.get();
    }
}

Skipped restoreMemory(const FrameCaptureReader& reader) {
    if (reader.Version() < 2) throw std::runtime_error("frame replay: calls format 1 does not record whether GPU writes had landed before each memory delta was read; record the capture again");
    std::uintptr_t base = 0;
    std::size_t size = 0;
    GuestArena::GuestArenaRange_nid_postfix(&base, &size);
    if (!reader.Header().empty() && headerNumber(reader.Header(), "arena_base") != base) throw std::runtime_error("frame replay: the guest arena is at a different address than in the capture");
    progress();
    Skipped skipped;
    mapHostRanges(reader, skipped);
    std::vector<bool> owned;
    for (const auto& chunk : reader.Chunks()) owned.push_back(chunk.kind == FrameMemoryKind::Range && chunk.rangeKind != FrameRangeKind::Host && committedWritable(chunk.address, chunk.bytes));
    const auto aliases = aliasedPages(reader);
    std::map<std::uint64_t, void*> sections;
    {
        GuestAllocations::Mutation mutation;
        for (std::size_t index = 0; index < reader.Chunks().size(); ++index) {
            const auto& chunk = reader.Chunks()[index];
            if (chunk.kind != FrameMemoryKind::Range || chunk.rangeKind == FrameRangeKind::Host) continue;
            const bool gpu = (chunk.access & FrameRangeAccess::Gpu) != 0;
            if (owned[index]) {
                if (!gpu) skipped.Add(chunk);
                else std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.address)), 0, static_cast<std::size_t>(chunk.bytes));
                continue;
            }
            auto* const pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.address));
            const auto bytes = static_cast<std::size_t>(chunk.bytes);
            try {
                GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, bytes);
            } catch (const std::exception&) {
                if (gpu) throw;
                skipped.Add(chunk);
                continue;
            }
            commitWithAliases(pointer, bytes, aliases, sections);
            mutation.Add(pointer, bytes, (chunk.access & FrameRangeAccess::Readable) != 0, (chunk.access & FrameRangeAccess::Writable) != 0, gpu);
        }
    }
#ifdef _WIN32
    for (const auto& [group, section] : sections) CloseHandle(section);
#endif
    if (!aliases.groupOf.empty()) std::fprintf(stderr, "[frame-replay] %zu aliased pages share %zu backings\n", aliases.groupOf.size(), sections.size());
    restoreSnapshot(reader, skipped);
    return skipped;
}

void restoreRewritten(const FrameCaptureReader& reader, std::span<const FrameMemoryChunk* const> rewritten, const Skipped& skipped) {
    std::vector<const FrameMemoryChunk*> snapshot;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Snapshot && !skipped.Overlaps(chunk)) snapshot.push_back(&chunk);
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto* left, const auto* right) { return left->address < right->address; });
    std::map<const FrameMemoryChunk*, std::vector<std::pair<std::uint64_t, std::uint64_t>>> pieces;
    for (const auto* chunk : rewritten) {
        if (skipped.Overlaps(*chunk)) continue;
        const auto begin = chunk->address;
        const auto end = chunk->address + chunk->bytes;
        std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(begin)), 0, static_cast<std::size_t>(chunk->bytes));
        auto at = std::upper_bound(snapshot.begin(), snapshot.end(), begin, [](std::uint64_t address, const auto* candidate) { return address < candidate->address; });
        if (at != snapshot.begin()) --at;
        for (; at != snapshot.end() && (*at)->address < end; ++at) {
            const auto low = std::max(begin, (*at)->address);
            const auto high = std::min(end, (*at)->address + (*at)->bytes);
            if (low < high) pieces[*at].emplace_back(low, high);
        }
    }
    for (const auto& [chunk, ranges] : pieces) {
        const auto bytes = reader.Read(*chunk);
        for (const auto& [low, high] : ranges) std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(low)), bytes.data() + (low - chunk->address), static_cast<std::size_t>(high - low));
    }
}

void resetMemory(const FrameCaptureReader& reader, const Skipped& skipped) {
    AgcDriverWaitIdle_nid_postfix();
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Range && !skipped.Overlaps(chunk)) std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.address)), 0, static_cast<std::size_t>(chunk.bytes));
    }
    restoreSnapshot(reader, skipped);
}

std::pair<std::uint64_t, std::uint64_t> progress() {
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    AgcDriverSerialProgress_nid_postfix(&accepted, &completed);
    return {accepted, completed};
}

struct Replay {
    const Options& options;
    const FrameCaptureReader& reader;
    Frames& frames = *new Frames;
    std::map<std::uint32_t, std::shared_ptr<ReplayOutput>> outputs;
    std::mutex memoryMutex;
    std::map<std::uint64_t, std::vector<const FrameMemoryChunk*>> groups;
    Skipped skipped;
    std::atomic<std::uint64_t> paceTimeouts{0};
    std::atomic<std::uint64_t> memoryPaceTimeouts{0};
    std::atomic<std::uint64_t> staleRegistrations{0};
    std::atomic<std::uint64_t> failedRegistrations{0};
    std::atomic<std::uint64_t> submits{0};
    std::atomic<std::uint64_t> deltaBytes{0};
    std::atomic<std::uint64_t> parkRestores{0};
    std::atomic<std::int64_t> paceWaitedNs{0};
    std::atomic<bool> stalled{false};
    std::atomic<std::uint64_t> stalledAt{0};
    std::atomic<std::uint64_t> earlyReleases{0};
    std::atomic<std::uint64_t> lateReleases{0};
    std::atomic<std::uint64_t> syntheticReleases{0};
    std::atomic<std::uint64_t> keptReservations{0};
    std::atomic<std::size_t> sequenced{0};
    std::atomic<int> busy{0};
    std::atomic<std::uint64_t> parkedReleases{0};
    std::atomic<std::uint64_t> fallbackReleases{0};
    std::atomic<bool> parkedWithoutRelease{false};
    std::uint64_t unfinished = 0;
    std::size_t threads = 0;
    std::size_t pass = 0;
    bool recording = false;
    // Frame windows: the call that ends a frame (a CPU flip, or the suspend point when the title
    // flips from the GPU), the windows closed and those closed before the GPU work drained.
    bool closeFrames = false;
    FrameCallKind boundary = FrameCallKind::Flip;
    std::atomic<std::size_t> framesClosed{0};
    std::atomic<std::size_t> undrainedWindows{0};
    // A frame's windows are closing: the next call waits for it, so the release timer (runWindow's
    // breaker) must not count that time as no progress and release awaited dwords early.
    std::atomic<bool> closingWindows{false};
    std::size_t bindlessPrimed = 0;
    std::vector<std::uint64_t> deferredBindless;
    bool bindlessWritten = false;

    void primeBindless(std::span<const std::uint64_t> words) {
        if (!options.bindlessPrime || words.empty()) return;
        AgcDriverPrimeBindlessTables_nid_postfix(words);
        bindlessPrimed += static_cast<std::size_t>(words.front());
    }

    // --bindless-out: the tables registered by the end of the first replayed frame (or of the replay).
    void writeBindless() {
        if (options.bindlessOut.empty() || bindlessWritten) return;
        bindlessWritten = true;
        const auto words = AgcDriverBindlessRegistrations_nid_postfix();
        std::ofstream file(options.bindlessOut, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(std::uint64_t)));
        if (!file.good()) throw std::runtime_error("frame replay: cannot write " + options.bindlessOut.string());
        std::fprintf(stderr, "[frame-replay] %llu bindless tables written to %s\n", static_cast<unsigned long long>(words.empty() ? 0 : words.front()), options.bindlessOut.string().c_str());
    }
    Clock::time_point startedAt{};
    Clock::time_point endedAt{};

    struct SettledHold {
        bool held;
        ~SettledHold() {
            if (held) AgcDriverReleaseSettled_nid_postfix();
        }
    };

    double paceWaitedSeconds() const {
        return static_cast<double>(paceWaitedNs.load()) / 1e9;
    }

    void report() const {
        const auto [accepted, completed] = progress();
        std::fprintf(stderr, "[frame-replay] %llu submits, %zu frames ready, %.1f s since the first frame, pacing waited %.1f s (%llu timeouts), serials %llu/%llu, blocked on%s\n",
                     static_cast<unsigned long long>(submits.load()), frames.Times().size(), std::chrono::duration<double>(Clock::now() - startedAt).count(), paceWaitedSeconds(),
                     static_cast<unsigned long long>(paceTimeouts.load()), static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted), describeBlocked().c_str());
    }

    static std::string describeBlocked() {
        std::array<AgcDriver::AwaitedWait, 64> awaited{};
        const auto count = AgcDriverAwaitedWaits_nid_postfix(awaited.data(), awaited.size());
        std::string text;
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto& wait = awaited[slot];
            if (wait.address == 0) continue;
            char entry[128];
            std::snprintf(entry, sizeof(entry), " 0x%llx (function %u, reference 0x%llx, mask 0x%llx, now 0x%llx)", static_cast<unsigned long long>(wait.address), static_cast<unsigned>(wait.function),
                          static_cast<unsigned long long>(wait.reference), static_cast<unsigned long long>(wait.mask), static_cast<unsigned long long>(load(wait.address, wait.bytes == 8 ? 8 : 4)));
            text += entry;
        }
        return text.empty() ? " nothing" : text;
    }

    static bool paced(FrameCallKind kind) {
        return kind == FrameCallKind::Submit || kind == FrameCallKind::Flip || kind == FrameCallKind::ReserveEopFlip || kind == FrameCallKind::Suspend;
    }

    void pace(const FrameCall& call) {
        if (!options.pace || !recording || !paced(call.kind) || call.accepted < call.completed) return;
        const bool memory = call.kind == FrameCallKind::Memory;
        if (!memory && stalled.load()) {
            if (progress().second == stalledAt.load()) return;
            stalled.store(false);
        }
        const auto inFlight = options.exact || memory ? 0 : call.accepted - call.completed;
        const auto began = Clock::now();
        const auto timeout = memory ? options.memoryPaceTimeout : options.paceTimeout;
        auto lastCompleted = progress().second;
        auto deadline = began + timeout;
        auto quietSince = began;
        const auto waited = [&] { paceWaitedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count(); };
        struct Parked {
            std::atomic<int>& busy;
            explicit Parked(std::atomic<int>& counter) : busy(counter) { --busy; }
            ~Parked() { ++busy; }
        } parked{busy};
        while (true) {
            const auto [accepted, completed] = progress();
            if (accepted - completed <= inFlight) {
                if (options.exact && accepted == completed) AgcDriverWaitIdle_nid_postfix();
                return waited();
            }
            const auto now = Clock::now();
            if (completed != lastCompleted) {
                lastCompleted = completed;
                deadline = now + timeout;
                quietSince = now;
            } else if (!AgcDriverQuiescent_nid_postfix()) {
                deadline = now + timeout;
            }
            if (memory && deltaReleasesWait(call)) return waited();
            if (now - quietSince >= (options.exact ? ExactQuiet : BlockedQuiet) && blockedOnWaits()) return waited();
            if (now >= deadline) {
                waited();
                if (memory) {
                    ++memoryPaceTimeouts;
                    std::fprintf(stderr, "[frame-replay] memory pacing gave up at call %llu: no completion for %lld ms, serials %llu/%llu, the capture had %llu in flight\n", static_cast<unsigned long long>(call.ordinal),
                                 static_cast<long long>(timeout.count()), static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(inFlight));
                    if (options.strict) throw std::runtime_error("frame replay: memory call " + std::to_string(call.ordinal) + " waited longer than the pacing timeout");
                    return;
                }
                stalled.store(true);
                stalledAt.store(completed);
                std::fprintf(stderr, "[frame-replay] pacing gave up at call %llu (%s): serials %llu/%llu, the capture had %llu in flight\n", static_cast<unsigned long long>(call.ordinal),
                             std::string(AgcDriver::FrameCallName(call.kind)).c_str(), static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(inFlight));
                ++paceTimeouts;
                if (options.strict) throw std::runtime_error("frame replay: call " + std::to_string(call.ordinal) + " waited longer than the pacing timeout");
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    static constexpr auto ExactQuiet = std::chrono::milliseconds(50);
    static constexpr auto ParkedQuiet = std::chrono::milliseconds(5);
    static constexpr auto BlockedQuiet = std::chrono::milliseconds(5);

    bool deltaReleasesWait(const FrameCall& call) {
        const auto found = groups.find(call.values.at(0));
        if (found == groups.end()) return false;
        std::array<AgcDriver::AwaitedWait, 64> awaited{};
        const auto count = AgcDriverAwaitedWaits_nid_postfix(awaited.data(), awaited.size());
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto& wait = awaited[slot];
            if (wait.address == 0) continue;
            for (const auto* chunk : found->second) {
                if (chunk->address <= wait.address && wait.address < chunk->address + chunk->bytes) return true;
            }
        }
        return false;
    }

    bool blockedOnWaits() {
        if (!AgcDriverQuiescent_nid_postfix()) return false;
        std::lock_guard lock(memoryMutex);
        std::array<AgcDriver::AwaitedWait, 64> awaited{};
        const auto count = AgcDriverAwaitedWaits_nid_postfix(awaited.data(), awaited.size());
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto& wait = awaited[slot];
            if (wait.address != 0 && (wait.function == 0 || !satisfies(wait, load(wait.address, wait.bytes == 8 ? 8 : 4)))) return true;
        }
        return false;
    }

    void execute(const FrameCall& call) {
        pace(call);
        switch (call.kind) {
        case FrameCallKind::RegisterShader:
            registration([&] { AgcDriverRegisterShader_nid_postfix(shader(call.address)); });
            return;
        case FrameCallKind::ResolveShaderAbi:
            registration([&] { AgcDriverResolveShaderAbi_nid_postfix(shader(call.address), call.context, call.primitive); });
            return;
        case FrameCallKind::ResolveGraphicsAbi:
            registration([&] { AgcDriverResolveGraphicsAbi_nid_postfix(shader(call.values.at(0)), shader(call.values.at(1)), static_cast<std::uint32_t>(call.values.at(2))); });
            return;
        case FrameCallKind::ResolveGraphicsStagesAbi: {
            std::vector<const Shader*> stages;
            for (const auto value : call.values) stages.push_back(shader(value));
            registration([&] { AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, call.context, call.primitive); });
            return;
        }
        case FrameCallKind::RegisterVideoOutput: {
            const auto handle = static_cast<std::uint32_t>(call.values.at(0));
            auto output = std::make_shared<ReplayOutput>(frames);
            AgcDriverRegisterVideoOutput_nid_postfix(handle, output);
            outputs.emplace(handle, std::move(output));
            return;
        }
        case FrameCallKind::UnregisterVideoOutput: {
            const auto handle = static_cast<std::uint32_t>(call.values.at(0));
            AgcDriverUnregisterVideoOutput_nid_postfix(handle, outputs.at(handle));
            outputs.erase(handle);
            return;
        }
        case FrameCallKind::SetTessellationFactorRing:
            sceAgcDriverSetTFRing(reinterpret_cast<const volatile void*>(static_cast<std::uintptr_t>(call.address)), static_cast<std::uint32_t>(call.size));
            return;
        case FrameCallKind::Submit: {
            const Packet packet{reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(call.address)), static_cast<std::uint32_t>(call.size), static_cast<std::uint8_t>(call.flags), {}};
            traced(call, [&] {
                if (call.queue == 0) sceAgcDriverSubmitDcb(&packet);
                else sceAgcDriverSubmitAcb(call.queue, &packet);
            });
            if (++submits % 25 == 0) report();
            return;
        }
        case FrameCallKind::Flip:
            traced(call, [&] { AgcDriverSubmitFlip_nid_postfix(flipInfo(call)); });
            if (closeFrames && boundary == FrameCallKind::Flip) closeFrame();
            return;
        case FrameCallKind::ReserveEopFlip: {
            const auto token = static_cast<std::uint32_t>(call.values.at(4));
            std::uint32_t reserved = 0;
            try {
                reserved = AgcDriverReserveEopFlipAt_nid_postfix(flipInfo(call), token);
            } catch (const AgcDriver::EopFlipAlreadyReserved&) {
                if (pass == 0) throw;
                ++keptReservations;
                return;
            }
            if (reserved != token) throw std::runtime_error("frame replay: EOP flip token " + std::to_string(token) + " was not reserved");
            return;
        }
        case FrameCallKind::Suspend:
            traced(call, [] { AgcDriverSuspendPoint_nid_postfix(); });
            if (closeFrames && boundary == FrameCallKind::Suspend) closeFrame();
            return;
        case FrameCallKind::AliasedPages:
        case FrameCallKind::Checkpoint:
            return;
        case FrameCallKind::BindlessTables:
            // In the prologue: primed once the prologue's memory is restored (run).
            if (!recording) deferredBindless = call.values;
            else primeBindless(call.values);
            return;
        case FrameCallKind::QueueStates: {
            std::vector<std::uint32_t> words;
            words.reserve(call.values.size());
            for (const auto value : call.values) words.push_back(static_cast<std::uint32_t>(value));
            AgcDriverLoadQueueStates_nid_postfix(words);
            return;
        }
        case FrameCallKind::Memory: {
            const std::unique_lock lock(memoryMutex);
            const bool pacing = options.pace && recording;
            bool parkedAsCaptured = true;
            std::vector<StagedWord> staged;
            {
                const auto began = Clock::now();
                const bool settled = !pacing || AgcDriverHoldSettled_nid_postfix(stalled.load() && progress().second == stalledAt.load() ? 0 : options.busyTimeout.count());
                const SettledHold hold{pacing};
                paceWaitedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count();
                if (!settled && options.strict) throw std::runtime_error("frame replay: the replay did not settle before memory delta " + std::to_string(call.ordinal));
                parkedAsCaptured = !pacing || !settled || call.values.size() < 2 || atCapturedPark(call);
                staged = restoreGroup(call.values.at(0), false, heldWaits());
            }
            releaseStaged(staged);
            if (!parkedAsCaptured) restoreAtCapturedPark(call);
            return;
        }
        case FrameCallKind::FrameStart:
        case FrameCallKind::End:
        case FrameCallKind::DisplayBuffer:
            return;
        }
        throw std::runtime_error("frame replay: unknown call kind");
    }

    // Closes the profile windows of the frame that just flipped: once the GPU work submitted so far
    // completed (as --exact pacing waits; a GPU blocked on a wait that a later call releases ends
    // the wait after 10 s, and the window then holds what completed), as "frame-<n>".
    void closeFrame() {
        closingWindows.store(true);
        struct Closed {
            std::atomic<bool>& flag;
            ~Closed() { flag.store(false); }
        } closed{closingWindows};
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        auto quietSince = Clock::now();
        auto lastCompleted = progress().second;
        bool drained = false;
        while (true) {
            const auto [accepted, completed] = progress();
            if (accepted == completed) {
                drained = true;
                break;
            }
            const auto now = Clock::now();
            if (completed != lastCompleted) {
                lastCompleted = completed;
                quietSince = now;
            }
            if (now >= deadline || (now - quietSince >= ExactQuiet && blockedOnWaits())) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (drained) AgcDriverWaitIdle_nid_postfix();
        else ++undrainedWindows;
        const auto label = "frame-" + std::to_string(++framesClosed);
        AgcDriver::ProfileWindows::Close(label.c_str());
        if (framesClosed.load() == 1) writeBindless();
    }

    static bool rewindWait(const AgcDriver::AwaitedWait& wait) {
        return wait.function == 0 && wait.bytes == 0;
    }

    bool atCapturedPark(const FrameCall& call) {
        std::array<AgcDriver::AwaitedWait, 64> awaited{};
        const auto count = AgcDriverAwaitedWaits_nid_postfix(awaited.data(), awaited.size());
        std::vector<std::uint64_t> parked;
        for (std::size_t slot = 0; slot < count; ++slot) parked.push_back(awaited[slot].address);
        std::ranges::sort(parked);
        const auto [accepted, completed] = progress();
        return accepted - completed == call.accepted - call.completed && std::ranges::equal(parked, std::span(call.values).subspan(2));
    }

    void restoreAtCapturedPark(const FrameCall& call) {
        const auto began = Clock::now();
        const auto deadline = began + options.busyTimeout;
        while (Clock::now() < deadline) {
            if (atCapturedPark(call) && AgcDriverHoldSettled_nid_postfix(0)) {
                const SettledHold hold{true};
                if (!atCapturedPark(call)) continue;
                restoreGroup(call.values.at(0), true);
                ++parkRestores;
                break;
            }
            if (AgcDriverSettled_nid_postfix() && !atCapturedPark(call)) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        paceWaitedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count();
    }

    struct StagedWord {
        std::uint64_t address;
        std::vector<std::byte> value;
    };

    static std::vector<AgcDriver::AwaitedWait> heldWaits() {
        std::array<AgcDriver::AwaitedWait, 64> awaited{};
        const auto count = AgcDriverAwaitedWaits_nid_postfix(awaited.data(), awaited.size());
        std::vector<AgcDriver::AwaitedWait> held;
        for (std::size_t slot = 0; slot < count; ++slot) {
            if (!rewindWait(awaited[slot]) && awaited[slot].bytes != 0) held.push_back(awaited[slot]);
        }
        if (held.size() < 2) held.clear();
        return held;
    }

    void releaseStaged(const std::vector<StagedWord>& words) {
        for (const auto& word : words) {
            const bool settled = AgcDriverHoldSettled_nid_postfix(options.busyTimeout.count());
            const SettledHold hold{true};
            if (!settled) return;
            AgcDriverPrepareCommandWrite_nid_no_patch(word.address, word.value.size());
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(word.address)), word.value.data(), word.value.size());
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    }

    std::vector<StagedWord> restoreGroup(std::uint64_t group, bool again, const std::vector<AgcDriver::AwaitedWait>& held = {}) {
        std::vector<StagedWord> staged;
        const auto found = groups.find(group);
        if (found == groups.end()) return staged;
        std::array<AgcDriver::AwaitedWait, 64> awaited{};
        const auto count = AgcDriverAwaitedWaits_nid_postfix(awaited.data(), awaited.size());
        std::vector<std::uint64_t> parked;
        for (std::size_t slot = 0; slot < count; ++slot) {
            if (rewindWait(awaited[slot])) parked.push_back(awaited[slot].address);
        }
        std::vector<std::pair<std::uint32_t*, std::uint32_t>> controls;
        for (const auto* chunk : found->second) {
            if (skipped.Overlaps(*chunk)) continue;
            auto bytes = reader.Read(*chunk);
            auto* const target = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(chunk->address));
            AgcDriverPrepareCommandWrite_nid_no_patch(chunk->address, static_cast<std::size_t>(chunk->bytes));
            for (auto at = static_cast<std::size_t>((4 - chunk->address % 4) % 4); at + 4 <= bytes.size(); at += 4) {
                std::uint32_t value = 0;
                std::memcpy(&value, bytes.data() + at, sizeof(value));
                if ((value & AgcDriver::Pm4::RewindValid) == 0) continue;
                std::uint32_t header = 0;
                if (at >= 4) std::memcpy(&header, bytes.data() + at - 4, sizeof(header));
                const bool rewind = (header >> 30u) == 3u && ((header >> 8u) & 0xffu) == 0x59u;
                if (!rewind && std::find(parked.begin(), parked.end(), chunk->address + at) == parked.end()) continue;
                if (!again) controls.emplace_back(reinterpret_cast<std::uint32_t*>(target + at), value);
                std::memcpy(bytes.data() + at, target + at, sizeof(value));
            }
            for (const auto& wait : held) {
                if (wait.address < chunk->address || wait.address + wait.bytes > chunk->address + bytes.size()) continue;
                const auto offset = static_cast<std::size_t>(wait.address - chunk->address);
                staged.push_back({wait.address, {bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + wait.bytes)}});
                std::memcpy(bytes.data() + offset, target + offset, wait.bytes);
            }
            std::memcpy(target, bytes.data(), bytes.size());
            deltaBytes += chunk->bytes;
            if (recording) AgcDriver::Graphics::ResidentCensus::NoteCpuWrite(chunk->address, chunk->bytes);
        }
        for (const auto& [control, value] : controls) std::atomic_ref<std::uint32_t>(*control).store(value, std::memory_order_release);
        return staged;
    }

    bool releaseRewind(const FrameCall& call, const AgcDriver::AwaitedWait& wait) {
        if (call.kind != FrameCallKind::Memory) return false;
        const auto found = groups.find(call.values.at(0));
        if (found == groups.end()) return false;
        for (const auto* chunk : found->second) {
            if (chunk->address > wait.address || wait.address + 4 > chunk->address + chunk->bytes || skipped.Overlaps(*chunk)) continue;
            const auto bytes = reader.Read(*chunk);
            std::uint32_t value = 0;
            std::memcpy(&value, bytes.data() + (wait.address - chunk->address), sizeof(value));
            if ((value & AgcDriver::Pm4::RewindValid) == 0 || !AgcDriverHoldSettled_nid_postfix(0)) return false;
            const SettledHold hold{true};
            restoreGroup(call.values.at(0), false);
            return true;
        }
        return false;
    }

    static std::uint64_t load(std::uint64_t address, std::size_t bytes) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), bytes);
        return value;
    }

    static bool satisfies(const AgcDriver::AwaitedWait& wait, std::uint64_t value) {
        const auto masked = value & wait.mask;
        const auto reference = wait.reference & wait.mask;
        switch (wait.function) {
        case 1: return masked < reference;
        case 2: return masked <= reference;
        case 3: return masked == reference;
        case 4: return masked != reference;
        case 5: return masked >= reference;
        case 6: return masked > reference;
        default: return false;
        }
    }

    static std::optional<std::uint64_t> satisfying(const AgcDriver::AwaitedWait& wait, std::uint64_t current) {
        const auto keep = current & ~wait.mask;
        const auto reference = wait.reference & wait.mask;
        switch (wait.function) {
        case 2:
        case 3:
        case 5: return keep | reference;
        case 1: return reference == 0 ? std::nullopt : std::optional<std::uint64_t>(keep);
        case 4: return keep | (~reference & wait.mask);
        case 6: return reference == wait.mask ? std::nullopt : std::optional<std::uint64_t>(keep | ((reference + 1) & wait.mask));
        default: return std::nullopt;
        }
    }

    bool writeRecordedValue(const FrameCall& call, const AgcDriver::AwaitedWait& wait) {
        if (call.kind != FrameCallKind::Memory) return false;
        const auto found = groups.find(call.values.at(0));
        if (found == groups.end()) return false;
        const auto address = wait.address;
        for (const auto* chunk : found->second) {
            if (chunk->address > address || address + 4 > chunk->address + chunk->bytes || skipped.Overlaps(*chunk)) continue;
            const auto bytes = reader.Read(*chunk);
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(wait.bytes == 8 ? 8 : 4, chunk->address + chunk->bytes - address));
            const auto* const recorded = bytes.data() + (address - chunk->address);
            auto* const target = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(address));
            if (std::memcmp(target, recorded, length) == 0) return false;
            std::uint64_t value = 0;
            std::memcpy(&value, recorded, length);
            if (wait.function != 0 && !satisfies(wait, value)) return false;
            std::memcpy(target, recorded, length);
            return true;
        }
        return false;
    }

    std::size_t recordedIndex(std::span<const FrameCall> calls, std::size_t from, const AgcDriver::AwaitedWait& wait) const {
        for (auto index = from; index < calls.size(); ++index) {
            if (calls[index].kind != FrameCallKind::Memory) continue;
            const auto found = groups.find(calls[index].values.at(0));
            if (found == groups.end()) continue;
            for (const auto* chunk : found->second) {
                if (chunk->address <= wait.address && wait.address + 4 <= chunk->address + chunk->bytes) return index;
            }
        }
        return calls.size();
    }

    void releaseAwaited(std::span<const FrameCall> calls, std::size_t from) {
        std::array<AgcDriver::AwaitedWait, 64> awaited{};
        const auto count = AgcDriverAwaitedWaits_nid_postfix(awaited.data(), awaited.size());
        if (options.exact) {
            std::array<std::pair<std::size_t, std::uint64_t>, 64> keys{};
            std::array<std::size_t, 64> order{};
            for (std::size_t slot = 0; slot < count; ++slot) {
                keys[slot] = {awaited[slot].address == 0 ? calls.size() + 1 : recordedIndex(calls, from, awaited[slot]), awaited[slot].address};
                order[slot] = slot;
            }
            std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(count), [&](std::size_t left, std::size_t right) { return keys[left] < keys[right]; });
            std::array<AgcDriver::AwaitedWait, 64> sorted{};
            for (std::size_t slot = 0; slot < count; ++slot) sorted[slot] = awaited[order[slot]];
            awaited = sorted;
        }
        bool released = false;
        for (std::size_t slot = 0; slot < count && !released; ++slot) {
            const auto& wait = awaited[slot];
            if (wait.address == 0) continue;
            const std::size_t bytes = wait.bytes == 8 ? 8 : 4;
            if (wait.function != 0 && satisfies(wait, load(wait.address, bytes))) continue;
            if (rewindWait(wait)) {
                for (auto index = from; index < calls.size() && !released; ++index) released = releaseRewind(calls[index], wait);
                if (released) ++earlyReleases;
                continue;
            }
            for (auto index = std::min(from, calls.size()); index > 0 && !released; --index) {
                released = writeRecordedValue(calls[index - 1], wait);
                if (released) ++lateReleases;
            }
            if (!released && options.synthesize) {
                if (const auto value = satisfying(wait, load(wait.address, bytes))) {
                    const auto before = load(wait.address, bytes);
                    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(wait.address)), &*value, bytes);
                    if (syntheticReleases.load() < 64) {
                        for (std::size_t other = 0; other < count; ++other) {
                            const auto& waiting = awaited[other];
                            if (waiting.address == 0) continue;
                            std::fprintf(stderr, "[frame-replay]   awaited 0x%llx == 0x%llx (function %u) holds 0x%llx\n", static_cast<unsigned long long>(waiting.address), static_cast<unsigned long long>(waiting.reference), waiting.function, static_cast<unsigned long long>(load(waiting.address, waiting.bytes == 8 ? 8 : 4)));
                        }
                        const auto [accepted, completed] = progress();
                        std::fprintf(stderr, "[frame-replay] synthesized 0x%llx: 0x%llx -> 0x%llx (function %u, reference 0x%llx, mask 0x%llx) at call %zu, serials %llu/%llu\n", static_cast<unsigned long long>(wait.address),
                                     static_cast<unsigned long long>(before), static_cast<unsigned long long>(*value), wait.function, static_cast<unsigned long long>(wait.reference), static_cast<unsigned long long>(wait.mask), from,
                                     static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted));
                    }
                    ++syntheticReleases;
                    released = true;
                }
            }
            if (released) ++earlyReleases;
        }
    }

    void runWindow(std::span<const FrameCall> calls) {
        std::map<std::uint64_t, std::vector<std::size_t>> byThread;
        for (std::size_t index = 0; index < calls.size(); ++index) byThread[calls[index].thread].push_back(index);
        threads = byThread.size();
        std::mutex mutex;
        std::condition_variable turn;
        std::size_t started = 0;
        std::exception_ptr failure;
        std::atomic<bool> finished{false};
        sequenced.store(0);
        stalled.store(false);
        std::thread breaker([&] {
            if (!options.release) return;
            auto lastCompleted = progress().second;
            auto lastSequenced = sequenced.load();
            auto lastProgress = Clock::now();
            std::optional<Clock::time_point> parkedSince;
            const auto quiet = options.exact ? std::max<std::chrono::milliseconds>(options.releaseAfter, ExactQuiet * 4) : options.releaseAfter;
            while (!finished.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const auto completed = progress().second;
                const auto issued = sequenced.load();
                if (completed != lastCompleted || issued != lastSequenced || closingWindows.load() || !AgcDriverQuiescent_nid_postfix()) {
                    lastCompleted = completed;
                    lastSequenced = issued;
                    lastProgress = Clock::now();
                    parkedSince.reset();
                    parkedWithoutRelease.store(false);
                    continue;
                }
                if (options.exact) {
                    const auto now = Clock::now();
                    if (busy.load() == 0 && blockedOnWaits()) {
                        if (!parkedSince) parkedSince = now;
                        if (now - *parkedSince < ParkedQuiet) continue;
                        const auto before = earlyReleases.load();
                        releaseAwaited(calls, sequenced.load());
                        parkedWithoutRelease.store(earlyReleases.load() == before);
                        ++parkedReleases;
                        parkedSince.reset();
                        lastProgress = Clock::now();
                        continue;
                    }
                    parkedSince.reset();
                    if (now - lastProgress < quiet) continue;
                    ++fallbackReleases;
                    const auto [accepted, completed] = progress();
                    std::fprintf(stderr, "[frame-replay] --exact: no parked state for %lld ms (%d replay threads running, serials %llu/%llu): wall-clock fallback release at call %zu\n", static_cast<long long>(quiet.count()), busy.load(),
                                 static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted), sequenced.load());
                    releaseAwaited(calls, sequenced.load());
                    lastProgress = Clock::now();
                    continue;
                }
                const std::unique_lock lock(memoryMutex, std::try_to_lock);
                if (!lock.owns_lock()) {
                    lastProgress = Clock::now();
                    continue;
                }
                if (Clock::now() - lastProgress < quiet || !AgcDriverSettled_nid_postfix()) continue;
                releaseAwaited(calls, sequenced.load());
                lastProgress = Clock::now();
            }
        });
        std::vector<std::thread> workers;
        for (const auto& entry : byThread) {
            workers.emplace_back([&, &indices = entry.second] {
                for (const auto index : indices) {
                    std::unique_lock lock(mutex);
                    turn.wait(lock, [&] { return started == index || failure != nullptr; });
                    if (failure != nullptr) return;
                    const auto advance = [&] {
                        ++started;
                        sequenced.store(started);
                        lock.unlock();
                        turn.notify_all();
                    };
                    const auto kind = calls[index].kind;
                    // A frame's closing call too when frame windows are on: the next frame's calls
                    // start after its windows closed.
                    const bool ordered = kind == FrameCallKind::Memory || kind == FrameCallKind::RegisterVideoOutput || kind == FrameCallKind::UnregisterVideoOutput || (closeFrames && kind == boundary);
                    if (!ordered) advance();
                    struct Running {
                        std::atomic<int>& busy;
                        explicit Running(std::atomic<int>& counter) : busy(counter) { ++busy; }
                        ~Running() { --busy; }
                    };
                    try {
                        const Running running{busy};
                        execute(calls[index]);
                        if (ordered) advance();
                    } catch (...) {
                        if (!lock.owns_lock()) lock.lock();
                        if (failure == nullptr) failure = std::current_exception();
                        turn.notify_all();
                        return;
                    }
                }
            });
        }
        for (auto& worker : workers) worker.join();
        if (failure == nullptr) drain();
        finished.store(true);
        breaker.join();
        if (options.exact && (parkedReleases.load() != 0 || fallbackReleases.load() != 0)) std::fprintf(stderr, "[frame-replay] --exact releases: %llu while parked, %llu by the wall-clock fallback\n", static_cast<unsigned long long>(parkedReleases.load()), static_cast<unsigned long long>(fallbackReleases.load()));
        if (failure != nullptr) std::rethrow_exception(failure);
    }

    void drain() {
        auto lastCompleted = progress().second;
        auto lastReleases = earlyReleases.load();
        auto lastProgress = Clock::now();
        const auto timeout = options.exact ? std::max<std::chrono::milliseconds>(options.drainTimeout, std::chrono::seconds(60)) : options.drainTimeout;
        auto lastReport = lastProgress;
        while (true) {
            const auto [accepted, completed] = progress();
            unfinished = accepted - completed;
            if (unfinished == 0) return;
            const auto now = Clock::now();
            if (completed != lastCompleted || earlyReleases.load() != lastReleases) {
                lastCompleted = completed;
                lastReleases = earlyReleases.load();
                lastProgress = now;
            } else if (options.exact && parkedWithoutRelease.load()) {
                std::fprintf(stderr, "[frame-replay] %llu submissions stay parked on waits nothing in the window releases (serials %llu/%llu)\n", static_cast<unsigned long long>(unfinished),
                             static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted));
                return;
            } else if (now - lastReport >= options.drainTimeout) {
                lastReport = now;
                std::fprintf(stderr, "[frame-replay] draining: %llu submissions pending for %.0f s (serials %llu/%llu), blocked on%s\n", static_cast<unsigned long long>(unfinished),
                             std::chrono::duration<double>(now - lastProgress).count(), static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted), describeBlocked().c_str());
            }
            if (const auto quiet = now - lastProgress; quiet >= (options.exact ? timeout : options.busyTimeout) || (!options.exact && quiet >= timeout && AgcDriverSettled_nid_postfix())) {
                std::fprintf(stderr, "[frame-replay] %llu submissions did not complete after the last captured call (serials %llu/%llu), blocked on%s\n", static_cast<unsigned long long>(unfinished),
                             static_cast<unsigned long long>(completed), static_cast<unsigned long long>(accepted), describeBlocked().c_str());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    template <typename TFunction>
    void traced(const FrameCall& call, TFunction&& function) {
        if (options.traceSerials) {
            std::fprintf(stderr, "[frame-replay] call %llu (%s, thread %llu, queue 0x%x, %llu dwords at 0x%llx)\n", static_cast<unsigned long long>(call.ordinal), std::string(AgcDriver::FrameCallName(call.kind)).c_str(),
                         static_cast<unsigned long long>(call.thread), call.queue, static_cast<unsigned long long>(call.size), static_cast<unsigned long long>(call.address));
        }
        function();
    }

    template <typename TFunction>
    void registration(TFunction&& function) {
        try {
            function();
        } catch (const std::exception& error) {
            if (recording && options.strict) throw;
            ++(recording ? failedRegistrations : staleRegistrations);
            std::fprintf(stderr, "[frame-replay] %s registration failed: %s\n", recording ? "recorded" : "prologue", error.what());
        }
    }

    static const Shader* shader(std::uint64_t address) {
        return reinterpret_cast<const Shader*>(static_cast<std::uintptr_t>(address));
    }

    static AgcDriver::FlipInfo flipInfo(const FrameCall& call) {
        return {static_cast<std::uint32_t>(call.values.at(0)), static_cast<std::int32_t>(call.values.at(1)), static_cast<std::uint32_t>(call.values.at(2)), static_cast<std::int64_t>(call.values.at(3))};
    }
};

int usage() {
    std::fprintf(stderr, "usage: agc_frame_replay <capture directory> [--no-pace] [--pace-timeout-ms <n>] [--no-release] [--release-after-ms <n>] [--strict] [--exact] [--frame-limit <n>] [--timeout-s <n>] [--wall-windows]\n"
                         "                        [--no-bindless-prime] [--bindless-in <file>] [--bindless-out <file>] [--dedupe-prologue]\n"
                         "                        [--repeat <n>] [--cold-pipelines] [--shaders] [--shader-code <dir>] [--trace-serials] [--submits <n>]\n"
                         "                        [--dump <address>:<bytes>:<file>]...\n"
                         "  replays the driver calls and guest memory that APS5_FRAME_CAPTURE=<directory> recorded, through this\n"
                         "  build's AGC driver with no title running, and prints one JSON line with the frame times\n"
                         "  --no-pace           issues calls without waiting for the in-flight depth the title had\n"
                         "  --pace-timeout-ms   milliseconds a call waits for the captured in-flight depth once no queue runs (10000)\n"
                         "  --no-release        never copies recorded CPU stores early to release a GPU wait\n"
                         "  --release-after-ms  milliseconds without a completion or a newly issued call before awaited dwords are released (50; at least 200 with --exact)\n"
                         "  --no-synthesize     releases a wait only with a value the capture recorded, never with its reference value\n"
                         "  --strict            fails on a pacing timeout or a failed registration, and exits with 5 unless the JSON gaps list is empty\n"
                         "  --exact             waits for every earlier submission and its GPU work to complete before each submission, flip and\n"
                         "                      memory delta,\n"
                         "                      so the GPU work runs in one order (a reproducible baseline, slower than the title), and releases\n"
                         "                      awaited dwords when every replay thread is parked and the driver is blocked on waits, in capture order\n"
                         "  --frame-limit <n>   replays the calls through the n-th CPU flip, or the n-th suspend point when the title flips from the GPU,\n"
                         "                      and compares the display buffer with the title's image at that frame\n"
                         "  --checkpoint <csv>  replays up to the checkpoint of a capture made with APS5_FRAME_CAPTURE_CHECKPOINT=<frame> and lists\n"
                         "                      the 64 KiB blocks whose replayed bytes differ from the title's at that point; their replayed bytes go to <csv>.replay.bin\n"
                         "  --timeout-s <n>     exits with code 4 when the replay has no result after n seconds (900; 0 waits forever)\n"
                         "  --repeat <n>        replays the frames n times in this process, restoring the snapshot before each pass;\n"
                         "                      with n > 1 the frame times come from passes 2 to n, after the first warmed the caches\n"
                         "  --cold-pipelines    starts with an empty Vulkan pipeline cache instead of the title's pipeline cache file\n"
                         "  --shaders           replays nothing and maps no guest memory; lists each shader registration with its code\n"
                         "                      address, size and code hash as the snapshot and the captured writes before it hold them,\n"
                         "                      and how many registrations share that code\n"
                         "  --shader-code <dir> as --shaders, and writes each distinct code as <dir>/type<n>-<hash>.bin\n"
                         "  --trace-serials     prints each submission, flip and suspend point with its call ordinal and command\n"
                         "                      address as it is issued; with APS5_TRACE_GPU the driver prints the serial it accepts\n"
                         "                      for each address\n"
                         "  --submits <n>       replays nothing; lists the first n submissions and flips of the captured frames with\n"
                         "                      their queue and serials, the packets of each and the fields of its waits and releases\n"
                         "  --dump              writes guest memory to a file after the replay (hexadecimal or decimal)\n"
                         "  --frames <dir>      writes each recorded and replayed display buffer as reference-<n>.bmp and replay-<n>.bmp\n"
                         "  --wall-windows      the profile lines ([gputime], [draws], [mem], [command-mix], [resident-census]) close their windows on\n"
                         "                      the wall clock as in the game; by default each replayed frame is one window, closed at its flip\n"
                         "                      (frame-<n>; the replay waits there for the frame's GPU work), and every line prints once more at\n"
                         "                      the end (replay-end)\n"
                         "  --no-bindless-prime the bindless heap starts empty (default: primed with the tables the capture's BindlessTables record lists)\n"
                         "  --bindless-in <f>   primes the bindless heap with the tables in <f> (a first replay's --bindless-out) before the frames\n"
                         "  --bindless-out <f>  writes the bindless tables registered by the end of the first replayed frame to <f>\n"
                         "  --dedupe-prologue   skips prologue ABI resolutions identical to an earlier one with no registration or memory call between\n"
                         "  the shader cache's Vulkan pipeline cache file is loaded as the title loads it (--cold-pipelines starts empty)\n");
    return 2;
}

Dump parseDump(std::string_view text) {
    const auto first = text.find(':');
    const auto second = first == std::string_view::npos ? std::string_view::npos : text.find(':', first + 1);
    if (second == std::string_view::npos) throw std::invalid_argument("frame replay: --dump takes <address>:<bytes>:<file>");
    const auto number = [](std::string_view part) {
        const std::string value(part);
        char* end = nullptr;
        const auto parsed = std::strtoull(value.c_str(), &end, 0);
        if (value.empty() || end != value.c_str() + value.size()) throw std::invalid_argument("frame replay: '" + value + "' is not a number");
        return static_cast<std::uint64_t>(parsed);
    };
    return {number(text.substr(0, first)), number(text.substr(first + 1, second - first - 1)), std::filesystem::path(std::string(text.substr(second + 1)))};
}

// One write call per `chunk` bytes at most: a single ofstream::write of more than 2 GiB ends in the
// CRT's _write, whose count is 32-bit, and never returns.
constexpr std::uint64_t DumpChunkBytes = 256ull << 20u;

bool writeChunked(std::ostream& file, const std::byte* bytes, std::uint64_t size, std::uint64_t chunk = DumpChunkBytes) {
    for (std::uint64_t at = 0; at < size && file.good(); at += chunk) {
        const auto part = std::min(chunk, size - at);
        file.write(reinterpret_cast<const char*>(bytes + at), static_cast<std::streamsize>(part));
    }
    return file.good();
}

void writeDump(const Dump& dump) {
    auto* const pointer = reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(dump.address));
    GuestArena::GuestArenaPrepareHostRead_nid_no_patch(pointer, static_cast<std::size_t>(dump.bytes));
    std::ofstream file(dump.path, std::ios::binary | std::ios::trunc);
    if (!writeChunked(file, pointer, dump.bytes) || !file.flush().good()) throw std::runtime_error("frame replay: cannot write " + dump.path.string());
}

struct FrameCheck {
    std::size_t compared = 0;
    std::size_t missing = 0;
    double identical = 1.0;
    int maxDifference = 0;
};

void writeFrame(const std::filesystem::path& path, const AgcDriver::DisplayBuffer& buffer, std::span<const std::byte> pixels) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    const auto failure = file != nullptr ? AgcDriver::WriteBmpAndClose_nid_no_patch(file, buffer.width, buffer.height, pixels) : AgcDriver::FrameDumpIoFailure{"open", errno};
    if (failure) std::fprintf(stderr, "[frame-replay] cannot write %s (%s failed)\n", path.string().c_str(), failure.operation);
}

FrameCheck compareFrames(const FrameCaptureReader& reader, const Options& options) {
    FrameCheck check;
    if (!options.frames.empty()) std::filesystem::create_directories(options.frames);
    for (const auto& call : reader.Calls()) {
        if (call.kind != FrameCallKind::DisplayBuffer || call.values.size() < 9) continue;
        AgcDriver::DisplayBuffer buffer{};
        buffer.address = call.address;
        buffer.pixelFormat = call.values[1];
        buffer.width = static_cast<std::uint32_t>(call.values[2]);
        buffer.height = static_cast<std::uint32_t>(call.values[3]);
        buffer.tilingMode = static_cast<std::uint32_t>(call.values[4]);
        buffer.pitchInPixel = static_cast<std::uint32_t>(call.values[5]);
        buffer.dccAddress = call.values[6];
        buffer.dccClearColor = call.values[7];
        buffer.alignment = static_cast<std::uint32_t>(call.values[8]);
        const auto chunk = std::find_if(reader.Chunks().begin(), reader.Chunks().end(), [&](const FrameMemoryChunk& candidate) { return candidate.kind == FrameMemoryKind::Reference && candidate.group == call.values[0]; });
        if (chunk == reader.Chunks().end()) {
            ++check.missing;
            continue;
        }
        const auto reference = reader.Read(*chunk);
        const bool perFrame = call.values.size() >= 11;
        if (perFrame && !options.frames.empty()) writeFrame(options.frames / ("game-frame-" + std::to_string(call.values[9]) + ".bmp"), buffer, reference);
        if (perFrame != (options.frameLimit != 0) || (perFrame && call.values[9] != options.frameLimit)) continue;
        const auto replayed = AgcDriver::ReadDisplayBuffer(buffer);
        ++check.compared;
        std::size_t same = 0;
        const auto pixels = std::min(reference.size(), replayed.size()) / 4;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            bool equal = true;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const auto difference = std::abs(static_cast<int>(reference[pixel * 4 + channel]) - static_cast<int>(replayed[pixel * 4 + channel]));
                check.maxDifference = std::max(check.maxDifference, difference);
                equal = equal && difference == 0;
            }
            same += equal ? 1 : 0;
        }
        const auto total = std::max(reference.size(), replayed.size()) / 4;
        check.identical = std::min(check.identical, total == 0 ? 1.0 : static_cast<double>(same) / static_cast<double>(total));
        if (options.frames.empty()) continue;
        const auto index = std::to_string(call.values[0]);
        writeFrame(options.frames / ("reference-" + index + ".bmp"), buffer, reference);
        writeFrame(options.frames / ("replay-" + index + ".bmp"), buffer, replayed);
    }
    return check;
}

void compareCheckpoint(const FrameCaptureReader& reader, const std::filesystem::path& path, const Skipped& skipped, bool drained = true) {
    if (drained) {
        AgcDriverWaitIdle_nid_postfix();
        if (!AgcDriverSettle_nid_postfix()) std::fprintf(stderr, "[frame-replay] storage images were still pending at the checkpoint\n");
    } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        auto quietSince = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            if (!AgcDriverQuiescent_nid_postfix()) quietSince = std::chrono::steady_clock::now();
            else if (std::chrono::steady_clock::now() - quietSince >= std::chrono::milliseconds(50)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fprintf(stderr, "[frame-replay] checkpoint compared with submissions still parked on waits whose writers lie beyond the window (driver %s)\n", AgcDriverQuiescent_nid_postfix() ? "quiescent" : "still running after 60 s");
    }
    std::ofstream report(path, std::ios::trunc);
    auto blocksPath = path;
    blocksPath += ".replay.bin";
    std::ofstream replayed(blocksPath, std::ios::binary | std::ios::trunc);
    report << "address,bytes,differing_bytes,first_difference\n";
    std::uint64_t blocks = 0, differing = 0, differingBytes = 0;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind != FrameMemoryKind::Checkpoint || skipped.Overlaps(chunk)) continue;
        ++blocks;
        const auto expected = reader.Read(chunk);
        auto* const pointer = reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(chunk.address));
        GuestArena::GuestArenaPrepareHostRead_nid_no_patch(pointer, static_cast<std::size_t>(chunk.bytes));
        std::uint64_t count = 0;
        std::int64_t first = -1;
        for (std::size_t at = 0; at < expected.size(); ++at) {
            if (pointer[at] == expected[at]) continue;
            if (first < 0) first = static_cast<std::int64_t>(at);
            ++count;
        }
        if (count == 0) continue;
        ++differing;
        differingBytes += count;
        const std::array<std::uint64_t, 2> head{chunk.address, chunk.bytes};
        replayed.write(reinterpret_cast<const char*>(head.data()), sizeof(head));
        replayed.write(reinterpret_cast<const char*>(pointer), static_cast<std::streamsize>(chunk.bytes));
        char line[96];
        std::snprintf(line, sizeof(line), "0x%llx,%llu,%llu,%lld\n", static_cast<unsigned long long>(chunk.address), static_cast<unsigned long long>(chunk.bytes), static_cast<unsigned long long>(count), static_cast<long long>(first));
        report << line;
    }
    std::fprintf(stderr, "[frame-replay] checkpoint: %llu of %llu changed blocks differ from the title (%.1f MiB of bytes), listed in %s\n", static_cast<unsigned long long>(differing), static_cast<unsigned long long>(blocks), static_cast<double>(differingBytes) / (1024.0 * 1024.0), path.string().c_str());
}

#ifdef _WIN32
constexpr int RelaunchAttempts = 8;

int relaunchAttempt() {
    const char* const text = std::getenv("APS5_FRAME_REPLAY_ATTEMPT");
    if (text == nullptr) return 0;
    const std::string_view value = text;
    int attempt = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), attempt);
    if (error != std::errc{} || end != value.data() + value.size() || attempt < 0 || attempt > RelaunchAttempts)
        throw std::runtime_error("frame replay: APS5_FRAME_REPLAY_ATTEMPT must be a whole number from 0 to " + std::to_string(RelaunchAttempts) + ", not '" + std::string(value) + "'");
    return attempt;
}

void relaunchIfHostRegionBlocks(const FrameCaptureReader& reader) {
    const int attempt = relaunchAttempt() + 1;
    const auto& chunks = reader.Chunks();
    const auto blocked = std::find_if(chunks.begin(), chunks.end(), [](const FrameMemoryChunk& chunk) {
        return chunk.kind == FrameMemoryKind::Range && chunk.rangeKind != FrameRangeKind::Host && (chunk.access & FrameRangeAccess::Gpu) != 0 &&
               GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(static_cast<std::uintptr_t>(chunk.address), static_cast<std::size_t>(chunk.bytes));
    });
    if (blocked == chunks.end()) return;
    if (attempt > RelaunchAttempts) return;
    std::fprintf(stderr, "[frame-replay] a host region sits in the captured GPU range 0x%llx+0x%llx; relaunching (%d of %d)\n", static_cast<unsigned long long>(blocked->address),
                 static_cast<unsigned long long>(blocked->bytes), attempt, RelaunchAttempts);
    std::fflush(stdout);
    std::fflush(stderr);
    if (_putenv_s("APS5_FRAME_REPLAY_ATTEMPT", std::to_string(attempt).c_str()) != 0) throw std::runtime_error("frame replay: cannot pass the relaunch count to the next process");
    std::wstring commandLine = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) throw std::runtime_error("frame replay: cannot relaunch, error " + std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(process.hProcess, &code);
    std::_Exit(static_cast<int>(code));
}
#endif
std::uint64_t contentHash(const std::byte* bytes, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= std::to_integer<std::uint64_t>(bytes[index]);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

class CapturedMemory {
public:
    explicit CapturedMemory(const FrameCaptureReader& reader) : reader(reader) {
        for (const auto& chunk : reader.Chunks()) {
            if (chunk.kind == FrameMemoryKind::Snapshot) snapshots.push_back(&chunk);
            else if (chunk.kind == FrameMemoryKind::Range) ranges.push_back(&chunk);
        }
        const auto byAddress = [](const FrameMemoryChunk* a, const FrameMemoryChunk* b) { return a->address < b->address; };
        std::sort(snapshots.begin(), snapshots.end(), byAddress);
        std::sort(ranges.begin(), ranges.end(), byAddress);
    }

    std::optional<std::vector<std::byte>> Read(std::uint64_t address, std::uint64_t bytes) {
        if (bytes > std::numeric_limits<std::size_t>::max() || bytes > std::numeric_limits<std::uint64_t>::max() - address) return std::nullopt;
        std::vector<std::byte> result(static_cast<std::size_t>(bytes));
        for (auto at = address; at < address + bytes;) {
            const auto* range = covering(ranges, at);
            if (range == nullptr) return std::nullopt;
            const auto* snapshot = covering(snapshots, at);
            auto end = std::min(address + bytes, range->address + range->bytes);
            if (snapshot != nullptr) {
                end = std::min(end, snapshot->address + snapshot->bytes);
                const auto& data = load(*snapshot);
                std::memcpy(result.data() + (at - address), data.data() + (at - snapshot->address), static_cast<std::size_t>(end - at));
            } else {
                const auto next = std::upper_bound(snapshots.begin(), snapshots.end(), at, [](std::uint64_t value, const FrameMemoryChunk* chunk) { return value < chunk->address; });
                if (next != snapshots.end()) end = std::min(end, (*next)->address);
            }
            at = end;
        }
        for (const auto* delta : deltas) {
            const auto begin = std::max(address, delta->address);
            const auto end = std::min(address + bytes, delta->address + delta->bytes);
            if (begin >= end) continue;
            const auto data = reader.Read(*delta);
            std::memcpy(result.data() + (begin - address), data.data() + (begin - delta->address), static_cast<std::size_t>(end - begin));
        }
        return result;
    }

    void Apply(std::span<const FrameMemoryChunk* const> chunks) {
        deltas.insert(deltas.end(), chunks.begin(), chunks.end());
    }

private:
    static const FrameMemoryChunk* covering(const std::vector<const FrameMemoryChunk*>& chunks, std::uint64_t address) {
        const auto next = std::upper_bound(chunks.begin(), chunks.end(), address, [](std::uint64_t value, const FrameMemoryChunk* chunk) { return value < chunk->address; });
        if (next == chunks.begin()) return nullptr;
        const auto* chunk = *std::prev(next);
        return address < chunk->address + chunk->bytes ? chunk : nullptr;
    }

    const std::vector<std::byte>& load(const FrameMemoryChunk& chunk) {
        if (cached != &chunk) {
            data = reader.Read(chunk);
            cached = &chunk;
        }
        return data;
    }

    const FrameCaptureReader& reader;
    std::vector<const FrameMemoryChunk*> snapshots;
    std::vector<const FrameMemoryChunk*> ranges;
    std::vector<const FrameMemoryChunk*> deltas;
    const FrameMemoryChunk* cached = nullptr;
    std::vector<std::byte> data;
};

int listShaders(const Options& options) {
    const FrameCaptureReader reader(options.directory);
    CapturedMemory memory(reader);
    if (!options.shaderCode.empty()) std::filesystem::create_directories(options.shaderCode);
    std::map<std::uint64_t, std::vector<const FrameMemoryChunk*>> groups;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Delta) groups[chunk.group].push_back(&chunk);
    }
    const auto& calls = reader.Calls();
    const auto frameStart = std::find_if(calls.begin(), calls.end(), [](const FrameCall& call) { return call.kind == FrameCallKind::FrameStart; });
    std::map<std::uint64_t, std::size_t> copies;
    std::size_t listed = 0, inWindow = 0, unreadable = 0;
    for (auto call = calls.begin(); call != calls.end(); ++call) {
        if (call->kind == FrameCallKind::Memory) {
            if (const auto found = groups.find(call->values.at(0)); found != groups.end()) memory.Apply(found->second);
            continue;
        }
        if (call->kind != FrameCallKind::RegisterShader) continue;
        const auto header = memory.Read(call->address, sizeof(Shader));
        if (!header) {
            ++unreadable;
            continue;
        }
        Shader fields{};
        std::memcpy(&fields, header->data(), sizeof(Shader));
        const auto code = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(const_cast<const void*>(fields.code)));
        const auto bytes = fields.shader_size != 0 ? memory.Read(code, fields.shader_size) : std::nullopt;
        if (!bytes) {
            ++unreadable;
            continue;
        }
        const auto hash = contentHash(bytes->data(), bytes->size());
        const auto copy = ++copies[hash];
        if (copy == 1 && !options.shaderCode.empty()) {
            char name[64];
            std::snprintf(name, sizeof(name), "type%u-%016llx.bin", static_cast<unsigned>(fields.type), static_cast<unsigned long long>(hash));
            const auto path = options.shaderCode / name;
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) throw std::runtime_error("frame replay: cannot open " + path.string());
            file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            if (!file) throw std::runtime_error("frame replay: cannot write " + path.string());
            file.flush();
            if (!file) throw std::runtime_error("frame replay: cannot flush " + path.string());
            file.close();
            if (!file) throw std::runtime_error("frame replay: cannot close " + path.string());
        }
        const bool window = frameStart != calls.end() && call > frameStart;
        ++listed;
        inWindow += window ? 1 : 0;
        std::printf("{\"ordinal\":%llu,\"in_window\":%s,\"header\":%llu,\"code\":%llu,\"bytes\":%u,\"type\":%u,\"hash\":\"%016llx\",\"copy\":%zu}\n", static_cast<unsigned long long>(call->ordinal), window ? "true" : "false",
                    static_cast<unsigned long long>(call->address), static_cast<unsigned long long>(code), fields.shader_size, static_cast<unsigned>(fields.type), static_cast<unsigned long long>(hash), copy);
    }
    std::printf("{\"registrations\":%zu,\"in_window\":%zu,\"distinct_code\":%zu,\"unreadable\":%zu}\n", listed, inWindow, copies.size(), unreadable);
    std::fflush(stdout);
    return 0;
}

std::string_view packetName(std::uint32_t opcode) {
    switch (opcode) {
    case 0x10: return "NOP";
    case 0x15: return "DISPATCH_DIRECT";
    case 0x16: return "DISPATCH_INDIRECT";
    case 0x27: return "DRAW_INDEX_2";
    case 0x2d: return "DRAW_INDEX_AUTO";
    case 0x35: return "DRAW_INDEX_OFFSET_2";
    case 0x37: return "WRITE_DATA";
    case 0x3c: return "WAIT_REG_MEM";
    case 0x3f: return "INDIRECT_BUFFER";
    case 0x46: return "EVENT_WRITE";
    case 0x49: return "RELEASE_MEM";
    case 0x50: return "DMA_DATA";
    case 0x58: return "ACQUIRE_MEM";
    case 0x69: return "SET_CONTEXT_REG";
    case 0x76: return "SET_SH_REG";
    case 0x79: return "SET_UCONFIG_REG";
    default: return {};
    }
}

void describeSubmission(const FrameCall& call, CapturedMemory& memory) {
    const auto bytes = call.size <= std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint32_t) ? memory.Read(call.address, call.size * sizeof(std::uint32_t)) : std::nullopt;
    if (!bytes) {
        std::printf("    (command buffer not readable)\n");
        return;
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(call.size));
    if (!bytes->empty()) std::memcpy(words.data(), bytes->data(), bytes->size());
    std::map<std::string, std::size_t> counts;
    for (std::size_t at = 0; at < call.size;) {
        const auto header = words[at];
        const auto type = header >> 30u;
        const auto length = AgcDriver::Pm4::PacketWords(header);
        if (length > words.size() - at) {
            std::printf("    @%zu (truncated packet: %zu dwords, %zu available)\n", at, length, words.size() - at);
            break;
        }
        if (type == 3u && !AgcDriver::Pm4::FillerPacket(header)) {
            const auto opcode = (header >> 8u) & 0xffu;
            const auto name = packetName(opcode);
            char label[24];
            std::snprintf(label, sizeof(label), "0x%02x", opcode);
            const std::string key = name.empty() ? std::string(label) : std::string(name);
            const auto word = [&](std::size_t index) { return index < length ? words[at + index] : 0u; };
            if (opcode == 0x3c) {
                std::printf("    @%zu WAIT_REG_MEM function %u space %u address 0x%llx reference 0x%x mask 0x%x\n", at, word(1) & 7u, (word(1) >> 4u) & 3u,
                            static_cast<unsigned long long>(word(2)) | (static_cast<unsigned long long>(word(3)) << 32u), word(4), word(5));
            } else if (opcode == 0x49 || opcode == 0x37 || opcode == 0x46) {
                std::printf("    @%zu %s", at, key.c_str());
                for (std::size_t index = 1; index < length && index < 8; ++index) std::printf(" %08x", word(index));
                std::printf("\n");
            }
            ++counts[key];
        }
        at += length;
    }
    std::printf("    packets:");
    for (const auto& [name, count] : counts) std::printf(" %s x%zu", name.c_str(), count);
    std::printf("\n");
}

int listSubmits(const Options& options) {
    const FrameCaptureReader reader(options.directory);
    CapturedMemory memory(reader);
    std::map<std::uint64_t, std::vector<const FrameMemoryChunk*>> groups;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Delta) groups[chunk.group].push_back(&chunk);
    }
    const auto& calls = reader.Calls();
    const auto frameStart = std::find_if(calls.begin(), calls.end(), [](const FrameCall& call) { return call.kind == FrameCallKind::FrameStart; });
    std::size_t listed = 0;
    for (auto call = calls.begin(); call != calls.end() && listed < options.submits; ++call) {
        if (call->kind == FrameCallKind::Memory) {
            if (const auto found = groups.find(call->values.at(0)); found != groups.end()) memory.Apply(found->second);
            continue;
        }
        if (call < frameStart) continue;
        if (call->kind != FrameCallKind::Submit && call->kind != FrameCallKind::Flip && call->kind != FrameCallKind::ReserveEopFlip) continue;
        ++listed;
        std::printf("#%zu ordinal %llu %s thread %llu queue 0x%x serials %llu/%llu address 0x%llx dwords %llu\n", listed, static_cast<unsigned long long>(call->ordinal), std::string(AgcDriver::FrameCallName(call->kind)).c_str(),
                    static_cast<unsigned long long>(call->thread), call->queue, static_cast<unsigned long long>(call->completed), static_cast<unsigned long long>(call->accepted),
                    static_cast<unsigned long long>(call->address), static_cast<unsigned long long>(call->size));
        if (call->kind == FrameCallKind::Submit) describeSubmission(*call, memory);
    }
    std::fflush(stdout);
    return 0;
}

struct Pass {
    std::size_t firstFrame = 0;
    std::size_t frames = 0;
    double seconds = 0.0;
    std::uint64_t paceTimeouts = 0;
    std::uint64_t synthesized = 0;
};

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size())))];
}

int run(const Options& options) {
    const auto loadStart = Clock::now();
    const FrameCaptureReader reader(options.directory);
#ifdef _WIN32
    relaunchIfHostRegionBlocks(reader);
#endif
    if (reader.Truncated()) std::fprintf(stderr, "[frame-replay] the capture is truncated; replaying the complete records\n");
    auto skipped = restoreMemory(reader);
    const auto loaded = Clock::now();
    Replay replay{options, reader};
    replay.skipped = std::move(skipped);
    if (const auto at = reader.Header().find("\"gds_address\":"); at != std::string::npos) AgcDriverUseGdsAt_nid_postfix(std::strtoull(reader.Header().c_str() + at + 14, nullptr, 10));
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Delta) replay.groups[chunk.group].push_back(&chunk);
    }
    const auto& calls = reader.Calls();
    const auto display = std::find_if(calls.begin(), calls.end(), [](const FrameCall& call) { return call.kind == FrameCallKind::DisplayBuffer && call.values.size() >= 4; });
    if (display != calls.end()) replay.frames.SetSize(static_cast<std::uint32_t>(display->values[2]), static_cast<std::uint32_t>(display->values[3]));
    const auto frameStart = std::find_if(calls.begin(), calls.end(), [](const FrameCall& call) { return call.kind == FrameCallKind::FrameStart; });
    std::vector<const FrameMemoryChunk*> rewritten;
    const auto prologueStart = Clock::now();
    PrologueDedupe dedupe;
    for (auto call = calls.begin(); call != frameStart; ++call) {
        if (call->kind == FrameCallKind::Memory) {
            if (const auto found = replay.groups.find(call->values.at(0)); found != replay.groups.end()) rewritten.insert(rewritten.end(), found->second.begin(), found->second.end());
        }
        if (options.dedupePrologue && dedupe.Redundant(*call)) continue;
        replay.execute(*call);
    }
    if (!rewritten.empty()) restoreRewritten(reader, rewritten, replay.skipped);
    replay.primeBindless(replay.deferredBindless);
    if (!options.bindlessIn.empty()) {
        std::ifstream file(options.bindlessIn, std::ios::binary);
        std::vector<std::uint64_t> words;
        for (std::uint64_t word = 0; file.read(reinterpret_cast<char*>(&word), sizeof(word));) words.push_back(word);
        if (words.empty()) throw std::runtime_error("frame replay: no bindless tables in " + options.bindlessIn.string());
        replay.primeBindless(words);
    }
    if (replay.bindlessPrimed != 0) std::fprintf(stderr, "[frame-replay] bindless heap primed with %zu tables\n", replay.bindlessPrimed);
    const auto prologueSeconds = std::chrono::duration<double>(Clock::now() - prologueStart).count();
    const auto prologueSkipped = dedupe.Skipped();
    std::fprintf(stderr, "[frame-replay] prologue: %zu calls (%llu redundant resolutions skipped) in %.1f s, %zu shader images rewritten and restored, %llu stale registrations\n", static_cast<std::size_t>(frameStart - calls.begin()), static_cast<unsigned long long>(dedupe.Skipped()),
                 std::chrono::duration<double>(Clock::now() - prologueStart).count(), rewritten.size(), static_cast<unsigned long long>(replay.staleRegistrations.load()));
    if (options.frameWindows) AgcDriver::ProfileWindows::Close("prologue");
    const auto journalStart = Clock::now();
    AgcDriverWaitForPipelineJournal_nid_postfix();
    const auto journalSeconds = std::chrono::duration<double>(Clock::now() - journalStart).count();
    std::set<std::uint32_t> initialOutputs;
    for (const auto& [handle, output] : replay.outputs) initialOutputs.insert(handle);
    std::vector<Pass> passes;
    for (std::size_t pass = 0; pass < options.repeat && (pass == 0 || replay.unfinished == 0); ++pass) {
        if (pass > 0) {
            resetMemory(reader, replay.skipped);
            replay.pass = pass;
            replay.recording = false;
            for (auto output = replay.outputs.begin(); output != replay.outputs.end();) {
                if (initialOutputs.contains(output->first)) {
                    ++output;
                    continue;
                }
                AgcDriverUnregisterVideoOutput_nid_postfix(output->first, output->second);
                output = replay.outputs.erase(output);
            }
            for (const auto handle : initialOutputs) {
                if (replay.outputs.contains(handle)) continue;
                auto output = std::make_shared<ReplayOutput>(replay.frames);
                AgcDriverRegisterVideoOutput_nid_postfix(handle, output);
                replay.outputs.emplace(handle, std::move(output));
            }
            for (auto call = calls.begin(); call != frameStart; ++call) {
                if (call->kind == FrameCallKind::ReserveEopFlip) replay.execute(*call);
            }
        }
        Pass measured{replay.frames.Times().size(), 0, 0.0, replay.paceTimeouts.load(), replay.syntheticReleases.load()};
        replay.recording = true;
        replay.startedAt = Clock::now();
        if (frameStart != calls.end()) {
            auto window = std::span(calls).subspan(static_cast<std::size_t>(frameStart - calls.begin()) + 1);
            replay.boundary = std::any_of(window.begin(), window.end(), [](const FrameCall& call) { return call.kind == FrameCallKind::Flip; }) ? FrameCallKind::Flip : FrameCallKind::Suspend;
            replay.closeFrames = options.frameWindows;
            if (!options.checkpointReport.empty()) {
                const auto cut = std::find_if(window.begin(), window.end(), [](const FrameCall& call) { return call.kind == FrameCallKind::Checkpoint; });
                if (cut == window.end()) throw std::runtime_error("frame replay: --checkpoint needs a capture made with APS5_FRAME_CAPTURE_CHECKPOINT");
                window = window.first(static_cast<std::size_t>(cut - window.begin()));
            } else if (options.frameLimit != 0) {
                const bool cpuFlips = std::any_of(window.begin(), window.end(), [](const FrameCall& call) { return call.kind == FrameCallKind::Flip; });
                const auto boundary = cpuFlips ? FrameCallKind::Flip : FrameCallKind::Suspend;
                std::size_t boundaries = 0;
                const auto cut = std::find_if(window.begin(), window.end(), [&](const FrameCall& call) { return call.kind == boundary && ++boundaries == options.frameLimit; });
                if (cut != window.end()) window = window.first(static_cast<std::size_t>(cut - window.begin()) + 1);
            }
            replay.runWindow(window);
        }
        else AgcDriverWaitIdle_nid_postfix();
        replay.endedAt = Clock::now();
        if (const auto failure = replay.frames.Failure()) std::rethrow_exception(failure);
        measured.frames = replay.frames.Times().size() - measured.firstFrame;
        measured.seconds = std::chrono::duration<double>(replay.endedAt - replay.startedAt).count();
        measured.paceTimeouts = replay.paceTimeouts.load() - measured.paceTimeouts;
        measured.synthesized = replay.syntheticReleases.load() - measured.synthesized;
        passes.push_back(measured);
    }
    if (!options.checkpointReport.empty()) compareCheckpoint(reader, options.checkpointReport, replay.skipped, replay.unfinished == 0);
    FrameCheck check;
    if (replay.unfinished == 0) {
        if (!AgcDriverSettle_nid_postfix()) std::fprintf(stderr, "[frame-replay] storage images were still pending before the comparison\n");
        for (const auto& dump : options.dumps) writeDump(dump);
        check = compareFrames(reader, options);
    } else if (!options.dumps.empty()) {
        std::fprintf(stderr, "[frame-replay] no dumps written: the GPU work did not finish\n");
    }
    if (check.compared == 0) check.identical = 0.0;
    // The final flush: what the last windows hold (a frame cut by --checkpoint, the settle).
    AgcDriver::ProfileWindows::Close("replay-end");
    replay.writeBindless();
    std::string gaps;
    const auto gap = [&](const char* name, bool present) {
        if (present) gaps += (gaps.empty() ? "" : ",") + std::string(name);
    };
    gap("truncated", reader.Truncated());
    gap("capture-warnings", reader.Header().find("\"warnings\":[]") == std::string::npos);
    gap("undrained", replay.unfinished != 0);
    gap("skipped-memory", replay.skipped.bytes != 0);
    gap("stale-registrations", replay.staleRegistrations.load() != 0);
    gap("failed-registrations", replay.failedRegistrations.load() != 0);
    gap("pacing-timeouts", replay.paceTimeouts.load() != 0 || replay.memoryPaceTimeouts.load() != 0);
    gap("synthesized-waits", replay.syntheticReleases.load() != 0);
    gap("partial-window", options.frameLimit != 0 && check.compared == 0);
    gap("no-reference-frames", check.compared == 0 && options.dumps.empty());
    gap("missing-reference-frames", check.missing != 0);
    gap("pixels-differ", check.compared != 0 && check.identical < 1.0);
    const auto ready = replay.frames.Times();
    const std::size_t firstMeasured = passes.size() > 1 ? 1 : 0;
    std::vector<double> frameMs;
    std::string frameList;
    std::string passList;
    double seconds = 0.0;
    std::size_t frames = 0;
    for (std::size_t index = 0; index < passes.size(); ++index) {
        const auto& measured = passes[index];
        std::vector<double> passMs;
        for (auto frame = measured.firstFrame + 1; frame < measured.firstFrame + measured.frames; ++frame) passMs.push_back(std::chrono::duration<double, std::milli>(ready[frame] - ready[frame - 1]).count());
        char entry[192];
        std::snprintf(entry, sizeof(entry), "%s{\"replay_s\":%.3f,\"frames\":%zu,\"p50\":%.3f,\"pace_timeouts\":%llu,\"synthesized\":%llu}", passList.empty() ? "" : ",", measured.seconds, measured.frames,
                      percentile(passMs, 0.5), static_cast<unsigned long long>(measured.paceTimeouts), static_cast<unsigned long long>(measured.synthesized));
        passList += entry;
        if (index < firstMeasured) continue;
        for (const auto value : passMs) frameList += (frameList.empty() ? "" : ",") + std::to_string(static_cast<long long>(value));
        frameMs.insert(frameMs.end(), passMs.begin(), passMs.end());
        seconds += measured.seconds;
        frames += measured.frames;
    }
    const auto slowest = frameMs.empty() ? 0.0 : *std::max_element(frameMs.begin(), frameMs.end());
    std::printf("{\"frames\":%zu,\"replay_s\":%.3f,\"fps\":%.3f,\"frame_ms\":{\"p50\":%.3f,\"p90\":%.3f,\"max\":%.3f,\"each\":[%s]},\"passes\":[%s],\"kept_reservations\":%llu,\"submits\":%llu,\"delta_mib\":%.1f,\"park_restores\":%llu,\"pace_timeouts\":%llu,\"pace_wait_s\":%.3f,\"stale_registrations\":%llu,\"failed_registrations\":%llu,\"skipped_mib\":%.1f,\"threads\":%zu,\"released_waits\":%llu,\"of_them_from_earlier_writes\":%llu,\"of_them_synthesized\":%llu,\"unfinished\":%llu,\"frames_compared\":%zu,\"pixels_identical\":%.6f,\"max_channel_diff\":%d,\"load_s\":%.3f,\"journal_s\":%.3f,\"memory_pace_timeouts\":%llu,\"prologue_s\":%.3f,\"prologue_skipped\":%llu,\"frame_windows\":%zu,\"undrained_windows\":%zu,\"bindless_primed\":%zu,\"gaps\":\"%s\"}\n",
                frames, seconds, seconds > 0 ? static_cast<double>(frames) / seconds : 0.0, percentile(frameMs, 0.5), percentile(frameMs, 0.9), slowest, frameList.c_str(), passList.c_str(),
                static_cast<unsigned long long>(replay.keptReservations.load()),
                static_cast<unsigned long long>(replay.submits.load()), static_cast<double>(replay.deltaBytes.load()) / (1024.0 * 1024.0), static_cast<unsigned long long>(replay.parkRestores.load()), static_cast<unsigned long long>(replay.paceTimeouts.load()),
                replay.paceWaitedSeconds(), static_cast<unsigned long long>(replay.staleRegistrations.load()), static_cast<unsigned long long>(replay.failedRegistrations.load()),
                static_cast<double>(replay.skipped.bytes) / (1024.0 * 1024.0), replay.threads, static_cast<unsigned long long>(replay.earlyReleases.load()), static_cast<unsigned long long>(replay.lateReleases.load()),
                static_cast<unsigned long long>(replay.syntheticReleases.load()),
                static_cast<unsigned long long>(replay.unfinished), check.compared, check.identical, check.maxDifference, std::chrono::duration<double>(loaded - loadStart).count(), journalSeconds, static_cast<unsigned long long>(replay.memoryPaceTimeouts.load()), prologueSeconds, static_cast<unsigned long long>(prologueSkipped), replay.framesClosed.load(), replay.undrainedWindows.load(), replay.bindlessPrimed, gaps.c_str());
    std::fflush(stdout);
    if (options.strict && !gaps.empty()) {
        std::fprintf(stderr, "agc_frame_replay: --strict rejects this replay: %s\n", gaps.c_str());
        return 5;
    }
    return replay.unfinished == 0 ? 0 : 3;
}

void configureEnvironment(const Options& options) {
#ifdef _WIN32
    _putenv_s("APS5_FRAME_CAPTURE", "");
    _putenv_s("APS5_HEADLESS_PIPELINE_CACHE", options.coldPipelines ? "" : "1");
    if (options.coldPipelines) _putenv_s("APS5_PIPELINE_CACHE_LOAD", "");
#else
    unsetenv("APS5_FRAME_CAPTURE");
    if (options.coldPipelines) unsetenv("APS5_HEADLESS_PIPELINE_CACHE");
    else setenv("APS5_HEADLESS_PIPELINE_CACHE", "1", 1);
    if (options.coldPipelines) unsetenv("APS5_PIPELINE_CACHE_LOAD");
#endif
}

}

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--no-pace") options.pace = false;
        else if (argument == "--no-release") options.release = false;
        else if (argument == "--no-synthesize") options.synthesize = false;
        else if (argument == "--pace-timeout-ms" && index + 1 < argc) options.paceTimeout = std::chrono::milliseconds(std::strtoll(argv[++index], nullptr, 10));
        else if (argument == "--release-after-ms" && index + 1 < argc) options.releaseAfter = std::chrono::milliseconds(std::strtoll(argv[++index], nullptr, 10));
        else if (argument == "--strict") options.strict = true;
        else if (argument == "--exact") options.exact = true;
        else if (argument == "--checkpoint" && index + 1 < argc) options.checkpointReport = argv[++index];
        else if (argument == "--frame-limit" && index + 1 < argc) options.frameLimit = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
        else if (argument == "--timeout-s" && index + 1 < argc) options.timeout = std::chrono::seconds(std::strtoll(argv[++index], nullptr, 10));
        else if (argument == "--repeat" && index + 1 < argc) options.repeat = std::max<std::size_t>(1, std::strtoull(argv[++index], nullptr, 10));
        else if (argument == "--shaders") options.shaders = true;
        else if (argument == "--shader-code" && index + 1 < argc) {
            options.shaders = true;
            options.shaderCode = argv[++index];
        }
        else if (argument == "--submits" && index + 1 < argc) {
            const std::string_view value = argv[++index];
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), options.submits);
            if (error != std::errc{} || end != value.data() + value.size() || options.submits == 0) return usage();
        } else if (argument == "--trace-serials") options.traceSerials = true;
        else if (argument == "--cold-pipelines") options.coldPipelines = true;
        else if (argument == "--dump" && index + 1 < argc) options.dumps.push_back(parseDump(argv[++index]));
        else if (argument == "--frames" && index + 1 < argc) options.frames = argv[++index];
        else if (argument == "--wall-windows") options.frameWindows = false;
        else if (argument == "--host-regions") options.hostRegions = true;
        else if (argument == "--dedupe-prologue") options.dedupePrologue = true;
        else if (argument == "--no-bindless-prime") options.bindlessPrime = false;
        else if (argument == "--bindless-in" && index + 1 < argc) options.bindlessIn = argv[++index];
        else if (argument == "--bindless-out" && index + 1 < argc) options.bindlessOut = argv[++index];
        else if (!argument.starts_with("-") && options.directory.empty()) options.directory = argument;
        else return usage();
    }
#ifdef _WIN32
    if (options.hostRegions) return PrintHostRegions();
#endif
    if (options.directory.empty()) return usage();
    configureEnvironment(options);
    int code = 2;
    const bool listing = options.shaders || options.submits != 0;
    try {
        if (!listing) {
            startWatchdog(options.timeout);
            AgcDriver::ProfileWindows::UseFrameWindows(options.frameWindows);
        }
        code = options.shaders ? listShaders(options) : options.submits != 0 ? listSubmits(options) : run(options);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_frame_replay: %s\n", error.what());
    }
    if (!listing && code == 0) {
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "agc_frame_replay: %s\n", error.what());
            code = 2;
        }
        if (code == 0) return 0;
    }
    hardExit(code);
}
