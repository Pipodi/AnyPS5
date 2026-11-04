#include "prx/libSceAgcDriver/Execution/include/GpuWindowSampler.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver::GpuWindowSampler {

namespace {

std::atomic<std::uint64_t> displayFrame{0};

}

bool ParseWindows(const char* text, std::vector<std::pair<std::uint64_t, std::uint64_t>>& windows) {
    windows.clear();
    if (text == nullptr) return false;
    const char* cursor = text;
    while (*cursor != '\0') {
        char* end = nullptr;
        const auto from = std::strtoull(cursor, &end, 10);
        if (end == cursor || *end != ':') return false;
        cursor = end + 1;
        const auto to = std::strtoull(cursor, &end, 10);
        if (end == cursor || from > to) return false;
        windows.emplace_back(from, to);
        cursor = end;
        if (*cursor == ',' || *cursor == '+') ++cursor;
        else if (*cursor != '\0') return false;
    }
    return !windows.empty();
}

void NoteDisplayFrame(std::uint64_t frame) {
    displayFrame.store(frame, std::memory_order_relaxed);
}

#ifdef _WIN32

namespace {

constexpr int UnwindDepth = 12;
constexpr std::size_t LeafRows = 40;
constexpr std::size_t OtherRows = 10;
constexpr std::size_t ViaRows = 25;

struct Sample {
    std::uint64_t leaf;
    std::uint64_t via;
};

struct Entry {
    HANDLE handle = nullptr;
    std::string label;
    std::vector<Sample> samples;
    std::int64_t suspendedTicks = 0;
    std::int64_t longestTicks = 0;
};

struct Where {
    std::string name;
    std::uint64_t base = 0;
};

struct State {
    std::mutex mutex;
    std::map<std::uint64_t, Entry> entries;
    std::uint64_t nextId = 1;
    std::thread thread;
    std::atomic<bool> stop{false};
    bool running = false;
    Options options;
};

State& GetState() {
    static State* const state = new State;
    return *state;
}

std::uint64_t PerformanceFrequency() {
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<std::uint64_t>(value.QuadPart);
    }();
    return frequency;
}

int Unwind(CONTEXT& context, std::array<std::uint64_t, UnwindDepth>& frames) {
    int count = 0;
    while (count < UnwindDepth && context.Rip != 0) {
        frames[count++] = context.Rip;
        DWORD64 imageBase = 0;
        auto* const function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function == nullptr) break;
        PVOID handler = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handler, &establisher, nullptr);
    }
    return count;
}

const Where& Locate(std::uint64_t address, std::map<std::uint64_t, Where>& cache) {
    const auto page = address >> 12u;
    const auto found = cache.find(page);
    if (found != cache.end()) return found->second;
    Where where{"?", 0};
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* slash = std::strrchr(path, '\\');
        where.name = slash != nullptr ? slash + 1 : path;
        where.base = reinterpret_cast<std::uint64_t>(module);
    }
    return cache.emplace(page, std::move(where)).first->second;
}

bool Ours(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string extension = name.substr(dot);
    for (auto& character : extension) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return extension == ".prx" || extension == ".exe";
}

std::string Symbol(std::uint64_t address, std::map<std::uint64_t, Where>& cache) {
    const auto& where = Locate(address, cache);
    char text[320];
    std::snprintf(text, sizeof(text), "%s+0x%llx", where.name.c_str(), static_cast<unsigned long long>(address - where.base));
    return text;
}

struct Group {
    std::vector<Sample> samples;
    std::int64_t suspendedTicks = 0;
    std::int64_t longestTicks = 0;
};

void Emit(const Options& options, const std::string& line) {
    if (options.sink) options.sink(line);
    else std::fprintf(stderr, "%s\n", line.c_str());
}

void Report(const Options& options, std::uint64_t from, std::uint64_t to, double wallMs, std::map<std::string, Group>& groups) {
    std::map<std::uint64_t, Where> cache;
    for (auto& [label, group] : groups) {
        if (group.samples.empty()) continue;
        const double total = static_cast<double>(group.samples.size());
        const double tickUs = 1e6 / static_cast<double>(PerformanceFrequency());
        std::map<std::string, std::uint64_t> modules;
        std::map<std::uint64_t, std::uint64_t> ours;
        std::map<std::uint64_t, std::uint64_t> others;
        std::map<std::uint64_t, std::uint64_t> via;
        for (const auto& sample : group.samples) {
            const auto& where = Locate(sample.leaf, cache);
            ++modules[where.name];
            ++(Ours(where.name) ? ours : others)[sample.leaf];
            if (sample.via != 0) ++via[sample.via];
        }
        std::vector<std::pair<std::uint64_t, std::string>> ranked;
        for (const auto& [name, count] : modules) ranked.emplace_back(count, name);
        std::sort(ranked.rbegin(), ranked.rend());
        std::string shares;
        for (std::size_t i = 0; i < ranked.size() && i < 8; ++i) {
            char text[160];
            std::snprintf(text, sizeof(text), " %s %.1f%%", ranked[i].second.c_str(), 100.0 * static_cast<double>(ranked[i].first) / total);
            shares += text;
        }
        char head[512];
        std::snprintf(head, sizeof(head), "[sample] f%llu:%llu %s: %llu samples in %.0f ms (suspended %.1f us mean, %.1f us max, %.2f%% of the thread's time); modules:%s",
            static_cast<unsigned long long>(from), static_cast<unsigned long long>(to), label.c_str(), static_cast<unsigned long long>(group.samples.size()), wallMs,
            static_cast<double>(group.suspendedTicks) * tickUs / total, static_cast<double>(group.longestTicks) * tickUs,
            wallMs > 0 ? 100.0 * static_cast<double>(group.suspendedTicks) * tickUs / (wallMs * 1000.0) : 0.0, shares.c_str());
        Emit(options, head);
        const auto table = [&](const char* kind, const std::map<std::uint64_t, std::uint64_t>& counts, std::size_t rows) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> top;
            for (const auto& [address, count] : counts) top.emplace_back(count, address);
            std::sort(top.rbegin(), top.rend());
            for (std::size_t i = 0; i < top.size() && i < rows; ++i) {
                char line[480];
                std::snprintf(line, sizeof(line), "[sample] f%llu:%llu %s %s %s %llu %.1f%%", static_cast<unsigned long long>(from), static_cast<unsigned long long>(to), label.c_str(), kind,
                    Symbol(top[i].second, cache).c_str(), static_cast<unsigned long long>(top[i].first), 100.0 * static_cast<double>(top[i].first) / total);
                Emit(options, line);
            }
        };
        table("leaf", ours, LeafRows);
        table("leaf other", others, OtherRows);
        table("via", via, ViaRows);
    }
}

