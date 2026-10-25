#include "prx/libSceAgcDriver/Graphics/include/PipelineCreationGate.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

using AgcDriver::Graphics::PipelineCreationGate;

namespace {

constexpr std::uint64_t MiB = 1ull << 20u;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

PipelineCreationGate::Settings Settings(std::uint64_t budget, std::uint64_t small = 0) {
    PipelineCreationGate::Settings settings;
    settings.bytesPerWord = 1024;
    settings.budgetBytes = budget;
    settings.smallBytes = small;
    return settings;
}

bool AdmittedWithin(std::atomic<bool>& admitted, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!admitted.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return admitted.load();
}

void aloneAndQueued() {
    PipelineCreationGate gate(Settings(8 * MiB));
    auto first = gate.Admit(16 * 1024, true);
    Require(gate.Totals().inFlight == 1 && gate.Totals().inFlightBytes == 16 * MiB, "a creation over the budget did not run alone");
    std::atomic<bool> admitted{false};
    std::thread second([&] {
        const auto ticket = gate.Admit(4 * 1024, true);
        admitted = true;
    });
    const bool early = AdmittedWithin(admitted, std::chrono::milliseconds(200));
    first = {};
    const bool late = AdmittedWithin(admitted, std::chrono::milliseconds(5000));
    second.join();
    const auto stats = gate.Totals();
    Require(!early, "a creation was admitted while the one in flight took the whole budget");
    Require(late && stats.waited == 1 && stats.admitted == 2 && stats.inFlight == 0 && stats.inFlightBytes == 0, "the waiting creation was not admitted once the first finished");
    Require(stats.peakInFlight == 1 && stats.peakBytes == 16 * MiB, "the peak counts more than one creation at once");
    std::puts("a creation over the budget runs alone; the next one waits for it");
}

void fitsTogether() {
    PipelineCreationGate gate(Settings(8 * MiB));
    const auto first = gate.Admit(3 * 1024, true);
    const auto second = gate.Admit(4 * 1024, true);
    const auto stats = gate.Totals();
    Require(stats.waited == 0 && stats.inFlight == 2 && stats.peakBytes == 7 * MiB, "two creations inside the budget did not run together");
    std::puts("creations that fit the budget run together");
}

void smallAndLocked() {
    PipelineCreationGate gate(Settings(8 * MiB, 2 * MiB));
    const auto big = gate.Admit(8 * 1024, true);
    const auto small = gate.Admit(2 * 1024, true);
    const auto locked = gate.Admit(6 * 1024, false);
    const auto stats = gate.Totals();
    Require(stats.waited == 0 && stats.unwaited == 2 && stats.inFlight == 3 && stats.inFlightBytes == 16 * MiB, "a small creation or one under the GPU mutex waited for the budget");
    std::puts("small creations and creations that may not wait run past the budget and are counted");
}

void availableMemory() {
    auto settings = Settings(1024 * MiB);
    std::atomic<std::uint64_t> available{2 * MiB};
    settings.availableBytes = [&] { return available.load(); };
    PipelineCreationGate gate(settings);
    const auto first = gate.Admit(1024, true);
    std::atomic<bool> admitted{false};
    std::thread second([&] {
        const auto ticket = gate.Admit(2 * 1024, true);
        admitted = true;
    });
    const bool early = AdmittedWithin(admitted, std::chrono::milliseconds(400));
    available = 512 * MiB;
    const bool late = AdmittedWithin(admitted, std::chrono::milliseconds(5000));
    second.join();
    Require(!early, "a creation was admitted past half the available memory");
    Require(late && gate.Totals().waited == 1, "the creation was not admitted once memory became available");
    std::puts("half the available memory bounds the creations in flight");
}

struct Reserve {
    std::atomic<std::uint64_t> available;
    PipelineCreationGate::Settings settings;
    Reserve(std::uint64_t budget, std::uint64_t reserve, std::uint64_t availableMiB, std::uint32_t maxLarge = 2) : available(availableMiB * MiB), settings(Settings(budget)) {
        settings.reserveBytes = reserve;
        settings.reserveAvailableBytes = [this] { return available.load(); };
        settings.maxLarge = maxLarge;
        settings.recheckMilliseconds = 20;
    }
};

bool SecondAdmitted(PipelineCreationGate& gate, std::atomic<std::uint64_t>& available, std::uint64_t secondWords, std::uint64_t availableLater, std::chrono::milliseconds first, std::chrono::milliseconds second, bool& early) {
    auto holder = gate.Admit(16 * 1024, true);
    std::atomic<bool> admitted{false};
    std::thread waiter([&] {
        const auto ticket = gate.Admit(static_cast<std::size_t>(secondWords), true);
        admitted = true;
    });
    early = AdmittedWithin(admitted, first);
    if (availableLater != 0) available = availableLater * MiB;
    const bool late = AdmittedWithin(admitted, second);
    holder = {};
    waiter.join();
    return late;
}

void reserveAdmitsWhenAvailable() {
    Reserve with(8 * MiB, 10 * MiB, 1000);
    PipelineCreationGate gate(with.settings);
    const auto first = gate.Admit(16 * 1024, true);
    const auto second = gate.Admit(16 * 1024, true);
    auto stats = gate.Totals();
    Require(stats.waited == 0 && stats.inFlight == 2 && stats.admittedByReserve == 1, "a second large creation was not admitted by the reserve while plenty of memory was available");
    Require(stats.minAvailableBytes == 1000 * MiB, "the minimum available memory at admission was not recorded");
    Reserve without(8 * MiB, 0, 1000);
    PipelineCreationGate plain(without.settings);
    auto alone = plain.Admit(16 * 1024, true);
    std::atomic<bool> admitted{false};
    std::thread waiter([&] {
        const auto ticket = plain.Admit(16 * 1024, true);
        admitted = true;
    });
    const bool early = AdmittedWithin(admitted, std::chrono::milliseconds(300));
    alone = {};
    waiter.join();
    Require(!early && plain.Totals().admittedByReserve == 0, "without a reserve the second large creation did not wait for the first");
    std::puts("a second large creation is admitted by the reserve when available memory is high, and waits without one");
}

void reserveHoldsWhenLow() {
    Reserve with(8 * MiB, 10 * MiB, 40);
    PipelineCreationGate gate(with.settings);
    bool early = false;
    const bool late = SecondAdmitted(gate, with.available, 16 * 1024, 0, std::chrono::milliseconds(300), std::chrono::milliseconds(200), early);
    Require(!early && !late, "a second creation was admitted with 8 MiB left over a 10 MiB reserve");
    Require(gate.Totals().admittedByReserve == 0 && gate.Totals().waited == 1, "the held creation was not counted as waiting");
    Reserve edge(8 * MiB, 10 * MiB, 42);
    PipelineCreationGate edgeGate(edge.settings);
    const auto first = edgeGate.Admit(16 * 1024, true);
    const auto second = edgeGate.Admit(16 * 1024, true);
    Require(edgeGate.Totals().admittedByReserve == 1, "a creation leaving exactly the reserve was not admitted");
    Reserve capped(8 * MiB, 10 * MiB, 1000, 2);
    PipelineCreationGate cappedGate(capped.settings);
    auto a = cappedGate.Admit(16 * 1024, true);
    const auto b = cappedGate.Admit(16 * 1024, true);
    std::atomic<bool> admitted{false};
    std::thread third([&] {
        const auto ticket = cappedGate.Admit(16 * 1024, true);
        admitted = true;
    });
    const bool cappedEarly = AdmittedWithin(admitted, std::chrono::milliseconds(300));
    Require(!cappedEarly, "a third large creation ran past the cap of two");
    a = {};
    third.join();
    Reserve open(8 * MiB, 10 * MiB, 1000, 3);
    PipelineCreationGate openGate(open.settings);
    const auto c = openGate.Admit(16 * 1024, true);
    const auto d = openGate.Admit(16 * 1024, true);
    const auto e = openGate.Admit(16 * 1024, true);
    Require(openGate.Totals().admittedByReserve == 2 && openGate.Totals().inFlight == 3, "a cap of three did not let the third large creation run");
    std::puts("the reserve holds a creation when available memory is low, admits it at exactly the reserve, and a cap bounds the large ones");
}

void reserveRechecksOnTimer() {
    Reserve with(8 * MiB, 10 * MiB, 40);
    PipelineCreationGate gate(with.settings);
    bool early = false;
    const bool late = SecondAdmitted(gate, with.available, 16 * 1024, 1000, std::chrono::milliseconds(200), std::chrono::milliseconds(5000), early);
    Require(!early && late, "memory freed with nothing finishing did not admit the waiting creation");
    Require(gate.Totals().admittedByReserve == 1, "the timer admission was not counted as by the reserve");
    Reserve off(8 * MiB, 0, 40);
    PipelineCreationGate plain(off.settings);
    bool plainEarly = false;
    const bool plainLate = SecondAdmitted(plain, off.available, 16 * 1024, 1000, std::chrono::milliseconds(200), std::chrono::milliseconds(600), plainEarly);
    Require(!plainEarly && !plainLate, "without a reserve freed memory admitted a creation while nothing finished");
    std::puts("a waiting creation is admitted when memory frees with nothing finishing");
}

void reserveUnsetIsToday() {
    PipelineCreationGate gate(Settings(8 * MiB));
    const auto first = gate.Admit(16 * 1024, true);
    const auto stats = gate.Totals();
    Require(stats.admittedByReserve == 0 && stats.minAvailableBytes == 0 && stats.reserveBytes == 0, "an unset reserve changed the statistics");
    const auto settings = PipelineCreationGate::SettingsFromEnvironment();
    if (std::getenv("APS5_PIPELINE_COMPILE_RESERVE_MIB") == nullptr) Require(settings.reserveBytes == 0 && !settings.reserveAvailableBytes, "the reserve is on without its variable");
    std::puts("an unset reserve leaves the gate as it was");
}

void disabled() {
    PipelineCreationGate gate(Settings(0));
    const auto first = gate.Admit(1u << 20u, true);
    const auto second = gate.Admit(1u << 20u, true);
    const auto stats = gate.Totals();
    Require(stats.admitted == 0 && stats.inFlight == 0, "a zero budget did not turn the gate off");
    std::puts("a zero budget turns the gate off");
}

void defaults() {
    const auto settings = PipelineCreationGate::SettingsFromEnvironment();
    const auto physical = PipelineCreationGate::PhysicalMemoryBytes();
    Require(physical != 0 && PipelineCreationGate::AvailableMemoryBytes() != 0, "physical or available memory reads zero");
    if (std::getenv("APS5_PIPELINE_COMPILE_BUDGET_MIB") == nullptr) Require(settings.budgetBytes == (physical / 8u >> 20u << 20u) && settings.availableBytes != nullptr, "the default budget is not an eighth of physical memory bounded by the available memory");
    std::printf("default budget %llu MiB of %llu MiB physical, %llu bytes per word, small below %llu MiB\n", static_cast<unsigned long long>(settings.budgetBytes >> 20u), static_cast<unsigned long long>(physical >> 20u), static_cast<unsigned long long>(settings.bytesPerWord), static_cast<unsigned long long>(settings.smallBytes >> 20u));
}

}

int main() {
    try {
        aloneAndQueued();
        fitsTogether();
        smallAndLocked();
        availableMemory();
        reserveUnsetIsToday();
        reserveAdmitsWhenAvailable();
        reserveHoldsWhenLow();
        reserveRechecksOnTimer();
        disabled();
        defaults();
        std::puts("pipeline creation gate tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pipeline creation gate test failed: %s\n", error.what());
        return 1;
    }
}
