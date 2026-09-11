#include "Schedule.h"
#include "FormulaGlsl.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>
#include <stdexcept>

namespace whimsical::dynamics {
namespace {
struct Domain {
    uint32_t set, first, count, header, heads, next, cells, buckets;
    BindingDomain binding;
    DifferenceBound bound;
};
std::vector<uint32_t> words(const BufferData& b) {
    std::vector<uint32_t> result(b.initial.size() / 4);
    if (!result.empty()) std::memcpy(result.data(), b.initial.data(), b.initial.size());
    return result;
}
std::string number(uint32_t n) { return std::to_string(n) + "u"; }
// Only the selected mathematical input columns become index coordinates. Neither
// state width nor object/field names imply a geometric interpretation.
std::string coordinate(const CompiledPlan& p, const Domain& d, uint32_t input) {
    const auto& t = *p.types[p.relations[d.set].type];
    uint32_t slot = 0;
    while (slot < t.spaces.size() && input >= t.spaces[slot]->stateSize)
        input -= t.spaces[slot++]->stateSize;
    const auto& field = d.binding.fields[slot];
    const auto& v = p.variables[field.first.set];
    return "x_q[" + number(v.values + field.first.index + input * v.count) +
           "+member*" + number(field.broadcast() ? 0 : field.stride) + "]";
}
std::string domainSource(const CompiledPlan& p, const Domain& d) {
    std::ostringstream s;
    s << "vec3 feature(uint member,bool left){return left?vec3(";
    for (uint32_t side = 0; side < 2; ++side) {
        if (side) s << "):vec3(";
        for (uint32_t c = 0; c < 3; ++c) {
            if (c) s << ",";
            s << (c < d.bound.coordinates.size()
                      ? coordinate(p, d, side ? d.bound.coordinates[c].second : d.bound.coordinates[c].first)
                      : "0.0");
        }
    }
    s << ");}\n"
         // A 25% margin dominates float subtraction/squaring and quantization
         // error with scaled coordinates bounded by 2^18 before conversion.
         "bool cellOf(vec3 f,out ivec3 cell){"
         "float r=uintBitsToFloat(x_candidates[" << d.header << "u]);"
         "vec3 q=f/(1.25*sqrt(r));"
         "if(any(isnan(q))||any(isinf(q))||any(greaterThan(abs(q),vec3(262144.0)))"
         "||any(greaterThan(abs(f),vec3(1e15))))return false;"
         "cell=ivec3(floor(q));return true;}\n"
         "uint bucket(ivec3 c){uvec3 u=uvec3(c);return ((u.x*73856093u)^(u.y*19349663u)^(u.z*83492791u))&"
      << d.buckets - 1 << "u;}\n"
         "uint rowOf(uint i,uint j){return " << d.first << "u+";
    switch (d.binding.map) {
    case BindingDomain::Map::Product: s << "i*" << d.binding.right << "u+j"; break;
    case BindingDomain::Map::Directed:
        s << "i*" << d.binding.right - 1 << "u+j-(j>i?1u:0u)"; break;
    default: {
        bool diagonal = d.binding.map == BindingDomain::Map::UpperDiagonal;
        s << "trianglePrefix(i," << d.binding.right << "u," << (diagonal ? "true" : "false")
          << ")+j-i-" << (diagonal ? 0 : 1) << "u";
    }}
    s << ";}\nbool accepts(uint i,uint j){return ";
    switch (d.binding.map) {
    case BindingDomain::Map::Upper: s << "i<j"; break;
    case BindingDomain::Map::UpperDiagonal: s << "i<=j"; break;
    case BindingDomain::Map::Directed: s << "i!=j"; break;
    default: s << "true";
    }
    s << ";}\n";
    return s.str();
}
} // namespace

void lowerCandidateDomains(CompiledPlan& p) {
    // Region ownership and dynamic endpoints have their own scheduling contracts.
    // This lowering consumes only global, static Jacobi snapshots.
    if (p.dynamicTopology || !p.local.empty() || p.policy.execution != ExecutionMode::Auto)
        return;
    const uint32_t relations = uint32_t(p.statistics.relations);
    auto work = words(p.buffers[size_t(BufferRole::RelationWork)]);
    std::vector<bool> jacobi(relations), selected(relations);
    std::vector<uint32_t> all;
    for (const auto& batch : p.solve)
        if (batch.color < 0)
            for (uint32_t k = 0; k < batch.count; ++k) {
                uint32_t id = work[batch.first + k];
                jacobi[id] = true;
                all.push_back(id);
            }
    if (all.empty()) return;
    std::vector<Domain> domains;
    for (uint32_t set = 0; set < p.relations.size(); ++set) {
        const auto& layout = p.relations[set];
        const auto& t = *p.types[layout.type];
        if (t.rows != 1 || t.kind == RelationKind::Equality || t.history || t.update)
            continue;
        uint32_t parameters = t.inputSize() - t.parameters - t.history - 2;
        auto bound = differenceBound(t.residual, t.kind == RelationKind::GreaterEqual, parameters, t.parameters);
        if (!bound || bound->coordinates.size() > 3) continue;
        for (const auto& binding : p.bindings[set].domains) {
            if (!binding.affineFields() || binding.map == BindingDomain::Map::Zip || binding.count < 4096)
                continue;
            uint32_t splitInput = 0;
            for (uint32_t e = 0; e < binding.split; ++e) splitInput += t.spaces[e]->stateSize;
            auto oriented = *bound;
            bool valid = true;
            for (auto& [a, b] : oriented.coordinates) {
                if (a >= splitInput && b < splitInput) std::swap(a, b);
                valid = valid && a < splitInput && b >= splitInput && b < parameters;
            }
            for (uint32_t row = 0; valid && row < binding.count; ++row)
                valid = jacobi[layout.first + binding.first + row];
            if (!valid) continue;
            domains.push_back({set, layout.first + binding.first, binding.count, 0, 0, 0, 0, 0,
                               binding, std::move(oriented)});
        }
    }
    const bool active = p.policy.weighting == JacobiWeighting::Active ||
                        (p.policy.weighting == JacobiWeighting::Auto && !domains.empty());
    // A proof enables indexing; it does not establish that indexing is cheaper.
    // Hash probes involve integer indirection, linked-list reads and an extra
    // dispatch. Small, cheap domains benefit more from a parallel activity scan.
    constexpr uint64_t ProbeCost = 12;
    domains.erase(std::remove_if(domains.begin(), domains.end(), [&](const Domain& d) {
        uint64_t neighbors = d.bound.coordinates.size() == 3 ? 27 : d.bound.coordinates.size() == 2 ? 9 : 3;
        return !p.policy.spatialCandidates ||
               d.count < ProbeCost * (uint64_t(d.binding.left) * neighbors + d.binding.right);
    }), domains.end());
    if (domains.empty() && !active) return;

    struct TypeQueue { uint32_t type, first, count, lanes; };
    std::vector<TypeQueue> typed;
    std::set<uint32_t> types;
    auto metadata = words(p.buffers[size_t(BufferRole::Relations)]);
    std::vector<uint32_t> typeCounts(p.types.size());
    for (auto id : all) ++typeCounts[metadata[size_t(id) * 9 + 8]];
    uint32_t typeFirst = 0, solveLanes = 0;
    for (uint32_t type = 0; type < typeCounts.size(); ++type) {
        auto count = typeCounts[type];
        if (!count) continue;
        auto width = uint32_t(std::min(uint64_t(4096), ((uint64_t(count) + 127) / 128) * 128));
        typed.push_back({type, typeFirst, count, width});
        types.insert(type);
        typeFirst += count;
        solveLanes += width;
    }

    // Full logical capacity guarantees correctness even when all pairs are active.
    // Queues compact execution; public row-indexed fields are intentionally dense.
    uint64_t storage = 1 + 2 * typed.size();
    auto reserve = [&](uint64_t n) {
        auto first = storage;
        storage += n;
        if (storage > UINT32_MAX) throw std::overflow_error("Candidate storage exceeds 32-bit addressing");
        return uint32_t(first);
    };
    uint32_t bits = reserve((uint64_t(relations) + 31) / 32);
    uint32_t queues = reserve(uint64_t(all.size()) * 2);
    for (auto& d : domains) {
        d.header = reserve(3); // squared bound, invalid bound, invalid coordinate
        d.buckets = 1;
        while (d.buckets < uint64_t(d.binding.right) * 2) {
            if (d.buckets >= (1u << 30)) throw std::overflow_error("Candidate bucket capacity");
            d.buckets *= 2;
        }
        d.heads = reserve(d.buckets);
        d.next = reserve(d.binding.right);
        d.cells = reserve(uint64_t(d.binding.right) * 3);
        for (uint32_t row = 0; row < d.count; ++row) selected[d.first + row] = true;
        p.statistics.candidateRelations += d.count;
    }
    p.statistics.candidateDomains = uint32_t(domains.size());
    p.statistics.activeJacobiRelations = active ? all.size() : 0;
    p.buffers[size_t(BufferRole::Candidates)].initial.resize(size_t(storage) * 4);
    p.buffers[size_t(BufferRole::ActiveDegrees)].initial.resize(size_t(p.statistics.variables) * 4);
    auto add = [&](const std::string& label, const std::string& source, uint32_t count, std::vector<Batch>& stages) {
        stages.push_back({uint32_t(p.kernels.size()), 0, count});
        p.kernels.push_back({label, source});
    };
    const uint32_t lanes = std::min(4096u, uint32_t((all.size() + 127) / 128) * 128);
    std::ostringstream activity;
    activity << stateAccess(false);
    for (auto type : types)
        activity << relationActivityFunction(*p.types[type], p.typeReadOnly[type], p.typeEndpointMode[type],
                                             "activity" + std::to_string(type), active).source;
    activity << "bool activeRow(uint id){switch(relation(id).type){";
    for (auto type : types)
        activity << "case " << type << "u:return activity" << type
                 << "(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration);";
    activity << "}return false;}\nvoid consider(uint id,bool sparse){"
             << "uint mask=1u<<(id&31u);if((atomicOr(x_candidates[" << bits
             << "u+id/32u],mask)&mask)!=0u)return;"
                "bool participates=activeRow(id);if(sparse&&!participates)return;"
                "uint side=x_candidates[0],slot=0u,first=0u;switch(relation(id).type){";
    for (uint32_t k = 0; k < typed.size(); ++k)
        activity << "case " << typed[k].type << "u:slot=" << k << "u;first=" << typed[k].first << "u;break;";
    activity << "}uint at=atomicAdd(x_candidates[1u+side*" << typed.size() << "u+slot],1u);"
                "x_candidates[" << queues << "u+side*" << all.size() << "u+first+at]=id;}\n";

    // Parameter expressions run on the GPU using the same float generator as the
    // residual. Runtime invalidates this cache on parameter patches.
    if (!domains.empty()) {
        std::ostringstream reset;
        reset << "void main(){if(invocation()!=0u)return;";
        for (const auto& d : domains)
            reset << "x_candidates[" << d.header << "u]=0u;x_candidates[" << d.header + 1 << "u]=0u;";
        reset << "}";
        add("Reset candidate bounds", reset.str(), 1, p.candidateBounds);
        for (const auto& d : domains) {
            const auto& layout = p.relations[d.set];
            const auto& t = *p.types[layout.type];
            uint32_t firstParameter = t.inputSize() - t.parameters - 2;
            std::ostringstream source;
            source << emitGlsl(d.bound.squaredRadius, "boundValue")
                   << "void main(){for(uint i=invocation();i<" << d.count << "u;i+=step.count){float x["
                   << t.inputSize() << "],r[1];";
            for (uint32_t k = 0; k < t.parameters; ++k)
                source << "x[" << firstParameter + k << "]=x_parameters["
                       << layout.parameters + d.binding.first + k * layout.count << "u+i];";
            source << "boundValue(x,r);if(isnan(r[0])||isinf(r[0])||r[0]<1e-20||r[0]>1e20)"
                      "atomicOr(x_candidates[" << d.header + 1 << "u],1u);else atomicMax(x_candidates["
                   << d.header << "u],floatBitsToUint(r[0]));}}";
            add("Reduce candidate bound", source.str(), lanes, p.candidateBounds);
        }
    }
    std::vector<Batch> stages;
    std::ostringstream reset;
    reset << "void resetCandidates(uint iteration){uint i=invocation();if(i>=step.count)return;"
             "if(i==0u){uint side=1u-x_candidates[0];"
             "x_candidates[0]=side;for(uint k=0u;k<" << typed.size()
          << "u;++k)x_candidates[1u+side*" << typed.size() << "u+k]=0u;}";
    reset << "for(uint j=i;j<" << (uint64_t(relations) + 31) / 32
          << "u;j+=step.count)x_candidates[" << bits << "u+j]=0u;";
    if (active)
        reset << "for(uint j=i;j<" << p.statistics.variables << "u;j+=step.count)x_activeDegrees[j]=0u;";
    for (const auto& d : domains) {
        reset << "if(i==0u)x_candidates[" << d.header + 2 << "u]=x_candidates[" << d.header + 1 << "u];"
              << "for(uint j=i;j<" << d.buckets << "u;j+=step.count)x_candidates[" << d.heads << "u+j]=0xffffffffu;"
              << "if(iteration==0u)for(uint j=i;j<" << d.count << "u;j+=step.count)x_lambda["
              << p.relations[d.set].multipliers + d.binding.first << "u+j]=0.0;";
    }
    reset << "}";
    if (p.apply.size() == 1) {
        add("Initialize Jacobi candidates", reset.str() + "void main(){resetCandidates(0u);}", lanes, p.prepareCandidates);
        // Gather has consumed all corrections; reuse its dispatch to prepare the
        // next snapshot. No relation state used by gather is cleared here.
        auto& gather = p.kernels[p.apply[0].kernel].source;
        gather = "#define main gatherValues\n" + gather + "\n#undef main\n" + reset.str() +
                 "void main(){gatherValues();resetCandidates((step.iteration+1u)%" + number(p.policy.iterations) + ");}";
    } else {
        add("Reset Jacobi candidates", reset.str() + "void main(){resetCandidates(step.iteration);}", lanes, stages);
    }
    uint32_t firstQuery = UINT32_MAX;
    std::string firstQueryBody;
    for (const auto& d : domains) {
        std::ostringstream source;
        source << domainSource(p, d) << "void main(){uint member=invocation();if(member>=" << d.binding.right
               << "u)return;ivec3 c;if(!cellOf(feature(member,false),c)){atomicOr(x_candidates["
               << d.header + 2 << "u],1u);return;}uint at=" << d.cells << "u+member*3u;"
                  "x_candidates[at]=uint(c.x);x_candidates[at+1u]=uint(c.y);x_candidates[at+2u]=uint(c.z);"
                  "x_candidates[" << d.next << "u+member]=atomicExchange(x_candidates[" << d.heads << "u+bucket(c)],member);}";
        add("Index candidate domain", source.str(), d.binding.right, stages);
        source.str(""); source.clear();
        const auto dimensions = d.bound.coordinates.size();
        uint32_t neighbors = dimensions == 3 ? 27 : dimensions == 2 ? 9 : 3;
        source << activity.str() << domainSource(p, d)
               << "void queryDomain(){uint lane=invocation(),i=lane/" << neighbors << "u,neighbor=lane%" << neighbors
               << "u;if(i>=" << d.binding.left << "u)return;ivec3 c;"
                  "if(x_candidates[" << d.header + 2 << "u]!=0u||!cellOf(feature(i,true),c)){"
                  "for(uint j=neighbor;j<" << d.binding.right << "u;j+=" << neighbors
               << "u)if(accepts(i,j))consider(rowOf(i,j),true);return;}"
                  "ivec3 offset=ivec3(int(neighbor%3u)-1,";
        source << (dimensions >= 2 ? "int((neighbor/3u)%3u)-1" : "0") << ","
               << (dimensions >= 3 ? "int(neighbor/9u)-1" : "0") << ");"
                  "ivec3 cell=c+offset;uint j=x_candidates["
               << d.heads << "u+bucket(cell)];while(j!=0xffffffffu){uint at=" << d.cells << "u+j*3u;"
                  "ivec3 actual=ivec3(x_candidates[at],x_candidates[at+1u],x_candidates[at+2u]);"
                  "if(all(equal(cell,actual))&&accepts(i,j))consider(rowOf(i,j),true);j=x_candidates["
               << d.next << "u+j];}}";
        if (uint64_t(d.binding.left) * neighbors > UINT32_MAX)
            throw std::overflow_error("Candidate query dispatch exceeds 32-bit addressing");
        if (firstQuery == UINT32_MAX) {
            firstQuery = uint32_t(p.kernels.size());
            firstQueryBody = source.str().substr(activity.str().size());
        }
        add("Query candidate domain", source.str() + "void main(){queryDomain();}", d.binding.left * neighbors, stages);
    }
    std::vector<uint32_t> ordinary;
    for (auto id : all) if (!selected[id]) ordinary.push_back(id);
    uint32_t ordinaryFirst = uint32_t(work.size());
    work.insert(work.end(), ordinary.begin(), ordinary.end());
    auto& workBuffer = p.buffers[size_t(BufferRole::RelationWork)].initial;
    workBuffer.resize(work.size() * 4);
    std::memcpy(workBuffer.data(), work.data(), workBuffer.size());
    std::ostringstream collect;
    collect << activity.str() << "bool indexed(uint id){return false";
    for (const auto& d : domains) collect << "||(id>=" << d.first << "u&&id-" << d.first << "u<" << d.count << "u)";
    collect << ";}\nbool sparseType(uint id){switch(relation(id).type){";
    for (auto type : types)
        if (p.types[type]->rows == 1 && p.types[type]->kind != RelationKind::Equality)
            collect << "case " << type << "u:return true;";
    collect << "}return false;}\nvoid collectActivity(){uint i=invocation();if(i>=step.count)return;for(uint k=i;k<" << ordinary.size()
            << "u;k+=step.count){uint id=x_relationWork[" << ordinaryFirst << "u+k];consider(id,sparseType(id));}"
               "if(step.iteration==0u)return;uint side=1u-x_candidates[0];";
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        if (std::none_of(domains.begin(), domains.end(), [&](const Domain& d) { return p.relations[d.set].type == q.type; }))
            continue;
        collect << "{uint count=x_candidates[" << 1 + slot << "u+side*" << typed.size() << "u];"
                   "for(uint k=i;k<count;k+=step.count){uint id=x_candidates[" << queues + q.first << "u+side*" << all.size()
                << "u+k];if(indexed(id)&&x_lambda[relation(id).l]!=0.0)consider(id,true);}}";
    }
    collect << "}";
    if (firstQuery != UINT32_MAX) {
        p.kernels[firstQuery].source = collect.str() + firstQueryBody +
                                      "void main(){collectActivity();queryDomain();}";
        for (auto& batch : stages)
            if (batch.kernel == firstQuery)
                batch.count = std::max(batch.count, uint32_t(ordinary.size()));
    } else {
        add("Collect Jacobi activity", collect.str() + "void main(){collectActivity();}", uint32_t(ordinary.size()), stages);
    }
    std::vector<std::pair<uint32_t, KernelFunction>> solve;
    for (auto type : types) {
        const auto& t = *p.types[type];
        solve.emplace_back(type, relationFunction(t, p.typeReadOnly[type], p.typeEndpointMode[type], true,
                            t.rows == 1 && t.kind != RelationKind::Equality,
                            p.statistics.directJacobiRelations != 0, false, "candidateSolve" + std::to_string(type), active, true));
    }
    std::ostringstream source;
    source << stateAccess(false);
    for (const auto& item : solve) source << item.second.source;
    source << "void main(){uint lane=invocation(),side=x_candidates[0];";
    uint32_t laneFirst = 0;
    // Whole workgroups consume one mathematical type. Interleaving unlike active
    // rows would serialize branches and inflate the per-warp register footprint.
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        source << "if(lane>=" << laneFirst << "u&&lane<" << laneFirst + q.lanes
               << "u){uint count=x_candidates[" << 1 + slot << "u+side*" << typed.size()
               << "u];for(uint i=lane-" << laneFirst << "u;i<count;i+=" << q.lanes << "u)candidateSolve" << q.type
               << "(x_candidates[" << queues + q.first << "u+side*" << all.size()
               << "u+i],step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration);}";
        laneFirst += q.lanes;
    }
    source << "}";
    add("Solve Jacobi candidates", source.str(), solveLanes, stages);
    auto oldSize = p.solve.size();
    p.solve.erase(std::remove_if(p.solve.begin(), p.solve.end(), [](const Batch& b) { return b.color < 0; }), p.solve.end());
    auto removed = oldSize - p.solve.size();
    p.solve.insert(p.solve.end(), stages.begin(), stages.end());
    p.statistics.dispatches -= uint64_t(p.policy.substeps) * p.policy.iterations * removed;
    p.statistics.dispatches += uint64_t(p.policy.substeps) * p.policy.iterations * stages.size()
                              + p.prepareCandidates.size();
    prunePlanKernels(p);
}
} // namespace whimsical::dynamics
