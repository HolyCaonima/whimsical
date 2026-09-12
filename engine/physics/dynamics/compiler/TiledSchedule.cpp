#include "Schedule.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace whimsical::dynamics {
namespace {
// Backend budgets: sixteen core rows expose many independent groups, while the
// full dependency cone and its temporary columns fit Vulkan's 16 KiB minimum.
constexpr uint32_t Missing = UINT32_MAX, CoreVariables = 16, SharedWords = 4096;
constexpr uint64_t MetadataWords = 16 * 1024 * 1024, DuplicationBudget = 8;
struct Incidence {
    uint32_t relation, column;
};
std::vector<uint32_t> words(const BufferData& data) {
    std::vector<uint32_t> result(data.initial.size() / 4);
    if (!result.empty())
        std::memcpy(result.data(), data.initial.data(), data.initial.size());
    return result;
}
void store(BufferData& data, const std::vector<uint32_t>& values) {
    data.initial.resize(values.size() * 4);
    if (!values.empty())
        std::memcpy(data.initial.data(), values.data(), data.initial.size());
    data.words = values.size();
    data.fills.clear();
}
} // namespace

bool lowerTiledSchedule(CompiledPlan& p, const std::vector<KernelFunction>& functions) {
    // An epoch reads the previous iteration and publishes disjoint results only
    // after every tile finishes. Direct atomics and dynamic domains have a
    // different execution contract and retain their existing lowering.
    if (p.dynamicTopology || p.policy.execution == ExecutionMode::Global ||
        p.policy.weighting == JacobiWeighting::Active ||
        p.statistics.directJacobiRelations || !p.deferredJacobiDomains.empty() ||
        p.solve.size() + p.apply.size() <= 3)
        return false;
    const auto variableCount = uint32_t(p.statistics.variables);
    const auto relationCount = uint32_t(p.statistics.relations);
    if (uint64_t(variableCount) * p.solve.size() > MetadataWords ||
        p.buffers[size_t(BufferRole::Values)].wordCount() +
            p.buffers[size_t(BufferRole::Multipliers)].wordCount() > UINT32_MAX)
        return false;
    const auto relationWork = words(p.buffers[size_t(BufferRole::RelationWork)]);
    const auto variableWork = words(p.buffers[size_t(BufferRole::VariableWork)]);
    std::vector<uint32_t> relationPhase(relationCount, Missing), applyKernel(variableCount, Missing);
    std::vector<uint32_t> scheduledRelations;
    std::vector<bool> participating(variableCount);
    std::set<uint32_t> usedKernels;
    uint64_t sourceBytes = 0;
    auto useKernel = [&](uint32_t kernel) {
        if (usedKernels.insert(kernel).second)
            sourceBytes += functions[kernel].source.size();
    };
    for (uint32_t phase = 0; phase < p.solve.size(); ++phase) {
        const auto& batch = p.solve[phase];
        useKernel(batch.kernel);
        for (uint32_t i = 0; i < batch.count; ++i)
            relationPhase[relationWork[batch.first + i]] = phase;
    }
    for (const auto& batch : p.apply) {
        useKernel(batch.kernel);
        for (uint32_t i = 0; i < batch.count; ++i)
            applyKernel[variableWork[batch.first + i]] = batch.kernel;
    }
    if (sourceBytes > 256 * 1024)
        return false;

    std::vector<std::vector<uint32_t>> endpoints(relationCount);
    std::vector<std::vector<Incidence>> incidence(variableCount);
    std::vector<std::vector<uint32_t>> writers(p.solve.size());
    for (uint32_t phase = 0; phase < p.solve.size(); ++phase)
        if (p.solve[phase].color >= 0)
            writers[phase].assign(variableCount, Missing);
    for (uint32_t relation = 0; relation < relationCount; ++relation) {
        if (relationPhase[relation] == Missing)
            continue; // a complete local region already owns this row
        scheduledRelations.push_back(relation);
        const auto type = p.relationType(relation);
        uint32_t column = 0;
        for (uint32_t slot = 0; slot < p.types[type]->spaces.size(); ++slot) {
            const auto variable = p.endpoint(relation, slot);
            auto& unique = endpoints[relation];
            if (!p.variableReadOnly(variable) &&
                std::find(unique.begin(), unique.end(), variable) == unique.end()) {
                unique.push_back(variable);
                participating[variable] = true;
                incidence[variable].push_back({relation, column});
                if (p.solve[relationPhase[relation]].color >= 0)
                    writers[relationPhase[relation]][variable] = relation;
            }
            column += p.activeTangent(type, slot);
        }
        // A relation without writable incidence still owns diagnostics. Leave
        // such graphs on the ordinary schedule rather than drop their operations.
        if (endpoints[relation].empty())
            return false;
    }
    // A leading sequence with one writable endpoint per operation has no
    // cross-variable dependence. Evaluate it once before overlapping cones;
    // repeating a costly local prefix in every halo would defeat fusion.
    uint32_t prefixPhases = 0;
    for (const auto& batch : p.solve) {
        if (batch.color < 0)
            break;
        bool independent = true;
        for (uint32_t i = 0; independent && i < batch.count; ++i)
            independent = endpoints[relationWork[batch.first + i]].size() == 1;
        if (!independent)
            break;
        ++prefixPhases;
    }
    if (p.solve.size() - prefixPhases + p.apply.size() <= 2)
        return false;

    // Breadth-first cores use only incidence, with no geometric interpretation.
    std::vector<uint32_t> owner(variableCount, Missing);
    std::vector<std::vector<uint32_t>> cores;
    for (uint32_t seed = 0; seed < variableCount; ++seed) {
        if (owner[seed] != Missing || !participating[seed])
            continue;
        const auto tile = uint32_t(cores.size());
        std::vector<uint32_t> core{seed};
        owner[seed] = tile;
        for (size_t at = 0; at < core.size() && core.size() < CoreVariables; ++at)
            for (const auto& edge : incidence[core[at]]) {
                for (auto variable : endpoints[edge.relation]) {
                    if (owner[variable] == Missing && core.size() < CoreVariables) {
                        owner[variable] = tile;
                        core.push_back(variable);
                    }
                }
                if (core.size() == CoreVariables)
                    break;
            }
        std::sort(core.begin(), core.end());
        cores.push_back(std::move(core));
    }
    if (cores.size() < 8)
        return false;
    std::vector<uint32_t> relationOwner(relationCount, Missing);
    for (auto relation : scheduledRelations)
        relationOwner[relation] = owner[endpoints[relation].front()];

    std::vector<uint32_t> data(cores.size());
    const auto phases = uint32_t(p.solve.size() - prefixPhases + p.apply.size());
    uint32_t maximumState = 0;
    uint64_t evaluations = 0;
    std::vector<uint32_t> neededStamp(variableCount, Missing), selectedStamp(relationCount, Missing);
    for (uint32_t tile = 0; tile < cores.size(); ++tile) {
        std::vector<std::vector<uint32_t>> selected(p.solve.size());
        std::vector<uint32_t> needed;
        uint32_t valueWords = 0;
        auto need = [&](uint32_t variable) {
            if (neededStamp[variable] == tile)
                return;
            neededStamp[variable] = tile;
            needed.push_back(variable);
            const auto& layout = p.variables[p.variableSet(variable)];
            valueWords += p.spaces[layout.space]->stateSize;
        };
        auto select = [&](uint32_t relation) {
            if (selectedStamp[relation] == tile)
                return;
            selectedStamp[relation] = tile;
            selected[relationPhase[relation]].push_back(relation);
            for (auto variable : endpoints[relation])
                need(variable);
        };
        for (auto variable : cores[tile]) {
            need(variable);
            for (const auto& edge : incidence[variable])
                if (p.solve[relationPhase[edge.relation]].color < 0)
                    select(edge.relation);
        }
        // Reverse liveness gives the exact dependency cone. New endpoints from a
        // color are dependencies of earlier colors, never of peers in that color.
        for (size_t phase = p.solve.size(); phase-- > prefixPhases;) {
            if (writers[phase].empty())
                continue;
            const auto count = needed.size();
            for (size_t i = 0; i < count; ++i) {
                const auto relation = writers[phase][needed[i]];
                if (relation != Missing)
                    select(relation);
            }
            if (valueWords > SharedWords)
                return false;
        }
        std::sort(needed.begin(), needed.end());
        std::unordered_map<uint32_t, uint32_t> localValues;
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> contributions;
        std::vector<uint32_t> loads, outputs;
        for (auto variable : needed) {
            const auto& layout = p.variables[p.variableSet(variable)];
            const auto width = p.spaces[layout.space]->stateSize;
            const auto offset = uint32_t(loads.size());
            localValues.emplace(variable, offset);
            for (uint32_t c = 0; c < width; ++c) {
                const auto address = layout.values + variable - layout.first + c * layout.count;
                loads.push_back(address);
                if (owner[variable] == tile) {
                    outputs.push_back(address);
                    outputs.push_back(offset + c);
                }
            }
        }
        uint32_t stateWords = valueWords;
        for (uint32_t phase = prefixPhases; phase < p.solve.size(); ++phase) {
            auto& work = selected[phase];
            std::sort(work.begin(), work.end());
            evaluations += work.size();
            if (p.solve[phase].color < 0 && !work.empty()) {
                const auto stride = (uint32_t(work.size()) + 31u) & ~31u;
                const auto first = stateWords;
                uint32_t row = 0;
                for (auto relation : work) {
                    contributions.emplace(relation, std::make_pair(first + row++, stride));
                }
                stateWords += stride * p.activeTangent(p.relationType(work.front()));
            }
        }
        if (stateWords > SharedWords || evaluations > scheduledRelations.size() * DuplicationBudget)
            return false;
        maximumState = std::max(maximumState, stateWords);
        const auto header = uint32_t(data.size());
        data[tile] = header;
        data.resize(data.size() + 4 + phases * 2);
        data[header] = uint32_t(data.size());
        data[header + 1] = valueWords;
        data.insert(data.end(), loads.begin(), loads.end());
        data[header + 2] = uint32_t(data.size());
        data[header + 3] = uint32_t(outputs.size() / 2);
        data.insert(data.end(), outputs.begin(), outputs.end());
        for (uint32_t phase = prefixPhases; phase < p.solve.size(); ++phase) {
            std::vector<uint32_t> records;
            for (auto relation : selected[phase]) {
                const auto first = uint32_t(data.size());
                const auto type = p.relationType(relation);
                for (uint32_t slot = 0; slot < p.types[type]->spaces.size(); ++slot) {
                    const auto variable = p.endpoint(relation, slot);
                    data.push_back(p.variableReadOnly(variable) ? Missing : localValues.at(variable));
                }
                records.push_back(relation);
                records.push_back(relationOwner[relation] == tile);
                records.push_back(p.solve[phase].color < 0 ? contributions.at(relation).first : 0);
                records.push_back(first);
            }
            data[header + 4 + (phase - prefixPhases) * 2] = uint32_t(data.size());
            data[header + 5 + (phase - prefixPhases) * 2] = uint32_t(selected[phase].size());
            data.insert(data.end(), records.begin(), records.end());
        }
        for (uint32_t stage = 0; stage < p.apply.size(); ++stage) {
            std::vector<uint32_t> records;
            for (auto variable : cores[tile]) {
                if (applyKernel[variable] != p.apply[stage].kernel)
                    continue;
                const auto first = uint32_t(data.size());
                // The original CSR is assembled by relation ID, then endpoint
                // slot. Keep that reduction order even when work is duplicated.
                for (const auto& edge : incidence[variable])
                    if (p.solve[relationPhase[edge.relation]].color < 0) {
                        const auto [address, stride] = contributions.at(edge.relation);
                        data.push_back(address + edge.column * stride);
                        data.push_back(stride);
                    }
                records.push_back(variable);
                records.push_back(first);
                records.push_back((uint32_t(data.size()) - first) / 2);
                records.push_back(localValues.at(variable));
            }
            const auto phase = uint32_t(p.solve.size()) - prefixPhases + stage;
            data[header + 4 + phase * 2] = uint32_t(data.size());
            data[header + 5 + phase * 2] = uint32_t(records.size() / 4);
            data.insert(data.end(), records.begin(), records.end());
        }
        if (data.size() > MetadataWords)
            return false;
    }

    const auto valueWords = p.buffers[size_t(BufferRole::Values)].wordCount();
    const auto lambdaWords = p.buffers[size_t(BufferRole::Multipliers)].wordCount();
    const auto commitFirst = uint32_t(data.size());
    for (const auto& layout : p.variables)
        for (uint32_t c = 0; c < p.spaces[layout.space]->stateSize; ++c)
            for (uint32_t row = 0; row < layout.count; ++row)
                if (participating[layout.first + row])
                    data.push_back(layout.values + row + c * layout.count);
    for (const auto& layout : p.relations)
        for (uint32_t row = 0; row < layout.count; ++row)
            if (relationPhase[layout.first + row] != Missing &&
                relationPhase[layout.first + row] >= prefixPhases)
                for (uint32_t c = 0; c < p.types[layout.type]->rows; ++c)
                    data.push_back(uint32_t(valueWords) + layout.multipliers + row + c * layout.count);
    const auto commitCount = uint32_t(data.size()) - commitFirst;

    uint32_t prefixFirst = uint32_t(data.size()), prefixCount = 0;
    std::set<uint32_t> prefixKernels, epochKernels;
    for (uint32_t phase = prefixPhases; phase < p.solve.size(); ++phase)
        epochKernels.insert(p.solve[phase].kernel);
    for (const auto& batch : p.apply)
        epochKernels.insert(batch.kernel);
    std::ostringstream prefixSource;
    if (prefixPhases) {
        std::map<uint32_t, std::vector<uint32_t>> rows;
        for (uint32_t phase = 0; phase < prefixPhases; ++phase) {
            const auto& batch = p.solve[phase];
            prefixKernels.insert(batch.kernel);
            for (uint32_t i = 0; i < batch.count; ++i) {
                const auto relation = relationWork[batch.first + i];
                auto& row = rows[endpoints[relation].front()];
                row.resize(prefixPhases, Missing);
                row[phase] = relation;
            }
        }
        prefixCount = uint32_t(rows.size());
        data.resize(data.size() + size_t(prefixCount) * prefixPhases, Missing);
        uint32_t lane = 0;
        for (const auto& [variable, row] : rows) {
            for (uint32_t phase = 0; phase < prefixPhases; ++phase)
                data[prefixFirst + phase * prefixCount + lane] = row[phase];
            ++lane;
        }
        prefixSource << stateAccess(StateStorage::Global);
        for (auto kernel : prefixKernels)
            prefixSource << functions[kernel].source;
        prefixSource << "void main(){uint lane=invocation();if(lane>=step.count)return;"
                        "float inverseH2=1.0/(step.h*step.h);";
        for (uint32_t phase = 0; phase < prefixPhases; ++phase)
            prefixSource << "{uint id=x_epochData[step.first+" << phase
                         << "u*step.count+lane];if(id!=0xffffffffu)"
                         << functions[p.solve[phase].kernel].entry
                         << "(id,step.h,step.time,step.relaxation,inverseH2,step.iteration);}";
        prefixSource << "}\n";
    }

    std::ostringstream source;
    source << "#define EPOCH_VALUE_WORDS " << valueWords << "u\n"
              "shared float epochState[" << maximumState << "];\n"
              "uint epochEndpointFirst,epochVariableOffset,epochContribution,epochContributionStride,epochIncidenceFirst,epochIncidenceCount;"
              "bool epochOwned;\n" << stateAccess(StateStorage::Epoch);
    for (auto kernel : epochKernels)
        source << functions[kernel].source;
    source << "void main(){uint tile=gl_WorkGroupID.x+gl_WorkGroupID.y*65535u;"
              "if(tile>=step.count/128u)return;uint lane=gl_LocalInvocationID.x;"
              "uint header=x_epochData[step.first+tile],loadFirst=x_epochData[header],"
              "loadCount=x_epochData[header+1u];"
              "for(uint j=lane;j<loadCount;j+=128u)epochState[j]=x_q[x_epochData[loadFirst+j]];"
              "barrier();float inverseH2=1.0/(step.h*step.h);";
    for (uint32_t phase = 0; phase < phases; ++phase) {
        const auto original = phase + prefixPhases;
        const bool relation = original < p.solve.size();
        const auto& stage = relation ? p.solve[original] : p.apply[original - p.solve.size()];
        source << "{uint first=x_epochData[header+" << 4 + phase * 2
               << "u],count=x_epochData[header+" << 5 + phase * 2
               << "u];epochContributionStride=(count+31u)&~31u;"
                  "for(uint j=lane;j<count;j+=128u){uint at=first+j*4u,id=x_epochData[at];";
        if (relation)
            source << "epochOwned=x_epochData[at+1u]!=0u;epochContribution=x_epochData[at+2u];"
                      "epochEndpointFirst=x_epochData[at+3u];";
        else
            source << "epochOwned=true;epochIncidenceFirst=x_epochData[at+1u];"
                      "epochIncidenceCount=x_epochData[at+2u];epochVariableOffset=x_epochData[at+3u];";
        source << functions[stage.kernel].entry
               << "(id,step.h,step.time,step.relaxation,inverseH2,step.iteration);}}";
        if (!relation || original + 1 == p.solve.size() || p.solve[original + 1].color != stage.color)
            source << "barrier();";
    }
    source << "uint outputFirst=x_epochData[header+2u],outputCount=x_epochData[header+3u];"
              "for(uint j=lane;j<outputCount;j+=128u){uint at=outputFirst+j*2u;"
              "x_epochOutput[x_epochData[at]]=epochState[x_epochData[at+1u]];}}\n";
    const auto kernel = uint32_t(p.kernels.size());
    p.kernels.push_back({"Solve overlapping dependency tiles", source.str()});
    source.str("");
    source.clear();
    source << "void main(){uint lane=invocation();if(lane>=step.count)return;"
              "uint at=x_epochData[step.first+lane];float value=x_epochOutput[at];"
              "if(at<" << valueWords << "u)x_q[at]=value;else x_lambda[at-" << valueWords << "u]=value;}\n";
    p.kernels.push_back({"Commit dependency epoch", source.str()});
    p.iteration = {{kernel, 0, uint32_t(cores.size()) * 128},
                   {kernel + 1, commitFirst, commitCount}};
    if (prefixPhases) {
        p.iteration.insert(p.iteration.begin(), {uint32_t(p.kernels.size()), prefixFirst, prefixCount});
        p.kernels.push_back({"Solve independent dependency prefix", prefixSource.str()});
    }
    p.solve.clear();
    p.apply.clear();
    store(p.buffers[size_t(BufferRole::EpochData)], data);
    auto& output = p.buffers[size_t(BufferRole::EpochOutput)];
    output.words = valueWords + lambdaWords;
    output.fills = {{0, output.words, 0}};
    p.statistics.overlapTiles = uint32_t(cores.size());
    p.statistics.overlapSharedBytes = maximumState * sizeof(float);
    p.statistics.overlapEvaluations = evaluations;
    return true;
}
} // namespace whimsical::dynamics
