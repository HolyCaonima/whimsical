#include "BindingIR.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace whimsical::dynamics {
std::pair<uint32_t, uint32_t> BindingDomain::members(uint32_t row) const {
    if (map == Map::Zip)
        return {row, row};
    if (map == Map::Product)
        return {row / right, row % right};
    if (map == Map::Directed) {
        auto i = row / (right - 1), j = row % (right - 1);
        return {i, j + (j >= i)};
    }
    const bool diagonal = map == Map::UpperDiagonal;
    auto prefix = [&](uint32_t i) { return uint64_t(i) * (2ull * right - i + (diagonal ? 1 : -1)) / 2; };
    // Invert the triangular prefix once instead of binary-searching it at every
    // incidence visit. The rationalized root avoids cancellation near row zero;
    // integer prefix checks preserve exact row boundaries independently of sqrt.
    const double b = 2.0 * right + (diagonal ? 1.0 : -1.0);
    uint32_t lo = std::min(left - 1, uint32_t(4.0 * row / (b + std::sqrt(b * b - 8.0 * row))));
    while (lo > 0 && prefix(lo) > row) --lo;
    while (lo + 1 < left && prefix(lo + 1) <= row) ++lo;
    return {lo, lo + (diagonal ? 0u : 1u) + uint32_t(row - prefix(lo))};
}
VariableRef BindingDomain::at(uint32_t row, uint32_t slot) const {
    const auto indices = members(row);
    return fields[slot].at(slot < split ? indices.first : indices.second);
}
bool BindingDomain::affineFields() const {
    return std::all_of(fields.begin(), fields.end(), [](const EndpointSource& source) {
        return source.kind != EndpointSource::Kind::Explicit;
    });
}
int32_t BindingDomain::endpointMode(bool dynamic) const {
    return !dynamic && affineFields() && 5ull + 2ull * fields.size() < uint64_t(count) * fields.size()
        ? int32_t(map) + 1 : 0;
}
const BindingDomain& BindingIR::domain(uint32_t row) const {
    if (singletonDomains)
        return domains[row];
    auto found = std::upper_bound(domains.begin(), domains.end(), row,
        [](uint32_t value, const BindingDomain& domain) { return value < domain.first; });
    return *std::prev(found);
}
VariableRef BindingIR::at(uint32_t row, uint32_t slot) const {
    const auto& d = domain(row);
    return d.at(row - d.first, slot);
}
BindingIR analyzeBindings(const ModelData& model, const RelationSet& set) {
    BindingIR ir;
    ir.readOnly.assign(set.type->spaces.size(), !set.dynamicEndpoints);
    if (!set.pairs) {
        BindingDomain domain;
        domain.count = set.count;
        domain.fields = set.endpoints;
        ir.domains.push_back(std::move(domain));
    } else {
        uint32_t first = 0;
        for (const auto& pair : *set.pairs) {
            const auto& a = model.objects.at(pair.a);
            const auto& b = model.objects.at(pair.b);
            BindingDomain domain;
            domain.first = first;
            domain.count = pairCount(model, pair);
            domain.left = a.count;
            domain.right = b.count;
            domain.split = uint32_t(set.type->objects[0].size());
            domain.map = BindingDomain::Map::Product;
            if (pair.a == pair.b && a.kind == Object::Kind::Collection) {
                if (pair.self == PairBinding::Self::Undirected)
                    domain.map = pair.includeSelf ? BindingDomain::Map::UpperDiagonal : BindingDomain::Map::Upper;
                else if (!pair.includeSelf)
                    domain.map = BindingDomain::Map::Directed;
            }
            for (uint32_t side = 0; side < 2; ++side) {
                const auto& object = side ? b : a;
                for (const auto& name : set.type->objects[side]) {
                    auto found = object.dofs.find(name);
                    if (found == object.dofs.end())
                        throw std::invalid_argument("Object '" + object.name + "' lacks DOF field '" + name + "'");
                    domain.fields.push_back(found->second);
                }
            }
            // A product with a singleton is a linear domain. Make that fact
            // explicit before GPU lowering so no division/modulo survives.
            if (domain.map == BindingDomain::Map::Product && (domain.left == 1 || domain.right == 1)) {
                for (uint32_t slot = 0; slot < domain.fields.size(); ++slot)
                    if ((slot < domain.split ? domain.left : domain.right) == 1)
                        domain.fields[slot] = EndpointSource::object(domain.fields[slot].at(0));
                domain.map = BindingDomain::Map::Zip;
            }
            first += domain.count;
            if (domain.count)
                ir.domains.push_back(std::move(domain));
        }
    }
    for (const auto& domain : ir.domains)
        for (uint32_t slot = 0; slot < domain.fields.size(); ++slot) {
            const auto& source = domain.fields[slot];
            if (source.kind == EndpointSource::Kind::Explicit) {
                for (const auto& ref : *source.references)
                    ir.readOnly[slot] = ir.readOnly[slot] && model.variables.at(ref.set).readOnly;
            } else
                ir.readOnly[slot] = ir.readOnly[slot] && model.variables.at(source.first.set).readOnly;
        }
    ir.singletonDomains = std::all_of(ir.domains.begin(), ir.domains.end(),
        [](const BindingDomain& domain) { return domain.count == 1; });
    return ir;
}
} // namespace whimsical::dynamics
