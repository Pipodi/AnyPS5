#include "SpirvBackend/SpirvOptimizer.hpp"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace ShaderRecompiler {

namespace {

constexpr std::uint32_t ScalarReplacementLimit = 64;
constexpr std::uintptr_t InlineOptimizerStackBytes = 256 * 1024;
constexpr std::uint32_t LargeOptimizeRounds = 3;
constexpr std::uint32_t SelectOnVectorsVersion = 0x00010400u;

std::atomic<std::uint64_t> optimizerHostThreads{0};
std::atomic<std::uint64_t> largeModuleOptimizeFallbacks{0};
thread_local std::uint64_t threadOptimizerNs = 0;

struct OptimizerTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~OptimizerTimer() { threadOptimizerNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); }
};

std::size_t LargeOptimizeMinWords() {
    const char* value = std::getenv("APS5_SPIRV_LARGE_OPT_MIN_WORDS");
    if (value == nullptr || *value == '\0') return 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0') return 0;
    return static_cast<std::size_t>(parsed);
}

bool StackHoldsOptimizer() {
#ifdef _WIN32
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
#else
    struct Bounds {
        std::uintptr_t low = 0;
        std::uintptr_t high = 0;
        Bounds() {
            pthread_attr_t attributes;
            if (pthread_getattr_np(pthread_self(), &attributes) != 0) return;
            void* base = nullptr;
            std::size_t size = 0;
            if (pthread_attr_getstack(&attributes, &base, &size) == 0 && base != nullptr) {
                low = reinterpret_cast<std::uintptr_t>(base);
                high = low + size;
            }
            pthread_attr_destroy(&attributes);
        }
    };
    thread_local const Bounds bounds;
    const auto low = bounds.low;
    const auto high = bounds.high;
#endif
    const auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    return frame >= low && frame < high && frame - low >= InlineOptimizerStackBytes;
}

bool HasLimitedUseTypes(std::span<const std::uint32_t> spirv) {
    if (spirv.size() < 5) throw std::runtime_error("SPIRV-Tools: incomplete module header");
    bool int8 = false, int16 = false, float16 = false;
    bool int8Capability = false, int16Capability = false, float16Capability = false;
    const bool swapped = spirv[0] != spv::MagicNumber;
    const auto wordAt = [spirv, swapped](std::size_t index) {
        const auto word = spirv[index];
        return swapped ? (word >> 24u) | ((word >> 8u) & 0x0000ff00u) |
                             ((word << 8u) & 0x00ff0000u) | (word << 24u)
                       : word;
    };
    if (wordAt(0) != spv::MagicNumber) throw std::runtime_error("SPIRV-Tools: invalid module magic");
    for (std::size_t offset = 5; offset < spirv.size();) {
        const auto instruction = wordAt(offset);
        const auto wordCount = instruction >> spv::WordCountShift;
        const auto opcode = instruction & spv::OpCodeMask;
        if (wordCount == 0 || wordCount > spirv.size() - offset ||
            (opcode == spv::OpCapability && wordCount < 2) ||
            (opcode == spv::OpTypeInt && wordCount < 4) ||
            (opcode == spv::OpTypeFloat && wordCount < 3)) {
            throw std::runtime_error("SPIRV-Tools: malformed instruction while scanning narrow types");
        }
        switch (opcode) {
        case spv::OpCapability:
            int8Capability |= wordAt(offset + 1) == spv::CapabilityInt8 ||
                              wordAt(offset + 1) == spv::CapabilityDotProductInput4x8Bit;
            int16Capability |= wordAt(offset + 1) == spv::CapabilityInt16;
            float16Capability |= wordAt(offset + 1) == spv::CapabilityFloat16;
            break;
        case spv::OpTypeInt:
            int8 |= wordAt(offset + 2) == 8;
            int16 |= wordAt(offset + 2) == 16;
            break;
        case spv::OpTypeFloat:
            float16 |= wordCount == 3 && wordAt(offset + 2) == 16;
            break;
        case spv::OpFunction:
            return (int8 && !int8Capability) || (int16 && !int16Capability) || (float16 && !float16Capability);
        }
        offset += wordCount;
    }
    return (int8 && !int8Capability) || (int16 && !int16Capability) || (float16 && !float16Capability);
}

