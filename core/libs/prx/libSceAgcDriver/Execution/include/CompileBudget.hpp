#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPILEBUDGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPILEBUDGET_HPP

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace AgcDriver {

class CompileBudget {
public:
    class Hold {
    public:
        Hold(CompileBudget& budget, std::uint64_t bytes) : budget(budget), bytes(bytes) {
            std::unique_lock lock(budget.mutex);
            budget.released.wait(lock, [&] { return budget.held == 0 || budget.held + bytes <= budget.limit; });
            budget.held += bytes;
            if (budget.held > budget.peak) budget.peak = budget.held;
        }

        ~Hold() {
            {
                std::lock_guard lock(budget.mutex);
                budget.held -= bytes;
            }
            budget.released.notify_all();
        }

        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

    private:
        CompileBudget& budget;
        std::uint64_t bytes;
    };

    explicit CompileBudget(std::uint64_t limit) : limit(limit) {}

    std::uint64_t Peak() const {
        std::lock_guard lock(mutex);
        return peak;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable released;
    std::uint64_t limit;
    std::uint64_t held = 0;
    std::uint64_t peak = 0;
};

inline std::uint64_t BackgroundCompileLimitFor(const char* configured) {
    constexpr std::uint64_t automaticKiB = 4096;
    return (configured != nullptr ? std::strtoull(configured, nullptr, 10) : automaticKiB) * 1024u;
}

inline CompileBudget& BackgroundCompileBudget() {
    static CompileBudget budget(BackgroundCompileLimitFor(std::getenv("APS5_BACKGROUND_COMPILE_KIB")));
    return budget;
}

}

#endif
