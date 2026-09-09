#include "Model.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace whimsical::xpbd {
namespace {
void finite(const std::vector<float>& values) {
    for (auto v : values)
        if (!std::isfinite(v))
            throw std::invalid_argument("XPBD field data must be finite");
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
    if (set.endpoints.size() != set.type->spaces.size())
        throw std::invalid_argument("Relation endpoint arity mismatch");
    for (const auto& column : set.endpoints)
        if (!column || column->size() != set.count)
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
        throw std::out_of_range("XPBD field index");
    const auto& page = pages_[row / PageRows];
    return page ? (*page)[size_t(row % PageRows) * width_ + component] : uniform_[component];
}
std::vector<float> Field::slice(uint32_t first, uint32_t count) const {
    if (first > count_ || count > count_ - first)
        throw std::out_of_range("XPBD field range");
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
        throw std::out_of_range("XPBD field patch shape/range");
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
    residual.validate();
    if (residual.inputs != inputSize() || residual.outputs.size() != rows)
        throw std::invalid_argument("Residual dimension mismatch");
    const auto lambdaFirst = stateSize() + parameters + history;
    for (const auto& n : residual.nodes)
        if (n.op == MathOp::Input && n.a >= lambdaFirst && n.a < lambdaFirst + rows)
            throw std::invalid_argument("Residual cannot depend on solver multipliers");
    if (domain == Domain::Projected && !projection)
        throw std::invalid_argument("Projected relation requires a multiplier projection");
    for (auto f : {projection ? &*projection : nullptr, update ? &*update : nullptr})
        if (f) {
            f->validate();
            if (f->inputs != inputSize())
                throw std::invalid_argument("Relation stage input mismatch");
        }
    if (projection && projection->outputs.size() != rows)
        throw std::invalid_argument("Projection dimension mismatch");
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
Scalar RelationBuilder::multiplier(uint32_t index) const {
    if (index >= type_.rows)
        throw std::out_of_range("Relation multiplier index");
    return expression_.input(type_.stateSize() + type_.parameters + type_.history + index);
}
Scalar RelationBuilder::dt() const {
    return expression_.input(type_.inputSize() - 2);
}
Scalar RelationBuilder::time() const {
    return expression_.input(type_.inputSize() - 1);
}
RelationRef RelationBuilder::finish(Vector residual, Domain domain, Vector projection, Vector update) const {
    auto type = std::make_shared<RelationType>(type_);
    type->domain = domain;
    type->residual = expression_.finish(residual);
    if (!projection.empty())
        type->projection = expression_.finish(projection);
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
SetId Model::relations(RelationSet set) {
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
    if (values.size() != set.count)
        throw std::invalid_argument("Endpoint replacement count mismatch");
    set.endpoints.at(endpoint) = std::make_shared<const std::vector<VariableRef>>(std::move(values));
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
    if (endpoints.size() != next.endpoints.size())
        throw std::invalid_argument("Append relation arity mismatch");
    const auto count = uint32_t(endpoints[0].size());
    for (size_t e = 0; e < endpoints.size(); ++e) {
        if (endpoints[e].size() != count)
            throw std::invalid_argument("Append endpoint count mismatch");
        auto column = std::make_shared<std::vector<VariableRef>>(*next.endpoints[e]);
        column->insert(column->end(), endpoints[e].begin(), endpoints[e].end());
        next.endpoints[e] = std::move(column);
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
} // namespace whimsical::xpbd