std::vector<std::uint32_t> CollapseBlockBallots(std::span<const std::uint32_t> spirv) {
    if (spirv.size() < 5) throw std::runtime_error("SPIRV-Tools: incomplete module header");
    const bool swapped = spirv[0] != spv::MagicNumber;
    const auto native = [swapped](std::uint32_t word) {
        return swapped ? (word >> 24u) | ((word >> 8u) & 0x0000ff00u) |
                             ((word << 8u) & 0x00ff0000u) | (word << 24u)
                       : word;
    };
    std::vector<std::uint32_t> collapsed(spirv.begin(), spirv.begin() + 5);
    collapsed.reserve(spirv.size());
    std::map<std::array<std::uint32_t, 3>, std::uint32_t> ballots;
    for (std::size_t offset = 5; offset < spirv.size();) {
        const auto instruction = native(spirv[offset]);
        const auto wordCount = instruction >> spv::WordCountShift;
        const auto opcode = instruction & spv::OpCodeMask;
        if (wordCount == 0 || wordCount > spirv.size() - offset ||
            (opcode == spv::OpGroupNonUniformBallot && wordCount != 5)) {
            throw std::runtime_error("SPIRV-Tools: malformed instruction while collapsing block ballots");
        }
        switch (opcode) {
        case spv::OpLabel:
        case spv::OpFunction:
        case spv::OpFunctionEnd:
        case spv::OpFunctionCall:
        case spv::OpDemoteToHelperInvocation:
            ballots.clear();
            break;
        case spv::OpGroupNonUniformBallot: {
            const auto [found, inserted] = ballots.try_emplace({spirv[offset + 1], spirv[offset + 3], spirv[offset + 4]}, spirv[offset + 2]);
            if (!inserted) {
                collapsed.insert(collapsed.end(), {native((4u << spv::WordCountShift) | spv::OpCopyObject), spirv[offset + 1], spirv[offset + 2], found->second});
                offset += wordCount;
                continue;
            }
            break;
        }
        }
        collapsed.insert(collapsed.end(), spirv.begin() + offset, spirv.begin() + offset + wordCount);
        offset += wordCount;
    }
    return collapsed;
}

