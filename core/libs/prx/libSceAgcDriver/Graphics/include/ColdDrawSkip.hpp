#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COLDDRAWSKIP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COLDDRAWSKIP_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

enum class ColdSkipRefusal : std::uint8_t {
    None,
    MemoryWrites,
    DepthOnly,
    Stencil,
    Fullscreen,
    Indirect,
    LinearTarget,
    CountingSamples,
    ParkedFull,
    NoPrebuild,
    DepthCopy,
    StencilReplay,
    Count,
};

inline const char* ColdSkipRefusalName(ColdSkipRefusal refusal) {
    switch (refusal) {
    case ColdSkipRefusal::None: return "none";
    case ColdSkipRefusal::MemoryWrites: return "memory-writes";
    case ColdSkipRefusal::DepthOnly: return "depth-only";
    case ColdSkipRefusal::Stencil: return "stencil";
    case ColdSkipRefusal::Fullscreen: return "fullscreen";
    case ColdSkipRefusal::Indirect: return "indirect";
    case ColdSkipRefusal::LinearTarget: return "linear-target";
    case ColdSkipRefusal::CountingSamples: return "counting-samples";
    case ColdSkipRefusal::ParkedFull: return "parked-full";
    case ColdSkipRefusal::NoPrebuild: return "no-prebuild";
    case ColdSkipRefusal::DepthCopy: return "depth-copy";
    case ColdSkipRefusal::StencilReplay: return "stencil-replay";
    case ColdSkipRefusal::Count: break;
    }
    return "unknown";
}

struct ColdSkipAllow {
    bool depthOnly = false;
    bool stencil = false;
    bool counting = false;
};

struct ColdDrawTraits {
    bool hasBuild = true;
    bool indirect = false;
    bool shadersWriteMemory = false;
    std::size_t colorTargets = 1;
    bool stencilWrites = false;
    bool depthCopySource = false;
    bool stencilReplay = false;
    bool rectOrQuadList = false;
    bool indexed = true;
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 1;
    bool linearTarget = false;
};

inline bool FullscreenLikeDraw(const ColdDrawTraits& traits) {
    return !traits.indexed && traits.indexCount <= 6 && traits.instanceCount == 1;
}

inline ColdSkipRefusal ColdDrawRefusal(const ColdDrawTraits& traits, const ColdSkipAllow& allow = {}) {
    if (!traits.hasBuild) return ColdSkipRefusal::NoPrebuild;
    if (traits.indirect) return ColdSkipRefusal::Indirect;
    if (traits.shadersWriteMemory) return ColdSkipRefusal::MemoryWrites;
    if (traits.colorTargets == 0 && !allow.depthOnly) return ColdSkipRefusal::DepthOnly;
    if (allow.stencil) {
        if (traits.depthCopySource) return ColdSkipRefusal::DepthCopy;
        if (traits.stencilReplay) return ColdSkipRefusal::StencilReplay;
    } else if (traits.stencilWrites || traits.depthCopySource || traits.stencilReplay) {
        return ColdSkipRefusal::Stencil;
    }
    if (traits.rectOrQuadList || FullscreenLikeDraw(traits)) return ColdSkipRefusal::Fullscreen;
    if (traits.linearTarget) return ColdSkipRefusal::LinearTarget;
    return ColdSkipRefusal::None;
}

inline ColdSkipRefusal ColdCommitRefusal(bool countingSamples, bool parkedFull, const ColdSkipAllow& allow = {}) {
    if (countingSamples && !allow.counting) return ColdSkipRefusal::CountingSamples;
    if (parkedFull) return ColdSkipRefusal::ParkedFull;
    return ColdSkipRefusal::None;
}

inline bool BuildReadyWithin(const std::shared_future<void>& build, std::chrono::nanoseconds budget) {
    if (!build.valid()) return true;
    return build.wait_for(budget) == std::future_status::ready;
}

struct ColdSkipAttempt {
    bool requested = false;
    bool skipped = false;
    ColdSkipRefusal refusal = ColdSkipRefusal::None;
    std::shared_future<void> build;
    std::shared_ptr<void> keep;
};

class ColdSkipKeys {
public:
    using Clock = std::chrono::steady_clock;

    struct Recovery {
        std::uint64_t skipped = 0;
        std::chrono::nanoseconds window{};
    };

    bool NoteSkip(std::uint64_t key, Clock::time_point now) {
        std::lock_guard lock(mutex_);
        const auto [it, inserted] = keys_.try_emplace(key, Entry{0, now});
        ++it->second.skipped;
        size_.store(keys_.size(), std::memory_order_relaxed);
        return inserted;
    }

    std::optional<Recovery> NoteDrawn(std::uint64_t key, Clock::time_point now) {
        if (size_.load(std::memory_order_relaxed) == 0) return std::nullopt;
        std::lock_guard lock(mutex_);
        const auto it = keys_.find(key);
        if (it == keys_.end()) return std::nullopt;
        Recovery recovery{it->second.skipped, std::chrono::duration_cast<std::chrono::nanoseconds>(now - it->second.first)};
        keys_.erase(it);
        size_.store(keys_.size(), std::memory_order_relaxed);
        return recovery;
    }

    std::size_t Size() const {
        return size_.load(std::memory_order_relaxed);
    }

private:
    struct Entry {
        std::uint64_t skipped = 0;
        Clock::time_point first;
    };

    std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> keys_;
    std::atomic<std::size_t> size_{0};
};

class ColdDrawParking {
public:
    using Clock = std::chrono::steady_clock;

    struct Reaped {
        std::size_t released = 0;
        std::uint64_t savedNs = 0;
    };

    explicit ColdDrawParking(std::size_t capacity) : capacity_(capacity) {}

    bool Full() const {
        std::lock_guard lock(mutex_);
        return entries_.size() >= capacity_;
    }

    bool Park(std::shared_ptr<void> owner, std::shared_future<void> build, Clock::time_point skipped) {
        std::lock_guard lock(mutex_);
        if (entries_.size() >= capacity_) return false;
        entries_.push_back(Entry{std::move(owner), std::move(build), skipped});
        peak_ = std::max(peak_, entries_.size());
        return true;
    }

    Reaped Reap(Clock::time_point now) {
        std::vector<Entry> released;
        {
            std::lock_guard lock(mutex_);
            const auto ready = [](const Entry& entry) { return !entry.build.valid() || entry.build.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
            const auto split = std::stable_partition(entries_.begin(), entries_.end(), [&](const Entry& entry) { return !ready(entry); });
            released.assign(std::make_move_iterator(split), std::make_move_iterator(entries_.end()));
            entries_.erase(split, entries_.end());
        }
        Reaped reaped;
        reaped.released = released.size();
        for (const auto& entry : released) {
            if (now > entry.skipped) reaped.savedNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - entry.skipped).count());
        }
        return reaped;
    }

    std::size_t Size() const {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

    std::size_t Peak() const {
        std::lock_guard lock(mutex_);
        return peak_;
    }

    std::size_t TakePeak() {
        std::lock_guard lock(mutex_);
        return std::exchange(peak_, entries_.size());
    }

private:
    struct Entry {
        std::shared_ptr<void> owner;
        std::shared_future<void> build;
        Clock::time_point skipped;
    };

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::size_t peak_ = 0;
    std::vector<Entry> entries_;
};

}

#endif
