#pragma once
#include "physics/dynamics/model/Expression.h"
#include <optional>

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
} // namespace whimsical::dynamics