#include "Schedule.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace whimsical::dynamics {
namespace {
constexpr uint32_t NoRegion = UINT32_MAX;
// Backend work budgets, independent of model names and numerical field values.
constexpr uint32_t Lanes = 128, RelationBudget = 512, StateWordBudget = 2048, ColorWindowBudget = 4;
constexpr uint64_t FunctionWordBudget = 2048, SourceByteBudget = 256 * 1024;
std::vector<uint32_t> words(const BufferData& data) {
    std::vector<uint32_t> result(data.initial.size() / 4);
    if (!result.empty())
        std::memcpy(result.data(), data.initial.data(), data.initial.size());
    return result;
}
void store(BufferData& data, const std::vector<uint32_t>& values) {
    if (values.size() > UINT32_MAX)
        throw std::overflow_error("Dynamics schedule exceeds 32-bit addressing");
    data.initial.resize(values.size() * 4);
    if (!values.empty())
        std::memcpy(data.initial.data(), values.data(), data.initial.size());
    data.words = values.size();
    data.fills.clear();
}
uint64_t dispatchCount(const CompiledPlan& p) {
    auto count = [](const std::vector<Batch>& batches) {
        return uint64_t(std::count_if(batches.begin(), batches.end(), [](const Batch& b) { return b.count; }));
    };
    if (!count(p.predict))
        return 0;
    return p.policy.substeps *
           (count(p.predict) + p.policy.iterations * (count(p.solve) + count(p.apply) + count(p.iteration)) +
            count(p.recover) + count(p.update));
}
struct Components {
    std::vector<uint32_t> parent, size;
    explicit Components(uint32_t count) : parent(count), size(count, 1) {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    uint32_t root(uint32_t v) {
        while (v != parent[v]) {
            parent[v] = parent[parent[v]];
            v = parent[v];
        }
        return v;
    }
    void join(uint32_t a, uint32_t b) {
        a = root(a);
        b = root(b);
        if (a == b)
            return;
        if (size[a] < size[b])
            std::swap(a, b);
        parent[b] = a;
        size[a] += size[b];
    }
};
void collapseVariableDispatches(CompiledPlan& p, const std::vector<KernelFunction>& functions) {
    if (p.policy.execution == ExecutionMode::Global)
        return;
    // Predict, gather and recover each own one variable. Space boundaries carry
    // no dependency; contiguous work ranges keep their type branches coherent.
    auto work = words(p.buffers[size_t(BufferRole::VariableWork)]);
    for (auto entry : {std::make_pair(&p.predict, "Predict variables"),
                       std::make_pair(&p.apply, "Gather variables"),
                       std::make_pair(&p.recover, "Recover variables")}) {
        auto& batches = *entry.first;
        if (batches.size() < 2)
            continue;
        std::set<uint32_t> kernels;
        uint64_t sourceBytes = 0;
        for (const auto& batch : batches)
            if (kernels.insert(batch.kernel).second)
                sourceBytes += functions[batch.kernel].source.size();
        if (sourceBytes > SourceByteBudget)
            continue;
        std::ostringstream source;
        source << stateAccess(StateStorage::Global);
        for (auto kernel : kernels)
            source << functions[kernel].source;
        source << "void main(){uint lane=invocation();if(lane>=step.count)return;"
                  "uint id=x_variableWork[step.first+lane];float inverseH2=1.0/(step.h*step.h);";
        std::vector<uint32_t> joined;
        for (size_t i = 0; i < batches.size(); ++i) {
            const auto& batch = batches[i];
            joined.insert(joined.end(), work.begin() + batch.first, work.begin() + batch.first + batch.count);
            if (i)
                source << "else ";
            if (i + 1 < batches.size())
                source << "if(lane<" << joined.size() << "u)";
            source << functions[batch.kernel].entry
                   << "(id,step.h,step.time,step.relaxation,inverseH2,step.iteration);";
        }
        source << "}\n";
        batches = {{uint32_t(p.kernels.size()), uint32_t(work.size()), uint32_t(joined.size())}};
        work.insert(work.end(), joined.begin(), joined.end());
        p.kernels.push_back({entry.second, source.str()});
    }
    store(p.buffers[size_t(BufferRole::VariableWork)], work);
}
void collapseRelationDispatches(CompiledPlan& p) {
    auto work = words(p.buffers[size_t(BufferRole::RelationWork)]);
    std::vector<Batch> collapsed;
    for (size_t first = 0; first < p.solve.size();) {
        size_t end = first + 1;
        while (end < p.solve.size() && p.solve[end].color == p.solve[first].color)
            ++end;
        const auto dispatch =
            p.solve[first].color < 0 ? p.jacobiDispatchKernel : p.coloredDispatchKernel;
        if (dispatch == UINT32_MAX || end == first + 1) {
            collapsed.insert(collapsed.end(), p.solve.begin() + first, p.solve.begin() + end);
        } else {
            auto batch = p.solve[first];
            batch.kernel = dispatch;
            std::vector<uint32_t> joined;
            for (size_t i = first; i < end; ++i) {
                const auto& source = p.solve[i];
                joined.insert(joined.end(), work.begin() + source.first,
                              work.begin() + source.first + source.count);
            }
            batch.first = uint32_t(work.size());
            batch.count = uint32_t(joined.size());
            work.insert(work.end(), joined.begin(), joined.end());
            collapsed.push_back(batch);
        }
        first = end;
    }
    p.solve = std::move(collapsed);
    store(p.buffers[size_t(BufferRole::RelationWork)], work);
}
bool buildColorWindow(const CompiledPlan& p, const std::vector<Batch>& stages,
                      const std::vector<uint32_t>& work,
                      std::vector<std::vector<std::vector<uint32_t>>>& regions) {
    struct Item {
        uint32_t relation, phase;
    };
    std::vector<Item> items;
    for (uint32_t phase = 0; phase < stages.size(); ++phase)
        for (uint32_t i = 0; i < stages[phase].count; ++i)
            items.push_back({work[stages[phase].first + i], phase});
    if (items.empty())
        return false;

    Components components(uint32_t(items.size()));
    std::vector<uint32_t> variableOwner(p.statistics.variables, NoRegion);
    for (uint32_t i = 0; i < items.size(); ++i) {
        auto relation = items[i].relation;
        auto type = p.relationType(relation);
        for (uint32_t slot = 0; slot < p.types[type]->spaces.size(); ++slot) {
            auto variable = p.endpoint(relation, slot);
            if (p.variableReadOnly(variable))
                continue;
            if (variableOwner[variable] == NoRegion)
                variableOwner[variable] = i;
            else
                components.join(i, variableOwner[variable]);
        }
    }

    using PhaseWork = std::vector<std::vector<uint32_t>>;
    std::vector<uint32_t> componentId(items.size(), NoRegion);
    std::vector<PhaseWork> componentWork;
    for (uint32_t i = 0; i < items.size(); ++i) {
        auto root = components.root(i);
        if (componentId[root] == NoRegion) {
            componentId[root] = uint32_t(componentWork.size());
            componentWork.emplace_back(stages.size());
        }
        componentWork[componentId[root]][items[i].phase].push_back(items[i].relation);
    }

    // A dependency component cannot cross workgroups. Pack only independent
    // components, keeping each color near one relation evaluation per lane.
    PhaseWork packed(stages.size());
    uint32_t packedTotal = 0;
    for (auto& component : componentWork) {
        uint32_t total = 0;
        for (const auto& phase : component)
            total += uint32_t(phase.size());
        if (total > RelationBudget)
            return false;
        bool fits = packedTotal + total <= RelationBudget;
        for (uint32_t phase = 0; phase < stages.size(); ++phase)
            fits = fits && packed[phase].size() + component[phase].size() <= Lanes;
        if (packedTotal && !fits) {
            regions.push_back(std::move(packed));
            packed = PhaseWork(stages.size());
            packedTotal = 0;
        }
        for (uint32_t phase = 0; phase < stages.size(); ++phase)
            packed[phase].insert(packed[phase].end(), component[phase].begin(), component[phase].end());
        packedTotal += total;
    }
    if (packedTotal)
        regions.push_back(std::move(packed));
    return !regions.empty();
}
void fuseColorWindows(CompiledPlan& p, const std::vector<KernelFunction>& functions) {
    if (p.dynamicTopology || p.policy.execution == ExecutionMode::Global)
        return;
    size_t colored = 0;
    while (colored < p.solve.size() && p.solve[colored].color >= 0)
        ++colored;
    if (colored < 2)
        return;

    auto relationWork = words(p.buffers[size_t(BufferRole::RelationWork)]);
    auto ranges = words(p.buffers[size_t(BufferRole::RegionRanges)]);
    std::map<uint32_t, uint32_t> kernels;
    auto windowKernel = [&](const std::vector<Batch>& stages) {
        auto function = stages.front().kernel;
        for (const auto& stage : stages)
            if (stage.kernel != function) {
                function = p.coloredDispatchKernel;
                break;
            }
        if (function == UINT32_MAX)
            return UINT32_MAX;
        auto found = kernels.find(function);
        if (found != kernels.end())
            return found->second;
        std::ostringstream source;
        // Window components own their writes. Only invocations in this workgroup
        // consume the preceding color, so a device-scope memory fence is needless.
        source << stateAccess(StateStorage::Global) << functions[function].source
               << "void main(){uint region=gl_WorkGroupID.x+gl_WorkGroupID.y*65535u;"
                  "if(region>=step.count/128u)return;uint lane=gl_LocalInvocationID.x;"
                  "uint at=x_regionRanges[step.first+region],phases=x_regionRanges[at++];"
                  "float inverseH2=1.0/(step.h*step.h);"
                  "for(uint phase=0u;phase<phases;++phase){"
                  "uint first=x_regionRanges[at+phase*2u],count=x_regionRanges[at+phase*2u+1u];"
                  "for(uint j=lane;j<count;j+=128u)"
               << functions[function].entry
               << "(x_relationWork[first+j],step.h,step.time,step.relaxation,inverseH2,step.iteration);"
                  "if(phase+1u<phases){groupMemoryBarrier();barrier();}}}\n";
        auto kernel = uint32_t(p.kernels.size());
        p.kernels.push_back({"Solve color windows / " + p.kernels[function].name, source.str(),
                             {BufferRole::Values}});
        kernels.emplace(function, kernel);
        return kernel;
    };
    std::vector<Batch> fused;
    for (size_t first = 0; first < colored;) {
        if (first + 1 == colored) {
            fused.push_back(p.solve[first++]);
            continue;
        }
        std::vector<Batch> stages;
        std::vector<std::vector<std::vector<uint32_t>>> regions;
        const auto limit = std::min(colored, first + size_t(ColorWindowBudget));
        for (size_t end = first + 2; end <= limit; ++end) {
            std::vector<Batch> candidate(p.solve.begin() + first, p.solve.begin() + end);
            std::vector<std::vector<std::vector<uint32_t>>> candidateRegions;
            if (!buildColorWindow(p, candidate, relationWork, candidateRegions))
                break;
            stages = std::move(candidate);
            regions = std::move(candidateRegions);
        }
        if (stages.empty()) {
            fused.push_back(p.solve[first++]);
            continue;
        }
        auto kernel = windowKernel(stages);
        if (kernel == UINT32_MAX) {
            fused.push_back(p.solve[first++]);
            continue;
        }
        auto table = uint32_t(ranges.size());
        ranges.resize(ranges.size() + regions.size());
        for (uint32_t region = 0; region < regions.size(); ++region) {
            ranges[table + region] = uint32_t(ranges.size());
            ranges.push_back(uint32_t(stages.size()));
            for (const auto& phase : regions[region]) {
                ranges.push_back(uint32_t(relationWork.size()));
                ranges.push_back(uint32_t(phase.size()));
                relationWork.insert(relationWork.end(), phase.begin(), phase.end());
            }
        }
        if (uint64_t(regions.size()) * Lanes > UINT32_MAX)
            throw std::overflow_error("Dynamics color-window dispatch exceeds 32-bit addressing");
        fused.push_back({kernel, table, uint32_t(regions.size()) * Lanes, stages.back().color});
        ++p.statistics.colorWindows;
        p.statistics.colorWindowRegions += regions.size();
        first += stages.size();
    }
    fused.insert(fused.end(), p.solve.begin() + colored, p.solve.end());
    if (kernels.empty())
        return;
    p.solve = std::move(fused);
    store(p.buffers[size_t(BufferRole::RelationWork)], relationWork);
    store(p.buffers[size_t(BufferRole::RegionRanges)], ranges);
}
void pruneKernels(CompiledPlan& p) {
    std::vector<bool> used(p.kernels.size());
    auto mark = [&](const std::vector<Batch>& batches) {
        for (const auto& batch : batches)
            used[batch.kernel] = true;
    };
    used[p.stateWriteKernel] = true;
    mark(p.predict);
    mark(p.solve);
    mark(p.apply);
    mark(p.iteration);
    mark(p.recover);
    mark(p.update);
    mark(p.local);
    mark(p.prepareCandidates);
    mark(p.candidateBounds);
    if (p.dynamicTopology) {
        used[p.resetTopologyKernel] = true;
        used[p.scanKernel] = true;
        used[p.addOffsetsKernel] = true;
        mark(p.countIncidence);
        mark(p.scatterIncidence);
    }
    std::vector<uint32_t> mapping(p.kernels.size(), UINT32_MAX);
    std::vector<Kernel> kernels;
    for (uint32_t id = 0; id < p.kernels.size(); ++id)
        if (used[id]) {
            mapping[id] = uint32_t(kernels.size());
            kernels.push_back(std::move(p.kernels[id]));
        }
    auto remap = [&](std::vector<Batch>& batches) {
        for (auto& batch : batches)
            batch.kernel = mapping[batch.kernel];
    };
    auto remapOptional = [&](uint32_t& id) {
        if (id != UINT32_MAX)
            id = mapping[id];
    };
    p.stateWriteKernel = mapping[p.stateWriteKernel];
    remapOptional(p.coloredDispatchKernel);
    remapOptional(p.jacobiDispatchKernel);
    if (p.dynamicTopology) {
        p.resetTopologyKernel = mapping[p.resetTopologyKernel];
        p.scanKernel = mapping[p.scanKernel];
        p.addOffsetsKernel = mapping[p.addOffsetsKernel];
        remap(p.countIncidence);
        remap(p.scatterIncidence);
    }
    remap(p.predict);
    remap(p.solve);
    remap(p.apply);
    remap(p.iteration);
    remap(p.recover);
    remap(p.update);
    remap(p.local);
    remap(p.prepareCandidates);
    remap(p.candidateBounds);
    p.kernels = std::move(kernels);
}
struct Cost {
    uint64_t variables = 0, relations = 0, state = 0, temporaries = 0;
    bool fits() const {
        return variables <= Lanes && relations <= RelationBudget && state <= StateWordBudget &&
               temporaries <= FunctionWordBudget;
    }
    Cost operator+(const Cost& b) const {
        return {variables + b.variables, relations + b.relations, state + b.state,
                std::max(temporaries, b.temporaries)};
    }
};
// Conservative estimate of the dense local solve's explicitly generated arrays.
uint64_t temporaryWords(const CompiledPlan& p, uint32_t type) {
    const auto& t = *p.types[type];
    uint64_t n = t.inputSize(), q = 0, d = p.activeTangent(type), m = t.rows;
    for (uint32_t slot = 0; slot < t.spaces.size(); ++slot)
        if (!p.typeReadOnly[type][slot])
            q += t.spaces[slot]->stateSize;
    uint64_t result = n + m * q + 2 * m * d + m * m + 6 * m;
    for (uint32_t slot = 0; slot < t.spaces.size(); ++slot)
        if (!p.typeReadOnly[type][slot]) {
            const auto& s = t.spaces[slot];
            result += 2 * s->stateSize + s->tangentSize + uint64_t(s->stateSize) * s->tangentSize;
        }
    return result;
}
} // namespace

void lowerSchedule(CompiledPlan& p, const std::vector<KernelFunction>& functions) {
    p.statistics.referenceDispatches = dispatchCount(p);
    // A summed relation traverses a variadic dependency domain. Fixed-endpoint
    // region/color-window fusion cannot consume that domain as one local pair.
    if (p.summedRelations) {
        p.statistics.dispatches = dispatchCount(p);
        pruneKernels(p);
        return;
    }
    // Dynamic endpoints can connect any compatible variable at a later tick.
    if (p.dynamicTopology || p.policy.execution == ExecutionMode::Global ||
        !p.deferredJacobiDomains.empty()) {
        collapseVariableDispatches(p, functions);
        collapseRelationDispatches(p);
        fuseColorWindows(p, functions);
        p.statistics.dispatches = dispatchCount(p);
        pruneKernels(p);
        return;
    }
    const auto variableCount = uint32_t(p.statistics.variables);
    Components components(variableCount);
    auto readOnly = [&](uint32_t id) { return p.variableReadOnly(id); };
    // Components are defined by writes. Shared read-only input is an effect at a
    // substep boundary, not a reason to merge independent solve regions.
    for (uint32_t set = 0; set < p.relations.size(); ++set) {
        for (const auto& domain : p.bindings[set].domains) {
            // The used vertices of a full/triangular bipartite domain form one
            // component when both sides write. Removing a directed diagonal
            // retains that property for at least three members. Visit columns,
            // not the Cartesian number of edges, to establish connectivity.
            if (domain.map != BindingDomain::Map::Zip && domain.affineFields() &&
                (domain.map != BindingDomain::Map::Directed || domain.right >= 3)) {
                bool active[2] = {};
                for (uint32_t slot = 0; slot < domain.fields.size(); ++slot)
                    active[slot < domain.split ? 0 : 1] = active[slot < domain.split ? 0 : 1] ||
                        !p.model.data->variables[domain.fields[slot].first.set].readOnly;
                uint32_t owner = NoRegion;
                for (uint32_t side = 0; side < 2; ++side) {
                    const auto begin = side ? domain.split : 0u;
                    const auto end = side ? uint32_t(domain.fields.size()) : domain.split;
                    const auto members = domain.memberRange(begin);
                    for (uint32_t member = members.first; member < members.second && active[side]; ++member) {
                        if (!(active[0] && active[1]))
                            owner = NoRegion;
                        for (uint32_t slot = begin; slot < end; ++slot) {
                            const auto ref = domain.fields[slot].at(member);
                            const auto id = p.variables[ref.set].first + ref.index;
                            if (readOnly(id))
                                continue;
                            if (owner == NoRegion)
                                owner = id;
                            else
                                components.join(owner, id);
                        }
                    }
                }
            } else {
                for (uint32_t row = 0; row < domain.count; ++row) {
                    uint32_t owner = NoRegion;
                    for (uint32_t slot = 0; slot < domain.fields.size(); ++slot) {
                        const auto ref = domain.at(row, slot);
                        const auto id = p.variables[ref.set].first + ref.index;
                        if (readOnly(id))
                            continue;
                        if (owner == NoRegion)
                            owner = id;
                        else
                            components.join(owner, id);
                    }
                }
            }
        }
    }
    std::vector<uint32_t> relationWriter(p.statistics.relations, NoRegion), inputOwner(variableCount, NoRegion);
    std::vector<bool> sharedInput(variableCount);
    for (uint32_t set = 0; set < p.relations.size(); ++set) {
        const auto& layout = p.relations[set];
        auto arity = uint32_t(p.types[layout.type]->spaces.size());
        for (const auto& domain : p.bindings[set].domains) {
            // A uniform writer component is a property of the reference image.
            // Prove it once over member columns, then broadcast it over rows.
            uint32_t writer = NoRegion;
            bool uniformWriter = domain.affineFields();
            for (uint32_t slot = 0; uniformWriter && slot < arity; ++slot) {
                const auto& source = domain.fields[slot];
                if (p.model.data->variables[source.first.set].readOnly) continue;
                const auto members = domain.memberRange(slot);
                for (uint32_t member = members.first; member < members.second; ++member) {
                    const auto ref = source.at(member);
                    const auto owner = components.root(p.variables[ref.set].first + ref.index);
                    if (writer == NoRegion) writer = owner;
                    else if (writer != owner) { uniformWriter = false; break; }
                    if (source.broadcast()) break;
                }
            }
            const auto first = layout.first + domain.first, end = first + domain.count;
            if (uniformWriter) {
                std::fill(relationWriter.begin() + first, relationWriter.begin() + end, writer);
                for (uint32_t slot = 0; slot < arity; ++slot) {
                    const auto& source = domain.fields[slot];
                    if (!p.model.data->variables[source.first.set].readOnly) continue;
                    const auto members = domain.memberRange(slot);
                    for (uint32_t member = members.first; member < members.second; ++member) {
                        const auto ref = source.at(member);
                        const auto variable = p.variables[ref.set].first + ref.index;
                        if (writer == NoRegion) sharedInput[variable] = true;
                        else if (inputOwner[variable] == NoRegion) inputOwner[variable] = writer;
                        else if (inputOwner[variable] != writer) sharedInput[variable] = true;
                        if (source.broadcast()) break;
                    }
                }
                continue;
            }
            for (uint32_t id = first; id < end; ++id) {
                auto& writer = relationWriter[id];
                for (uint32_t slot = 0; slot < arity; ++slot) {
                    auto variable = p.endpoint(id, slot);
                    if (!readOnly(variable)) {
                        writer = components.root(variable);
                        break;
                    }
                }
                for (uint32_t slot = 0; slot < arity; ++slot) {
                    auto variable = p.endpoint(id, slot);
                    if (!readOnly(variable))
                        continue;
                    if (writer == NoRegion)
                        sharedInput[variable] = true;
                    else if (inputOwner[variable] == NoRegion)
                        inputOwner[variable] = writer;
                    else if (inputOwner[variable] != writer)
                        sharedInput[variable] = true;
                }
            }
        }
    }
    // A read-only input with one consumer component can be predicted privately
    // inside that region. Inputs shared between components stay globally owned.
    for (uint32_t id = 0; id < variableCount; ++id)
        if (!sharedInput[id] && inputOwner[id] != NoRegion)
            components.join(inputOwner[id], id);
    std::vector<Cost> costs(variableCount);
    for (const auto& layout : p.variables) {
        const auto& space = *p.spaces[layout.space];
        for (uint32_t id = layout.first; id < layout.first + layout.count; ++id) {
            if (sharedInput[id])
                continue;
            auto& cost = costs[components.root(id)];
            ++cost.variables;
            cost.state += 2 * uint64_t(space.stateSize) + space.tangentSize;
            cost.temporaries = std::max(cost.temporaries,
                                       3 * uint64_t(space.stateSize) + space.tangentSize);
        }
    }
    std::vector<uint32_t> relationOwner(p.statistics.relations, NoRegion);
    for (const auto& layout : p.relations) {
        auto temporary = temporaryWords(p, layout.type);
        for (uint32_t id = layout.first; id < layout.first + layout.count; ++id) {
            if (relationWriter[id] == NoRegion)
                continue;
            auto owner = components.root(relationWriter[id]);
            relationOwner[id] = owner;
            ++costs[owner].relations;
            costs[owner].state += uint64_t(p.types[layout.type]->history) + p.types[layout.type]->rows;
            costs[owner].temporaries = std::max(costs[owner].temporaries, temporary);
        }
    }
    // Pack independent small components to avoid a workgroup per scalar constraint.
    std::vector<uint32_t> regions(variableCount, NoRegion);
    uint32_t regionCount = 0;
    Cost packed;
    for (uint32_t id = 0; id < variableCount; ++id) {
        if (!costs[id].variables)
            continue;
        ++p.statistics.components;
        if (!costs[id].fits())
            continue;
        if (!regionCount || !(packed + costs[id]).fits()) {
            ++regionCount;
            packed = {};
        }
        regions[id] = regionCount - 1;
        packed = packed + costs[id];
    }
    if (!regionCount) {
        if (lowerTiledSchedule(p, functions)) {
            collapseVariableDispatches(p, functions);
            p.statistics.dispatches = dispatchCount(p);
            pruneKernels(p);
            return;
        }
        collapseVariableDispatches(p, functions);
        collapseRelationDispatches(p);
        fuseColorWindows(p, functions);
        p.statistics.dispatches = dispatchCount(p);
        pruneKernels(p);
        return;
    }
    auto oldVariables = words(p.buffers[size_t(BufferRole::VariableWork)]);
    auto oldRelations = words(p.buffers[size_t(BufferRole::RelationWork)]);
    // Only functions used by eligible regions contribute to the fused program.
    // A large global component must not prevent unrelated small ones from fusing.
    std::vector<bool> localFunctions(functions.size());
    for (auto batches : {&p.predict, &p.solve, &p.apply, &p.recover, &p.update})
        for (const auto& b : *batches) {
            bool variable = functions[b.kernel].work == BufferRole::VariableWork;
            const auto& source = variable ? oldVariables : oldRelations;
            for (uint32_t i = 0; !localFunctions[b.kernel] && i < b.count; ++i) {
                auto id = source[b.first + i];
                auto owner = variable ? components.root(id) : relationOwner[id];
                localFunctions[b.kernel] = owner != NoRegion && regions[owner] != NoRegion;
            }
        }
    // Bound a mixed-type program's code footprint before committing the mapping.
    uint64_t sourceBytes = 0;
    for (size_t i = 0; i < functions.size(); ++i)
        if (localFunctions[i])
            sourceBytes += functions[i].source.size();
    if (sourceBytes > SourceByteBudget) {
        collapseVariableDispatches(p, functions);
        collapseRelationDispatches(p);
        fuseColorWindows(p, functions);
        p.statistics.dispatches = dispatchCount(p);
        pruneKernels(p);
        return;
    }
    std::vector<uint32_t> variableRegion(variableCount);
    for (uint32_t id = 0; id < variableCount; ++id) {
        variableRegion[id] = regions[components.root(id)];
        p.statistics.localVariables += variableRegion[id] != NoRegion;
    }
    for (auto& owner : relationOwner) {
        if (owner != NoRegion)
            owner = regions[owner];
        p.statistics.localRelations += owner != NoRegion;
    }
    p.statistics.localRegions = regionCount;
    for (const auto& layout : p.relations)
        for (uint32_t id = layout.first; id < layout.first + layout.count; ++id)
            if (relationOwner[id] != NoRegion)
                for (uint32_t slot = 0; slot < p.types[layout.type]->spaces.size(); ++slot)
                    p.localPerSubstep = p.localPerSubstep || sharedInput[p.endpoint(id, slot)];
    // Each region owns a packed shared-state image. Layout records map it back to
    // stable public SoA addresses; operation accessors use variable/relation IDs.
    std::vector<std::vector<uint32_t>> state(regionCount);
    std::vector<uint32_t> localOffsets(size_t(variableCount) * 3 + size_t(p.statistics.relations) * 2, NoRegion);
    auto stateField = [&](uint32_t owner, uint32_t kind, uint32_t address, uint32_t width, uint32_t stride) {
        auto offset = uint32_t(state[owner].size() / 2);
        for (uint32_t c = 0; c < width; ++c) {
            state[owner].push_back(kind);
            state[owner].push_back(address + c * stride);
        }
        return offset;
    };
    for (const auto& layout : p.variables) {
        const auto& space = *p.spaces[layout.space];
        for (uint32_t i = 0; i < layout.count; ++i) {
            uint32_t id = layout.first + i, owner = variableRegion[id];
            if (owner == NoRegion)
                continue;
            localOffsets[size_t(id) * 3] =
                stateField(owner, 0, layout.values + i, space.stateSize, layout.count);
            localOffsets[size_t(id) * 3 + 1] =
                stateField(owner, 1, layout.values + i, space.stateSize, layout.count);
            localOffsets[size_t(id) * 3 + 2] =
                stateField(owner, 2, layout.velocity + i, space.tangentSize, layout.count);
        }
    }
    for (const auto& layout : p.relations) {
        const auto& type = *p.types[layout.type];
        for (uint32_t i = 0; i < layout.count; ++i) {
            uint32_t id = layout.first + i, owner = relationOwner[id];
            if (owner == NoRegion)
                continue;
            auto at = size_t(variableCount) * 3 + size_t(id) * 2;
            localOffsets[at] = stateField(owner, 3, layout.history + i, type.history, layout.count);
            localOffsets[at + 1] = stateField(owner, 4, layout.multipliers + i, type.rows, layout.count);
        }
    }
    std::vector<uint32_t> stateLayout(size_t(regionCount) * 2);
    uint32_t sharedWords = 0;
    for (uint32_t region = 0; region < regionCount; ++region) {
        auto count = uint32_t(state[region].size() / 2);
        sharedWords = std::max(sharedWords, count);
        stateLayout[size_t(region) * 2] = uint32_t(stateLayout.size());
        stateLayout[size_t(region) * 2 + 1] = count;
        stateLayout.insert(stateLayout.end(), state[region].begin(), state[region].end());
    }
    store(p.buffers[size_t(BufferRole::LocalOffsets)], localOffsets);
    store(p.buffers[size_t(BufferRole::RegionState)], stateLayout);
    p.statistics.localSharedBytes = sharedWords * sizeof(float);
    std::vector<uint32_t> variableWork, relationWork, ranges;
    struct Phase {
        uint32_t kernel;
        bool synchronize = true;
    };
    std::vector<Phase> phases;
    // Ranges are phase-major: each region has a (first,count) pair in each phase.
    auto phase = [&](uint32_t kernel, const std::vector<std::vector<uint32_t>>& work,
                     std::vector<uint32_t>& target) {
        phases.push_back({kernel});
        for (const auto& items : work) {
            ranges.push_back(uint32_t(target.size()));
            ranges.push_back(uint32_t(items.size()));
            target.insert(target.end(), items.begin(), items.end());
        }
        return uint32_t(phases.size() - 1);
    };
    auto lower = [&](std::vector<Batch>& batches, bool combineColor = false) {
        uint32_t previousPhase = UINT32_MAX;
        int32_t previousColor = -2;
        for (auto& b : batches) {
            const bool variable = functions[b.kernel].work == BufferRole::VariableWork;
            const auto& source = variable ? oldVariables : oldRelations;
            const auto& owners = variable ? variableRegion : relationOwner;
            auto& target = variable ? variableWork : relationWork;
            std::vector<std::vector<uint32_t>> work(regionCount);
            auto first = uint32_t(target.size());
            for (uint32_t i = 0; i < b.count; ++i) {
                auto id = source[b.first + i], owner = owners[id];
                if (owner == NoRegion)
                    target.push_back(id);
                else
                    work[owner].push_back(id);
            }
            b.first = first;
            b.count = uint32_t(target.size()) - first;
            if (std::any_of(work.begin(), work.end(), [](const auto& items) { return !items.empty(); })) {
                if (combineColor && previousPhase != UINT32_MAX && previousColor == b.color)
                    phases[previousPhase].synchronize = false;
                previousPhase = phase(b.kernel, work, target);
                previousColor = b.color;
            }
        }
        batches.erase(std::remove_if(batches.begin(), batches.end(), [](const Batch& b) { return !b.count; }),
                      batches.end());
    };
    lower(p.predict);
    const auto iterationBegin = uint32_t(phases.size());
    // Local lanes call homogeneous mathematical functions directly. Batches in one
    // dependency color need no barrier between types; the color boundary remains.
    lower(p.solve, true);
    lower(p.apply);
    const auto iterationEnd = uint32_t(phases.size());
    lower(p.recover);
    lower(p.update);

    std::ostringstream source;
    source << "shared float regionState[" << sharedWords << "];\n"
           << stateAccess(StateStorage::Region, variableCount, p.localPerSubstep);
    std::set<uint32_t> emitted;
    for (const auto& stage : phases)
        if (stage.kernel != NoRegion && emitted.insert(stage.kernel).second)
            source << functions[stage.kernel].source;
    source << "void main(){uint region=gl_WorkGroupID.x+gl_WorkGroupID.y*65535u;"
              "if(region>=step.count/128u)return;"
              "uint lane=gl_LocalInvocationID.x;\n"
              "uint stateFirst=x_regionState[region*2u],stateCount=x_regionState[region*2u+1u];\n";
    auto copyState = [&](bool write) {
        source << "for(uint j=lane;j<stateCount;j+=128u){"
                  "uint kind=x_regionState[stateFirst+j*2u],at=x_regionState[stateFirst+j*2u+1u];\n";
        const char* names[] = {"q", "oldq", "velocity", "history", "lambda"};
        for (uint32_t kind = 0; kind < 5; ++kind) {
            source << (kind ? "else " : "") << "if(kind==" << kind << "u)";
            if (write)
                source << "x_" << names[kind] << "[at]=regionState[j];\n";
            else
                source << "regionState[j]=x_" << names[kind] << "[at];\n";
        }
        source << "}\n";
    };
    copyState(false);
    source << "barrier();\nfor(uint substep=0u;substep<" << (p.localPerSubstep ? 1u : p.policy.substeps) << "u;++substep){"
              "float h=step.h,time=step.time+h*float(substep+1u),inverseH2=1.0/(h*h);\n";
    for (uint32_t i = 0; i < phases.size(); ++i) {
        if (i == iterationBegin && iterationBegin != iterationEnd)
            source << "for(uint iteration=0u;iteration<" << p.policy.iterations << "u;++iteration){\n";
        if (i == iterationEnd && iterationBegin != iterationEnd)
            source << "}\n";
        source << "{uint at=(" << uint64_t(i) * regionCount << "u+region)*2u;"
                  "uint first=x_regionRanges[at],count=x_regionRanges[at+1u];"
                  "for(uint j=lane;j<count;j+=128u){";
        const auto& function = functions[phases[i].kernel];
        source << function.entry << "(x_"
               << (function.work == BufferRole::VariableWork ? "variableWork" : "relationWork")
               << "[first+j],h,time,step.relaxation,inverseH2,"
               << (i >= iterationBegin && i < iterationEnd ? "iteration" : "0u") << ");";
        source << "}}";
        // Jacobi contributions retain their global CSR layout. All other iterative
        // state is shared, so its phase boundary needs only workgroup synchronization.
        if (phases[i].synchronize && functions[phases[i].kernel].writesContributions)
            source << "memoryBarrierBuffer();";
        if (phases[i].synchronize)
            source << "barrier();";
        source << "\n";
    }
    source << "}\n";
    copyState(true);
    source << "}\n";
    store(p.buffers[size_t(BufferRole::VariableWork)], variableWork);
    store(p.buffers[size_t(BufferRole::RelationWork)], relationWork);
    store(p.buffers[size_t(BufferRole::RegionRanges)], ranges);
    if (uint64_t(regionCount) * Lanes > UINT32_MAX)
        throw std::overflow_error("Dynamics region dispatch exceeds 32-bit addressing");
    p.local.push_back({uint32_t(p.kernels.size()), 0, regionCount * Lanes});
    p.kernels.push_back({"Solve local regions", source.str(), {BufferRole::Contributions}});
    // Complete regions and overlapping epochs own disjoint writable state. A
    // small independent component must not disable tiling of the remaining graph.
    const bool tiled = lowerTiledSchedule(p, functions);
    collapseVariableDispatches(p, functions);
    if (!tiled) {
        collapseRelationDispatches(p);
        fuseColorWindows(p, functions);
    }
    p.statistics.dispatches = dispatchCount(p) + (p.localPerSubstep ? p.policy.substeps : 1);
    pruneKernels(p);
}
void prunePlanKernels(CompiledPlan& p) { pruneKernels(p); }
} // namespace whimsical::dynamics