std::optional<std::vector<std::uint32_t>> OptimizeLargeModule(std::span<const std::uint32_t> input, spv_target_env environment, const spvtools::MessageConsumer& consumer, const spvtools::OptimizerOptions& options, bool allowFolding, const spvtools::SpirvTools& tools, const spvtools::ValidatorOptions& validatorOptions, std::string& diagnostics) {
    try {
        std::vector<std::uint32_t> current;
        spvtools::Optimizer extra(environment);
        extra.SetMessageConsumer(consumer);
        extra.RegisterPass(spvtools::CreateLoopUnrollPass(true))
            .RegisterPass(spvtools::CreateDeadBranchElimPass())
            .RegisterPass(spvtools::CreateRedundancyEliminationPass())
            .RegisterPass(spvtools::CreateCombineAccessChainsPass());
        if (allowFolding) extra.RegisterPass(spvtools::CreateSimplificationPass());
        extra.SetValidateAfterAll(false);
        diagnostics.clear();
        if (!extra.Run(input.data(), input.size(), &current, options)) return std::nullopt;
        for (std::uint32_t round = 0; round < LargeOptimizeRounds; ++round) {
            std::size_t converted = 0;
            const auto hoisted = HoistPureSelections(current, &converted);
            if (converted == 0) break;
            spvtools::Optimizer merge(environment);
            merge.SetMessageConsumer(consumer);
            merge.RegisterPass(spvtools::CreateBlockMergePass());
            merge.SetValidateAfterAll(false);
            if (!merge.Run(hoisted.data(), hoisted.size(), &current, options)) return std::nullopt;
        }
        current = CollapseBlockBallots(current);
        diagnostics.clear();
        if (!tools.Validate(current.data(), current.size(), validatorOptions)) return std::nullopt;
        return current;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::uint32_t> ValidateAndOptimizeSpirvInline(std::span<const std::uint32_t> spirv, std::uint32_t vulkanVersion, std::uint32_t spirvVersion, bool allowOffsetTextureOperand, bool optimize, bool specialize) {
    spv_target_env environment;
    const auto apiVersion = vulkanVersion & ~0xfffu;
    std::uint32_t maxSpirvVersion = 0;
    switch (apiVersion) {
    case 0x00400000u: environment = SPV_ENV_VULKAN_1_0; maxSpirvVersion = 0x00010000u; break;
    case 0x00401000u: environment = spirvVersion == 0x00010400u ? SPV_ENV_VULKAN_1_1_SPIRV_1_4 : SPV_ENV_VULKAN_1_1; maxSpirvVersion = 0x00010400u; break;
    case 0x00402000u: environment = SPV_ENV_VULKAN_1_2; maxSpirvVersion = 0x00010500u; break;
    case 0x00403000u: environment = SPV_ENV_VULKAN_1_3; maxSpirvVersion = 0x00010600u; break;
    case 0x00404000u: environment = SPV_ENV_VULKAN_1_4; maxSpirvVersion = 0x00010600u; break;
    default: throw std::runtime_error("SPIRV-Tools: unsupported Vulkan target");
    }
    if (spirvVersion < 0x00010000u || spirvVersion > maxSpirvVersion || (spirvVersion & 0xffu) != 0) {
        throw std::runtime_error("SPIRV-Tools: unsupported Vulkan/SPIR-V target");
    }
    if (spirv.size() >= 5 && spirv[1] > spirvVersion) {
        throw std::runtime_error("SPIRV-Tools: module version exceeds requested SPIR-V target");
    }
    std::string diagnostics;
    const auto consumer = [&diagnostics](spv_message_level_t level, const char* source, const spv_position_t& position, const char* message) {
        if (!diagnostics.empty()) diagnostics += '\n';
        diagnostics += "level=" + std::to_string(static_cast<int>(level)) + " word=" + std::to_string(position.index);
        if (source != nullptr && *source != '\0') diagnostics += " source=" + std::string(source);
        if (message != nullptr) diagnostics += ": " + std::string(message);
    };
    spvtools::SpirvTools tools(environment);
    if (!tools.IsValid()) throw std::runtime_error("SPIRV-Tools: cannot create validator");
    tools.SetMessageConsumer(consumer);
    spvtools::ValidatorOptions validatorOptions;
    validatorOptions.SetAllowOffsetTextureOperand(allowOffsetTextureOperand);
    validatorOptions.SetFriendlyNames(false);
    spvtools::OptimizerOptions options;
    options.set_run_validator(false);
    options.set_preserve_bindings(true);
    options.set_preserve_spec_constants(!specialize);
    options.set_validator_options(validatorOptions);
    static const char* mode = std::getenv("APS5_SPIRV_OPT");
    static const std::size_t largeMinWords = LargeOptimizeMinWords();
    if (mode != nullptr && std::string(mode) == "large") {
        if (!optimize || !specialize || largeMinWords == 0 || spirv.size() < largeMinWords) return {spirv.begin(), spirv.end()};
        bool allowFolding = false;
        try {
            allowFolding = !HasLimitedUseTypes(spirv);
        } catch (...) {
            largeModuleOptimizeFallbacks.fetch_add(1, std::memory_order_relaxed);
            return {spirv.begin(), spirv.end()};
        }
        auto large = OptimizeLargeModule(spirv, environment, consumer, options, allowFolding, tools, validatorOptions, diagnostics);
        if (large.has_value()) return std::move(*large);
        largeModuleOptimizeFallbacks.fetch_add(1, std::memory_order_relaxed);
        return {spirv.begin(), spirv.end()};
    }
    if (!tools.Validate(spirv.data(), spirv.size(), validatorOptions)) {
        throw std::runtime_error("SPIR-V validation before optimization failed:\n" + diagnostics);
    }
    if (!optimize) return CollapseBlockBallots(spirv);
    spvtools::Optimizer optimizer(environment);
    optimizer.SetMessageConsumer(consumer);
    if (mode != nullptr && std::string(mode) == "none") return CollapseBlockBallots(spirv);
    // The recompiler emits helpers (BDA lookup, fault reporting) as functions called from every memory
    // access. Exhaustive inlining multiplies module size by ~10x and optimization time by ~20x, so the
    // default pipeline keeps the performance passes that work per function and leaves inlining to the driver.
    // APS5_SPIRV_OPT=full restores the SPIRV-Tools performance pipeline, =none skips optimization.
    const bool allowFolding = !HasLimitedUseTypes(spirv);
    const bool full = mode != nullptr && std::string(mode) == "full";
    if (full && allowFolding) {
        optimizer.RegisterPerformancePasses(true);
    } else {
        optimizer.RegisterPass(spvtools::CreateWrapOpKillPass())
            .RegisterPass(spvtools::CreateDeadBranchElimPass());
        if (full) {
            optimizer.RegisterPass(spvtools::CreateMergeReturnPass())
                .RegisterPass(spvtools::CreateInlineExhaustivePass())
                .RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
        }
        optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true))
            .RegisterPass(spvtools::CreatePrivateToLocalPass())
            .RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass())
            .RegisterPass(spvtools::CreateLocalSingleStoreElimPass())
            .RegisterPass(spvtools::CreateAggressiveDCEPass(true))
            .RegisterPass(spvtools::CreateScalarReplacementPass(ScalarReplacementLimit))
            .RegisterPass(spvtools::CreateLocalAccessChainConvertPass())
            .RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass())
            .RegisterPass(spvtools::CreateLocalSingleStoreElimPass())
            .RegisterPass(spvtools::CreateAggressiveDCEPass(true))
            .RegisterPass(spvtools::CreateLocalMultiStoreElimPass())
            .RegisterPass(spvtools::CreateAggressiveDCEPass(true));
        if (allowFolding) optimizer.RegisterPass(spvtools::CreateCCPPass());
        optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true));
        if (full) optimizer.RegisterPass(spvtools::CreateLoopUnrollPass(true));
        optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass())
            .RegisterPass(spvtools::CreateRedundancyEliminationPass());
        if (full) optimizer.RegisterPass(spvtools::CreateCombineAccessChainsPass());
        if (allowFolding) optimizer.RegisterPass(spvtools::CreateSimplificationPass());
        if (full) {
            optimizer.RegisterPass(spvtools::CreateScalarReplacementPass(ScalarReplacementLimit))
                .RegisterPass(spvtools::CreateLocalAccessChainConvertPass())
                .RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass())
                .RegisterPass(spvtools::CreateLocalSingleStoreElimPass())
                .RegisterPass(spvtools::CreateAggressiveDCEPass(true))
                .RegisterPass(spvtools::CreateSSARewritePass())
                .RegisterPass(spvtools::CreateAggressiveDCEPass(true))
                .RegisterPass(spvtools::CreateVectorDCEPass())
                .RegisterPass(spvtools::CreateDeadInsertElimPass())
                .RegisterPass(spvtools::CreateDeadBranchElimPass())
                .RegisterPass(spvtools::CreateIfConversionPass())
                .RegisterPass(spvtools::CreateCopyPropagateArraysPass())
                .RegisterPass(spvtools::CreateReduceLoadSizePass());
        }
        optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true))
            .RegisterPass(spvtools::CreateBlockMergePass());
        if (full) {
            optimizer.RegisterPass(spvtools::CreateRedundancyEliminationPass())
                .RegisterPass(spvtools::CreateDeadBranchElimPass())
                .RegisterPass(spvtools::CreateBlockMergePass());
        }
        if (allowFolding) optimizer.RegisterPass(spvtools::CreateSimplificationPass());
    }
    // Validating after every pass costs more than the passes on large shaders; the result is validated below.
    optimizer.SetValidateAfterAll(false);
    std::vector<std::uint32_t> optimized;
    diagnostics.clear();
    if (!optimizer.Run(spirv.data(), spirv.size(), &optimized, options)) {
        throw std::runtime_error("SPIR-V optimization failed:\n" + diagnostics);
    }
    optimized = CollapseBlockBallots(optimized);
    diagnostics.clear();
    if (!tools.Validate(optimized.data(), optimized.size(), validatorOptions)) {
        throw std::runtime_error("SPIR-V validation after optimization failed:\n" + diagnostics);
    }
    if (largeMinWords != 0 && !full && spirv.size() >= largeMinWords) {
        auto large = OptimizeLargeModule(optimized, environment, consumer, options, allowFolding, tools, validatorOptions, diagnostics);
        if (large.has_value()) return std::move(*large);
        largeModuleOptimizeFallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    return optimized;
}

}

