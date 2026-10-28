#include "Recompiler.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ShaderRecompiler;

constexpr std::uint32_t PrivateApertureHigh = 0x70000000u;
constexpr std::uint32_t FlatStoreDword = 0xdc700000u;
constexpr std::uint32_t FlatStoreOperands = 0x007d0402u;
constexpr std::uint32_t FlatAtomicAddU32 = 0xdcc90000u;
constexpr std::uint32_t FlatAtomicOperands = 0x0a7d0402u;
constexpr std::uint32_t MovPrivateBase = 0xbe8a04edu;
constexpr std::uint32_t MovPrivateLimit = 0xbe8a04eeu;
constexpr std::uint32_t MovSharedBase = 0xbe8a04ecu;
constexpr std::uint32_t EndProgram = 0xbf810000u;

struct Module {
    bool privateCompare = false;
    std::size_t scratchArrays = 0;
};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Module Compile(std::span<const std::uint32_t> code) {
    const std::array<std::uint32_t, 7> capabilities{spv::CapabilityShader, spv::CapabilityGroupNonUniform, spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle, spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    const std::array<std::uint32_t, 4> userData{};
    const std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
    request.context.waveSize = 32;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 4u, {false, false, false}, false, 1u};
    request.context.compute->scratchDwords = 4u;
    request.target.vulkanVersion = 0x00403000u;
    request.target.spirvVersion = 0x00010600u;
    request.target.subgroupSize = 32;
    request.target.bdaAbiVersion = 1;
    request.target.spannedWaves = true;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
    request.target.maxWorkgroupInvocations = 1024;
    request.target.maxWorkgroupSharedMemoryBytes = 49152;
    request.layout = {0, 0, 0, 128};
    request.useCache = false;
    const auto result = Recompile(request);
    Require(!result.spirv.empty(), "the FLAT program did not recompile");
    Module module;
    const auto words = result.spirv.Words();
    std::vector<std::uint32_t> arrays;
    std::vector<std::uint32_t> arrayPointers;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        const auto op = static_cast<spv::Op>(words[cursor] & spv::OpCodeMask);
        Require(count != 0 && cursor + count <= words.size(), "truncated SPIR-V instruction");
        if (op == spv::OpConstant && count == 4 && words[cursor + 3] == PrivateApertureHigh) module.privateCompare = true;
        if (op == spv::OpTypeArray) arrays.push_back(words[cursor + 1]);
        if (op == spv::OpTypePointer && words[cursor + 2] == spv::StorageClassFunction) {
            for (const auto array : arrays) {
                if (array == words[cursor + 3]) arrayPointers.push_back(words[cursor + 1]);
            }
        }
        if (op == spv::OpVariable && words[cursor + 3] == spv::StorageClassFunction) {
            for (const auto pointer : arrayPointers) {
                if (pointer == words[cursor + 1]) ++module.scratchArrays;
            }
        }
        cursor += count;
    }
    return module;
}

void CheckStoreWithoutOperand() {
    const std::array<std::uint32_t, 3> code{FlatStoreDword, FlatStoreOperands, EndProgram};
    const auto module = Compile(code);
    Require(!module.privateCompare, "a FLAT store in a program without src_private_base tests the private aperture");
    Require(module.scratchArrays == 0, "a FLAT store in a program without src_private_base keeps the scratch arrays");
}

void CheckAtomicWithoutOperand() {
    const std::array<std::uint32_t, 3> code{FlatAtomicAddU32, FlatAtomicOperands, EndProgram};
    const auto module = Compile(code);
    Require(!module.privateCompare && module.scratchArrays == 0, "a FLAT atomic in a program without src_private_base routes the private aperture");
}

void CheckSharedBaseDoesNotRoutePrivate() {
    const std::array<std::uint32_t, 4> code{MovSharedBase, FlatStoreDword, FlatStoreOperands, EndProgram};
    const auto module = Compile(code);
    Require(!module.privateCompare, "src_shared_base marks the program as forming a private address");
}

void CheckOperandRestoresRouting(std::uint32_t move, const char* name) {
    const std::array<std::uint32_t, 4> store{move, FlatStoreDword, FlatStoreOperands, EndProgram};
    const auto storeModule = Compile(store);
    Require(storeModule.privateCompare && storeModule.scratchArrays != 0, std::string("a FLAT store after ") + name + " does not route the private aperture to scratch");
    const std::array<std::uint32_t, 4> atomic{move, FlatAtomicAddU32, FlatAtomicOperands, EndProgram};
    const auto atomicModule = Compile(atomic);
    Require(atomicModule.privateCompare && atomicModule.scratchArrays != 0, std::string("a FLAT atomic after ") + name + " does not route the private aperture to scratch");
}

}

int main() {
    try {
        CheckStoreWithoutOperand();
        CheckAtomicWithoutOperand();
        CheckSharedBaseDoesNotRoutePrivate();
        CheckOperandRestoresRouting(MovPrivateBase, "src_private_base");
        CheckOperandRestoresRouting(MovPrivateLimit, "src_private_limit");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SpirvFlatPrivateAperture: %s\n", error.what());
        return 1;
    }
    std::puts("SpirvFlatPrivateAperture: FLAT accesses route the private aperture to scratch only in programs that read src_private_base or src_private_limit");
    return 0;
}
