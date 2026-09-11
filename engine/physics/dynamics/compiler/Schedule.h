#pragma once
#include "CompiledPlan.h"
#include <utility>

namespace whimsical::dynamics {
// A numerical operation is callable by either a global dispatch or a region lane.
// Early returns leave this operation, never the enclosing synchronization scope.
struct KernelFunction {
    std::string entry, source;
    BufferRole work;
    bool writesContributions = false;
};
KernelFunction variableFunction(const Space&, const char* operation, const std::string& name);
KernelFunction relationFunction(const RelationType&, const std::vector<bool>& readOnly,
                                int32_t endpointMode, bool jacobi, bool update, const std::string& name);
KernelFunction relationDispatchFunction(
    const std::vector<std::pair<uint32_t, KernelFunction>>&, bool jacobi, const std::string& name);
std::string globalKernel(const KernelFunction&);
std::string stateAccess(bool local, uint32_t variableCount = 0, bool externalInputs = false);

// Preserve the semantic batch ordering; choose ownership and execution placement.
// Layouts and work IDs remain stable across this lowering pass.
void lowerSchedule(CompiledPlan&, const std::vector<KernelFunction>&);
} // namespace whimsical::dynamics
