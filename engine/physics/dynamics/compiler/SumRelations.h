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
    std::string solve, update, activity, gather, refine;
    uint32_t gatherCount = 0;
    uint32_t activityCount = 0;
    uint32_t solveCount = 0;
    std::vector<std::pair<std::string, uint32_t>> bounds, index, assembly;
};
std::vector<SumDomain> lowerSumRelations(CompiledPlan&);
}
