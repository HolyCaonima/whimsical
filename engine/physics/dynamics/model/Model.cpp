#include "Model.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace whimsical::dynamics {
namespace {
void finite(const std::vector<float>& values) {
    for (auto v : values)
        if (!std::isfinite(v))
            throw std::invalid_argument("Dynamics field data must be finite");
}
void shape(const Field& f, uint32_t count, uint32_t width, const char* name) {
    if (f.count() != count || f.width() != width)
        throw std::invalid_argument(std::string("Field shape mismatch: ") + name);
}
Field identity(uint32_t count, uint32_t dimensions) {
    std::vector<float> values(size_t(dimensions) * dimensions);
    for (uint32_t i = 0; i < dimensions; ++i)
        values[size_t(i) * dimensions + i] = 1;
    return Field::uniform(count, std::move(values));
}
void metric(const Field& field, uint32_t n) {
    std::vector<double> a(size_t(n) * n);
    for (uint32_t row = 0; row < field.count(); ++row) {
        double scale = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (field.at(row, i * n + i) < 0)
                throw std::invalid_argument("Inverse metric has a negative diagonal");
            scale = std::max(scale, double(field.at(row, i * n + i)));
            for (uint32_t j = 0; j < n; ++j) {
                a[size_t(i) * n + j] = field.at(row, i * n + j);
                if (std::abs(field.at(row, i * n + j) - field.at(row, j * n + i)) >
                    1e-6 * std::max(1., scale))
                    throw std::invalid_argument("Inverse metric must be symmetric");
            }
        }
        const double tolerance = scale * 1e-6;
        for (uint32_t k = 0; k < n; ++k) {
            const double pivot = a[size_t(k) * n + k];
            if (pivot < -tolerance)
                throw std::invalid_argument("Inverse metric must be positive semidefinite");
            if (pivot <= 0) {
                for (uint32_t i = k + 1; i < n; ++i)
                    if (std::abs(a[size_t(i) * n + k]) > tolerance)
                        throw std::invalid_argument("Inverse metric must be positive semidefinite");
            } else
                for (uint32_t i = k + 1; i < n; ++i)
                    for (uint32_t j = i; j < n; ++j)
                        a[size_t(j) * n + i] = a[size_t(i) * n + j] -=
                            a[size_t(i) * n + k] * a[size_t(j) * n + k] / pivot;
        }
    }
}
void defaults(VariableSet& set) {
    if (!set.space)
        throw std::invalid_argument("Variable set requires a space");
    set.space->validate();
    if (!set.velocity.width())
        set.velocity = Field::uniform(set.count, std::vector<float>(set.space->tangentSize));
    if (!set.inverseMetric.width())
        set.inverseMetric = identity(set.count, set.space->tangentSize);
    if (!set.enabled.width())
        set.enabled = Field::uniform(set.count, {1});
    shape(set.initial, set.count, set.space->stateSize, "initial");
    shape(set.velocity, set.count, set.space->tangentSize, "velocity");
    shape(set.inverseMetric, set.count, set.space->tangentSize * set.space->tangentSize, "inverseMetric");
    shape(set.enabled, set.count, 1, "enabled");
    metric(set.inverseMetric, set.space->tangentSize);
}
void defaults(RelationSet& set) {
    if (!set.type)
        throw std::invalid_argument("Relation set requires a mathematical definition");
    set.type->validate();
    if (!set.parameters.width())
        set.parameters = Field::uniform(set.count, std::vector<float>(set.type->parameters));
    if (!set.compliance.width())
        set.compliance = Field::uniform(set.count, std::vector<float>(set.type->rows));
    if (!set.initialHistory.width())
        set.initialHistory = Field::uniform(set.count, std::vector<float>(set.type->history));
    if (!set.enabled.width())
        set.enabled = Field::uniform(set.count, {1});
    shape(set.parameters, set.count, set.type->parameters, "parameters");
    shape(set.compliance, set.count, set.type->rows, "compliance");
    shape(set.initialHistory, set.count, set.type->history, "history");
    shape(set.enabled, set.count, 1, "enabled");
    for (uint32_t i = 0; i < set.count; ++i)
        for (uint32_t c = 0; c < set.type->rows; ++c)
            if (set.compliance.at(i, c) < 0)
                throw std::invalid_argument("Compliance must be nonnegative");
    if (set.pairs) {
        if (set.type->objects.size() != 2 || !set.endpoints.empty() || set.dynamicEndpoints)
            throw std::invalid_argument("Object pairs require two formal objects and static object bindings");
        return;
    }
    if (set.endpoints.size() != set.type->spaces.size())
        throw std::invalid_argument("Relation endpoint arity mismatch");
    for (const auto& column : set.endpoints)
        if (!column.broadcast() && column.rows() != set.count)
            throw std::invalid_argument("Relation endpoint column length mismatch");
}
uint32_t totalState(const std::vector<SpaceRef>& spaces) {
    uint32_t size = 0;
    for (const auto& s : spaces) {
        if (!s)
            throw std::invalid_argument("Null endpoint space");
        size += s->stateSize;
    }
    return size;
}
} // namespace
EndpointSource::EndpointSource(std::shared_ptr<const std::vector<VariableRef>> values)
    : references(std::move(values)) {
    if (references)
        count = uint32_t(references->size());
}
EndpointSource EndpointSource::object(VariableRef value) {
    EndpointSource result;
    result.kind = Kind::Object;
    result.first = value;
    result.count = 1;
    result.stride = 0;
    return result;
}
EndpointSource EndpointSource::collection(SetId set, uint32_t first, uint32_t count, uint32_t stride) {
    if (!stride)
        throw std::invalid_argument("Endpoint collection stride must be positive");
    if (count && uint64_t(first) + uint64_t(count - 1) * stride > UINT32_MAX)
        throw std::overflow_error("Endpoint collection exceeds 32-bit indices");
    EndpointSource result;
    result.kind = Kind::Collection;
    result.first = {set, first};
    result.count = count;
    result.stride = stride;
    return result;
}
uint32_t EndpointSource::rows() const {
    if (kind == Kind::Explicit) {
        if (!references)
            throw std::invalid_argument("Explicit endpoint source requires references");
        return uint32_t(references->size());
    }
    return count;
}
VariableRef EndpointSource::at(uint32_t row) const {
    if (kind == Kind::Explicit)
        return references->at(row);
    if (kind == Kind::Object)
        return first;
    if (row >= count)
        throw std::out_of_range("Endpoint collection row");
    return {first.set, uint32_t(uint64_t(first.index) + uint64_t(row) * stride)};
}
Field Field::uniform(uint32_t count, std::vector<float> value) {
    finite(value);
    Field result;
    result.count_ = count;
    result.width_ = uint32_t(value.size());
    result.uniform_ = std::move(value);
    result.pages_.resize((uint64_t(count) + PageRows - 1) / PageRows);
    return result;
}
Field Field::dense(uint32_t count, uint32_t width, const std::vector<float>& values) {
    if (values.size() != size_t(count) * width)
        throw std::invalid_argument("Dense field length mismatch");
    auto result = uniform(count, std::vector<float>(width));
    result.patch(0, values);
    return result;
}
float Field::at(uint32_t row, uint32_t component) const {
    if (row >= count_ || component >= width_)
        throw std::out_of_range("Dynamics field index");
    const auto& page = pages_[row / PageRows];
    return page ? (*page)[size_t(row % PageRows) * width_ + component] : uniform_[component];
}
std::vector<float> Field::slice(uint32_t first, uint32_t count) const {
    if (first > count_ || count > count_ - first)
        throw std::out_of_range("Dynamics field range");
    std::vector<float> out(size_t(count) * width_);
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t c = 0; c < width_; ++c)
            out[size_t(i) * width_ + c] = at(first + i, c);
    return out;
}
void Field::patch(uint32_t first, const std::vector<float>& values) {
    if (!width_) {
        if (!values.empty())
            throw std::invalid_argument("Zero-width field data");
        return;
    }
    if (values.size() % width_ || first > count_ || values.size() / width_ > count_ - first)
        throw std::out_of_range("Dynamics field patch shape/range");
    finite(values);
    const auto rows = uint32_t(values.size() / width_);
    uint32_t consumed = 0;
    while (consumed < rows) {
        const auto row = first + consumed, pageIndex = row / PageRows, local = row % PageRows;
        const auto n = std::min(rows - consumed, PageRows - local);
        auto page = std::make_shared<std::vector<float>>(size_t(PageRows) * width_);
        if (pages_[pageIndex])
            *page = *pages_[pageIndex];
        else
            for (uint32_t i = 0; i < PageRows; ++i)
                std::copy(uniform_.begin(), uniform_.end(), page->begin() + size_t(i) * width_);
        std::copy_n(values.begin() + size_t(consumed) * width_, size_t(n) * width_,
                    page->begin() + size_t(local) * width_);
        pages_[pageIndex] = std::move(page);
        consumed += n;
    }
}
void Field::append(uint32_t count, const std::vector<float>& values) {
    if (values.size() != size_t(count) * width_ || uint64_t(count_) + count > UINT32_MAX)
        throw std::invalid_argument("Append field shape or index capacity");
    auto first = count_;
    count_ += count;
    pages_.resize((uint64_t(count_) + PageRows - 1) / PageRows);
    patch(first, values);
}
void Space::validate() const {
    if (name.empty() || !stateSize || !tangentSize)
        throw std::invalid_argument("Space requires a name and dimensions");
    retract.validate();
    difference.validate();
    if (retract.inputs != stateSize + tangentSize || retract.outputs.size() != stateSize ||
        difference.inputs != stateSize * 2 || difference.outputs.size() != tangentSize)
        throw std::invalid_argument("Space operation dimension mismatch");
}
SpaceRef Space::euclidean(uint32_t dimensions) {
    if (!dimensions)
        throw std::invalid_argument("Euclidean space dimension is zero");
    auto s = std::make_shared<Space>();
    s->name = "R" + std::to_string(dimensions);
    s->stateSize = s->tangentSize = dimensions;
    Expression e(dimensions * 2);
    auto a = e.inputs(0, dimensions), b = e.inputs(dimensions, dimensions);
    s->retract = e.finish(a + b);
    s->difference = e.finish(a - b);
    return s;
}
SpaceRef Space::rotation() {
    auto s = std::make_shared<Space>();
    s->name = "SO3";
    s->stateSize = 4;
    s->tangentSize = 3;
    Expression e(7);
    auto q = e.inputs(0, 4), d = e.inputs(4, 3);
    Vector r = {q[0] - .5f * dot(Vector{q[1], q[2], q[3]}, d),
                q[1] + .5f * (q[0] * d[0] + q[2] * d[2] - q[3] * d[1]),
                q[2] + .5f * (q[0] * d[1] + q[3] * d[0] - q[1] * d[2]),
                q[3] + .5f * (q[0] * d[2] + q[1] * d[1] - q[2] * d[0])};
    r = r * (1.f / length(r));
    s->retract = e.finish(r);
    Expression f(8);
    auto n = f.inputs(0, 4), o = f.inputs(4, 4);
    auto w = dot(n, o);
    Vector v = {o[0] * n[1] - o[1] * n[0] - o[2] * n[3] + o[3] * n[2],
                o[0] * n[2] + o[1] * n[3] - o[2] * n[0] - o[3] * n[1],
                o[0] * n[3] - o[1] * n[2] + o[2] * n[1] - o[3] * n[0]};
    auto sign = select(less(w, f.constant(0)), f.constant(-1), f.constant(1));
    v = v * sign;
    w = w * sign;
    auto magnitude = length(v);
    auto scale =
        select(less(magnitude, f.constant(1e-7f)), f.constant(2), 2.f * atan2(magnitude, w) / magnitude);
    s->difference = f.finish(v * scale);
    return s;
}
uint32_t RelationType::stateSize() const {
    return totalState(spaces);
}
uint32_t RelationType::tangentSize() const {
    uint32_t n = 0;
    for (const auto& s : spaces)
        n += s->tangentSize;
    return n;
}
void RelationType::validate() const {
    if (name.empty() || spaces.empty() || !rows)
        throw std::invalid_argument("Relation requires name, endpoints and residual rows");
    for (const auto& s : spaces)
        s->validate();
    if (!objects.empty()) {
        if (objects.size() != 2 || objects[0].size() + objects[1].size() != spaces.size())
            throw std::invalid_argument("Relation requires exactly two named object schemas");
        for (const auto& fields : objects) {
            auto names = fields;
            std::sort(names.begin(), names.end());
            if (std::adjacent_find(names.begin(), names.end()) != names.end())
                throw std::invalid_argument("Duplicate formal object DOF field");
        }
    }
    residual.validate();
    if (residual.inputs != inputSize() || residual.outputs.size() != rows)
        throw std::invalid_argument("Residual dimension mismatch");
    if (update) {
        update->validate();
        if (update->inputs != inputSize())
            throw std::invalid_argument("Relation state update input mismatch");
    }
    if (update && update->outputs.size() != history)
        throw std::invalid_argument("History update dimension mismatch");
}
RelationBuilder::RelationBuilder(std::string name, std::vector<SpaceRef> spaces, uint32_t parameters,
                                 uint32_t history, uint32_t rows)
    : type_{std::move(name), std::move(spaces), parameters, history, rows}, expression_(type_.inputSize()) {}
