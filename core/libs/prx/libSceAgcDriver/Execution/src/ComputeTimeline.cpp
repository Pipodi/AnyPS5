#include "prx/libSceAgcDriver/Execution/include/ComputeTimeline.hpp"
#include "prx/libc/include/general/LogMacros.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace AgcDriver::ComputeTimeline {

namespace {

const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
thread_local bool prebuildThread = false;
thread_local CreateStats createStats;

#ifdef _WIN32
struct CoreClasses {
    std::vector<unsigned char> classes;
    unsigned performance = 0;
    unsigned efficiency = 0;
    unsigned char lowest = 0;
    bool hybrid = false;
};

const CoreClasses& Cores() {
    static const CoreClasses cores = [] {
        CoreClasses result;
        using QueryCpuSets = BOOL(WINAPI*)(void*, ULONG, PULONG, HANDLE, ULONG);
        const auto query = reinterpret_cast<QueryCpuSets>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemCpuSetInformation")));
        if (query == nullptr) return result;
        ULONG length = 0;
        query(nullptr, 0, &length, GetCurrentProcess(), 0);
        if (length == 0) return result;
        std::vector<unsigned char> buffer(length);
        if (!query(buffer.data(), length, &length, GetCurrentProcess(), 0)) return result;
        unsigned char lowest = 255;
        unsigned char highest = 0;
        for (ULONG offset = 0; offset + 20u <= length;) {
            const unsigned char* entry = buffer.data() + offset;
            ULONG size = 0;
            ULONG type = 0;
            std::memcpy(&size, entry, sizeof size);
            std::memcpy(&type, entry + 4, sizeof type);
            if (size == 0) break;
            if (type == 0) {
                WORD group = 0;
                std::memcpy(&group, entry + 12, sizeof group);
                const auto index = static_cast<std::size_t>(group) * 64u + entry[14];
                const auto efficiency = entry[18];
                if (result.classes.size() <= index) result.classes.resize(index + 1, 255);
                result.classes[index] = efficiency;
                lowest = std::min(lowest, efficiency);
                highest = std::max(highest, efficiency);
            }
            offset += size;
        }
        result.lowest = lowest;
        result.hybrid = highest > lowest;
        for (const auto value : result.classes) {
            if (value == 255) continue;
            if (result.hybrid && value == lowest) ++result.efficiency;
            else ++result.performance;
        }
        return result;
    }();
    return cores;
}

char ClassOf(const PROCESSOR_NUMBER& number) {
    const auto& cores = Cores();
    const auto index = static_cast<std::size_t>(number.Group) * 64u + number.Number;
    if (index >= cores.classes.size() || cores.classes[index] == 255) return '?';
    return cores.hybrid && cores.classes[index] == cores.lowest ? 'E' : 'P';
}

char CurrentClass() {
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    return ClassOf(number);
}

std::uint64_t Ticks(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
}

std::uint64_t ThreadTicks(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    return Ticks(kernel) + Ticks(user);
}

[[maybe_unused]] const bool announced = [] {
    if (!Enabled()) return false;
    const auto& cores = Cores();
    std::fprintf(stderr, "[cpu] logical processors: P=%u E=%u\n", cores.performance, cores.efficiency);
    return true;
}();
#endif

const char* Name(Event event) {
    switch (event) {
    case Event::Registered: return "registered";
    case Event::FirstDispatch: return "first dispatch";
    case Event::Lookahead: return "lookahead";
    case Event::PrebuildStart: return "prebuild start";
    }
    return "?";
}

}

bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_LOG_COMPUTE_TIMELINE");
        return text != nullptr && *text != 0 && *text != '0';
    }();
    return enabled;
}

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

bool Once(Event event, std::uint64_t address) {
    static std::mutex mutex;
    static std::set<std::pair<unsigned, std::uint64_t>> seen;
    std::lock_guard lock(mutex);
    return seen.emplace(static_cast<unsigned>(event), address).second;
}

void MarkPrebuildThread() {
    prebuildThread = true;
}

bool OnPrebuildThread() {
    return prebuildThread;
}

std::uint32_t ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<std::uint32_t>(syscall(SYS_gettid));
#endif
}

