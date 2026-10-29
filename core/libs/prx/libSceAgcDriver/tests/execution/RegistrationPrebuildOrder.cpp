#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("registration prebuild order: " + message);
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

class Blocker {
public:
    void Submit() {
        AgcDriver::DriverDetail::SubmitLibraryPrebuild([this] {
            std::unique_lock lock(mutex);
            started = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
        });
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return started; });
    }

    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool released = false;
};

class Order {
public:
    void Submit(int id, std::uint64_t cost, const AgcDriver::DriverDetail::PreparedShaders* prepared = nullptr) {
        AgcDriver::DriverDetail::SubmitLibraryPrebuild([this, id] {
            std::lock_guard lock(mutex);
            ran.push_back(id);
            changed.notify_all();
        }, cost, prepared);
    }

    std::vector<int> Wait(std::size_t count) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return ran.size() >= count; });
        auto result = ran;
        ran.clear();
        return result;
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<int> ran;
};

void Check(const char* name, const std::vector<int>& actual, const std::vector<int>& expected) {
    if (actual == expected) return;
    std::string text = std::string(name) + " ran";
    for (const int id : actual) text += " " + std::to_string(id);
    text += ", expected";
    for (const int id : expected) text += " " + std::to_string(id);
    Require(false, text);
}

}

int main(int argc, char** argv) {
    try {
        const bool largestFirst = argc > 1 && std::strcmp(argv[1], "--largest-first") == 0;
        SetEnvironment("APS5_PREBUILD_THREADS", "2");
        SetEnvironment("APS5_PREBUILD_LARGEST_FIRST", largestFirst ? "1" : "0");
        AgcDriver::DriverDetail::Driver::Get();
        Blocker first;
        Blocker second;
        Blocker third;
        first.Submit();
        second.Submit();
        Order order;
        order.Submit(1, 1);
        order.Submit(5, 5);
        order.Submit(3, 3);
        first.Release();
        Check("queued costs 1, 5, 3", order.Wait(3), largestFirst ? std::vector<int>{5, 3, 1} : std::vector<int>{1, 5, 3});
        third.Submit();
        AgcDriver::DriverDetail::PreparedShaders demanded;
        demanded.demanded.store(true);
        order.Submit(1, 1);
        order.Submit(5, 5);
        order.Submit(9, 0, &demanded);
        third.Release();
        Check("queued costs 1, 5 and a demanded task", order.Wait(3), largestFirst ? std::vector<int>{9, 5, 1} : std::vector<int>{9, 1, 5});
        second.Release();
        AgcDriver::DriverDetail::CloseRegistrationPreparation();
        std::puts("registration prebuild order tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
