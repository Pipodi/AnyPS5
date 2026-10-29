#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}

int main(int argc, char** argv) {
    try {
        const bool enabled = argc > 1 && std::strcmp(argv[1], "--on") == 0;
        SetEnvironment("APS5_LOG_REGCOMPUTE_GATES", enabled ? "1" : "");
        if (AgcDriver::DriverDetail::RegisteredComputeGateLogging() != enabled) throw std::runtime_error(std::string("registration compute gate log: logging is ") + (enabled ? "off" : "on") + " with the variable " + (enabled ? "set" : "unset"));
        AgcDriver::DriverDetail::LogRegisteredComputeGate(0x1000, 4, "registration", "queued", "10");
        std::puts("registration compute gate log tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
