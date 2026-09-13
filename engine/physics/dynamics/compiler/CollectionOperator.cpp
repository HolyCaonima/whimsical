#include "CollectionOperator.h"
#include "SumRelations.h"
#include <cmath>
#include <numeric>

namespace whimsical::dynamics {
std::optional<CollectionOperator> analyzeCollectionOperator(
    const CompiledPlan& p, uint32_t set, uint32_t domain) {
    const auto& d = p.bindings[set].domains[domain];
    const auto& t = *p.types[p.relations[set].type];
    const auto& readOnly = p.bindings[set].readOnly;
    if (t.rows != 1 || t.spaces.size() != 2 || d.split != 1 ||
        readOnly[0] || readOnly[1] || d.left != d.right || d.count != d.left ||
        (d.map != BindingDomain::Map::Product && d.map != BindingDomain::Map::Directed)) return {};
    const auto& a = d.fields[0];
    const auto& b = d.fields[1];
    if (a.kind != EndpointSource::Kind::Collection || b.kind != a.kind ||
        !(a.first == b.first) || a.stride != b.stride || !a.stride) return {};
    const auto& space = *t.spaces[0];
    const auto width = space.stateSize;
    if (width != space.tangentSize || t.spaces[0] != t.spaces[1]) return {};
    std::vector<uint32_t> tangent(width);
    std::iota(tangent.begin(), tangent.end(), width);
    const auto map = constantJacobian(space.retract, tangent);
    if (!map) return {};
    for (uint32_t r = 0; r < width; ++r)
        for (uint32_t c = 0; c < width; ++c)
            if ((*map)[r*width+c] != float(r == c)) return {};
    auto expression = splitSums(t.residual, t);
    if (expression.terms.outputs.size() != 1) return {};
    auto bound = supportBound(expression.terms, t.stateSize(), t.parameters);
    if (!bound || bound->coordinates.size() != width) return {};
    const uint32_t anchor = d.summedObject == 1 ? 0 : 1, member = 1-anchor;
    std::vector<std::pair<uint32_t,uint32_t>> coordinates;
    std::vector<uint32_t> projection;
    for (uint32_t c = 0; c < width; ++c) {
        coordinates.emplace_back(anchor*width+c, member*width+c);
        projection.push_back(anchor*width+c);
    }
    projection.push_back(t.inputSize());
    const auto outer = constantJacobian(expression.outer, projection);
    if (!outer || !std::isfinite(outer->back()) || outer->back() == 0) return {};
    for (uint32_t c = 0; c < width; ++c) if ((*outer)[c] != 0) return {};
    auto factor = factorDifferenceNorm(expression.terms, t.stateSize(), coordinates);
    if (!factor) return {};
    std::vector<uint32_t> varying;
    for (uint32_t c = 0; c < factor->scalar.inputs; ++c)
        if (c < t.stateSize() || c >= t.stateSize()+t.parameters) varying.push_back(c);
    auto parameters = stageFormula(factor->scalar, varying);
    factor->scalar = parameters.varying;
    return CollectionOperator{std::move(expression.outer), std::move(*factor), std::move(*bound),
                              std::move(parameters), anchor, member, width, outer->back()};
}

std::vector<CollectionSolvePlan::Step> CollectionSolvePlan::actions(bool fuseInitialForward) const {
    if (method == Method::ProjectedLineSearch)
        return {{Action::LinearizeForward,UINT32_MAX}, {Action::QuadraticTranspose,0},
                {Action::LineSearchRetract,0}};
    std::vector<Step> result{{fuseInitialForward ? Action::LinearizeForward : Action::Linearize, UINT32_MAX}};
    for (uint32_t i = 0; i < sweeps; ++i) {
        if (i || !fuseInitialForward) result.push_back({Action::Forward,i});
        result.push_back({ownsCorrection && i+1 == sweeps ? Action::TransposeRetract : Action::Transpose,i});
    }
    return result;
}
}