std::vector<std::uint32_t> HoistPureSelections(std::span<const std::uint32_t> spirv, std::size_t* converted) {
    using Words = std::vector<std::uint32_t>;
    struct Block {
        std::uint32_t id = 0;
        bool alive = true;
        std::vector<Words> body;
    };
    struct Function {
        std::vector<Words> head;
        std::vector<Block> blocks;
        Words tail;
    };
    if (converted != nullptr) *converted = 0;
    if (spirv.size() < 5) throw std::runtime_error("SPIRV-Tools: incomplete module header");
    if (spirv[0] != spv::MagicNumber) throw std::runtime_error("SPIRV-Tools: unsupported module byte order");
    const auto opcodeOf = [](const Words& words) { return static_cast<spv::Op>(words[0] & spv::OpCodeMask); };
    std::vector<Words> pre;
    std::vector<Function> functions;
    Function* current = nullptr;
    Block* block = nullptr;
    for (std::size_t offset = 5; offset < spirv.size();) {
        const auto wordCount = spirv[offset] >> spv::WordCountShift;
        if (wordCount == 0 || wordCount > spirv.size() - offset) {
            throw std::runtime_error("SPIRV-Tools: malformed instruction while hoisting selections");
        }
        Words words(spirv.begin() + offset, spirv.begin() + offset + wordCount);
        offset += wordCount;
        const auto opcode = opcodeOf(words);
        if (opcode == spv::OpFunction) {
            functions.emplace_back();
            current = &functions.back();
            current->head.push_back(std::move(words));
            block = nullptr;
            continue;
        }
        if (current == nullptr) {
            pre.push_back(std::move(words));
            continue;
        }
        if (opcode == spv::OpFunctionEnd) {
            current->tail = std::move(words);
            current = nullptr;
            block = nullptr;
            continue;
        }
        if (opcode == spv::OpLabel) {
            if (words.size() < 2) throw std::runtime_error("SPIRV-Tools: malformed label while hoisting selections");
            current->blocks.push_back(Block{words[1], true, {}});
            block = &current->blocks.back();
            continue;
        }
        if (block == nullptr) current->head.push_back(std::move(words));
        else block->body.push_back(std::move(words));
    }
    if (current != nullptr) throw std::runtime_error("SPIRV-Tools: unterminated function while hoisting selections");

    const auto version = spirv[1];
    std::unordered_map<std::uint32_t, std::uint32_t> typeKind;
    std::unordered_set<std::uint32_t> pointerTypes;
    std::unordered_map<std::uint32_t, std::uint32_t> variableStorage;
    std::unordered_map<std::uint32_t, std::uint32_t> chainBase;
    std::uint32_t glslSet = 0;
    const auto scan = [&](const Words& words) {
        switch (opcodeOf(words)) {
        case spv::OpTypeBool:
        case spv::OpTypeInt:
        case spv::OpTypeFloat:
            if (words.size() >= 2) typeKind[words[1]] = 1;
            break;
        case spv::OpTypeVector:
            if (words.size() >= 3) {
                const auto component = typeKind.find(words[2]);
                if (component != typeKind.end() && component->second == 1) typeKind[words[1]] = 2;
            }
            break;
        case spv::OpTypePointer:
        case spv::OpTypeForwardPointer:
            if (words.size() >= 2) pointerTypes.insert(words[1]);
            break;
        case spv::OpVariable:
            if (words.size() >= 4) variableStorage[words[2]] = words[3];
            break;
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
        case spv::OpPtrAccessChain:
            if (words.size() >= 4) chainBase[words[2]] = words[3];
            break;
        case spv::OpExtInstImport:
            if (words.size() >= 3) {
                std::string name;
                bool terminated = false;
                for (std::size_t index = 2; index < words.size() && !terminated; ++index) {
                    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
                        const auto character = static_cast<char>((words[index] >> shift) & 0xffu);
                        if (character == '\0') {
                            terminated = true;
                            break;
                        }
                        name += character;
                    }
                }
                if (name == "GLSL.std.450") glslSet = words[1];
            }
            break;
        default:
            break;
        }
    };
    for (const auto& words : pre) scan(words);
    for (const auto& function : functions) {
        for (const auto& words : function.head) scan(words);
        for (const auto& candidate : function.blocks) {
            for (const auto& words : candidate.body) scan(words);
        }
    }

    const auto loadIsSafe = [&](const Words& words) {
        if (words.size() != 4) return false;
        auto pointer = words[3];
        for (std::size_t depth = 0; depth < 64; ++depth) {
            const auto chain = chainBase.find(pointer);
            if (chain == chainBase.end()) break;
            pointer = chain->second;
        }
        const auto variable = variableStorage.find(pointer);
        if (variable == variableStorage.end()) return false;
        return variable->second == spv::StorageClassInput || variable->second == spv::StorageClassFunction ||
               variable->second == spv::StorageClassPushConstant || variable->second == spv::StorageClassStorageBuffer;
    };
    const auto hoistable = [&](const Words& words) {
        const auto opcode = opcodeOf(words);
        if (opcode == spv::OpExtInst) {
            if (words.size() < 5 || glslSet == 0 || words[3] != glslSet) return false;
            switch (words[4]) {
            case GLSLstd450Modf:
            case GLSLstd450Frexp:
            case GLSLstd450FrexpStruct:
            case GLSLstd450InterpolateAtCentroid:
            case GLSLstd450InterpolateAtSample:
            case GLSLstd450InterpolateAtOffset:
                return false;
            default:
                return true;
            }
        }
        if (opcode == spv::OpLoad) return loadIsSafe(words);
        if (opcode == spv::OpConvertPtrToU || opcode == spv::OpConvertUToPtr) return false;
        switch (opcode) {
        case spv::OpNop:
        case spv::OpUndef:
        case spv::OpLine:
        case spv::OpNoLine:
        case spv::OpCopyObject:
        case spv::OpVectorExtractDynamic:
        case spv::OpVectorInsertDynamic:
        case spv::OpVectorShuffle:
        case spv::OpCompositeConstruct:
        case spv::OpCompositeExtract:
        case spv::OpCompositeInsert:
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
            return true;
        default:
            break;
        }
        return (opcode >= spv::OpConvertFToU && opcode <= spv::OpBitcast) ||
               (opcode >= spv::OpSNegate && opcode <= spv::OpSMulExtended) ||
               (opcode >= spv::OpAny && opcode <= spv::OpFUnordGreaterThanEqual) ||
               (opcode >= spv::OpShiftRightLogical && opcode <= spv::OpNot) ||
               (opcode >= spv::OpBitFieldInsert && opcode <= spv::OpBitCount);
    };
    const auto phiTypeAllowed = [&](std::uint32_t type) {
        if (pointerTypes.count(type) != 0) return false;
        const auto kind = typeKind.find(type);
        if (kind == typeKind.end()) return false;
        return kind->second == 1 || (kind->second == 2 && version >= SelectOnVectorsVersion);
    };

    std::size_t total = 0;
    for (auto& function : functions) {
        auto& blocks = function.blocks;
        std::unordered_map<std::uint32_t, std::size_t> index;
        bool supported = true;
        for (std::size_t position = 0; position < blocks.size(); ++position) {
            index[blocks[position].id] = position;
            const auto& body = blocks[position].body;
            if (body.empty() || opcodeOf(body.back()) == spv::OpLine || opcodeOf(body.back()) == spv::OpNoLine) {
                supported = false;
            }
        }
        if (!supported) continue;
        bool changed = true;
        while (changed) {
            changed = false;
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> preds;
            std::unordered_set<std::uint32_t> mergeTargets;
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> phiParents;
            for (const auto& candidate : blocks) {
                if (!candidate.alive) continue;
                for (const auto& words : candidate.body) {
                    switch (opcodeOf(words)) {
                    case spv::OpSelectionMerge:
                        if (words.size() >= 2) mergeTargets.insert(words[1]);
                        break;
                    case spv::OpLoopMerge:
                        if (words.size() >= 3) {
                            mergeTargets.insert(words[1]);
                            mergeTargets.insert(words[2]);
                        }
                        break;
                    case spv::OpPhi:
                        for (std::size_t operand = 4; operand < words.size(); operand += 2) {
                            phiParents[words[operand]].push_back(candidate.id);
                        }
                        break;
                    default:
                        break;
                    }
                }
                const auto& last = candidate.body.back();
                switch (opcodeOf(last)) {
                case spv::OpBranch:
                    if (last.size() >= 2) preds[last[1]].push_back(candidate.id);
                    break;
                case spv::OpBranchConditional:
                    if (last.size() >= 4) {
                        preds[last[2]].push_back(candidate.id);
                        preds[last[3]].push_back(candidate.id);
                    }
                    break;
                case spv::OpSwitch:
                    if (last.size() >= 3) {
                        preds[last[2]].push_back(candidate.id);
                        for (std::size_t operand = 4; operand < last.size(); operand += 2) {
                            preds[last[operand]].push_back(candidate.id);
                        }
                    }
                    break;
                default:
                    break;
                }
            }
            for (std::size_t headerPosition = 0; headerPosition < blocks.size(); ++headerPosition) {
                auto& header = blocks[headerPosition];
                if (!header.alive || header.body.size() < 2) continue;
                const auto& mergeInstruction = header.body[header.body.size() - 2];
                const auto& branch = header.body.back();
                if (opcodeOf(mergeInstruction) != spv::OpSelectionMerge || mergeInstruction.size() < 2 ||
                    opcodeOf(branch) != spv::OpBranchConditional || branch.size() < 4) {
                    continue;
                }
                const auto merge = mergeInstruction[1];
                const auto condition = branch[1];
                const auto trueTarget = branch[2];
                const auto falseTarget = branch[3];
                if (trueTarget == falseTarget || merge == header.id) continue;
                const auto mergeIndex = index.find(merge);
                if (mergeIndex == index.end() || !blocks[mergeIndex->second].alive) continue;
                std::vector<std::uint32_t> arms;
                for (const auto target : {trueTarget, falseTarget}) {
                    if (target != merge) arms.push_back(target);
                }
                bool ok = true;
                std::vector<Words> hoisted;
                for (const auto arm : arms) {
                    const auto armIndex = index.find(arm);
                    if (arm == header.id || armIndex == index.end() || !blocks[armIndex->second].alive) {
                        ok = false;
                        break;
                    }
                    const auto& armBlock = blocks[armIndex->second];
                    const auto armPreds = preds.find(arm);
                    if (armPreds == preds.end() || armPreds->second.size() != 1 || armPreds->second[0] != header.id ||
                        mergeTargets.count(arm) != 0) {
                        ok = false;
                        break;
                    }
                    const auto parents = phiParents.find(arm);
                    if (parents != phiParents.end() &&
                        std::any_of(parents->second.begin(), parents->second.end(), [merge](std::uint32_t id) { return id != merge; })) {
                        ok = false;
                        break;
                    }
                    const auto& terminator = armBlock.body.back();
                    if (opcodeOf(terminator) != spv::OpBranch || terminator.size() < 2 || terminator[1] != merge) {
                        ok = false;
                        break;
                    }
                    for (std::size_t position = 0; position + 1 < armBlock.body.size(); ++position) {
                        if (!hoistable(armBlock.body[position])) {
                            ok = false;
                            break;
                        }
                        hoisted.push_back(armBlock.body[position]);
                    }
                    if (!ok) break;
                }
                if (!ok) continue;
                std::vector<std::uint32_t> expected = arms;
                if (arms.size() == 1) expected.push_back(header.id);
                auto actual = preds[merge];
                std::sort(expected.begin(), expected.end());
                std::sort(actual.begin(), actual.end());
                if (actual != expected) continue;
                auto& mergeBlock = blocks[mergeIndex->second];
                const auto trueSource = trueTarget != merge ? trueTarget : header.id;
                const auto falseSource = falseTarget != merge ? falseTarget : header.id;
                std::vector<Words> selects;
                std::vector<Words> remaining;
                for (const auto& words : mergeBlock.body) {
                    if (opcodeOf(words) != spv::OpPhi) {
                        remaining.push_back(words);
                        continue;
                    }
                    if (words.size() < 5 || (words.size() & 1u) == 0 || !phiTypeAllowed(words[1])) {
                        ok = false;
                        break;
                    }
                    std::uint32_t trueValue = 0, falseValue = 0;
                    bool hasTrue = false, hasFalse = false;
                    for (std::size_t operand = 3; operand + 1 < words.size(); operand += 2) {
                        if (words[operand + 1] == trueSource) {
                            trueValue = words[operand];
                            hasTrue = true;
                        }
                        if (words[operand + 1] == falseSource) {
                            falseValue = words[operand];
                            hasFalse = true;
                        }
                    }
                    if (!hasTrue || !hasFalse) {
                        ok = false;
                        break;
                    }
                    selects.push_back({(6u << spv::WordCountShift) | spv::OpSelect, words[1], words[2], condition, trueValue, falseValue});
                }
                if (!ok) continue;
                header.body.pop_back();
                header.body.pop_back();
                for (auto& words : hoisted) header.body.push_back(std::move(words));
                for (auto& words : selects) header.body.push_back(std::move(words));
                header.body.push_back({(2u << spv::WordCountShift) | spv::OpBranch, merge});
                for (const auto arm : arms) blocks[index[arm]].alive = false;
                mergeBlock.body = std::move(remaining);
                preds[merge] = {header.id};
                ++total;
                changed = true;
            }
        }
    }
    if (converted != nullptr) *converted = total;

    Words output(spirv.begin(), spirv.begin() + 5);
    output.reserve(spirv.size());
    const auto emit = [&output](const Words& words) { output.insert(output.end(), words.begin(), words.end()); };
    for (const auto& words : pre) emit(words);
    for (const auto& function : functions) {
        for (const auto& words : function.head) emit(words);
        for (const auto& candidate : function.blocks) {
            if (!candidate.alive) continue;
            output.insert(output.end(), {(2u << spv::WordCountShift) | spv::OpLabel, candidate.id});
            for (const auto& words : candidate.body) emit(words);
        }
        emit(function.tail);
    }
    return output;
}

