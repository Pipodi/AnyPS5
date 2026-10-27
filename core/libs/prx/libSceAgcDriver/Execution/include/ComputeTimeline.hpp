#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTETIMELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTETIMELINE_HPP

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

}

#endif