void SampleRound(State& state) {
    std::map<std::uint64_t, Where> cache;
    std::lock_guard lock(state.mutex);
    for (auto& [id, entry] : state.entries) {
        if (entry.handle == nullptr) continue;
        alignas(16) CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        std::array<std::uint64_t, UnwindDepth> frames{};
        int count = 0;
        LARGE_INTEGER suspended{};
        LARGE_INTEGER resumed{};
        QueryPerformanceCounter(&suspended);
        if (SuspendThread(entry.handle) == static_cast<DWORD>(-1)) continue;
        if (GetThreadContext(entry.handle, &context) != 0) count = Unwind(context, frames);
        ResumeThread(entry.handle);
        QueryPerformanceCounter(&resumed);
        const auto ticks = resumed.QuadPart - suspended.QuadPart;
        entry.suspendedTicks += ticks;
        entry.longestTicks = std::max<std::int64_t>(entry.longestTicks, ticks);
        if (count == 0) continue;
        Sample sample{frames[0], 0};
        if (!Ours(Locate(sample.leaf, cache).name)) {
            for (int i = 1; i < count; ++i) {
                if (Ours(Locate(frames[i], cache).name)) {
                    sample.via = frames[i];
                    break;
                }
            }
        }
        entry.samples.push_back(sample);
    }
}

void BeginWindow(State& state) {
    std::lock_guard lock(state.mutex);
    for (auto& [id, entry] : state.entries) {
        entry.samples.clear();
        entry.suspendedTicks = 0;
        entry.longestTicks = 0;
    }
}

void EndWindow(State& state, std::uint64_t from, std::uint64_t to, double wallMs) {
    std::map<std::string, Group> groups;
    {
        std::lock_guard lock(state.mutex);
        for (auto it = state.entries.begin(); it != state.entries.end();) {
            auto& entry = it->second;
            auto& group = groups[entry.label];
            group.samples.insert(group.samples.end(), entry.samples.begin(), entry.samples.end());
            group.suspendedTicks += entry.suspendedTicks;
            group.longestTicks = std::max(group.longestTicks, entry.longestTicks);
            entry.samples.clear();
            entry.suspendedTicks = 0;
            entry.longestTicks = 0;
            it = entry.handle == nullptr ? state.entries.erase(it) : std::next(it);
        }
    }
    Report(state.options, from, to, wallMs, groups);
}

void Pause(HANDLE timer, double ms) {
    if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

void Run(State& state) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto& windows = state.options.windows;
    std::vector<bool> done(windows.size(), false);
    int current = -1;
    auto startedAt = std::chrono::steady_clock::now();
    const auto finish = [&] {
        done[current] = true;
        EndWindow(state, windows[current].first, windows[current].second, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count());
    };
    while (!state.stop.load(std::memory_order_acquire)) {
        const auto frame = displayFrame.load(std::memory_order_relaxed);
        int next = -1;
        for (std::size_t i = 0; i < windows.size(); ++i) {
            if (!done[i] && frame >= windows[i].first && frame <= windows[i].second) {
                next = static_cast<int>(i);
                break;
            }
        }
        if (next != current) {
            if (current >= 0) finish();
            current = next;
            if (current >= 0) {
                BeginWindow(state);
                startedAt = std::chrono::steady_clock::now();
            }
        }
        if (current >= 0) SampleRound(state);
        Pause(timer, current >= 0 ? state.options.intervalMs : 5.0);
    }
    if (current >= 0) finish();
    if (timer != nullptr) CloseHandle(timer);
}

}

void Start(Options options) {
    auto& state = GetState();
    if (state.running || options.windows.empty() || options.intervalMs <= 0.0) return;
    state.options = std::move(options);
    state.stop.store(false, std::memory_order_release);
    state.running = true;
    state.thread = std::thread([&state] { Run(state); });
}

void Stop() {
    auto& state = GetState();
    if (!state.running) return;
    state.stop.store(true, std::memory_order_release);
    if (state.thread.joinable()) state.thread.join();
    state.running = false;
}

ThreadScope::ThreadScope(const std::string& label, bool active) {
    if (!active) return;
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) return;
    auto& state = GetState();
    std::lock_guard lock(state.mutex);
    id = state.nextId++;
    auto& entry = state.entries[id];
    entry.handle = handle;
    entry.label = label;
}

ThreadScope::~ThreadScope() {
    if (id == 0) return;
    auto& state = GetState();
    std::lock_guard lock(state.mutex);
    auto& entry = state.entries[id];
    CloseHandle(entry.handle);
    entry.handle = nullptr;
}

#else

void Start(Options) {}
void Stop() {}
ThreadScope::ThreadScope(const std::string&, bool) {}
ThreadScope::~ThreadScope() {}

#endif

}
