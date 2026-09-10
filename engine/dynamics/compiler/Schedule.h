#pragma once
#include "CompiledPlan.h"

namespace whimsical::dynamics {
// A numerical operation is callable by either a global dispatch or a region lane.
// Early returns leave this operation, never the enclosing synchronization scope.
struct KernelFunction {
    std::string entry, source;
    BufferRole work;
    bool writesContributions = false;
};
KernelFunction variableFunction(const Space&, const char* operation, const std::string& name);
KernelFunction relationFunction(const RelationType&, bool jacobi, bool update, const std::string& name);
std::string globalKernel(const KernelFunction&);
std::string stateAccess(bool local, uint32_t variableCount = 0);

// Preserve the semantic batch ordering; choose ownership and execution placement.
// Layouts and work IDs remain stable across this lowering pass.
void lowerSchedule(CompiledPlan&, const std::vector<KernelFunction>&);
} // namespace whimsical::dynamics
