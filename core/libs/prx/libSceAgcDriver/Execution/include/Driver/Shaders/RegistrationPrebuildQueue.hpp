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
        bool idleGate = false;
        bool newestFirst = false;
        std::chrono::milliseconds demandQuiet{0};
        std::chrono::milliseconds stallLift{0};
        std::uint64_t liftOnlyWords = 0;
        bool liftGate = false;
        std::chrono::milliseconds batch{2000};
        unsigned liftBatches = 0;
    };

    struct Hooks {
        std::function<bool()> demandActive;
        std::function<std::uint64_t()> usedBytes;
        std::function<void(std::size_t, std::uint64_t, std::uint64_t)> deferred;
        std::function<bool()> normalBusy;
        std::function<void(std::size_t, std::uint64_t, std::uint64_t, std::uint64_t)> held;
        std::function<std::chrono::milliseconds()> demandIdleFor;
        std::function<bool()> lifted;
        std::function<void(std::size_t, std::size_t)> liftSnapshot;
    };

    using Submitter = std::function<void(std::function<void()>)>;
    using ReleaseLog = std::function<void(std::size_t, const char*)>;

    static Settings SettingsFromEnvironment() {
        Settings settings;
        settings.concurrency = 0;
        if (const char* start = std::getenv("APS5_COMPUTE_REGISTRATION_START")) settings.waitForPresent = std::strcmp(start, "immediate") != 0;
        if (const char* delay = std::getenv("APS5_COMPUTE_REGISTRATION_FALLBACK_MS")) settings.fallback = std::chrono::milliseconds(std::strtoull(delay, nullptr, 10));
        if (const char* limit = std::getenv("APS5_COMPUTE_REGISTRATION_CONCURRENCY")) settings.concurrency = std::max(1u, static_cast<unsigned>(std::strtoul(limit, nullptr, 10)));
        if (const char* quiet = std::getenv("APS5_COMPUTE_REGISTRATION_DEMAND_QUIET_MS")) settings.demandQuiet = std::chrono::milliseconds(std::strtoull(quiet, nullptr, 10));
        if (const char* stall = std::getenv("APS5_COMPUTE_REGISTRATION_QUIET_LIFT_STALL_MS")) settings.stallLift = std::chrono::milliseconds(std::strtoull(stall, nullptr, 10));
        if (const char* words = std::getenv("APS5_COMPUTE_REGISTRATION_LIFT_ONLY_WORDS")) settings.liftOnlyWords = std::strtoull(words, nullptr, 10);
        return settings;
    }

    static unsigned LowThreads() {
        const char* text = std::getenv("APS5_COMPUTE_REGISTRATION_LOW_THREADS");
        return text != nullptr && *text != 0 ? std::max(1u, static_cast<unsigned>(std::strtoul(text, nullptr, 10))) : 2u;
    }

    static bool LowGateLift() {
        const char* gate = std::getenv("APS5_COMPUTE_REGISTRATION_LOW_GATE");
        return gate != nullptr && std::strcmp(gate, "lift") == 0;
    }

    static unsigned LowLiftBatches() {
        const char* text = std::getenv("APS5_COMPUTE_REGISTRATION_LOW_LIFT_BATCHES");
        return text != nullptr && *text != 0 ? static_cast<unsigned>(std::strtoul(text, nullptr, 10)) : 0u;
    }

    static Settings LowSettingsFromEnvironment() {
        Settings settings = SettingsFromEnvironment();
        const char* gate = std::getenv("APS5_COMPUTE_REGISTRATION_LOW_GATE");
        if (LowGateLift()) {
            settings.liftGate = true;
            settings.newestFirst = true;
            settings.demandQuiet = std::chrono::milliseconds(0);
            settings.concurrency = LowThreads();
            if (const char* batch = std::getenv("APS5_COMPUTE_REGISTRATION_LOW_BATCH_MS")) settings.batch = std::chrono::milliseconds(std::strtoull(batch, nullptr, 10));
            settings.liftBatches = LowLiftBatches();
            return settings;
        }
        if (gate != nullptr && std::strcmp(gate, "free") == 0) return settings;
        settings.idleGate = true;
        settings.newestFirst = true;
        settings.concurrency = LowThreads();
        return settings;
    }

    static unsigned FragmentThreads() {
        const char* text = std::getenv("APS5_FRAGMENT_PREBUILD_THREADS");
        return text != nullptr && *text != 0 ? std::max(1u, static_cast<unsigned>(std::strtoul(text, nullptr, 10))) : 2u;
    }

    static Settings FragmentSettings() {
        Settings settings;
        settings.waitForPresent = false;
        settings.idleGate = false;
        settings.newestFirst = true;
        settings.concurrency = FragmentThreads();
        return settings;
    }

    RegistrationPrebuildQueue(Settings settings, Submitter submit, ReleaseLog log = {}, Hooks hooks = {}) : settings(settings), submit(std::move(submit)), log(std::move(log)), hooks(std::move(hooks)), opened(!settings.waitForPresent) {}

    RegistrationPrebuildQueue(const RegistrationPrebuildQueue&) = delete;
    RegistrationPrebuildQueue& operator=(const RegistrationPrebuildQueue&) = delete;

    ~RegistrationPrebuildQueue() {
        Stop();
    }

    void Enqueue(std::uint64_t key, std::uint64_t cost, std::function<void()> task, std::uint64_t words = 0) {
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            pending.push_back({key, cost, std::move(task), words, std::chrono::steady_clock::now(), !snapshotTaken});
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

    bool Presented(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        if (settings.stallLift.count() == 0 || quietLifted.load()) return false;
        const auto ticks = now.time_since_epoch().count();
        const auto previous = lastPresent.exchange(ticks);
        if (previous == 0 || std::chrono::steady_clock::duration(ticks - previous) < settings.stallLift) return false;
        if (quietLifted.exchange(true)) return false;
        Pump();
        return true;
    }

    bool Busy() const {
        std::lock_guard lock(mutex);
        return !pending.empty() || running != 0;
    }

    bool QuietLifted() const {
        return quietLifted.load();
    }

    bool Opened() const {
        return opened.load();
    }

    void Stop() {
        std::thread joined[2];
        {
            std::lock_guard lock(mutex);
            stopped = true;
            pending.clear();
            joined[0].swap(timer);
            joined[1].swap(retry);
        }
        wake.notify_all();
        for (auto& thread : joined) {
            if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
            else if (thread.joinable()) thread.detach();
        }
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

    void ArmRetry(std::chrono::steady_clock::time_point at) {
        if (!retryAt || at < *retryAt) retryAt = at;
        if (!retry.joinable()) retry = std::thread([this] { RetryLoop(); });
        wake.notify_all();
    }

    void RetryLoop() {
        std::unique_lock lock(mutex);
        while (!stopped) {
            if (!retryAt) {
                wake.wait(lock);
                continue;
            }
            const auto at = *retryAt;
            if (wake.wait_until(lock, at, [&] { return stopped || !retryAt || *retryAt != at; })) continue;
            retryAt.reset();
            lock.unlock();
            Pump();
            lock.lock();
        }
    }

    struct Item {
        std::uint64_t key = 0;
        std::uint64_t cost = 0;
        std::function<void()> task;
        std::uint64_t words = 0;
        std::chrono::steady_clock::time_point stamp;
        bool eligible = true;
    };

    struct Summary {
        std::size_t queued = 0;
        std::uint64_t demand = 0;
        std::uint64_t memory = 0;
        std::uint64_t normal = 0;
    };

    void TakeLiftSnapshot() {
        if (!settings.liftGate || settings.liftBatches == 0 || !hooks.lifted || !hooks.lifted()) return;
        std::size_t released = 0;
        std::size_t held = 0;
        {
            std::lock_guard lock(mutex);
            if (stopped || snapshotTaken) return;
            snapshotTaken = true;
            unsigned batches = 0;
            for (std::size_t i = pending.size(); i-- > 0;) {
                if (i + 1 == pending.size() || pending[i + 1].stamp - pending[i].stamp > settings.batch) ++batches;
                pending[i].eligible = batches <= settings.liftBatches;
            }
            for (const auto& item : pending) {
                if (item.eligible) ++released;
                else ++held;
            }
        }
        if (hooks.liftSnapshot) hooks.liftSnapshot(released, held);
    }

    void Pump() {
        if (!opened.load()) return;
        TakeLiftSnapshot();
        for (;;) {
            Item item;
            std::optional<Summary> summary;
            bool started = false;
            {
                std::lock_guard lock(mutex);
                if (stopped || pending.empty() || (settings.concurrency != 0 && running >= settings.concurrency)) return;
                if (settings.liftGate && !(hooks.lifted && hooks.lifted())) return;
                bool blocked = false;
                const bool holding = settings.demandQuiet.count() != 0 && !quietLifted.load();
                const auto startable = [&](const Item& queued) { return !holding || settings.liftOnlyWords == 0 || queued.words < settings.liftOnlyWords; };
                auto chosen = settings.newestFirst ? std::prev(pending.end()) : pending.begin();
                if (settings.liftGate) {
                    std::size_t newest = pending.size();
                    while (newest != 0 && !pending[newest - 1].eligible) --newest;
                    if (newest == 0) return;
                    --newest;
                    std::size_t best = newest;
                    for (std::size_t i = newest; ; --i) {
                        if (pending[i].words > pending[best].words) best = i;
                        std::size_t before = i;
                        while (before != 0 && !pending[before - 1].eligible) --before;
                        if (before == 0 || pending[i].stamp - pending[before - 1].stamp > settings.batch) break;
                        i = before;
                    }
                    chosen = pending.begin() + static_cast<std::ptrdiff_t>(best);
                } else if (settings.newestFirst) {
                    const auto found = std::find_if(pending.rbegin(), pending.rend(), startable);
                    if (found != pending.rend()) chosen = std::prev(found.base());
                } else {
                    const auto found = std::find_if(pending.begin(), pending.end(), startable);
                    if (found != pending.end()) chosen = found;
                }
                const auto& candidate = *chosen;
                if (settings.idleGate && hooks.normalBusy && hooks.normalBusy()) {
                    ++deferredForNormal;
                    blocked = true;
                } else if (hooks.demandActive && hooks.demandActive()) {
                    ++deferredForDemand;
                    blocked = true;
                } else if (!startable(candidate)) {
                    ++deferredForDemand;
                    blocked = true;
                } else if (holding && hooks.demandIdleFor && hooks.demandIdleFor() < settings.demandQuiet) {
                    ++deferredForDemand;
                    blocked = true;
                    ArmRetry(std::chrono::steady_clock::now() + (settings.demandQuiet - hooks.demandIdleFor()));
                } else if (settings.idleGate && settings.budgetBytes != 0) {
                    const auto used = std::max(runningBytes, hooks.usedBytes ? hooks.usedBytes() : std::uint64_t{0});
                    if (used != 0 && used + candidate.cost > settings.budgetBytes / 2u) {
                        ++deferredForMemory;
                        blocked = true;
                    }
                } else if (running != 0 && settings.budgetBytes != 0) {
                    const auto used = std::max(runningBytes, hooks.usedBytes ? hooks.usedBytes() : std::uint64_t{0});
                    if (used + candidate.cost > settings.budgetBytes) {
                        ++deferredForMemory;
                        blocked = true;
                    }
                }
                if (blocked) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!summaryAt) summaryAt = now;
                    if (now - *summaryAt >= settings.summary) {
                        summary = Summary{pending.size(), deferredForDemand, deferredForMemory, deferredForNormal};
                        deferredForNormal = 0;
                        deferredForDemand = 0;
                        deferredForMemory = 0;
                        summaryAt = now;
                    }
                } else {
                    item = std::move(*chosen);
                    pending.erase(chosen);
                    ++running;
                    runningBytes += item.cost;
                    unsigned now = running;
                    unsigned seen = peak.load();
                    while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
                    started = true;
                }
            }
            if (summary && settings.idleGate && hooks.held) hooks.held(summary->queued, summary->normal, summary->demand, summary->memory);
            else if (summary && hooks.deferred) hooks.deferred(summary->queued, summary->demand, summary->memory);
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
    std::atomic<bool> quietLifted{false};
    std::atomic<std::chrono::steady_clock::rep> lastPresent{0};
    std::atomic<unsigned> peak{0};
    std::thread timer;
    std::thread retry;
    std::optional<std::chrono::steady_clock::time_point> retryAt;
    unsigned running = 0;
    std::uint64_t runningBytes = 0;
    std::uint64_t deferredForDemand = 0;
    std::uint64_t deferredForMemory = 0;
    std::uint64_t deferredForNormal = 0;
    std::optional<std::chrono::steady_clock::time_point> summaryAt;
    bool stopped = false;
    bool snapshotTaken = false;
};

}

#endif