std::vector<std::uint32_t> ValidateAndOptimizeSpirv(std::span<const std::uint32_t> spirv, std::uint32_t vulkanVersion, std::uint32_t spirvVersion, bool allowOffsetTextureOperand, bool optimize, bool specialize) {
    const OptimizerTimer timer;
    if (StackHoldsOptimizer()) return ValidateAndOptimizeSpirvInline(spirv, vulkanVersion, spirvVersion, allowOffsetTextureOperand, optimize, specialize);
    std::vector<std::uint32_t> result;
    std::exception_ptr failure;
    std::thread host([&] {
        try {
            result = ValidateAndOptimizeSpirvInline(spirv, vulkanVersion, spirvVersion, allowOffsetTextureOperand, optimize, specialize);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    optimizerHostThreads.fetch_add(1, std::memory_order_relaxed);
    host.join();
    if (failure) std::rethrow_exception(failure);
    return result;
}

std::vector<std::uint32_t> BenchVariantSpirv(std::span<const std::uint32_t> spirv, int variant, std::uint32_t nonce) {
    if (spirv.size() < 5) return {};
    std::vector<std::uint32_t> body(spirv.begin(), spirv.end());
    if (variant != 0) {
        const auto version = spirv[1];
        const spv_target_env environment = version >= 0x00010600u ? SPV_ENV_VULKAN_1_3 : version >= 0x00010500u ? SPV_ENV_VULKAN_1_2 : version >= 0x00010400u ? SPV_ENV_VULKAN_1_1_SPIRV_1_4 : SPV_ENV_VULKAN_1_0;
        spvtools::Optimizer optimizer(environment);
        optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char*) {});
        if (variant == 1) optimizer.RegisterPerformancePasses(true);
        else optimizer.RegisterSizePasses(true);
        spvtools::OptimizerOptions options;
        options.set_run_validator(false);
        options.set_preserve_bindings(true);
        options.set_preserve_spec_constants(true);
        std::vector<std::uint32_t> result;
        std::thread host([&] {
            try {
                if (!optimizer.Run(body.data(), body.size(), &result, options)) result.clear();
            } catch (...) {
                result.clear();
            }
        });
        host.join();
        if (result.size() < 5) return {};
        body = std::move(result);
    }
    std::size_t cursor = 5;
    while (cursor < body.size()) {
        const auto op = body[cursor] & 0xffffu;
        const auto count = body[cursor] >> 16u;
        if (count == 0u || (op != 17u && op != 10u && op != 11u && op != 14u && op != 15u && op != 16u && op != 331u)) break;
        cursor += count;
    }
    char text[20]{};
    std::snprintf(text, sizeof text, "aps5b%08x", nonce);
    std::array<std::uint32_t, 5> tag{(5u << 16u) | 4u, 0u, 0u, 0u, 0u};
    std::memcpy(tag.data() + 1, text, 16);
    body.insert(body.begin() + static_cast<std::ptrdiff_t>(cursor), tag.begin(), tag.end());
    return body;
}

std::uint64_t ThreadOptimizerNs() {
    return threadOptimizerNs;
}

std::uint64_t OptimizerHostThreads() {
    return optimizerHostThreads.load(std::memory_order_relaxed);
}

std::uint64_t LargeModuleOptimizeFallbacks() {
    return largeModuleOptimizeFallbacks.load(std::memory_order_relaxed);
}

}