double ThreadCpuMs() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
    const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) return 0.0;
    return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1.0e6;
#endif
}

CreateStats& CurrentCreateStats() {
    return createStats;
}

void ResetCreateStats() {
    createStats = CreateStats{};
}

#ifdef _WIN32
struct CreateWatch::Impl {
    HANDLE thread = nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    std::thread worker;
    unsigned performance = 0;
    unsigned efficiency = 0;
    unsigned starved = 0;
    unsigned samples = 0;
    char startClass = '?';

    void Run() {
        auto lastWall = std::chrono::steady_clock::now();
        auto lastTicks = ThreadTicks(thread);
        std::unique_lock lock(mutex);
        while (!wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop; })) {
            const auto now = std::chrono::steady_clock::now();
            const auto ticks = ThreadTicks(thread);
            const double wallMs = std::chrono::duration<double, std::milli>(now - lastWall).count();
            const double cpuMs = static_cast<double>(ticks - lastTicks) / 10000.0;
            lastWall = now;
            lastTicks = ticks;
            PROCESSOR_NUMBER ideal{};
            if (GetThreadIdealProcessorEx(thread, &ideal)) {
                if (ClassOf(ideal) == 'E') ++efficiency;
                else ++performance;
            }
            if (cpuMs < wallMs * 0.5) ++starved;
            ++samples;
        }
    }
};

CreateWatch::CreateWatch(bool active) {
    if (!active) return;
    impl = new Impl;
    impl->startClass = CurrentClass();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &impl->thread, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        delete impl;
        impl = nullptr;
        return;
    }
    impl->worker = std::thread([impl = impl] { impl->Run(); });
}

void CreateWatch::Finish() {
    if (impl == nullptr) return;
    {
        std::lock_guard lock(impl->mutex);
        impl->stop = true;
    }
    impl->wake.notify_all();
    impl->worker.join();
    createStats.idealPerformance += impl->performance;
    createStats.idealEfficiency += impl->efficiency;
    createStats.starved += impl->starved;
    createStats.samples += impl->samples;
    createStats.startClass = impl->startClass;
    createStats.endClass = CurrentClass();
    CloseHandle(impl->thread);
    delete impl;
    impl = nullptr;
}
#else
struct CreateWatch::Impl {};

CreateWatch::CreateWatch(bool) {}

void CreateWatch::Finish() {}
#endif

CreateWatch::~CreateWatch() {
    Finish();
}

void Mark(Event event, std::uint64_t address, const char* pipeline) {
    if (!Enabled() || !Once(event, address)) return;
    if (pipeline != nullptr) WriteLogLine(stderr, "[compute-timeline] %s program 0x%llx t=%.0f pipeline=%s\n", Name(event), static_cast<unsigned long long>(address), NowMs(), pipeline);
    else WriteLogLine(stderr, "[compute-timeline] %s program 0x%llx t=%.0f\n", Name(event), static_cast<unsigned long long>(address), NowMs());
}

int CompilePriority() {
    static const int priority = [] {
        const char* text = std::getenv("APS5_COMPILE_THREAD_PRIORITY");
        if (text == nullptr || *text == 0) return 0x7fffffff;
        return static_cast<int>(std::strtol(text, nullptr, 10));
    }();
    return priority;
}

void ApplyCompilePriority() {
#ifdef _WIN32
    if (CompilePriority() != 0x7fffffff) SetThreadPriority(GetCurrentThread(), CompilePriority());
#endif
}

CompilePriorityScope::CompilePriorityScope() {
#ifdef _WIN32
    if (CompilePriority() == 0x7fffffff) return;
    const auto current = GetThreadPriority(GetCurrentThread());
    if (current == THREAD_PRIORITY_ERROR_RETURN || current == CompilePriority()) return;
    if (SetThreadPriority(GetCurrentThread(), CompilePriority())) {
        previous = current;
        raised = true;
    }
#endif
}

CompilePriorityScope::~CompilePriorityScope() {
#ifdef _WIN32
    if (raised) SetThreadPriority(GetCurrentThread(), previous);
#endif
}

}
