#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_REGISTRATIONPREBUILDQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_REGISTRATIONPREBUILDQUEUE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace AgcDriver::DriverDetail {

class RegistrationPrebuildQueue {
public:
    struct Settings {
        bool waitForPresent = true;
        unsigned concurrency = 2;
        std::chrono::milliseconds fallback{120000};
    };

    using Submitter = std::function<void(std::function<void()>)>;
    using ReleaseLog = std::function<void(std::size_t, const char*)>;

    static Settings SettingsFromEnvironment() {
        Settings settings;
        if (const char* start = std::getenv("APS5_COMPUTE_REGISTRATION_START")) settings.waitForPresent = std::strcmp(start, "immediate") != 0;
        if (const char* delay = std::getenv("APS5_COMPUTE_REGISTRATION_FALLBACK_MS")) settings.fallback = std::chrono::milliseconds(std::strtoull(delay, nullptr, 10));
        if (const char* limit = std::getenv("APS5_COMPUTE_REGISTRATION_CONCURRENCY")) settings.concurrency = std::max(1u, static_cast<unsigned>(std::strtoul(limit, nullptr, 10)));
        return settings;
    }

    RegistrationPrebuildQueue(Settings settings, Submitter submit, ReleaseLog log = {}) : settings(settings), submit(std::move(submit)), log(std::move(log)), opened(!settings.waitForPresent) {}

    RegistrationPrebuildQueue(const RegistrationPrebuildQueue&) = delete;
    RegistrationPrebuildQueue& operator=(const RegistrationPrebuildQueue&) = delete;

    ~RegistrationPrebuildQueue() {
        Stop();
    }

    void Enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            pending.push_back(std::move(task));
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

    void Pump() {
        if (!opened.load()) return;
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex);
                if (stopped || pending.empty() || running >= settings.concurrency) return;
                task = std::move(pending.front());
                pending.pop_front();
                ++running;
                unsigned now = running;
                unsigned seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
            }
            submit([this, task = std::move(task)] {
                task();
                {
                    std::lock_guard lock(mutex);
                    --running;
                }
                Pump();
            });
        }
    }

    Settings settings;
    Submitter submit;
    ReleaseLog log;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> pending;
    std::atomic<bool> opened;
    std::atomic<unsigned> peak{0};
    std::thread timer;
    unsigned running = 0;
    bool stopped = false;
};

}

#endif
