#pragma once
#include "CompiledPlan.h"

namespace whimsical::dynamics {
// A sum is split at its bound index. Local AD operates on the integrands and
// the outer expression; the generated program composes their derivatives.
struct SumExpression {
    Formula terms, outer;
};
SumExpression splitSums(const Formula&, const RelationType&);
struct SumDomain {
    uint32_t set, domain;
    // Exact static incidence counts, shared across all rows of this domain.
    std::vector<std::pair<uint32_t, uint32_t>> degrees;
    struct Step {
        std::string name, source;
        uint32_t count, sweep;
        std::optional<OwnerOutput> ownerOutput;
    };
    // UINT32_MAX is the shared snapshot preparation epoch; subsequent numbers
    // are ordered block sweeps. Compiler schedules domains without inventing
    // additional numerical work or inspecting a physical storage choice.
    std::vector<Step> program;
    std::optional<OwnerTransform> snapshot;
    std::string update;
    std::vector<uint32_t> appliedVariables;
    bool activeDegrees = true;
    // Ordered candidate preparation. Substep stages run at the first iteration
    // only, for consumers that stay exact under any motion since that build.
    struct IndexStage {
        std::string source;
        uint32_t count;
        bool substep = false;
        uint32_t launch = UINT32_MAX;
    };
    std::vector<IndexStage> index;
    std::vector<std::pair<std::string, uint32_t>> bounds, assembly;
};
// The colored prefix has already been selected. Non-sum Jacobi incidence is
// used to prove whether an operator owns its cumulative correction field.
std::vector<SumDomain> lowerSumRelations(CompiledPlan&, const std::vector<uint32_t>& externalDegrees);
}
