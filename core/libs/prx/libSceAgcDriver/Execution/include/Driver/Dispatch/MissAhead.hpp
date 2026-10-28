#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_MISSAHEAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_MISSAHEAD_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>

namespace AgcDriver::DriverDetail {

class MissAheadPlan {
public:
    explicit MissAheadPlan(std::size_t cap) : cap(cap) {}

    bool Visit() {
        if (visited >= cap) return false;
        ++visited;
        return true;
    }

    bool Admit(std::uint64_t key) { return keys.insert(key).second; }

    std::size_t Visited() const { return visited; }

private:
    std::size_t cap;
    std::size_t visited = 0;
    std::set<std::uint64_t> keys;
};

class MissAheadRange {
public:
    bool Covered(std::uint64_t submission, std::uint32_t queue, std::size_t from) const {
        return recorded && submission == lastSubmission && queue == lastQueue && from < lastEnd;
    }

    void Record(std::uint64_t submission, std::uint32_t queue, std::size_t end) {
        recorded = true;
        lastSubmission = submission;
        lastQueue = queue;
        lastEnd = end;
    }

private:
    bool recorded = false;
    std::uint64_t lastSubmission = 0;
    std::uint32_t lastQueue = 0;
    std::size_t lastEnd = 0;
};

class MissAheadLedger {
public:
    struct Totals {
        std::uint64_t started = 0, hit = 0, waited = 0;
        std::uint64_t Unused() const { return started - hit - waited; }
    };

    void Started(std::uint64_t key) {
        std::lock_guard lock(mutex);
        if (pending.size() >= Retained) pending.clear();
        pending[key] = false;
        ++totals.started;
        any.store(true, std::memory_order_relaxed);
    }

    void Finished(std::uint64_t key) {
        std::lock_guard lock(mutex);
        if (const auto found = pending.find(key); found != pending.end()) found->second = true;
    }

    bool Used(std::uint64_t key) {
        if (!any.load(std::memory_order_relaxed)) return false;
        std::lock_guard lock(mutex);
        const auto found = pending.find(key);
        if (found == pending.end()) return false;
        if (found->second) ++totals.hit;
        else ++totals.waited;
        pending.erase(found);
        return true;
    }

    bool Active() const { return any.load(std::memory_order_relaxed); }

    Totals Snapshot() const {
        std::lock_guard lock(mutex);
        return totals;
    }

    static constexpr std::size_t Retained = 4096;

private:
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, bool> pending;
    Totals totals;
    std::atomic<bool> any{false};
};

}

#endif
