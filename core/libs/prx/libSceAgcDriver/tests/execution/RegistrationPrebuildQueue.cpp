#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/RegistrationPrebuildQueue.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::RegistrationPrebuildQueue;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("registration prebuild queue: " + message);
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

class Workers {
public:
    ~Workers() {
        Join();
    }

    void Run(std::function<void()> task) {
        std::lock_guard lock(mutex);
        ++submitted;
        threads.emplace_back(std::move(task));
    }

    void Join() {
        for (;;) {
            std::thread thread;
            {
                std::lock_guard lock(mutex);
                if (threads.empty()) return;
                thread = std::move(threads.back());
                threads.pop_back();
            }
            thread.join();
        }
    }

    std::size_t Submitted() {
        std::lock_guard lock(mutex);
        return submitted;
    }

private:
    std::mutex mutex;
    std::vector<std::thread> threads;
    std::size_t submitted = 0;
};

struct Counters {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    std::atomic<int> done{0};

    void Task() {
        const int now = ++running;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        --running;
        ++done;
    }
};

bool WaitFor(const std::atomic<int>& value, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (value.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return value.load() >= expected;
}

void NothingBeforePresentEverythingAfter() {
    Workers workers;
    Counters counters;
    std::size_t released = 0;
    {
        RegistrationPrebuildQueue queue({true, 2, std::chrono::hours(1)}, [&](std::function<void()> task) { workers.Run(std::move(task)); }, [&](std::size_t queued, const char*) { released = queued; });
        for (int index = 0; index < 9; ++index) queue.Enqueue([&] { counters.Task(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Require(workers.Submitted() == 0 && counters.done == 0, "a prebuild was submitted before the present signal");
        Require(queue.Queued() == 9, "queued prebuilds were lost before the present signal");
        queue.Open("present");
        Require(WaitFor(counters.done, 9), "queued prebuilds did not all run after the present signal");
        Require(released == 9, "the release reported a wrong queued count");
        Require(counters.peak <= 2, "more prebuilds ran at once than the cap");
        Require(counters.peak == 2, "the cap was never used");
        queue.Open("present");
        queue.Enqueue([&] { counters.Task(); });
        Require(WaitFor(counters.done, 10), "a prebuild registered after the release did not run");
        workers.Join();
        Require(queue.PeakRunning() == 2, "the queue's own peak differs from the observed one");
    }
}

void ImmediateStartsAtOnceWithinCap() {
    Workers workers;
    Counters counters;
    RegistrationPrebuildQueue queue({false, 3, std::chrono::hours(1)}, [&](std::function<void()> task) { workers.Run(std::move(task)); });
    for (int index = 0; index < 7; ++index) queue.Enqueue([&] { counters.Task(); });
    Require(WaitFor(counters.done, 7), "immediate prebuilds did not all run");
    Require(counters.peak <= 3, "immediate prebuilds exceeded the cap");
}

void FallbackReleasesWithoutPresent() {
    Workers workers;
    Counters counters;
    const char* reason = nullptr;
    RegistrationPrebuildQueue queue({true, 2, std::chrono::milliseconds(80)}, [&](std::function<void()> task) { workers.Run(std::move(task)); }, [&](std::size_t, const char* why) { reason = why; });
    for (int index = 0; index < 4; ++index) queue.Enqueue([&] { counters.Task(); });
    Require(WaitFor(counters.done, 4), "the time fallback did not release the queue");
    Require(reason != nullptr && std::string(reason) == "fallback", "the fallback release was not reported as such");
}

void StopDropsQueued() {
    Workers workers;
    Counters counters;
    RegistrationPrebuildQueue queue({true, 2, std::chrono::hours(1)}, [&](std::function<void()> task) { workers.Run(std::move(task)); });
    queue.Enqueue([&] { counters.Task(); });
    queue.Stop();
    queue.Open("present");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Require(counters.done == 0 && workers.Submitted() == 0, "a stopped queue ran a prebuild");
}

void EnvironmentSettings() {
    SetEnvironment("APS5_COMPUTE_REGISTRATION_START", "immediate");
    SetEnvironment("APS5_COMPUTE_REGISTRATION_CONCURRENCY", "5");
    auto settings = RegistrationPrebuildQueue::SettingsFromEnvironment();
    Require(!settings.waitForPresent && settings.concurrency == 5, "the environment was not read");
    SetEnvironment("APS5_COMPUTE_REGISTRATION_START", "present");
    SetEnvironment("APS5_COMPUTE_REGISTRATION_CONCURRENCY", "0");
    settings = RegistrationPrebuildQueue::SettingsFromEnvironment();
    Require(settings.waitForPresent && settings.concurrency == 1, "a zero cap was not raised to one");
}

}

int main() {
    try {
        NothingBeforePresentEverythingAfter();
        ImmediateStartsAtOnceWithinCap();
        FallbackReleasesWithoutPresent();
        StopDropsQueued();
        EnvironmentSettings();
        std::cout << "registration prebuild queue tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
