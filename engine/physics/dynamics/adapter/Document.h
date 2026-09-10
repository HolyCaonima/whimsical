#pragma once
#include "assets/Json.h"
#include "physics/dynamics/compiler/CompiledPlan.h"
#include "physics/dynamics/runtime/State.h"

namespace whimsical::dynamics {
Formula formula(const Json&);
Json document(const Formula&);
Json document(const ModelSnapshot&);
Object objectFromDocument(const Json&);
PairBinding pairFromDocument(const Json&);
std::unique_ptr<Model> modelFromDocument(const Json&);
SolverPolicy solverPolicy(const Json&);
StateField stateField(const std::string&);
FieldKind modelField(const std::string&);
} // namespace whimsical::dynamics
