#pragma once
#include "Expression.h"
#include <map>
#include <optional>

namespace whimsical::dynamics {
using SetId = uint32_t;
struct VariableRef {
    SetId set = 0;
    uint32_t index = 0;
    bool operator==(VariableRef other) const {
        return set == other.set && index == other.index;
    }
};

// Immutable pages plus a uniform fallback. Editing a small range copies only its
// pages; snapshots and unchanged fields share storage. Public arrays are row-major.
class Field {
    static constexpr uint32_t PageRows = 1024;
    uint32_t count_ = 0, width_ = 0;
    std::vector<float> uniform_;
    std::vector<std::shared_ptr<const std::vector<float>>> pages_;

  public:
    Field() = default;
    static Field uniform(uint32_t count, std::vector<float> value);
    static Field dense(uint32_t count, uint32_t width, const std::vector<float>& values);
    uint32_t count() const {
        return count_;
    }
    uint32_t width() const {
        return width_;
    }
    float at(uint32_t row, uint32_t component = 0) const;
    std::vector<float> slice(uint32_t first, uint32_t count) const;
    void patch(uint32_t first, const std::vector<float>& values);
    void append(uint32_t count, const std::vector<float>& values);
};

struct Space {
    std::string name;
    uint32_t stateSize = 0, tangentSize = 0;
    // retract inputs: [state, tangent increment]; difference inputs: [new state, old state].
    Formula retract, difference;
    void validate() const;
    static std::shared_ptr<const Space> euclidean(uint32_t dimensions);
    // Quaternion [w,x,y,z], body-local angular increments; these are mathematical
    // coordinates, not a body/object type or a collision representation.
    static std::shared_ptr<const Space> rotation();
};
using SpaceRef = std::shared_ptr<const Space>;
// Conditions on each residual row C, independent of solver multiplier conventions.
enum class RelationKind { Equality, GreaterEqual, LessEqual };
struct RelationType {
    std::string name;
    std::vector<SpaceRef> spaces;
    uint32_t parameters = 0, history = 0, rows = 1;
    RelationKind kind = RelationKind::Equality;
    // Inputs: endpoint states, parameters, history, dt, time. No solver state.
    Formula residual;
    std::optional<Formula> update;
    uint32_t stateSize() const;
    uint32_t tangentSize() const;
    uint32_t inputSize() const {
        return stateSize() + parameters + history + 2;
    }
    void validate() const;
};
using RelationRef = std::shared_ptr<const RelationType>;
class RelationBuilder {
    RelationType type_;
    Expression expression_;

  public:
    RelationBuilder(std::string name, std::vector<SpaceRef>, uint32_t parameters = 0, uint32_t history = 0,
                    uint32_t rows = 1);
    Vector endpoint(uint32_t) const;
    Scalar parameter(uint32_t) const;
    Scalar state(uint32_t) const;
    Scalar dt() const;
    Scalar time() const;
    Scalar constant(float v) const {
        return expression_.constant(v);
    }
    RelationRef finish(Vector residual, RelationKind = RelationKind::Equality, Vector update = {}) const;
};

struct VariableSet {
    std::string name;
    SpaceRef space;
    uint32_t count = 0;
    Field initial, velocity, inverseMetric, enabled;
    // Read-only is structural. A zero metric in a writable variable is not silently
    // optimized into a read-only scheduling promise.
    bool readOnly = false;
};
struct RelationSet {
    std::string name;
    RelationRef type;
    uint32_t count = 0;
    // One endpoint column per declared slot; all instances share the mathematical type.
    std::vector<std::shared_ptr<const std::vector<VariableRef>>> endpoints;
    Field parameters, compliance, initialHistory, enabled;
    // Fixed capacity, runtime endpoint columns. Compiler schedules these through
    // GPU-built Jacobi incidence; changing their endpoints does not rebuild a plan.
    bool dynamicEndpoints = false;
};
struct ModelData {
    std::vector<VariableSet> variables;
    std::vector<RelationSet> relations;
};
struct ModelSnapshot {
    uint64_t model = 0, version = 0;
    std::shared_ptr<const ModelData> data;
};
enum class FieldKind { InverseMetric, VariableEnabled, Parameters, Compliance, RelationEnabled };
struct FieldChange {
    FieldKind field;
    SetId set;
    uint32_t first, count;
};
struct ChangeSet {
    uint64_t model = 0, from = 0, to = 0;
    bool topology = false;
    std::vector<FieldChange> fields;
};
struct ModelCommit {
    ModelSnapshot snapshot;
    ChangeSet changes;
};
class Model {
    uint64_t id_, version_ = 0;
    std::shared_ptr<ModelData> data_;
    ChangeSet pending_;
    void writable();

  public:
    Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    SetId variables(VariableSet);
    SetId relations(RelationSet);
    void patch(FieldKind, SetId, uint32_t first, const std::vector<float>&);
    // Topology changes are committed atomically with all other pending edits.
    void replaceEndpoints(SetId, uint32_t endpoint, std::vector<VariableRef>);
    void appendVariables(SetId, uint32_t count, const std::vector<float>& initial,
                         const std::vector<float>& velocity, const std::vector<float>& inverseMetric);
    void appendRelations(SetId, std::vector<std::vector<VariableRef>>, const std::vector<float>& parameters,
                         const std::vector<float>& compliance, const std::vector<float>& history = {});
    ModelCommit commit();
};
} // namespace whimsical::dynamics
