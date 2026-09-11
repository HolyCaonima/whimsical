#pragma once
#include "physics/dynamics/model/Expression.h"
#include <optional>
#include <utility>

namespace whimsical::dynamics {
// GPU code generation belongs to the compiler; Formula remains mathematical data.
std::string emitGlsl(const Formula&, const std::string& name);
// Emit a compact row-major Jacobian for only the requested input columns.
// This is a compiler projection; the mathematical Formula remains unchanged.
std::string emitGlslDerivative(const Formula&, const std::string& name,
                               const std::vector<uint32_t>& derivativeInputs);
// Emit only the requested Jacobian. This avoids evaluating the primal outputs
// when a caller needs a local linearization but not the mapped value.
std::string emitGlslJacobian(const Formula&, const std::string& name,
                            const std::vector<uint32_t>& derivativeInputs);
// Returns the projected Jacobian when every entry is compile-time constant.
// This lets downstream lowering compose maps without materializing a matrix.
std::optional<std::vector<float>> constantJacobian(
    const Formula&, const std::vector<uint32_t>& derivativeInputs);
// A violated sum-of-squares bound implies a finite interval for each input
// difference. This is a mathematical proof, independent of objects and solvers.
struct DifferenceBound {
    std::vector<std::pair<uint32_t, uint32_t>> coordinates;
    Formula squaredRadius;
};
std::optional<DifferenceBound> differenceBound(const Formula&, bool nonnegative,
                                              uint32_t parameterFirst, uint32_t parameterCount);
} // namespace whimsical::dynamics
