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
KernelFunction variableFunction(
    const Space&, const char* operation, const std::string& name, bool directContributions = false);
// The same small-block linear solve is used by fixed and summed relations.
std::string solveBlock(uint32_t rows);
KernelFunction relationFunction(const RelationType&, const std::vector<bool>& readOnly,
                                int32_t endpointMode, bool jacobi, bool directContributions,
                                bool separateDegrees, bool update, const std::string& name,
                                bool activeDegrees = false, bool activityChecked = false,
                                bool distinctWritableEndpoints = false);
// The predicate counts unique enabled writable endpoints only when a relation
// participates in this Jacobi snapshot. Invalid residuals reach the solve's
// existing diagnostic path rather than disappearing during candidate selection.
// mappedEndpoints adds left/right member arguments for an already-decoded
// affine invocation; the predicate and endpoint field loads are shared.
KernelFunction relationActivityFunction(const RelationType&, const std::vector<bool>& readOnly,
                                       int32_t endpointMode, const std::string& name, bool countDegrees = true,
                                       bool mappedEndpoints = false, bool residualChecked = false);
KernelFunction relationDispatchFunction(
    const std::vector<std::pair<uint32_t, KernelFunction>>&, bool jacobi, const std::string& name);
std::string globalKernel(const KernelFunction&);
std::string fieldAccess(bool local);
enum class StateStorage { Global, Region, Epoch };
std::string stateAccess(StateStorage, uint32_t variableCount = 0, bool externalInputs = false);

// Preserve the semantic batch ordering; choose ownership and execution placement.
// Layouts and work IDs remain stable across this lowering pass.
void lowerSchedule(CompiledPlan&, const std::vector<KernelFunction>&);
bool lowerTiledSchedule(CompiledPlan&, const std::vector<KernelFunction>&);
bool canDeferCandidateDomain(const CompiledPlan&, uint32_t set, const BindingDomain&);
void lowerCandidateDomains(CompiledPlan&);
void prunePlanKernels(CompiledPlan&);
} // namespace whimsical::dynamics
