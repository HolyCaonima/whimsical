#include "physics/dynamics/compiler/FormulaGlsl.h"
#include "Schedule.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace whimsical::dynamics {
// Kernel generation is local to the physics compiler; RenderCore never sees spaces,
// constraints, multiplier state, coloring, or the numerical method.
std::string incidenceKernel(const RelationType&, bool scatter);
namespace {
constexpr const char* Names[] = {"q",
                                 "oldq",
                                 "velocity",
                                 "metric",
                                 "acceleration",
                                 "variables",
                                 "variableWork",
                                 "relations",
                                 "endpoints",
                                 "parameters",
                                 "compliance",
                                 "history",
                                 "lambda",
                                 "variableEnabled",
                                 "relationEnabled",
                                 "relationWork",
                                 "contributions",
                                 "adjOffsets",
                                 "adjEntries",
                                 "diagnostics",
                                 "scanScratch",
                                 "adjCursors",
                                 "regionRanges", "regionState", "localOffsets"};
bool integer(BufferRole r) {
    return r == BufferRole::Variables || r == BufferRole::VariableWork || r == BufferRole::Relations ||
           r == BufferRole::Endpoints || r == BufferRole::RelationWork || r == BufferRole::AdjacencyOffsets ||
           r == BufferRole::AdjacencyEntries || r == BufferRole::Diagnostics ||
           r == BufferRole::ScanScratch || r == BufferRole::AdjacencyCursors || r == BufferRole::RegionRanges ||
           r == BufferRole::RegionState || r == BufferRole::LocalOffsets;
}
uint32_t checked(size_t n) {
    if (n > UINT32_MAX)
        throw std::overflow_error("Dynamics plan exceeds 32-bit element addressing");
    return uint32_t(n);
}
template <class T> uint32_t append(BufferData& target, const std::vector<T>& values) {
    static_assert(sizeof(T) == 4);
    auto first = checked(target.initial.size() / 4);
    if (uint64_t(first) + values.size() > UINT32_MAX)
        throw std::overflow_error("Dynamics buffer element capacity");
    auto offset = target.initial.size();
    target.initial.resize(offset + values.size() * 4);
    if (!values.empty())
        std::memcpy(target.initial.data() + offset, values.data(), values.size() * 4);
    return first;
}
uint32_t field(BufferData& target, const Field& values) {
    std::vector<float> packed(size_t(values.width()) * values.count());
    for (uint32_t c = 0; c < values.width(); ++c)
        for (uint32_t i = 0; i < values.count(); ++i)
            packed[size_t(c) * values.count() + i] = values.at(i, c);
    return append(target, packed);
}
void setWord(BufferData& target, uint32_t index, uint32_t value) {
    std::memcpy(target.initial.data() + size_t(index) * 4, &value, 4);
}
std::string signature(const Space& s) {
    return std::to_string(s.stateSize) + ":" + std::to_string(s.tangentSize) + emitGlsl(s.retract, "r") +
           emitGlsl(s.difference, "d");
}
std::string signature(const RelationType& t) {
    std::string s =
        std::to_string(t.parameters) + ":" + std::to_string(t.history) + ":" + std::to_string(int(t.kind));
    for (const auto& space : t.spaces)
        s += signature(*space);
    s += emitGlsl(t.residual, "r");
    if (t.update)
        s += emitGlsl(*t.update, "u");
    return s;
}
} // namespace
std::string CompiledPlan::interface() const {
    return R"(
layout(local_size_x=128) in;
layout(push_constant) uniform Step {
    uint first; uint count; float h; float time;
    uint tick; uint iteration; float relaxation; uint reserved;
} step;
uint invocation() { return gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * 8388480u; }
struct Variable {uint q; uint v; uint m; uint stride; uint flags; uint id;};
Variable variable(uint id) {
    uint k=id*5u; return Variable(x_variables[k],x_variables[k+1u],x_variables[k+2u],x_variables[k+3u],x_variables[k+4u],id);
}
struct Relation {uint e; uint p; uint a; uint h; uint l; uint c; uint stride; uint cs; uint id;};
Relation relation(uint id) {
    uint k=id*8u;return Relation(x_relations[k],x_relations[k+1u],x_relations[k+2u],x_relations[k+3u],
        x_relations[k+4u],x_relations[k+5u],x_relations[k+6u],x_relations[k+7u],id);
}
)";
}
PlanRef Compiler::compile(const ModelSnapshot& model, const SolverPolicy& policy) const {
    if (!model.data || !model.model || !model.version)
        throw std::invalid_argument("Compile requires a committed model");
    if (!policy.substeps || !policy.iterations || policy.colorBudget > 64 ||
        !std::isfinite(policy.relaxation) || policy.relaxation <= 0 || policy.relaxation > 1)
        throw std::invalid_argument("Invalid Dynamics solver policy");
    auto p = std::make_shared<CompiledPlan>();
    p->model = model;
    p->policy = policy;
    for (uint32_t i = 0; i < uint32_t(BufferRole::Count); ++i) {
        p->buffers[i].name = Names[i];
        p->buffers[i].integers = integer(BufferRole(i));
    }
    auto buffer = [&](BufferRole r) -> BufferData& { return p->buffers[size_t(r)]; };
    std::map<std::string, uint32_t> spaceIds, typeIds;
    std::vector<std::vector<uint32_t>> variableGroups;
    std::vector<bool> writable;
    std::vector<uint32_t> variableMeta, relationMeta, endpointData;
    for (const auto& set : model.data->variables) {
        set.space->validate();
        auto key = signature(*set.space);
        auto it = spaceIds.find(key);
        uint32_t space;
        if (it == spaceIds.end()) {
            space = checked(p->spaces.size());
            spaceIds.emplace(key, space);
            p->spaces.push_back(set.space);
            variableGroups.emplace_back();
        } else
            space = it->second;
        VariableLayout layout{checked(writable.size()),
                              set.count,
                              space,
                              field(buffer(BufferRole::Values), set.initial),
                              field(buffer(BufferRole::Velocity), set.velocity),
                              field(buffer(BufferRole::Metric), set.inverseMetric)};
        p->variables.push_back(layout);
        field(buffer(BufferRole::VariableEnabled), set.enabled);
        for (uint32_t i = 0; i < set.count; ++i) {
            // Metrics are symmetric positive semidefinite. Reject negative diagonal or
            // asymmetric input instead of silently turning model errors into damping.
            for (uint32_t a = 0; a < set.space->tangentSize; ++a)
                for (uint32_t b = 0; b < set.space->tangentSize; ++b) {
                    float v = set.inverseMetric.at(i, a * set.space->tangentSize + b);
                    if ((a == b && v < 0) ||
                        std::abs(v - set.inverseMetric.at(i, b * set.space->tangentSize + a)) > 1e-6f)
                        throw std::invalid_argument(
                            "Inverse metric must be symmetric with nonnegative diagonal: " + set.name);
                }
            variableMeta.insert(variableMeta.end(), {layout.values + i, layout.velocity + i,
                                                     layout.metric + i, set.count, set.readOnly ? 1u : 0u});
            variableGroups[space].push_back(layout.first + i);
            writable.push_back(!set.readOnly);
        }
    }
    append(buffer(BufferRole::Variables), variableMeta);
    p->statistics.variables = writable.size();
    buffer(BufferRole::Previous).initial = buffer(BufferRole::Values).initial;
    buffer(BufferRole::Acceleration).initial.resize(buffer(BufferRole::Velocity).initial.size());
    struct Instance {
        uint32_t id, set, local, type;
        int32_t color;
        uint32_t endpoints, arity;
    };
    // Endpoint columns are already O(E). Temporary compiler instances keep only their
    // endpoint references; no quadratic constraint-conflict graph is materialized.
    std::vector<Instance> instances;
    std::vector<uint64_t> used(writable.size());
    std::vector<uint32_t> degree(writable.size());
    const uint64_t allowed =
        policy.colorBudget == 64 ? UINT64_MAX : ((uint64_t(1) << policy.colorBudget) - 1);
    for (uint32_t setId = 0; setId < model.data->relations.size(); ++setId) {
        const auto& set = model.data->relations[setId];
        set.type->validate();
        auto key = signature(*set.type);
        auto it = typeIds.find(key);
        uint32_t type;
        if (it == typeIds.end()) {
            type = checked(p->types.size());
            typeIds.emplace(key, type);
            p->types.push_back(set.type);
        } else
            type = it->second;
        RelationLayout layout{
            checked(instances.size()),
            set.count,
            type,
            field(buffer(BufferRole::Parameters), set.parameters),
            field(buffer(BufferRole::Compliance), set.compliance),
            field(buffer(BufferRole::History), set.initialHistory),
            append(buffer(BufferRole::Multipliers), std::vector<float>(size_t(set.count) * set.type->rows)),
            checked(endpointData.size())};
        p->dynamicTopology = p->dynamicTopology || set.dynamicEndpoints;
        if (set.dynamicEndpoints && policy.mode == SolveMode::Colored)
            throw std::invalid_argument("Dynamic endpoint sets require Hybrid or Jacobi policy");
        p->relations.push_back(layout);
        field(buffer(BufferRole::RelationEnabled), set.enabled);
        std::vector<uint32_t> endpointSpaces;
        for (const auto& space : set.type->spaces) {
            auto found = spaceIds.find(signature(*space));
            endpointSpaces.push_back(found == spaceIds.end() ? UINT32_MAX : found->second);
        }
        for (uint32_t row = 0; row < set.count; ++row) {
            Instance instance{
                checked(instances.size()),    setId, row, type, -1, checked(endpointData.size()),
                checked(set.endpoints.size())};
            uint64_t conflict = 0;
            for (uint32_t e = 0; e < set.endpoints.size(); ++e) {
                auto ref = set.endpoints[e]->at(row);
                if (ref.set >= p->variables.size() || ref.index >= p->variables[ref.set].count)
                    throw std::invalid_argument("Relation references a missing variable: " + set.name);
                if (p->variables[ref.set].space != endpointSpaces[e])
                    throw std::invalid_argument("Relation endpoint space mismatch: " + set.name);
                auto id = p->variables[ref.set].first + ref.index;
                endpointData.push_back(id);
                if (writable[id])
                    conflict |= used[id];
            }
            for (uint32_t r = 0; r < set.type->rows; ++r)
                if (set.compliance.at(row, r) < 0)
                    throw std::invalid_argument("Compliance must be nonnegative: " + set.name);
            auto available = allowed & ~conflict;
            if (policy.mode != SolveMode::Jacobi && !set.dynamicEndpoints && available) {
                uint32_t color = 0;
                while (!(available & (uint64_t(1) << color)))
                    ++color;
                instance.color = int32_t(color);
                p->statistics.colors = std::max(p->statistics.colors, color + 1);
                for (uint32_t e = 0; e < instance.arity; ++e) {
                    auto id = endpointData[instance.endpoints + e];
                    if (writable[id])
                        used[id] |= uint64_t(1) << color;
                }
                ++p->statistics.coloredRelations;
            } else {
                if (policy.mode == SolveMode::Colored)
                    throw std::runtime_error(
                        "Color budget exhausted; choose Hybrid/Jacobi or increase the budget");
                for (uint32_t e = 0; e < instance.arity; ++e) {
                    auto id = endpointData[instance.endpoints + e];
                    auto begin = endpointData.begin() + instance.endpoints;
                    if (writable[id] && std::find(begin, begin + e, id) == begin + e)
                        ++degree[id];
                }
                ++p->statistics.jacobiRelations;
            }
            auto end = instance.endpoints;
            relationMeta.insert(relationMeta.end(),
                                {end, layout.parameters + row, layout.compliance + row, layout.history + row,
                                 layout.multipliers + row, 0, set.count, 0});
            p->statistics.endpointReferences += instance.arity;
            instances.push_back(std::move(instance));
        }
    }
    append(buffer(BufferRole::Endpoints), endpointData);
    append(buffer(BufferRole::Relations), relationMeta);
    buffer(BufferRole::Diagnostics).initial.resize(8);
    p->statistics.relations = instances.size();
    p->multiplierCount = checked(buffer(BufferRole::Multipliers).initial.size() / 4);
    std::vector<uint32_t> offsets(writable.size() + 1);
    for (size_t i = 0; i < degree.size(); ++i)
        offsets[i + 1] = checked(uint64_t(offsets[i]) + degree[i]);
    std::vector<uint32_t> entries(size_t(offsets.back()) * 2), cursor = offsets;
    for (uint32_t setId = 0; setId < p->relations.size(); ++setId) {
        const auto& layout = p->relations[setId];
        const auto& type = *p->types[layout.type];
        uint32_t count = 0;
        for (uint32_t i = 0; i < layout.count; ++i)
            if (instances[layout.first + i].color < 0)
                ++count;
        auto base =
            append(buffer(BufferRole::Contributions), std::vector<float>(size_t(count) * type.tangentSize()));
        uint32_t index = 0;
        for (uint32_t i = 0; i < layout.count; ++i) {
            const auto& in = instances[layout.first + i];
            if (in.color >= 0)
                continue;
            auto rowBase = base + index++;
            uint32_t local = 0;
            for (uint32_t e = 0; e < in.arity; ++e) {
                auto id = endpointData[in.endpoints + e];
                auto begin = endpointData.begin() + in.endpoints;
                if (writable[id] && std::find(begin, begin + e, id) == begin + e) {
                    auto at = cursor[id]++;
                    entries[size_t(at) * 2] = rowBase + local * count;
                    entries[size_t(at) * 2 + 1] = count;
                }
                local += type.spaces[e]->tangentSize;
            }
            setWord(buffer(BufferRole::Relations), in.id * 8 + 5, rowBase);
            setWord(buffer(BufferRole::Relations), in.id * 8 + 7, count);
        }
    }
    if (p->dynamicTopology) {
        uint64_t capacity = 0;
        for (const auto& in : instances)
            if (in.color < 0)
                capacity += in.arity;
        entries.resize(size_t(checked(capacity)) * 2);
        append(buffer(BufferRole::AdjacencyCursors), std::vector<uint32_t>(writable.size()));
        auto count = checked(writable.size() + 1);
        for (;;) {
            auto first = append(buffer(BufferRole::ScanScratch), std::vector<uint32_t>(count));
            p->scanLevels.push_back({first, count});
            if (count == 1)
                break;
            count = uint32_t((uint64_t(count) + 127) / 128);
        }
    }
    append(buffer(BufferRole::AdjacencyOffsets), offsets);
    append(buffer(BufferRole::AdjacencyEntries), entries);
    p->statistics.incidenceEntries = offsets.back();
    std::vector<KernelFunction> functions;
    auto kernel = [&](std::string name, std::string source) {
        auto id = checked(p->kernels.size());
        p->kernels.push_back({std::move(name), std::move(source)});
        functions.emplace_back();
        return id;
    };
    auto operation = [&](std::string name, KernelFunction function) {
        auto id = kernel(std::move(name), globalKernel(function));
        functions[id] = std::move(function);
        return id;
    };
    for (uint32_t space = 0; space < p->spaces.size(); ++space) {
        const auto& s = *p->spaces[space];
        auto first = append(buffer(BufferRole::VariableWork), variableGroups[space]),
             count = checked(variableGroups[space].size());
        auto suffix = std::to_string(space);
        p->predict.push_back(
            {operation("Predict " + s.name, variableFunction(s, "predict", "predict" + suffix)), first, count});
        p->recover.push_back(
            {operation("Recover " + s.name, variableFunction(s, "recover", "recover" + suffix)), first, count});
        if (p->statistics.jacobiRelations)
            p->apply.push_back(
                {operation("Gather " + s.name, variableFunction(s, "apply", "gather" + suffix)), first, count});
    }
    std::map<std::pair<int32_t, uint32_t>, std::vector<uint32_t>> groups;
    for (const auto& in : instances)
        groups[{in.color < 0 ? int32_t(policy.colorBudget) : in.color, in.type}].push_back(in.id);
    std::map<std::pair<uint32_t, bool>, uint32_t> solveKernels;
    for (const auto& group : groups) {
        auto type = group.first.second;
        bool jacobi = group.first.first == int32_t(policy.colorBudget);
        auto key = std::make_pair(type, jacobi);
        auto found = solveKernels.find(key);
        uint32_t program;
        if (found == solveKernels.end()) {
            program = operation(std::string(jacobi ? "Jacobi " : "Colored ") + p->types[type]->name,
                                relationFunction(*p->types[type], jacobi, false,
                                                 std::string(jacobi ? "jacobi" : "colored") +
                                                     std::to_string(type)));
            solveKernels.emplace(key, program);
        } else
            program = found->second;
        p->solve.push_back({program, append(buffer(BufferRole::RelationWork), group.second),
                            checked(group.second.size()), jacobi ? -1 : group.first.first});
        if (p->dynamicTopology && jacobi) {
            auto batch = p->solve.back();
            batch.kernel =
                kernel("Count incidence " + p->types[type]->name, incidenceKernel(*p->types[type], false));
            p->countIncidence.push_back(batch);
            batch.kernel =
                kernel("Scatter incidence " + p->types[type]->name, incidenceKernel(*p->types[type], true));
            p->scatterIncidence.push_back(batch);
        }
    }
    for (uint32_t type = 0; type < p->types.size(); ++type)
        if (p->types[type]->update) {
            std::vector<uint32_t> work;
            for (const auto& in : instances)
                if (in.type == type)
                    work.push_back(in.id);
            p->update.push_back(
                {operation("Commit " + p->types[type]->name,
                           relationFunction(*p->types[type], false, true, "commit" + std::to_string(type))),
                 append(buffer(BufferRole::RelationWork), work), checked(work.size())});
        }
    p->resetKernel =
        kernel("Reset multipliers", "void main(){uint i=invocation();if(i<step.count)x_lambda[i]=0.0;}\n");
    if (p->dynamicTopology) {
        p->resetTopologyKernel = kernel("Reset incidence counts", R"(
void main(){uint i=invocation();if(i>=step.count)return;x_scanScratch[i]=0u;if(i+1u<step.count)x_adjCursors[i]=0u;}
)");
        p->scanKernel = kernel("Scan incidence blocks", R"(
shared uint partial[128];
void main(){uint i=invocation(),lane=gl_LocalInvocationID.x;uint value=i<step.count?x_scanScratch[step.first+i]:0u;
partial[lane]=value;barrier();
for(uint offset=1u;offset<128u;offset*=2u){uint addend=lane>=offset?partial[lane-offset]:0u;barrier();partial[lane]+=addend;barrier();}
if(i<step.count)x_scanScratch[step.first+i]=partial[lane]-value;
if(lane==127u && i-lane<step.count)x_scanScratch[step.reserved+i/128u]=partial[lane];}
)");
        p->addOffsetsKernel = kernel("Propagate incidence offsets", R"(
void main(){uint i=invocation();if(i<step.count)x_scanScratch[step.first+i]+=x_scanScratch[step.reserved+i/128u];}
)");
    }
    lowerSchedule(*p, functions);
    for (auto& b : p->buffers) {
        if (b.initial.empty())
            b.initial.resize(4);
        p->statistics.storageBytes += b.initial.size();
    }
    return p;
}
} // namespace whimsical::dynamics
