#include "prx/libSceAgcDriver/Execution/include/ComputeTimeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
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

void Mark(Event event, std::uint64_t address) {
    if (!Enabled() || !Once(event, address)) return;
    std::fprintf(stderr, "[compute-timeline] %s program 0x%llx t=%.0f\n", Name(event), static_cast<unsigned long long>(address), NowMs());
}

}
