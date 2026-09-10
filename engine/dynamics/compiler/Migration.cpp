#include "dynamics/compiler/FormulaGlsl.h"
#include "CompiledPlan.h"
#include <algorithm>
#include <stdexcept>
namespace whimsical::dynamics {
StateMigration Compiler::migration(const CompiledPlan& previous, const CompiledPlan& next) const {
    if (previous.model.model != next.model.model || previous.model.version >= next.model.version)
        throw std::invalid_argument("Migration must advance the same model");
    StateMigration result;
    auto& mapping = result.words;
    auto mapField = [&](uint32_t kind, uint32_t from, uint32_t to, uint32_t oldStride, uint32_t newStride,
                        uint32_t width, uint32_t count) {
        for (uint32_t c = 0; c < width; ++c)
            for (uint32_t i = 0; i < count; ++i)
                mapping.insert(mapping.end(), {kind, to + c * newStride + i, from + c * oldStride + i});
    };
    for (uint32_t id = 0; id < std::min(previous.variables.size(), next.variables.size()); ++id) {
        const auto& a = previous.variables[id];
        const auto& b = next.variables[id];
        const auto& before = *previous.spaces[a.space];
        const auto& after = *next.spaces[b.space];
        if (before.stateSize != after.stateSize || before.tangentSize != after.tangentSize ||
            emitGlsl(before.retract, "r") != emitGlsl(after.retract, "r") ||
            emitGlsl(before.difference, "d") != emitGlsl(after.difference, "d"))
            throw std::invalid_argument("Cannot migrate an existing variable into a different space");
        auto count = std::min(a.count, b.count);
        mapField(0, a.values, b.values, a.count, b.count, before.stateSize, count);
        mapField(1, a.velocity, b.velocity, a.count, b.count, before.tangentSize, count);
        mapField(3, a.velocity, b.velocity, a.count, b.count, before.tangentSize, count);
    }
    for (uint32_t id = 0; id < std::min(previous.relations.size(), next.relations.size()); ++id) {
        const auto& a = previous.relations[id];
        const auto& b = next.relations[id];
        const auto& type = *next.types[b.type];
        const auto& oldType = *previous.types[a.type];
        if (type.history != oldType.history || emitGlsl(type.residual, "r") != emitGlsl(oldType.residual, "r"))
            continue;
        const auto& before = previous.model.data->relations[id];
        const auto& after = next.model.data->relations[id];
        if (before.dynamicEndpoints || after.dynamicEndpoints ||
            before.endpoints.size() != after.endpoints.size())
            continue;
        for (uint32_t i = 0; i < std::min(a.count, b.count); ++i) {
            bool same = true;
            for (size_t e = 0; e < before.endpoints.size(); ++e)
                same = same && before.endpoints[e]->at(i) == after.endpoints[e]->at(i);
            if (same)
                for (uint32_t c = 0; c < type.history; ++c)
                    mapping.insert(mapping.end(),
                                   {2, b.history + c * b.count + i, a.history + c * a.count + i});
        }
    }
    return result;
}
} // namespace whimsical::dynamics
