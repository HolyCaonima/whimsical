#pragma once
#include "physics/dynamics/model/Model.h"
namespace whimsical::dynamics {
enum class StateField { Value, Velocity, Acceleration, History };
struct StateWrite {
    StateField field = StateField::Value;
    SetId set = 0;
    uint32_t first = 0;
    std::vector<float> values; // row-major, complete rows
};
struct TickInput {
    uint64_t tick = 0, modelVersion = 0;
    float dt = 1.f / 60.f;
    uint64_t profileRequest = 0;
    std::vector<StateWrite> writes;
    struct Endpoints {
        SetId set = 0;
        uint32_t first = 0;
        std::vector<std::vector<VariableRef>> columns;
    };
    // Supplied dynamic rows reset their persistent history at the tick boundary.
    std::vector<Endpoints> endpoints;
};
struct CompletedState {
    uint64_t model = 0, modelVersion = 0, tick = 0;
    double time = 0, gpuMilliseconds = 0;
    uint32_t invalidEvaluations = 0, singularSystems = 0;
};
} // namespace whimsical::dynamics
