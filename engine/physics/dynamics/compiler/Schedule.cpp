#include "Schedule.h"
#include <algorithm>
#include <cstring>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace whimsical::dynamics {
namespace {
constexpr uint32_t NoRegion = UINT32_MAX;
// Backend work budgets, independent of model names and numerical field values.
constexpr uint32_t Lanes = 128, RelationBudget = 512, StateWordBudget = 2048;
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
}
uint64_t dispatchCount(const CompiledPlan& p) {
    auto count = [](const std::vector<Batch>& batches) {
        return uint64_t(std::count_if(batches.begin(), batches.end(), [](const Batch& b) { return b.count; }));
    };
    if (!count(p.predict))
        return 0;
    return p.policy.substeps * (count(p.predict) + (p.multiplierCount ? 1 : 0) +
        p.policy.iterations * (count(p.solve) + count(p.apply)) + count(p.recover) + count(p.update));
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
uint64_t temporaryWords(const RelationType& t) {
    uint64_t n = t.inputSize(), q = t.stateSize(), d = t.tangentSize(), m = t.rows;
    uint64_t result = n + m * q + 2 * m * d + m * m + 6 * m;
    for (const auto& s : t.spaces)
        result += 2 * s->stateSize + s->tangentSize +
                  uint64_t(s->stateSize) * s->tangentSize;
    return result;
}
} // namespace

