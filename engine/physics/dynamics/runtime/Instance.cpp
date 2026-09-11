#include "physics/dynamics/compiler/FormulaGlsl.h"
#include "Instance.h"
#include "renderCore/graph/RenderGraph.h"
#include "renderCore/vulkan/VulkanAccess.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace whimsical::dynamics {
namespace {
template <class T> std::vector<uint8_t> bytes(const std::vector<T>& data) {
    std::vector<uint8_t> out(data.size() * sizeof(T));
    if (!out.empty())
        std::memcpy(out.data(), data.data(), out.size());
    return out;
}
struct Constants {
    uint32_t first, count;
    float h, time;
    uint32_t tick, iteration;
    float relaxation;
    uint32_t reserved = 0;
};
static_assert(sizeof(Constants) == CompiledPlan::ConstantBytes);
std::vector<uint8_t> bytes(const Constants& data) {
    std::vector<uint8_t> out(sizeof(data));
    std::memcpy(out.data(), &data, sizeof(data));
    return out;
}
constexpr uint32_t StateWritePassThreshold = 8;
struct Range {
    BufferRole role;
    uint32_t first, count, width, stride;
};
Range range(const CompiledPlan& plan, StateField field, SetId id) {
    if (field == StateField::History) {
        const auto& layout = plan.relations.at(id);
        const auto& t = *plan.types[layout.type];
        return {BufferRole::History, layout.history, layout.count, t.history, layout.count};
    }
    const auto& layout = plan.variables.at(id);
    const auto& s = *plan.spaces[layout.space];
    return {field == StateField::Value      ? BufferRole::Values
            : field == StateField::Velocity ? BufferRole::Velocity
                                            : BufferRole::Acceleration,
            field == StateField::Value ? layout.values : layout.velocity, layout.count,
            field == StateField::Value ? s.stateSize : s.tangentSize, layout.count};
}
rg::Extent3D extent(uint32_t count) {
    constexpr uint32_t row = 65535 * 128;
    return {std::min(count, row), uint32_t((uint64_t(count) + row - 1) / row), 1};
}
rg::Declaration declaration(const std::string& name, uint64_t size, bool integer, bool bound = true) {
    rg::Declaration d;
    d.name = "Dynamics " + name;
    d.kind = rg::Kind::Buffer;
    d.lifetime = rg::Lifetime::Persistent;
    d.byteSize = std::max(uint64_t(4), size);
    if (bound)
        d.view = {rg::BindingType::Storage,
                  false,
                  {},
                  "X_" + name,
                  std::string(integer ? "uint" : "float") + " x_" + name + "[];"};
    return d;
}
} // namespace
struct Instance::Storage {
    PlanRef plan;
    rg::Registry registry{CompiledPlan::ConstantBytes};
    std::array<rg::ResourceId, BufferCount> resources;
    rg::ResourceId readback, oldValues, oldVelocity, oldHistory, oldAcceleration, migration;
    std::unique_ptr<rc::GraphContext> execution;
    std::vector<const rg::Program*> programs;
    std::string interface;
    bool candidateBoundsDirty = true;
    explicit Storage(rc::RenderCore& core, PlanRef value) : plan(std::move(value)) {
        for (uint32_t i = 0; i < BufferCount; ++i) {
            const auto& b = plan->buffers[i];
            auto d = declaration(b.name, b.initial.size(), b.integers);
            // Model fields and compiled tables may be uploaded between steps, but
            // no kernel writes them. Preserve that fact in the shader interface.
            switch (BufferRole(i)) {
            case BufferRole::Metric:
            case BufferRole::Variables:
            case BufferRole::VariableWork:
            case BufferRole::Relations:
            case BufferRole::Endpoints:
            case BufferRole::Parameters:
            case BufferRole::Compliance:
            case BufferRole::VariableEnabled:
            case BufferRole::RelationEnabled:
            case BufferRole::RelationWork:
            case BufferRole::RegionRanges:
            case BufferRole::RegionState:
            case BufferRole::LocalOffsets:
            case BufferRole::StateWrites:
                d.view.readOnly = true;
                break;
            default:
                break;
            }
            if (plan->statistics.colorWindows && BufferRole(i) == BufferRole::Values)
                d.view.body = "coherent " + d.view.body;
            if (!plan->local.empty() && BufferRole(i) == BufferRole::Contributions)
                d.view.body = "coherent " + d.view.body;
            if (BufferRole(i) == BufferRole::Diagnostics)
                d.handover = rg::Access::Host;
            resources[i] = registry.declare(std::move(d));
        }
        auto d = declaration("readback", 4, false, false);
        d.handover = rg::Access::Host;
        d.bytes = [](uint32_t rows, uint32_t columns) { return uint64_t(rows) * columns * 4; };
        readback = registry.declare(std::move(d));
        for (auto pair :
             {std::make_pair(&oldValues, "sourceValues"), std::make_pair(&oldVelocity, "sourceVelocity"),
              std::make_pair(&oldHistory, "sourceHistory"),
              std::make_pair(&oldAcceleration, "sourceAcceleration")}) {
            auto input = declaration(pair.second, 4, false);
            input.lifetime = rg::Lifetime::Imported;
            input.view.readOnly = true;
            *pair.first = registry.declare(std::move(input));
        }
        auto remap = declaration("migration", 4, true);
        remap.lifetime = rg::Lifetime::External;
        migration = registry.declare(std::move(remap));
        execution = std::make_unique<rc::GraphContext>(
            core, registry, "Dynamics model " + std::to_string(plan->model.model), rc::QueueClass::Compute,
            "Dynamics");
        interface = "#version 450\n" + registry.glsl().at("graph.compute.glsl") + plan->interface();
        for (const auto& kernel : plan->kernels)
            programs.push_back(&execution->compute(kernel.name + ".comp", interface + kernel.source));
    }
    rg::ResourceId id(BufferRole role) const {
        return resources[size_t(role)];
    }
    uint32_t readSize(const std::vector<StateRange>& ranges) const {
        uint64_t total = 0;
        for (auto r : ranges) {
            auto source = range(*plan, r.field, r.set);
            if (r.first > source.count || r.count > source.count - r.first)
                throw std::out_of_range("Dynamics readback range");
            total += uint64_t(r.count) * source.width;
        }
        if (total > UINT32_MAX)
            throw std::length_error("Dynamics readback exceeds buffer extent");
        return uint32_t(total);
    }
    void recordReads(const std::vector<StateRange>& ranges, uint32_t total) {
        if (!total)
            return;
        // Pack only requested columns; sparse observers never force a full-model copy.
        execution->upload(readback, std::vector<uint8_t>(size_t(total) * sizeof(float)));
        uint64_t offset = 0;
        for (auto r : ranges) {
            auto source = range(*plan, r.field, r.set);
            if (r.count)
                for (uint32_t c = 0; c < source.width; ++c)
                    execution->copyBuffer(id(source.role), readback,
                                          uint64_t(source.first + r.first + c * source.stride) * 4,
                                          (offset + uint64_t(c) * r.count) * 4, uint64_t(r.count) * 4);
            offset += uint64_t(r.count) * source.width;
        }
    }
    std::vector<std::vector<float>> decodeReads(const std::vector<StateRange>& ranges) {
        std::vector<std::vector<float>> result(ranges.size());
        if (!readSize(ranges))
            return result;
        auto data = execution->readbackData(readback);
        uint64_t offset = 0;
        for (size_t k = 0; k < ranges.size(); ++k) {
            auto r = ranges[k];
            auto source = range(*plan, r.field, r.set);
            auto& values = result[k];
            values.resize(size_t(r.count) * source.width);
            for (uint32_t i = 0; i < r.count; ++i)
                for (uint32_t c = 0; c < source.width; ++c)
                    std::memcpy(&values[size_t(i) * source.width + c],
                                data.data() + (offset + size_t(c) * r.count + i) * 4, 4);
            offset += values.size();
        }
        return result;
    }
    void initialize() {
        auto& graph = execution->graph();
        graph.reset();
        for (uint32_t i = 0; i < BufferCount; ++i)
            execution->upload(resources[i], plan->buffers[i].initial);
    }
    void submit(uint64_t tick = 0, uint64_t profile = 0, uint32_t width = 1, uint32_t height = 1) {
        execution->compile(width, height);
        execution->record({profile, tick});
        execution->submit();
    }
    void dispatch(const rg::Program& program, const std::string& name, Constants constants) {
        if (!constants.count)
            return;
        auto pass = execution->graph().add(name);
        for (const auto& access : program.accesses)
            if (access.writes)
                pass.modify(registry.binding(access.binding), access.access);
        pass.dispatch(program, extent(constants.count)).constants(bytes(constants));
    }
    void batch(const Batch& batch, float h, float time, uint64_t tick, uint32_t iteration) {
        dispatch(*programs[batch.kernel], plan->kernels[batch.kernel].name,
                 {batch.first, batch.count, h, time, uint32_t(tick), iteration, plan->policy.relaxation});
    }
    uint32_t writeRows(Range destination, uint32_t first, const std::vector<float>& values) const {
        if (!destination.width) {
            if (!values.empty())
                throw std::invalid_argument("Zero-width state write");
            return 0;
        }
        if (values.size() % destination.width || first > destination.count ||
            values.size() / destination.width > destination.count - first)
            throw std::out_of_range("State write range/shape mismatch");
        for (auto value : values)
            if (!std::isfinite(value))
                throw std::invalid_argument("State input must be finite");
        return uint32_t(values.size() / destination.width);
    }
    void write(Range destination, uint32_t first, const std::vector<float>& values) {
        auto count = writeRows(destination, first, values);
        if (!count)
            return;
        std::vector<float> column(count);
        for (uint32_t c = 0; c < destination.width; ++c) {
            for (uint32_t i = 0; i < count; ++i)
                column[i] = values[size_t(i) * destination.width + c];
            execution->uploadRange(id(destination.role),
                                   uint64_t(destination.first + first + c * destination.stride) * 4,
                                   bytes(column));
        }
    }
    void writes(const std::vector<StateWrite>& inputs, uint64_t tick) {
        std::vector<Range> destinations;
        std::vector<uint32_t> rows;
        std::array<size_t, 4> assignments{};
        destinations.reserve(inputs.size());
        rows.reserve(inputs.size());
        uint64_t passes = 0;
        for (const auto& input : inputs) {
            auto destination = range(*plan, input.field, input.set);
            auto count = writeRows(destination, input.first, input.values);
            destinations.push_back(destination);
            rows.push_back(count);
            if (count) {
                passes += destination.width;
                assignments[size_t(input.field)] += size_t(count) * destination.width;
            }
        }
        if (passes < StateWritePassThreshold) {
            for (size_t i = 0; i < inputs.size(); ++i)
                write(destinations[i], inputs[i].first, inputs[i].values);
            return;
        }

        struct FieldWrites {
            std::vector<uint32_t> words;
            std::unordered_map<uint32_t, uint32_t> positions;
        };
        std::array<FieldWrites, 4> fields;
        const auto capacity =
            plan->buffers[size_t(BufferRole::StateWrites)].initial.size() / (3 * sizeof(uint32_t));
        for (uint32_t field = 0; field < fields.size(); ++field) {
            auto reserve = std::min(assignments[field], capacity);
            fields[field].words.reserve(reserve * 2);
            fields[field].positions.reserve(reserve);
        }
        for (size_t w = 0; w < inputs.size(); ++w) {
            const auto& input = inputs[w];
            const auto& destination = destinations[w];
            auto& field = fields[size_t(input.field)];
            for (uint32_t c = 0; c < destination.width; ++c)
                for (uint32_t i = 0; i < rows[w]; ++i) {
                    auto at = destination.first + input.first + i + c * destination.stride;
                    uint32_t value;
                    std::memcpy(&value, &input.values[size_t(i) * destination.width + c], sizeof(value));
                    auto [found, inserted] =
                        field.positions.emplace(at, uint32_t(field.words.size() / 2));
                    if (inserted) {
                        field.words.push_back(at);
                        field.words.push_back(value);
                    } else
                        field.words[size_t(found->second) * 2 + 1] = value;
                }
        }
        uint64_t count = 0;
        for (const auto& field : fields)
            count += field.words.size() / 2;
        if (count > capacity) {
            for (size_t i = 0; i < inputs.size(); ++i)
                write(destinations[i], inputs[i].first, inputs[i].values);
            return;
        }

        std::vector<uint32_t> packed;
        packed.reserve(size_t(count) * 3);
        for (uint32_t field = 0; field < fields.size(); ++field)
            for (size_t i = 0; i < fields[field].words.size(); i += 2) {
                packed.push_back(field);
                packed.push_back(fields[field].words[i]);
                packed.push_back(fields[field].words[i + 1]);
            }
        if (packed.empty())
            return;
        execution->uploadRange(id(BufferRole::StateWrites), 0, bytes(packed));
        dispatch(*programs[plan->stateWriteKernel], plan->kernels[plan->stateWriteKernel].name,
                 {0, uint32_t(count), 0, 0, uint32_t(tick), 0, 1});
    }
    void dynamicEndpoints(const TickInput::Endpoints& input) {
        const auto& layout = plan->relations.at(input.set);
        const auto& set = plan->model.data->relations.at(input.set);
        const auto& type = *plan->types[layout.type];
        if (!set.dynamicEndpoints)
            throw std::invalid_argument("Tick endpoint input requires a dynamic relation set");
        if (input.columns.size() != type.spaces.size())
            throw std::invalid_argument("Dynamic endpoint arity mismatch");
        const auto count = uint32_t(input.columns[0].size());
        if (input.first > layout.count || count > layout.count - input.first)
            throw std::out_of_range("Dynamic endpoint range");
        std::vector<uint32_t> packed(size_t(count) * type.spaces.size());
        for (uint32_t e = 0; e < type.spaces.size(); ++e) {
            if (input.columns[e].size() != count)
                throw std::invalid_argument("Dynamic endpoint columns differ in length");
            // Cache compatible sets once per slot, not a formula comparison per endpoint.
            std::vector<bool> compatible(plan->variables.size());
            const auto& expected = *type.spaces[e];
            auto retract = emitGlsl(expected.retract, "r"), difference = emitGlsl(expected.difference, "d");
            for (uint32_t v = 0; v < plan->variables.size(); ++v) {
                const auto& actual = *plan->spaces[plan->variables[v].space];
                compatible[v] = actual.stateSize == expected.stateSize &&
                                actual.tangentSize == expected.tangentSize &&
                                emitGlsl(actual.retract, "r") == retract &&
                                emitGlsl(actual.difference, "d") == difference;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const auto ref = input.columns[e][i];
                const auto& variable = plan->variables.at(ref.set);
                if (ref.index >= variable.count || !compatible[ref.set])
                    throw std::invalid_argument("Dynamic endpoint reference/space mismatch");
                packed[size_t(i) * type.spaces.size() + e] = variable.first + ref.index;
            }
        }
        if (!packed.empty())
            execution->uploadRange(id(BufferRole::Endpoints),
                                   uint64_t(layout.endpoints + input.first * type.spaces.size()) * 4,
                                   bytes(packed));
        write(range(*plan, StateField::History, input.set), input.first,
              std::vector<float>(size_t(count) * type.history));
    }
    void topology(uint64_t tick) {
        if (!plan->dynamicTopology)
            return;
        const auto& levels = plan->scanLevels;
        dispatch(*programs[plan->resetTopologyKernel], "Reset dynamic incidence",
                 {0, levels[0].count, 0, 0, uint32_t(tick), 0, 1});
        for (const auto& b : plan->countIncidence)
            batch(b, 0, 0, tick, 0);
        for (size_t i = 0; i + 1 < levels.size(); ++i)
            dispatch(*programs[plan->scanKernel], "Scan incidence level " + std::to_string(i),
                     {levels[i].first, levels[i].count, 0, 0, uint32_t(tick), 0, 1, levels[i + 1].first});
        for (int i = int(levels.size()) - 3; i >= 0; --i)
            dispatch(*programs[plan->addOffsetsKernel], "Propagate incidence level " + std::to_string(i),
                     {levels[i].first, levels[i].count, 0, 0, uint32_t(tick), 0, 1, levels[i + 1].first});
        execution->copyBuffer(id(BufferRole::ScanScratch), id(BufferRole::AdjacencyOffsets), 0, 0,
                              uint64_t(levels[0].count) * 4);
        for (const auto& b : plan->scatterIncidence)
            batch(b, 0, 0, tick, 0);
    }
};
struct PublishedState::Storage {
    rg::Registry registry;
    std::array<rg::ResourceId, 3> output, source;
    std::array<Buffer, 3> buffers;
    std::array<rg::AccessState, 3> states;
    std::unique_ptr<rc::GraphContext> execution;
};
void PublishedState::import(rc::GraphContext& consumer, rg::ResourceId target, StateField field) const {
    uint32_t index = field == StateField::Value      ? 0
                     : field == StateField::Velocity ? 1
                     : field == StateField::History  ? 2
                                                     : 3;
    if (!storage_ || index == 3)
        throw std::invalid_argument("Published fields are value, velocity and history");
    rc::NativeResources imports(consumer);
    const auto& d = imports.registry()[target];
    if (d.kind != rg::Kind::Buffer || d.lifetime != rg::Lifetime::Imported || !d.view.readOnly)
        throw std::invalid_argument("Published state requires a read-only imported buffer declaration");
    // The snapshot is immutable; access tracking belongs to each consumer, not
    // to shared snapshot storage that other context threads may also import.
    struct Binding {
        std::shared_ptr<Storage> snapshot;
        rg::AccessState state;
    };
    auto binding = std::make_shared<Binding>(Binding{storage_, storage_->states[index]});
    imports.importBuffer(target, storage_->buffers[index], binding->state, binding);
}
PublishedState Instance::publish() {
    idle();
    PublishedState result;
    result.completed = completed_;
    result.plan = storage_->plan;
    auto copy = std::make_shared<PublishedState::Storage>();
    const std::array<BufferRole, 3> roles = {BufferRole::Values, BufferRole::Velocity, BufferRole::History};
    for (uint32_t i = 0; i < 3; ++i) {
        auto size = plan().buffers[size_t(roles[i])].initial.size();
        auto d = declaration("snapshot" + std::to_string(i), size, false, false);
        copy->output[i] = copy->registry.declare(d);
        d.lifetime = rg::Lifetime::Imported;
        d.name += " source";
        copy->source[i] = copy->registry.declare(d);
    }
    copy->execution = std::make_unique<rc::GraphContext>(core_, copy->registry, "Dynamics snapshot",
                                                         rc::QueueClass::Compute);
    rc::NativeResources from(*storage_->execution), to(*copy->execution);
    std::array<Buffer, 3> sources;
    std::array<rg::AccessState, 3> states;
    for (uint32_t i = 0; i < 3; ++i) {
        sources[i] = from.buffer(storage_->id(roles[i]));
        states[i] = from.bufferState(storage_->id(roles[i]));
        to.importBuffer(copy->source[i], sources[i], states[i]);
        copy->execution->copyBuffer(copy->source[i], copy->output[i]);
    }
    copy->execution->compile();
    copy->execution->record();
    copy->execution->submit();
    copy->execution->wait();
    for (uint32_t i = 0; i < 3; ++i) {
        copy->buffers[i] = to.buffer(copy->output[i]);
        copy->states[i] = to.bufferState(copy->output[i]);
        to.clearImport(copy->source[i]);
    }
    result.storage_ = std::move(copy);
    return result;
}
Instance::Instance(rc::RenderCore& core, PlanRef plan) : core_(core) {
    if (!plan)
        throw std::invalid_argument("Dynamics instance requires a compiled plan");
    model_ = plan->model;
    storage_ = std::make_unique<Storage>(core, std::move(plan));
    storage_->initialize();
    storage_->submit();
    storage_->execution->wait();
    completed_.model = model_.model;
    completed_.modelVersion = model_.version;
}
Instance::~Instance() = default;
const CompiledPlan& Instance::plan() const {
    return *storage_->plan;
}
void Instance::idle() const {
    if (pending_)
        throw std::logic_error("Complete the submitted Dynamics tick before changing this instance");
}
void Instance::step(const TickInput& input) {
    idle();
    if (input.modelVersion != model_.version || input.tick <= completed_.tick || !std::isfinite(input.dt) ||
        input.dt <= 0)
        throw std::invalid_argument(
            "Dynamics tick requires the current model version, advancing tick and positive dt");
    auto& s = *storage_;
    auto readSize = s.readSize(input.reads);
    s.execution->graph().reset();
    s.writes(input.writes, input.tick);
    s.execution->upload(s.id(BufferRole::Diagnostics), std::vector<uint8_t>(8));
    for (const auto& endpoints : input.endpoints)
        s.dynamicEndpoints(endpoints);
    s.topology(input.tick);
    float h = input.dt / s.plan->policy.substeps;
    if (s.candidateBoundsDirty) {
        for (const auto& batch : s.plan->candidateBounds)
            s.batch(batch, h, float(completed_.time), input.tick, 0);
        s.candidateBoundsDirty = false;
    }
    for (const auto& batch : s.plan->prepareCandidates)
        s.batch(batch, h, float(completed_.time), input.tick, 0);
    for (uint32_t substep = 0; !s.plan->predict.empty() && substep < s.plan->policy.substeps; ++substep) {
        float time = float(completed_.time) + h * (substep + 1);
        for (const auto& batch : s.plan->predict)
            s.batch(batch, h, time, input.tick, 0);
        // Shared read-only inputs have just been predicted for this substep.
        // Independent local regions consume that value before the next prediction.
        if (s.plan->localPerSubstep)
            for (const auto& batch : s.plan->local)
                s.batch(batch, h, time - h, input.tick, 0);
        for (uint32_t iteration = 0; iteration < s.plan->policy.iterations; ++iteration) {
            for (const auto& batch : s.plan->solve)
                s.batch(batch, h, time, input.tick, iteration);
            for (const auto& batch : s.plan->apply)
                s.batch(batch, h, time, input.tick, iteration);
        }
        for (const auto& batch : s.plan->recover)
            s.batch(batch, h, time, input.tick, 0);
        for (const auto& batch : s.plan->update)
            s.batch(batch, h, time, input.tick, 0);
    }
    // Closed regions are incidence-independent from the remaining global work.
    if (!s.plan->localPerSubstep)
        for (const auto& batch : s.plan->local)
            s.batch(batch, h, float(completed_.time), input.tick, 0);
    s.recordReads(input.reads, readSize);
    s.submit(input.tick, input.profileRequest, std::max(1u, readSize), 1);
    submittedReads_ = input.reads;
    samples_.clear();
    submitted_ = {model_.model, model_.version, input.tick, completed_.time + input.dt};
    pending_ = true;
}
void Instance::complete() {
    if (!pending_)
        return;
    auto result = storage_->execution->readbackData(storage_->id(BufferRole::Diagnostics));
    std::memcpy(&submitted_.invalidEvaluations, result.data(), 4);
    std::memcpy(&submitted_.singularSystems, result.data() + 4, 4);
    submitted_.gpuMilliseconds = storage_->execution->gpuMilliseconds();
    samples_ = storage_->decodeReads(submittedReads_);
    completed_ = submitted_;
    pending_ = false;
}
bool Instance::poll() {
    if (!pending_)
        return true;
    if (!storage_->execution->poll())
        return false;
    complete();
    return true;
}
const CompletedState& Instance::wait() {
    if (pending_) {
        storage_->execution->wait();
        complete();
    }
    return completed_;
}
void Instance::apply(const ModelCommit& commit) {
    idle();
    const auto& change = commit.changes;
    if (change.topology || change.model != model_.model || change.from != model_.version ||
        change.to != commit.snapshot.version || commit.snapshot.model != model_.model ||
        !commit.snapshot.data)
        throw std::invalid_argument(
            "Numeric commit does not extend this instance; topology changes require a compiled plan");
    auto& s = *storage_;
    s.execution->graph().reset();
    // Merge repeated/overlapping edits before reading the final snapshot's data.
    std::map<std::pair<FieldKind, SetId>, std::vector<std::pair<uint32_t, uint32_t>>> edits;
    for (const auto& f : change.fields)
        if (f.count)
            edits[{f.field, f.set}].push_back({f.first, f.first + f.count});
    for (auto& group : edits) {
        auto kind = group.first.first;
        if (kind == FieldKind::Parameters)
            s.candidateBoundsDirty = true;
        auto set = group.first.second;
        const Field* field = nullptr;
        Range r{};
        if (kind == FieldKind::InverseMetric || kind == FieldKind::VariableEnabled) {
            const auto& layout = s.plan->variables.at(set);
            const auto& data = commit.snapshot.data->variables.at(set);
            if (kind == FieldKind::InverseMetric) {
                field = &data.inverseMetric;
                r = {BufferRole::Metric, layout.metric, layout.count, field->width(), layout.count};
            } else {
                field = &data.enabled;
                r = {BufferRole::VariableEnabled, layout.first, layout.count, 1, layout.count};
            }
        } else {
            const auto& layout = s.plan->relations.at(set);
            const auto& data = commit.snapshot.data->relations.at(set);
            if (kind == FieldKind::Parameters) {
                field = &data.parameters;
                r = {BufferRole::Parameters, layout.parameters, layout.count, field->width(), layout.count};
            } else if (kind == FieldKind::Compliance) {
                field = &data.compliance;
                r = {BufferRole::Compliance, layout.compliance, layout.count, field->width(), layout.count};
            } else {
                field = &data.enabled;
                r = {BufferRole::RelationEnabled, layout.first, layout.count, 1, layout.count};
            }
        }
        auto& ranges = group.second;
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 0; i < ranges.size();) {
            auto first = ranges[i].first, end = ranges[i++].second;
            while (i < ranges.size() && ranges[i].first <= end)
                end = std::max(end, ranges[i++].second);
            auto values = field->slice(first, end - first);
            if (kind == FieldKind::Compliance)
                for (auto value : values)
                    if (value < 0)
                        throw std::invalid_argument("Compliance must be nonnegative");
            s.write(r, first, values);
        }
    }
    if (!edits.empty()) {
        s.submit(completed_.tick);
        s.execution->wait();
    }
    model_ = commit.snapshot;
    completed_.modelVersion = model_.version;
}
std::vector<float> Instance::read(StateField field, SetId set, uint32_t first, uint32_t count) {
    return read(std::vector<StateRange>{{field, set, first, count}}).front();
}
std::vector<std::vector<float>> Instance::read(const std::vector<StateRange>& ranges) {
    idle();
    auto& s = *storage_;
    auto total = s.readSize(ranges);
    if (!total)
        return std::vector<std::vector<float>>(ranges.size());
    s.execution->graph().reset();
    s.recordReads(ranges, total);
    s.submit(completed_.tick, 0, total, 1);
    s.execution->wait();
    return s.decodeReads(ranges);
}
void Instance::install(PlanRef next) {
    idle();
    if (!next || next->model.model != model_.model || next->model.version <= model_.version)
        throw std::invalid_argument("Installed plan must advance this model's version");
    // Source storage remains alive and idle until the migration submission completes.
    auto target = std::make_unique<Storage>(core_, next);
    target->initialize();
    rc::NativeResources old(*storage_->execution), imports(*target->execution);
    Buffer sourceValues = old.buffer(storage_->id(BufferRole::Values));
    Buffer sourceVelocity = old.buffer(storage_->id(BufferRole::Velocity));
    Buffer sourceHistory = old.buffer(storage_->id(BufferRole::History));
    Buffer sourceAcceleration = old.buffer(storage_->id(BufferRole::Acceleration));
    auto valuesState = old.bufferState(storage_->id(BufferRole::Values));
    auto velocityState = old.bufferState(storage_->id(BufferRole::Velocity));
    auto historyState = old.bufferState(storage_->id(BufferRole::History));
    auto accelerationState = old.bufferState(storage_->id(BufferRole::Acceleration));
    imports.importBuffer(target->oldValues, sourceValues, valuesState);
    imports.importBuffer(target->oldVelocity, sourceVelocity, velocityState);
    imports.importBuffer(target->oldHistory, sourceHistory, historyState);
    imports.importBuffer(target->oldAcceleration, sourceAcceleration, accelerationState);
    auto migration = Compiler().migration(plan(), *next);
    const auto& mapping = migration.words;
    if (!mapping.empty()) {
        // Registry resource sizes are fixed before context construction. Migration is
        // streamed through a dynamically sized unbound input rather than modifying it.
        const auto& mapProgram =
            target->execution->compute("Migrate Dynamics state.comp", target->interface + R"(
void main(){uint i=invocation();if(i>=step.count)return;uint k=i*3u,kind=x_migration[k],dst=x_migration[k+1u],src=x_migration[k+2u];
if(kind==0u)x_q[dst]=x_sourceValues[src];else if(kind==1u)x_velocity[dst]=x_sourceVelocity[src];else if(kind==2u)x_history[dst]=x_sourceHistory[src];else x_acceleration[dst]=x_sourceAcceleration[src];}
)");
        // The native import uses one temporary buffer for the whole remap table.
        auto& vk = rc::VulkanAccess::device(core_);
        auto mapBuffer =
            vk.buffer(mapping.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
        std::memcpy(mapBuffer.mapped, mapping.data(), mapping.size() * 4);
        imports.importBuffer(target->migration, mapBuffer);
        try {
            target->dispatch(mapProgram, "Migrate retained Dynamics state",
                             {0, uint32_t(mapping.size() / 3), 0, 0, 0, 0, 1});
            target->submit();
            target->execution->wait();
        } catch (...) {
            vk.destroy(mapBuffer);
            throw;
        }
        vk.destroy(mapBuffer);
    } else {
        target->submit();
        target->execution->wait();
    }
    for (auto id : {target->oldValues, target->oldVelocity, target->oldHistory, target->oldAcceleration,
                    target->migration})
        imports.clearImport(id);
    storage_ = std::move(target);
    model_ = next->model;
    completed_.modelVersion = model_.version;
}
std::optional<GpuProfile> Instance::takeProfile() {
    return storage_->execution->takeProfile();
}
} // namespace whimsical::dynamics
