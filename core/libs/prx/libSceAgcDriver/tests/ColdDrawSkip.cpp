#include "prx/libSceAgcDriver/Graphics/include/ColdDrawSkip.hpp"
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace AgcDriver::Graphics;

namespace {

using Clock = std::chrono::steady_clock;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void boundedWaitTimesOutThenSeesTheBuild() {
    std::promise<void> promise;
    const auto build = promise.get_future().share();
    const auto start = Clock::now();
    Require(!BuildReadyWithin(build, std::chrono::milliseconds(20)), "a pending build reads as ready");
    Require(Clock::now() - start >= std::chrono::milliseconds(15), "the bounded wait returned before its budget");
    std::thread fulfil([&promise] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        promise.set_value();
    });
    Require(BuildReadyWithin(build, std::chrono::seconds(10)), "a build fulfilled inside the budget reads as pending");
    fulfil.join();
    Require(BuildReadyWithin(build, std::chrono::nanoseconds(0)), "a finished build reads as pending");
    Require(BuildReadyWithin({}, std::chrono::nanoseconds(0)), "a missing build reads as pending");
}

void eligibilityRefusesEachUnsafeDraw() {
    ColdDrawTraits clean;
    clean.indexed = true;
    clean.indexCount = 300;
    Require(ColdDrawRefusal(clean) == ColdSkipRefusal::None, "a plain indexed colour draw is refused");
    ColdDrawTraits traits = clean;
    traits.hasBuild = false;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::NoPrebuild, "a draw without a build is not refused as no-prebuild");
    traits = clean;
    traits.indirect = true;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::Indirect, "an indirect draw is not refused");
    traits = clean;
    traits.shadersWriteMemory = true;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::MemoryWrites, "a draw whose shaders write memory is not refused");
    traits = clean;
    traits.colorTargets = 0;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::DepthOnly, "a depth-only draw is not refused");
    traits = clean;
    traits.stencilWrites = true;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::Stencil, "a stencil-writing draw is not refused");
    traits = clean;
    traits.depthCopySource = true;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::Stencil, "a depth copy source is not refused");
    traits = clean;
    traits.stencilReplay = true;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::Stencil, "a stencil replay is not refused");
    traits = clean;
    traits.rectOrQuadList = true;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::Fullscreen, "a rect or quad list is not refused");
    traits = clean;
    traits.indexed = false;
    traits.indexCount = 3;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::Fullscreen, "a three-vertex draw is not refused as fullscreen");
    traits.instanceCount = 2;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::None, "an instanced small draw is refused as fullscreen");
    traits = clean;
    traits.indexed = false;
    traits.indexCount = 7;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::None, "a seven-vertex draw is refused as fullscreen");
    traits = clean;
    traits.linearTarget = true;
    Require(ColdDrawRefusal(traits) == ColdSkipRefusal::LinearTarget, "a linear colour target is not refused");
    Require(ColdCommitRefusal(false, false) == ColdSkipRefusal::None, "a free commit is refused");
    Require(ColdCommitRefusal(true, false) == ColdSkipRefusal::CountingSamples, "counting samples is not refused");
    Require(ColdCommitRefusal(false, true) == ColdSkipRefusal::ParkedFull, "a full parking is not refused");
}

void keyLogsFireOncePerKey() {
    ColdSkipKeys keys;
    const auto start = Clock::now();
    Require(keys.NoteSkip(7, start), "the first skip of a key is not new");
    Require(!keys.NoteSkip(7, start + std::chrono::milliseconds(5)), "the second skip of a key is new");
    Require(keys.NoteSkip(8, start), "another key is not new");
    Require(keys.Size() == 2, "two keys are not tracked");
    Require(!keys.NoteDrawn(9, start).has_value(), "a key never skipped recovers");
    const auto recovery = keys.NoteDrawn(7, start + std::chrono::milliseconds(40));
    Require(recovery.has_value() && recovery->skipped == 2, "the recovery does not count the skipped draws");
    Require(recovery->window == std::chrono::milliseconds(40), "the recovery window is not the time since the first skip");
    Require(!keys.NoteDrawn(7, start + std::chrono::milliseconds(50)).has_value(), "a key recovers twice");
    Require(keys.Size() == 1, "a recovered key stays tracked");
    Require(keys.NoteSkip(7, start + std::chrono::milliseconds(60)), "a key skipped again after its recovery is not new");
}

void parkingKeepsTheOwnerUntilTheBuildIsReady() {
    ColdDrawParking parking(2);
    const auto start = Clock::now();
    std::promise<void> first;
    std::promise<void> second;
    std::promise<void> third;
    auto firstOwner = std::make_shared<int>(1);
    auto secondOwner = std::make_shared<int>(2);
    auto thirdOwner = std::make_shared<int>(3);
    const std::weak_ptr<int> firstWatch = firstOwner;
    const std::weak_ptr<int> secondWatch = secondOwner;
    Require(parking.Park(std::move(firstOwner), first.get_future().share(), start), "the first park is refused");
    Require(!parking.Full(), "one of two is full");
    Require(parking.Park(std::move(secondOwner), second.get_future().share(), start), "the second park is refused");
    Require(parking.Full(), "two of two is not full");
    Require(!parking.Park(thirdOwner, third.get_future().share(), start), "a park beyond the capacity is accepted");
    Require(thirdOwner.use_count() == 1, "a refused park kept the owner");
    Require(parking.Peak() == 2, "the peak is not two");
    auto reaped = parking.Reap(start + std::chrono::milliseconds(5));
    Require(reaped.released == 0 && !firstWatch.expired() && !secondWatch.expired(), "a pending build released its owner");
    first.set_value();
    reaped = parking.Reap(start + std::chrono::milliseconds(30));
    Require(reaped.released == 1, "a ready build was not reaped");
    Require(firstWatch.expired(), "the reaped owner is still alive");
    Require(!secondWatch.expired(), "an unready owner was released");
    Require(reaped.savedNs == 30'000'000u, "the saved time is not the time since the skip");
    Require(!parking.Full() && parking.Size() == 1, "the reap did not free a slot");
    second.set_value();
    reaped = parking.Reap(start + std::chrono::milliseconds(31));
    Require(reaped.released == 1 && secondWatch.expired() && parking.Size() == 0, "the second owner was not released");
    Require(parking.TakePeak() == 2, "the peak was lost");
    Require(parking.TakePeak() == 0, "the peak was not reset");
}

}

int main() {
    try {
        boundedWaitTimesOutThenSeesTheBuild();
        eligibilityRefusesEachUnsafeDraw();
        keyLogsFireOncePerKey();
        parkingKeepsTheOwnerUntilTheBuildIsReady();
        std::puts("cold draw skip tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
