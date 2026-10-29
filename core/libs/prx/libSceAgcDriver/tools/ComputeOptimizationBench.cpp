#include "prx/libSceAgcDriver/Execution/include/ComputePipelineCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

double Median(std::vector<double> values) {
    std::ranges::sort(values);
    return values.empty() ? 0.0 : values[values.size() / 2];
}

std::string Join(const std::vector<double>& values) {
    std::string result;
    for (const auto value : values) result += (result.empty() ? "" : ",") + std::to_string(static_cast<long long>(value + 0.5));
    return result;
}

}

int main(int argc, char** argv) {
    int repeats = 3;
    int only = -1;
    std::vector<std::filesystem::path> sources;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--repeats" && index + 1 < argc) repeats = std::atoi(argv[++index]);
        else if (argument == "--only" && index + 1 < argc) only = std::string_view(argv[++index]) == "unopt" ? 1 : 0;
        else sources.emplace_back(argv[index]);
    }
    if (sources.empty() || repeats < 1) {
        std::cerr << "agc_compute_optimization_bench [--repeats N] CAPTURE...\n";
        return 2;
    }
    try {
        AgcDriver::VulkanDevice device(AgcDriver::VulkanDevice::CompileOnly{});
        std::cout << std::fixed << std::setprecision(2);
        for (const auto& source : sources) {
            auto capture = AgcDriver::ComputePipelineCapture::Read(source);
            capture.flags &= ~static_cast<VkPipelineCreateFlags>(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT);
            std::vector<double> optimized;
            std::vector<double> unoptimized;
            try {
                for (int round = 0; round < repeats; ++round) {
                    for (int slot = 0; slot < 2; ++slot) {
                        const bool unoptimizedFirst = (round % 2) != 0;
                        const bool disable = (slot == 0) == unoptimizedFirst;
                        if (only >= 0 && disable != (only == 1)) continue;
                        auto variant = capture;
                        if (disable) variant.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
                        const auto result = device.ReplayComputePipeline(variant, {}, true);
                        (disable ? unoptimized : optimized).push_back(result.pipelineMilliseconds);
                    }
                }
            } catch (const std::exception& error) {
                std::cout << source.parent_path().filename().string() << " words=" << capture.spirv.size() << " FAILED " << error.what() << '\n';
                continue;
            }
            const auto optimizedMedian = Median(optimized);
            const auto unoptimizedMedian = Median(unoptimized);
            std::cout << source.parent_path().filename().string() << " words=" << capture.spirv.size()
                      << " opt_ms=" << optimizedMedian << " unopt_ms=" << unoptimizedMedian
                      << " ratio=" << (optimizedMedian > 0 ? unoptimizedMedian / optimizedMedian : 0.0)
                      << " opt_runs=[" << Join(optimized) << "] unopt_runs=[" << Join(unoptimized) << "]\n"
                      << std::flush;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "compute optimization bench: " << error.what() << '\n';
        return 1;
    }
}
