#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparation.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t Programs = 4;

struct Program {
    alignas(256) std::array<std::uint32_t, 8> code{};
    Shader shader{};
    std::array<ShaderRegister, 7> registers{};
    ShaderSpecialRegs specials{};
};

std::array<Program, Programs> Shaders;
alignas(8) std::uint32_t Condition = 0;

void Fill(Program& program, std::uint32_t index) {
    for (std::uint32_t nop = 0; nop <= index; ++nop) program.code[nop] = 0xbf800000u;
    program.code[index + 1] = 0xbf810000u;
    const auto address = reinterpret_cast<std::uintptr_t>(program.code.data());
    program.shader.file_header = 0x34333231u;
    program.shader.version = 0x18;
    program.shader.header_size = sizeof(Shader) + sizeof(program.registers) + sizeof(program.specials);
    program.shader.shader_size = static_cast<std::uint32_t>(sizeof(program.code));
    program.shader.code = program.code.data();
    program.shader.sh_registers = program.registers.data();
    program.shader.num_sh_registers = program.registers.size();
    program.shader.specials = &program.specials;
    program.specials.dispatch_modifier = 0x8000;
    program.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)}, {0x207, index + 1}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, 2}}};
}

void Dispatch(std::vector<std::uint32_t>& words, std::size_t index) {
    for (const auto reg : Shaders[index].registers) words.insert(words.end(), {0xc0017600u, reg.offset, reg.value});
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void CondExec(std::vector<std::uint32_t>& words, std::size_t index) {
    const auto address = reinterpret_cast<std::uintptr_t>(&Condition);
    const auto at = words.size();
    words.insert(words.end(), {0xc0032200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 0, 0});
    Dispatch(words, index);
    words[at + 4] = static_cast<std::uint32_t>(words.size() - at - 5);
}

void Submit(const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "miss ahead past COND_EXEC submit failed");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::DriverDetail::AwaitRegistrationPreparation();
}

std::pair<std::uint64_t, std::uint64_t> Totals() {
    std::uint64_t started = 0;
    std::uint64_t used = 0;
    AgcDriverMissAheadTotals_nid_postfix(&started, &used);
    return {started, used};
}

std::string Describe(const std::pair<std::uint64_t, std::uint64_t>& totals) {
    return "(started " + std::to_string(totals.first) + ", used " + std::to_string(totals.second) + ")";
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", "1");
        _putenv_s("APS5_DISPATCH_MISS_AHEAD_PAST_COND_EXEC", "1");
#else
        setenv("APS5_PROFILE_DRAW", "1", 1);
        setenv("APS5_DISPATCH_MISS_AHEAD_PAST_COND_EXEC", "1", 1);
#endif
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        for (std::uint32_t index = 0; index < Programs; ++index) {
            Fill(Shaders[index], index);
            AgcDriverRegisterShader_nid_postfix(&Shaders[index].shader);
        }
        AgcDriver::DriverDetail::AwaitRegistrationPreparation();
        Require(Totals() == std::pair<std::uint64_t, std::uint64_t>{0, 0}, "builds started before any dispatch " + Describe(Totals()));

        std::vector<std::uint32_t> words;
        Dispatch(words, 0);
        CondExec(words, 1);
        Dispatch(words, 2);
        Submit(words);
        auto totals = Totals();
        Require(totals.first == 1, "a miss followed by a skipped COND_EXEC block and a new program did not start exactly the program after the block " + Describe(totals));
        Require(totals.second == 1, "the program after the COND_EXEC block was not used by its dispatch " + Describe(totals));
        std::cout << "scan continues after a COND_EXEC block: passed\n";

        words.clear();
        Dispatch(words, 3);
        CondExec(words, 1);
        Submit(words);
        totals = Totals();
        Require(totals.first == 1, "a miss followed only by a COND_EXEC block started a build inside the block " + Describe(totals));
        std::cout << "COND_EXEC block not scanned: passed\n";

        std::cout << "miss ahead past COND_EXEC test passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