Vector RelationBuilder::endpoint(uint32_t index) const {
    uint32_t first = 0;
    for (uint32_t i = 0; i < index; ++i)
        first += type_.spaces.at(i)->stateSize;
    return expression_.inputs(first, type_.spaces.at(index)->stateSize);
}
Scalar RelationBuilder::parameter(uint32_t index) const {
    if (index >= type_.parameters)
        throw std::out_of_range("Relation parameter index");
    return expression_.input(type_.stateSize() + index);
}
Scalar RelationBuilder::state(uint32_t index) const {
    if (index >= type_.history)
        throw std::out_of_range("Relation history index");
    return expression_.input(type_.stateSize() + type_.parameters + index);
}
Scalar RelationBuilder::dt() const {
    return expression_.input(type_.inputSize() - 2);
}
Scalar RelationBuilder::time() const {
    return expression_.input(type_.inputSize() - 1);
}
RelationRef RelationBuilder::finish(Vector residual, RelationKind kind, Vector update) const {
    auto type = std::make_shared<RelationType>(type_);
    type->kind = kind;
    type->residual = expression_.finish(residual);
    if (!update.empty())
        type->update = expression_.finish(update);
    type->validate();
    return type;
}
Model::Model() : data_(std::make_shared<ModelData>()) {
    static std::atomic<uint64_t> next{0};
    id_ = ++next;
}
void Model::writable() {
    if (data_.use_count() != 1)
        data_ = std::make_shared<ModelData>(*data_);
}
SetId Model::variables(VariableSet set) {
    defaults(set);
    writable();
    auto id = uint32_t(data_->variables.size());
    data_->variables.push_back(std::move(set));
    pending_.topology = true;
    return id;
}
uint32_t Model::object(Object object) {
    if (object.kind == Object::Kind::Single && object.count != 1)
        throw std::invalid_argument("A single object has exactly one member");
    for (const auto& [name, source] : object.dofs) {
        if (name.empty() || source.rows() != object.count)
            throw std::invalid_argument("Object DOF fields must match its declared shape");
        for (uint32_t i = 0; i < source.rows(); ++i) {
            const auto ref = source.at(i);
            if (ref.set >= data_->variables.size() || ref.index >= data_->variables[ref.set].count)
                throw std::invalid_argument("Object references a missing DOF");
        }
    }
    writable();
    auto id = uint32_t(data_->objects.size());
    data_->objects.push_back(std::move(object));
    pending_.topology = true;
    return id;
}
uint32_t Model::member(uint32_t id, uint32_t index) {
    const auto& parent = data_->objects.at(id);
    if (parent.kind != Object::Kind::Collection || index >= parent.count)
        throw std::invalid_argument("Member requires a collection and an in-range index");
    Object child;
    child.name = parent.name + "[" + std::to_string(index) + "]";
    child.member = Object::Member{id, index};
    for (const auto& [name, source] : parent.dofs)
        child.dofs.emplace(name, EndpointSource::object(source.at(index)));
    return object(std::move(child));
}
uint32_t pairCount(const ModelData& data, const PairBinding& pair) {
    const auto& a = data.objects.at(pair.a);
    const auto& b = data.objects.at(pair.b);
    uint64_t count = uint64_t(a.count) * b.count;
    if (pair.a == pair.b && a.kind == Object::Kind::Collection) {
        if (pair.self == PairBinding::Self::Unspecified)
            throw std::invalid_argument("A collection paired with itself requires directed/undirected and includeSelf rules");
        if (!pair.includeSelf)
            count -= a.count;
        if (pair.self == PairBinding::Self::Undirected)
            count = pair.includeSelf ? (count + a.count) / 2 : count / 2;
    } else if (pair.self != PairBinding::Self::Unspecified)
        throw std::invalid_argument("Self-pair rules apply only to the same collection object");
    if (count > UINT32_MAX)
        throw std::overflow_error("Expanded pair count exceeds 32-bit addressing");
    return uint32_t(count);
}
SetId Model::relations(RelationSet set) {
    if (set.pairs) {
        uint64_t count = 0;
        for (const auto& pair : *set.pairs)
            count += pairCount(*data_, pair);
        if (count != set.count)
            throw std::invalid_argument("Expanded pair field count mismatch");
    }
    defaults(set);
    writable();
    auto id = uint32_t(data_->relations.size());
    data_->relations.push_back(std::move(set));
    pending_.topology = true;
    return id;
}
void Model::patch(FieldKind kind, SetId set, uint32_t first, const std::vector<float>& values) {
    writable();
    Field* field = nullptr;
    switch (kind) {
    case FieldKind::InverseMetric:
        field = &data_->variables.at(set).inverseMetric;
        break;
    case FieldKind::VariableEnabled:
        field = &data_->variables.at(set).enabled;
        break;
    case FieldKind::Parameters:
        field = &data_->relations.at(set).parameters;
        break;
    case FieldKind::Compliance:
        field = &data_->relations.at(set).compliance;
        break;
    case FieldKind::RelationEnabled:
        field = &data_->relations.at(set).enabled;
        break;
    }
    if (kind == FieldKind::InverseMetric) {
        auto n = data_->variables.at(set).space->tangentSize;
        metric(Field::dense(uint32_t(values.size() / (n * n)), n * n, values), n);
    }
    if (kind == FieldKind::Compliance)
        for (auto v : values)
            if (v < 0)
                throw std::invalid_argument("Compliance must be nonnegative");
    field->patch(first, values);
    pending_.fields.push_back(
        {kind, set, first, field->width() ? uint32_t(values.size() / field->width()) : 0});
}
void Model::replaceEndpoints(SetId id, uint32_t endpoint, std::vector<VariableRef> values) {
    writable();
    auto& set = data_->relations.at(id);
    if (set.pairs)
        throw std::invalid_argument("Object pair topology cannot be edited as endpoint columns");
    if (values.size() != set.count)
        throw std::invalid_argument("Endpoint replacement count mismatch");
    set.endpoints.at(endpoint) =
        EndpointSource(std::make_shared<const std::vector<VariableRef>>(std::move(values)));
    pending_.topology = true;
}
void Model::appendVariables(SetId id, uint32_t count, const std::vector<float>& initial,
                            const std::vector<float>& velocity, const std::vector<float>& metric) {
    writable();
    auto next = data_->variables.at(id);
    next.initial.append(count, initial);
    next.velocity.append(count, velocity);
    next.inverseMetric.append(count, metric);
    next.enabled.append(count, std::vector<float>(count, 1));
    next.count += count;
    defaults(next);
    data_->variables[id] = std::move(next);
    pending_.topology = true;
}
void Model::appendRelations(SetId id, std::vector<std::vector<VariableRef>> endpoints,
                            const std::vector<float>& parameters, const std::vector<float>& compliance,
                            const std::vector<float>& history) {
    writable();
    auto next = data_->relations.at(id);
    if (next.pairs)
        throw std::invalid_argument("Object pairs cannot append endpoint columns");
    if (endpoints.size() != next.endpoints.size())
        throw std::invalid_argument("Append relation arity mismatch");
    const auto count = uint32_t(endpoints[0].size());
    for (size_t e = 0; e < endpoints.size(); ++e) {
        if (endpoints[e].size() != count)
            throw std::invalid_argument("Append endpoint count mismatch");
        if (next.endpoints[e].kind != EndpointSource::Kind::Explicit)
            throw std::invalid_argument("Cannot append explicit rows to a declarative relation set");
        auto column = std::make_shared<std::vector<VariableRef>>(*next.endpoints[e].references);
        column->insert(column->end(), endpoints[e].begin(), endpoints[e].end());
        next.endpoints[e] = EndpointSource(std::move(column));
    }
    next.parameters.append(count, parameters);
    next.compliance.append(count, compliance);
    next.initialHistory.append(count, history);
    next.enabled.append(count, std::vector<float>(count, 1));
    next.count += count;
    defaults(next);
    data_->relations[id] = std::move(next);
    pending_.topology = true;
}
ModelCommit Model::commit() {
    pending_.model = id_;
    pending_.from = version_;
    pending_.to = ++version_;
    ModelCommit result{{id_, version_, data_}, std::move(pending_)};
    pending_ = {};
    return result;
}
} // namespace whimsical::dynamics
