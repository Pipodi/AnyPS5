#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_REGISTRATIONPREBUILDQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_REGISTRATIONPREBUILDQUEUE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace AgcDriver::DriverDetail {

class RegistrationPrebuildQueue {
public:
    struct Settings {
        bool waitForPresent = true;
        unsigned concurrency = 2;
        std::chrono::milliseconds fallback{120000};
        std::uint64_t budgetBytes = 0;
        std::chrono::milliseconds summary{10000};
    };

    struct Hooks {
        std::function<bool()> demandActive;
        std::function<std::uint64_t()> usedBytes;
        std::function<void(std::size_t, std::uint64_t, std::uint64_t)> deferred;
    };

    using Submitter = std::function<void(std::function<void()>)>;
    using ReleaseLog = std::function<void(std::size_t, const char*)>;

    static Settings SettingsFromEnvironment() {
        Settings settings;
        settings.concurrency = 0;
        if (const char* start = std::getenv("APS5_COMPUTE_REGISTRATION_START")) settings.waitForPresent = std::strcmp(start, "immediate") != 0;
        if (const char* delay = std::getenv("APS5_COMPUTE_REGISTRATION_FALLBACK_MS")) settings.fallback = std::chrono::milliseconds(std::strtoull(delay, nullptr, 10));
        if (const char* limit = std::getenv("APS5_COMPUTE_REGISTRATION_CONCURRENCY")) settings.concurrency = std::max(1u, static_cast<unsigned>(std::strtoul(limit, nullptr, 10)));
        return settings;
    }

    RegistrationPrebuildQueue(Settings settings, Submitter submit, ReleaseLog log = {}, Hooks hooks = {}) : settings(settings), submit(std::move(submit)), log(std::move(log)), hooks(std::move(hooks)), opened(!settings.waitForPresent) {}

    RegistrationPrebuildQueue(const RegistrationPrebuildQueue&) = delete;
    RegistrationPrebuildQueue& operator=(const RegistrationPrebuildQueue&) = delete;

    ~RegistrationPrebuildQueue() {
        Stop();
    }

    void Enqueue(std::uint64_t key, std::uint64_t cost, std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            pending.push_back({key, cost, std::move(task)});
            if (!opened.load() && !timer.joinable()) timer = std::thread([this] { Fallback(); });
        }
        Pump();
    }

    void Open(const char* reason) {
        if (opened.exchange(true)) return;
        std::size_t queued = 0;
        {
            std::lock_guard lock(mutex);
            queued = pending.size();
        }
        wake.notify_all();
        if (log) log(queued, reason);
        Pump();
    }

    bool Promote(std::uint64_t key) {
        std::lock_guard lock(mutex);
        const auto found = std::find_if(pending.begin(), pending.end(), [&](const Item& item) { return item.key == key; });
        if (found == pending.end()) return false;
        pending.erase(found);
        return true;
    }

    void Poke() {
        Pump();
    }

    bool Opened() const {
        return opened.load();
    }

    void Stop() {
        std::thread joined;
        {
            std::lock_guard lock(mutex);
            stopped = true;
            pending.clear();
            joined.swap(timer);
        }
        wake.notify_all();
        if (joined.joinable() && joined.get_id() != std::this_thread::get_id()) joined.join();
        else if (joined.joinable()) joined.detach();
    }

    std::size_t Queued() const {
        std::lock_guard lock(mutex);
        return pending.size();
    }

    unsigned PeakRunning() const {
        return peak.load();
    }

private:
    void Fallback() {
        std::unique_lock lock(mutex);
        const bool woken = wake.wait_for(lock, settings.fallback, [&] { return opened.load() || stopped; });
        lock.unlock();
        if (!woken) Open("fallback");
    }

    struct Item {
        std::uint64_t key = 0;
        std::uint64_t cost = 0;
        std::function<void()> task;
    };

    struct Summary {
        std::size_t queued = 0;
        std::uint64_t demand = 0;
        std::uint64_t memory = 0;
    };

    void Pump() {
        if (!opened.load()) return;
        for (;;) {
            Item item;
            std::optional<Summary> summary;
            bool started = false;
            {
                std::lock_guard lock(mutex);
                if (stopped || pending.empty() || (settings.concurrency != 0 && running >= settings.concurrency)) return;
                bool blocked = false;
                if (hooks.demandActive && hooks.demandActive()) {
                    ++deferredForDemand;
                    blocked = true;
                } else if (running != 0 && settings.budgetBytes != 0) {
                    const auto used = std::max(runningBytes, hooks.usedBytes ? hooks.usedBytes() : std::uint64_t{0});
                    if (used + pending.front().cost > settings.budgetBytes) {
                        ++deferredForMemory;
                        blocked = true;
                    }
                }
                if (blocked) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!summaryAt) summaryAt = now;
                    if (now - *summaryAt >= settings.summary) {
                        summary = Summary{pending.size(), deferredForDemand, deferredForMemory};
                        deferredForDemand = 0;
                        deferredForMemory = 0;
                        summaryAt = now;
                    }
                } else {
                    item = std::move(pending.front());
                    pending.pop_front();
                    ++running;
                    runningBytes += item.cost;
                    unsigned now = running;
                    unsigned seen = peak.load();
                    while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
                    started = true;
                }
            }
            if (summary && hooks.deferred) hooks.deferred(summary->queued, summary->demand, summary->memory);
            if (!started) return;
            submit([this, cost = item.cost, task = std::move(item.task)] {
                task();
                {
                    std::lock_guard lock(mutex);
                    --running;
                    runningBytes -= std::min(runningBytes, cost);
                }
                Pump();
            });
        }
    }

    Settings settings;
    Submitter submit;
    ReleaseLog log;
    Hooks hooks;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Item> pending;
    std::atomic<bool> opened;
    std::atomic<unsigned> peak{0};
    std::thread timer;
    unsigned running = 0;
    std::uint64_t runningBytes = 0;
    std::uint64_t deferredForDemand = 0;
    std::uint64_t deferredForMemory = 0;
    std::optional<std::chrono::steady_clock::time_point> summaryAt;
    bool stopped = false;
};

}

#endif
