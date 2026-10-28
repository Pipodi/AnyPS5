#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DISPATCHCREATECONTEXT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DISPATCHCREATECONTEXT_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics {

struct DispatchCreateContext {
    enum class MissAheadOutcome { NotCalled, Disabled, PlanActive, Resolving, Covered, FirstPacket, CapZero, Scanned };

    std::uint32_t queue = 0;
    std::uint64_t serial = 0;
    std::size_t offset = 0;
    std::uint64_t program = 0;
    bool indirect = false;
    bool fromAhead = false;
    bool cached = false;
    bool dataHit = false;
    bool missItem = false;
    bool pipelined = false;
    bool pipelineCandidate = false;
    MissAheadOutcome outcome = MissAheadOutcome::NotCalled;
    bool (*covered)(std::uint64_t serial, std::uint32_t queue, std::size_t offset) = nullptr;

    static DispatchCreateContext*& Current() {
        thread_local DispatchCreateContext* current = nullptr;
        return current;
    }

    static const char* Name(MissAheadOutcome value) {
        switch (value) {
        case MissAheadOutcome::NotCalled: return "not_called";
        case MissAheadOutcome::Disabled: return "disabled";
        case MissAheadOutcome::PlanActive: return "plan_active";
        case MissAheadOutcome::Resolving: return "resolving";
        case MissAheadOutcome::Covered: return "covered";
        case MissAheadOutcome::FirstPacket: return "first_packet";
        case MissAheadOutcome::CapZero: return "cap_zero";
        case MissAheadOutcome::Scanned: return "scanned";
        }
        return "unknown";
    }

    class Scope {
    public:
        explicit Scope(DispatchCreateContext& context) : previous(Current()) { Current() = &context; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { Current() = previous; }

    private:
        DispatchCreateContext* previous;
    };
};

}

#endif
