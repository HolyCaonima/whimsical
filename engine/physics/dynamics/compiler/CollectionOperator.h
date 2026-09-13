#pragma once
#include "CompiledPlan.h"
#include "FormulaGlsl.h"

namespace whimsical::dynamics {
// A derivative factorization over a bound index, before choosing a sparse
// representation or an execution mapping. No resource addresses live here.
struct CollectionOperator {
    Formula outer;
    DifferenceFactor difference;
    DifferenceBound support;
    StagedFormula parameters;
    uint32_t anchorSlot = 0, memberSlot = 0, dimension = 0;
    float scale = 0;
};
std::optional<CollectionOperator> analyzeCollectionOperator(
    const CompiledPlan&, uint32_t set, uint32_t domain);

// Numerical work is selected before candidate storage. Projected line search
// uses d=project(lambda+D^-1*r)-lambda and u=B*J^T*d. Its curvature is
// (J^T*d)^T*B*(J^T*d)+d^T*alpha*d, so one transpose also evaluates the step.
// Cached and streamed rows participate in the same minimization.
struct CollectionSolvePlan {
    enum class Method { Jacobi, ProjectedLineSearch };
    enum class Action { Linearize, Forward, Transpose, LinearizeForward, TransposeRetract,
                        QuadraticTranspose, LineSearchRetract };
    struct Step { Action action; uint32_t sweep; };
    uint32_t sweeps = 4;
    Method method = Method::Jacobi;
    bool ownsCorrection = false;
    std::vector<Step> actions(bool fuseInitialForward) const;
};
}
