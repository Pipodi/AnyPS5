#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr unsigned Threads = 8;
constexpr unsigned LowLimit = 2;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("compute low lane: " + message);
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned permits = 0;
    bool open = false;
    unsigned blockers = 0;
    unsigned lowRunning = 0;
    unsigned lowMost = 0;
    unsigned lowFinished = 0;
    unsigned normalStarted = 0;
    unsigned lowStarted = 0;
};

void Drain(const std::shared_ptr<Gate>& gate) {
    {
        std::lock_guard lock(gate->mutex);
        gate->open = true;
    }
    gate->changed.notify_all();
    AgcDriver::DriverDetail::AwaitLibraryPrebuilds();
}

struct DrainOnExit {
    std::shared_ptr<Gate> gate;
    ~DrainOnExit() {
        Drain(gate);
    }
};

std::function<void()> LowTask(const std::shared_ptr<Gate>& gate, bool block) {
    return [gate, block] {
        std::unique_lock lock(gate->mutex);
        ++gate->lowStarted;
        ++gate->lowRunning;
        gate->lowMost = std::max(gate->lowMost, gate->lowRunning);
        gate->changed.notify_all();
        if (block) gate->changed.wait(lock, [&] { return gate->open; });
        --gate->lowRunning;
        ++gate->lowFinished;
        gate->changed.notify_all();
    };
}

void CheckLowTaskStartsWhenNothingElseIsQueued() {
    const auto gate = std::make_shared<Gate>();
    const DrainOnExit release{gate};
    AgcDriver::DriverDetail::SubmitLowComputePrebuild(LowTask(gate, false));
    std::unique_lock lock(gate->mutex);
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->lowFinished == 1; }), "a low task did not run on an idle pool");
}

void CheckLowTasksAreLimited() {
    const auto gate = std::make_shared<Gate>();
    const DrainOnExit release{gate};
    for (unsigned index = 0; index < Threads; ++index) AgcDriver::DriverDetail::SubmitLowComputePrebuild(LowTask(gate, true));
    std::unique_lock lock(gate->mutex);
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->lowRunning == LowLimit; }), std::to_string(gate->lowRunning) + " low tasks started, not " + std::to_string(LowLimit));
    gate->changed.wait_for(lock, std::chrono::milliseconds(300), [&] { return gate->lowRunning > LowLimit; });
    Require(gate->lowMost == LowLimit, std::to_string(gate->lowMost) + " low tasks ran at once, not " + std::to_string(LowLimit));
    gate->open = true;
    gate->changed.notify_all();
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->lowFinished == Threads; }), "only " + std::to_string(gate->lowFinished) + " of " + std::to_string(Threads) + " low tasks finished after they were released");
    Require(gate->lowMost == LowLimit, std::to_string(gate->lowMost) + " low tasks ran at once while the queue drained, not " + std::to_string(LowLimit));
}

void CheckLowTaskWaitsForQueuedNormalTask() {
    const auto gate = std::make_shared<Gate>();
    const DrainOnExit release{gate};
    for (unsigned index = 0; index < Threads; ++index) {
        AgcDriver::DriverDetail::SubmitLibraryPrebuild([gate] {
            std::unique_lock lock(gate->mutex);
            ++gate->blockers;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->permits > 0 || gate->open; });
            if (gate->permits > 0) --gate->permits;
        });
    }
    std::unique_lock lock(gate->mutex);
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->blockers == Threads; }), "only " + std::to_string(gate->blockers) + " of " + std::to_string(Threads) + " blocking prebuilds started");
    lock.unlock();
    AgcDriver::DriverDetail::SubmitLowComputePrebuild(LowTask(gate, true));
    AgcDriver::DriverDetail::SubmitLibraryPrebuild([gate] {
        std::unique_lock held(gate->mutex);
        ++gate->normalStarted;
        gate->changed.notify_all();
        gate->changed.wait(held, [&] { return gate->open; });
    });
    lock.lock();
    gate->permits = 1;
    gate->changed.notify_all();
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->normalStarted + gate->lowStarted > 0; }), "no queued task started after a thread was freed");
    Require(gate->normalStarted == 1 && gate->lowStarted == 0, "a low task queued before a normal task took the freed thread");
    gate->changed.wait_for(lock, std::chrono::milliseconds(300), [&] { return gate->lowStarted > 0; });
    Require(gate->lowStarted == 0, "a low task started while every thread was busy");
    gate->permits = 1;
    gate->changed.notify_all();
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->lowStarted == 1; }), "a low task did not start once a thread was free and no normal task was queued");
}

}

int main() {
    try {
        AgcDriver::DriverDetail::Driver::Get();
        CheckLowTaskStartsWhenNothingElseIsQueued();
        CheckLowTasksAreLimited();
        CheckLowTaskWaitsForQueuedNormalTask();
        AgcDriver::DriverDetail::CloseRegistrationPreparation();
        std::puts("compute low lane tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