void lowerSchedule(CompiledPlan& p, const std::vector<KernelFunction>& functions) {
    p.statistics.referenceDispatches = p.statistics.dispatches = dispatchCount(p);
    // Dynamic endpoints can connect any compatible variable at a later tick.
    if (p.dynamicTopology || p.policy.execution == ExecutionMode::Global)
        return;
    const auto variableCount = uint32_t(p.statistics.variables);
    Components components(variableCount);
    auto endpoints = words(p.buffers[size_t(BufferRole::Endpoints)]);
    auto relations = words(p.buffers[size_t(BufferRole::Relations)]);
    for (const auto& layout : p.relations) {
        const auto arity = uint32_t(p.types[layout.type]->spaces.size());
        for (uint32_t id = layout.first; id < layout.first + layout.count; ++id) {
            auto e = relations[size_t(id) * 8];
            for (uint32_t slot = 1; slot < arity; ++slot)
                components.join(endpoints[e], endpoints[e + slot]);
        }
    }
    std::vector<Cost> costs(variableCount);
    for (const auto& layout : p.variables) {
        const auto& space = *p.spaces[layout.space];
        for (uint32_t id = layout.first; id < layout.first + layout.count; ++id) {
            auto& cost = costs[components.root(id)];
            ++cost.variables;
            cost.state += 2 * uint64_t(space.stateSize) + space.tangentSize;
            cost.temporaries = std::max(cost.temporaries,
                                       3 * uint64_t(space.stateSize) + space.tangentSize);
        }
    }
    std::vector<uint32_t> relationOwner(p.statistics.relations, NoRegion);
    for (const auto& layout : p.relations) {
        auto temporary = temporaryWords(*p.types[layout.type]);
        for (uint32_t id = layout.first; id < layout.first + layout.count; ++id) {
            auto owner = components.root(endpoints[relations[size_t(id) * 8]]);
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
    if (!regionCount)
        return;
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
                localFunctions[b.kernel] = regions[owner] != NoRegion;
            }
        }
    // Bound a mixed-type program's code footprint before committing the mapping.
    uint64_t sourceBytes = 0;
    for (size_t i = 0; i < functions.size(); ++i)
        if (localFunctions[i])
            sourceBytes += functions[i].source.size();
    if (sourceBytes > SourceByteBudget)
        return;
    std::vector<uint32_t> variableRegion(variableCount);
    for (uint32_t id = 0; id < variableCount; ++id) {
        variableRegion[id] = regions[components.root(id)];
        p.statistics.localVariables += variableRegion[id] != NoRegion;
    }
    for (auto& owner : relationOwner) {
        owner = regions[owner];
        p.statistics.localRelations += owner != NoRegion;
    }
    p.statistics.localRegions = regionCount;
    // Each region owns a packed shared-state image. Layout records map it back to
    // stable public SoA addresses; operation accessors use variable/relation IDs.
    std::vector<std::vector<uint32_t>> state(regionCount);
    std::vector<uint32_t> localOffsets(size_t(variableCount) * 3 + size_t(p.statistics.relations) * 2);
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
    };
    auto lower = [&](std::vector<Batch>& batches) {
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
            if (std::any_of(work.begin(), work.end(), [](const auto& items) { return !items.empty(); }))
                phase(b.kernel, work, target);
        }
        batches.erase(std::remove_if(batches.begin(), batches.end(), [](const Batch& b) { return !b.count; }),
                      batches.end());
    };
    lower(p.predict);
    std::vector<std::vector<uint32_t>> reset(regionCount);
    for (const auto& layout : p.relations)
        for (uint32_t id = layout.first; id < layout.first + layout.count; ++id) {
            auto owner = relationOwner[id];
            if (owner != NoRegion)
                for (uint32_t row = 0; row < p.types[layout.type]->rows; ++row)
                    reset[owner].push_back(localOffsets[size_t(variableCount) * 3 + size_t(id) * 2 + 1] + row);
        }
    const auto resetPhase = uint32_t(phases.size());
    phase(NoRegion, reset, variableWork);
    const auto iterationBegin = uint32_t(phases.size());
    lower(p.solve);
    lower(p.apply);
    const auto iterationEnd = uint32_t(phases.size());
    lower(p.recover);
    lower(p.update);

    std::ostringstream source;
    source << "shared float regionState[" << sharedWords << "];\n" << stateAccess(true, variableCount);
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
    source << "barrier();\nfor(uint substep=0u;substep<" << p.policy.substeps << "u;++substep){"
              "float h=step.h,time=step.time+h*float(substep+1u);\n";
    for (uint32_t i = 0; i < phases.size(); ++i) {
        if (i == iterationBegin && iterationBegin != iterationEnd)
            source << "for(uint iteration=0u;iteration<" << p.policy.iterations << "u;++iteration){\n";
        if (i == iterationEnd && iterationBegin != iterationEnd)
            source << "}\n";
        source << "{uint at=(" << uint64_t(i) * regionCount << "u+region)*2u;"
                  "uint first=x_regionRanges[at],count=x_regionRanges[at+1u];"
                  "for(uint j=lane;j<count;j+=128u){";
        if (i == resetPhase)
            source << "regionState[x_variableWork[first+j]]=0.0;";
        else {
            const auto& function = functions[phases[i].kernel];
            source << function.entry << "(x_"
                   << (function.work == BufferRole::VariableWork ? "variableWork" : "relationWork")
                   << "[first+j],h,time,step.relaxation);";
        }
        source << "}}";
        // Jacobi contributions retain their global CSR layout. All other iterative
        // state is shared, so its phase boundary needs only workgroup synchronization.
        if (i != resetPhase && functions[phases[i].kernel].writesContributions)
            source << "memoryBarrierBuffer();";
        source << "barrier();\n";
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
    p.kernels.push_back({"Solve local regions", source.str()});
    p.statistics.dispatches = dispatchCount(p) + 1;

    // Do not compile standalone programs that were completely absorbed by regions.
    std::vector<bool> used(p.kernels.size());
    used[p.resetKernel] = !p.predict.empty();
    for (auto batches : {&p.predict, &p.solve, &p.apply, &p.recover, &p.update, &p.local})
        for (const auto& b : *batches)
            used[b.kernel] = true;
    std::vector<uint32_t> mapping(p.kernels.size(), UINT32_MAX);
    std::vector<Kernel> kernels;
    for (uint32_t id = 0; id < p.kernels.size(); ++id)
        if (used[id]) {
            mapping[id] = uint32_t(kernels.size());
            kernels.push_back(std::move(p.kernels[id]));
        }
    p.resetKernel = mapping[p.resetKernel];
    for (auto batches : {&p.predict, &p.solve, &p.apply, &p.recover, &p.update, &p.local})
        for (auto& b : *batches)
            b.kernel = mapping[b.kernel];
    p.kernels = std::move(kernels);
}
} // namespace whimsical::dynamics
