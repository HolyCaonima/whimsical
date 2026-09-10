#pragma once
#include "dynamics/model/Expression.h"

namespace whimsical::dynamics {
// GPU code generation belongs to the compiler; Formula remains mathematical data.
std::string emitGlsl(const Formula&, const std::string& name, bool derivatives = false);
} // namespace whimsical::dynamics