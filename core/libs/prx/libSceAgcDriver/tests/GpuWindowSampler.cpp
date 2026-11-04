#include "prx/libSceAgcDriver/Execution/include/GpuWindowSampler.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t Spin(std::uint64_t value) {
    volatile std::uint64_t state = value;
    for (int i = 0; i < 2000; ++i) state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state;
}

void ParsesWindows() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> windows;
    Check(AgcDriver::GpuWindowSampler::ParseWindows("1160:1210", windows) && windows.size() == 1 && windows[0].first == 1160 && windows[0].second == 1210, "single window");
    Check(AgcDriver::GpuWindowSampler::ParseWindows("864:1034,1160:1210", windows) && windows.size() == 2 && windows[1].first == 1160, "two windows");
    Check(!AgcDriver::GpuWindowSampler::ParseWindows("1210:1160", windows), "reversed window accepted");
    Check(!AgcDriver::GpuWindowSampler::ParseWindows("12", windows), "window without a colon accepted");
    Check(!AgcDriver::GpuWindowSampler::ParseWindows("1:2;3:4", windows), "stray separator accepted");
    Check(!AgcDriver::GpuWindowSampler::ParseWindows(nullptr, windows), "null accepted");
}

void StartsAndStops() {
#ifdef _WIN32
    std::mutex mutex;
    std::vector<std::string> lines;
    AgcDriver::GpuWindowSampler::Options options;
    options.intervalMs = 1.0;
    options.windows = {{5, 9}};
    options.sink = [&](const std::string& line) {
        std::lock_guard lock(mutex);
        lines.push_back(line);
    };
    AgcDriver::GpuWindowSampler::NoteDisplayFrame(1);
    AgcDriver::GpuWindowSampler::Start(options);
    std::atomic<bool> stop{false};
    std::atomic<bool> registered{false};
    std::atomic<std::uint64_t> iterations{0};
    std::thread busy([&] {
        const AgcDriver::GpuWindowSampler::ThreadScope scope("q7", true);
        registered.store(true);
        std::uint64_t value = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            value = Spin(value);
            iterations.fetch_add(1, std::memory_order_relaxed);
        }
        Check(value != 0, "spin collapsed");
    });
    while (!registered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(lines.empty(), "sampled outside the window");
    const auto before = iterations.load();
    AgcDriver::GpuWindowSampler::NoteDisplayFrame(6);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto during = iterations.load() - before;
    AgcDriver::GpuWindowSampler::NoteDisplayFrame(10);
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard lock(mutex);
            if (!lines.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop.store(true);
    busy.join();
    AgcDriver::GpuWindowSampler::Stop();
    Check(!lines.empty(), "no report at the end of the window");
    Check(lines.front().find("[sample] f5:9 q7: ") == 0, "report header changed");
    Check(lines.front().find("modules:") != std::string::npos, "module shares missing");
    bool leaf = false;
    for (const auto& line : lines) leaf = leaf || line.find(" q7 leaf ") != std::string::npos;
    Check(leaf, "no leaf rows");
    std::cout << lines.front() << "\n";
    std::cout << "iterations in 400 ms sampled: " << during << " (100 ms unsampled: " << before << ")\n";
    AgcDriver::GpuWindowSampler::Stop();
#endif
}

}

int main() {
    try {
        ParsesWindows();
        StartsAndStops();
    } catch (const std::exception& error) {
        std::cerr << "gpu window sampler: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
