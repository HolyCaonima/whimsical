#pragma once
#include "physics/dynamics/model/Model.h"
#include <utility>

namespace whimsical::dynamics {
// Solver-independent iteration domains. Object fields are resolved once;
// products remain index maps rather than arrays of expanded endpoints.
struct BindingDomain {
    enum class Map : uint32_t { Zip, Product, Directed, Upper, UpperDiagonal };
    Map map = Map::Zip;
    uint32_t first = 0, count = 0, left = 0, right = 0, split = 0;
    std::vector<EndpointSource> fields;
    std::pair<uint32_t, uint32_t> members(uint32_t row) const;
    VariableRef at(uint32_t row, uint32_t slot) const;
    bool affineFields() const;
    // 0: explicit table; 1..5: a fixed index map; -1 is reserved for mixed sets.
    int32_t endpointMode(bool dynamic) const;
};
struct BindingIR {
    std::vector<BindingDomain> domains;
    std::vector<bool> readOnly;
    bool singletonDomains = false;
    const BindingDomain& domain(uint32_t row) const;
    VariableRef at(uint32_t row, uint32_t slot) const;
};
BindingIR analyzeBindings(const ModelData&, const RelationSet&);
} // namespace whimsical::dynamics
