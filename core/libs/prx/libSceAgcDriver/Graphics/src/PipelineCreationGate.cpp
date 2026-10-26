#include "prx/libSceAgcDriver/Graphics/include/PipelineCreationGate.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver::Graphics {

namespace {

std::uint64_t EnvironmentNumber(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    return std::strtoull(text, nullptr, 10);
}

}

PipelineCreationGate::PipelineCreationGate(Settings settings) : settings(std::move(settings)) {
    stats.budgetBytes = this->settings.budgetBytes;
    stats.reserveBytes = this->settings.reserveBytes;
}

std::uint64_t PipelineCreationGate::PhysicalMemoryBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0u;
#else
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto page = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page) : 0u;
#endif
}

std::uint64_t PipelineCreationGate::AvailableMemoryBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0u;
#else
    const auto pages = sysconf(_SC_AVPHYS_PAGES);
    const auto page = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page) : 0u;
#endif
}

PipelineCreationGate::Settings PipelineCreationGate::SettingsFromEnvironment() {
    Settings settings;
    settings.bytesPerWord = std::max<std::uint64_t>(1u, EnvironmentNumber("APS5_PIPELINE_COMPILE_BYTES_PER_WORD", settings.bytesPerWord));
    settings.budgetBytes = EnvironmentNumber("APS5_PIPELINE_COMPILE_BUDGET_MIB", PhysicalMemoryBytes() / 16u * 7u >> 20u) << 20u;
    settings.smallBytes = EnvironmentNumber("APS5_PIPELINE_COMPILE_SMALL_MIB", settings.smallBytes >> 20u) << 20u;
    settings.reserveBytes = EnvironmentNumber("APS5_PIPELINE_COMPILE_RESERVE_MIB", 0) << 20u;
    if (settings.reserveBytes != 0) settings.reserveAvailableBytes = &PipelineCreationGate::AvailableMemoryBytes;
    settings.maxLarge = static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, EnvironmentNumber("APS5_PIPELINE_COMPILE_MAX_LARGE", settings.maxLarge)));
    settings.log = std::getenv("APS5_LOG_PIPELINE_BUILDS") != nullptr;
    return settings;
}

PipelineCreationGate& PipelineCreationGate::Shared() {
    static PipelineCreationGate gate(SettingsFromEnvironment());
    return gate;
}

void PipelineCreationGate::log(const char* what, std::uint64_t bytes, std::uint64_t available) const {
    if (!settings.log || bytes <= settings.smallBytes) return;
    std::fprintf(stderr, "[gate] %s est=%llu MiB avail=%llu MiB inflight=%llu MiB in %llu creations (%llu large)\n", what, static_cast<unsigned long long>(bytes >> 20u), static_cast<unsigned long long>(available >> 20u), static_cast<unsigned long long>(stats.inFlightBytes >> 20u), static_cast<unsigned long long>(stats.inFlight), static_cast<unsigned long long>(inFlightLarge));
}

PipelineCreationGate::Verdict PipelineCreationGate::evaluate(std::uint64_t bytes) {
    if (stats.inFlight == 0) return Verdict::Budget;
    auto budget = settings.budgetBytes;
    if (settings.availableBytes) {
        const auto available = settings.availableBytes();
        if (available != 0) budget = std::min(budget, stats.inFlightBytes + available / 2u);
    }
    if (stats.inFlightBytes + bytes <= budget) return Verdict::Budget;
    if (settings.reserveBytes == 0 || !settings.reserveAvailableBytes || bytes <= settings.smallBytes) return Verdict::Wait;
    const auto available = settings.reserveAvailableBytes();
    if (available != 0) {
        stats.minAvailableBytes = hasMinAvailable ? std::min(stats.minAvailableBytes, available) : available;
        hasMinAvailable = true;
    }
    if (available == 0 || inFlightLarge >= settings.maxLarge) return Verdict::Wait;
    const auto committed = stats.inFlightBytes + bytes;
    return available >= committed && available - committed >= settings.reserveBytes ? Verdict::Reserve : Verdict::Wait;
}

PipelineCreationGate::Ticket PipelineCreationGate::Admit(std::size_t words, bool mayWait) {
    if (settings.budgetBytes == 0) return {};
    const auto bytes = EstimateBytes(words);
    const bool large = bytes > settings.smallBytes;
    std::unique_lock lock(mutex);
    auto verdict = evaluate(bytes);
    if (verdict == Verdict::Wait) {
        if (mayWait && large) {
            const auto start = std::chrono::steady_clock::now();
            ++stats.waited;
            log("wait", bytes, settings.log && settings.reserveAvailableBytes ? settings.reserveAvailableBytes() : 0u);
            while ((verdict = evaluate(bytes)) == Verdict::Wait) released.wait_for(lock, std::chrono::milliseconds(settings.recheckMilliseconds));
            stats.waitNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        } else {
            ++stats.unwaited;
        }
    }
    if (verdict == Verdict::Reserve) ++stats.admittedByReserve;
    log(verdict == Verdict::Reserve ? "admit-by-reserve" : "admit", bytes, settings.log && settings.reserveAvailableBytes ? settings.reserveAvailableBytes() : 0u);
    ++stats.admitted;
    ++stats.inFlight;
    if (large) ++inFlightLarge;
    stats.inFlightBytes += bytes;
    stats.peakInFlight = std::max(stats.peakInFlight, stats.inFlight);
    stats.peakBytes = std::max(stats.peakBytes, stats.inFlightBytes);
    return Ticket(this, bytes, large);
}

void PipelineCreationGate::release(std::uint64_t bytes, bool large) noexcept {
    {
        std::lock_guard lock(mutex);
        --stats.inFlight;
        if (large) --inFlightLarge;
        stats.inFlightBytes -= std::min(stats.inFlightBytes, bytes);
    }
    released.notify_all();
}

PipelineCreationGate::Stats PipelineCreationGate::Totals() const {
    std::lock_guard lock(mutex);
    return stats;
}

}
