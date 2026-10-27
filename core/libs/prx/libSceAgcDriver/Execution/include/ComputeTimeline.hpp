#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTETIMELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTETIMELINE_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::ComputeTimeline {

enum class Event : unsigned { Registered, FirstDispatch, Lookahead, PrebuildStart };

bool Enabled();
double NowMs();
bool Once(Event event, std::uint64_t address);
void MarkPrebuildThread();
bool OnPrebuildThread();
std::uint32_t ThreadId();
double ThreadCpuMs();
void Mark(Event event, std::uint64_t address, const char* pipeline = nullptr);

struct CreateStats {
    double gateWaitMs = 0.0;
    double lockWaitMs = 0.0;
    double createWallMs = 0.0;
    double createCpuMs = 0.0;
    unsigned creates = 0;
    unsigned idealPerformance = 0;
    unsigned idealEfficiency = 0;
    unsigned starved = 0;
    unsigned samples = 0;
    char startClass = '?';
    char endClass = '?';
};

constexpr std::size_t WatchedCreateWords = 300000;

CreateStats& CurrentCreateStats();
void ResetCreateStats();

class CreateWatch {
public:
    explicit CreateWatch(bool active);
    ~CreateWatch();
    CreateWatch(const CreateWatch&) = delete;
    CreateWatch& operator=(const CreateWatch&) = delete;
    void Finish();

private:
    struct Impl;
    Impl* impl = nullptr;
};

int CompilePriority();
void ApplyCompilePriority();

class CompilePriorityScope {
public:
    CompilePriorityScope();
    ~CompilePriorityScope();
    CompilePriorityScope(const CompilePriorityScope&) = delete;
    CompilePriorityScope& operator=(const CompilePriorityScope&) = delete;

private:
    int previous = 0;
    bool raised = false;
};

}

#endif
