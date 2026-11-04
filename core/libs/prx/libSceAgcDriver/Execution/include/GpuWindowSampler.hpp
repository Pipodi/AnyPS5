#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GPUWINDOWSAMPLER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GPUWINDOWSAMPLER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::GpuWindowSampler {

struct Options {
    double intervalMs = 1.0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> windows;
    std::function<void(const std::string&)> sink;
};

bool ParseWindows(const char* text, std::vector<std::pair<std::uint64_t, std::uint64_t>>& windows);
void Start(Options options);
void Stop();
void NoteDisplayFrame(std::uint64_t frame);

class ThreadScope {
public:
    ThreadScope(const std::string& label, bool active);
    ThreadScope(const ThreadScope&) = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;
    ~ThreadScope();

private:
    std::uint64_t id = 0;
};

}

#endif
