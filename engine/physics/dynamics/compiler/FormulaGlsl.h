#pragma once
#include "physics/dynamics/model/Expression.h"

namespace whimsical::dynamics {
// GPU code generation belongs to the compiler; Formula remains mathematical data.
std::string emitGlsl(const Formula&, const std::string& name);
// Emit a compact row-major Jacobian for only the requested input columns.
// This is a compiler projection; the mathematical Formula remains unchanged.
std::string emitGlslDerivative(const Formula&, const std::string& name,
                               const std::vector<uint32_t>& derivativeInputs);
} // namespace whimsical::dynamics