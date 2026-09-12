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
    uint32_t leftCells = 0;
};
std::vector<uint32_t> words(const BufferData& b) {
    std::vector<uint32_t> result(b.initial.size() / 4);
    if (!result.empty()) std::memcpy(result.data(), b.initial.data(), b.initial.size());
    return result;
}
std::string number(uint32_t n) { return std::to_string(n) + "u"; }
void reserveZero(BufferData& buffer, uint64_t words) {
    if (buffer.wordCount())
        throw std::logic_error("Candidate buffers must be reserved once");
    if (words > UINT32_MAX)
        throw std::overflow_error("Candidate buffer exceeds 32-bit addressing");
    buffer.words = words;
    if (words)
        buffer.fills.push_back({0, words, 0});
}
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
std::optional<DifferenceBound> candidateBound(
    const CompiledPlan& p, uint32_t set, const BindingDomain& binding) {
    const auto& layout = p.relations[set];
    const auto& type = *p.types[layout.type];
    if (type.rows != 1 || type.kind == RelationKind::Equality || type.history || type.update ||
        !binding.affineFields() || binding.map == BindingDomain::Map::Zip || binding.count < 4096)
        return {};
    const uint32_t parameterFirst = type.inputSize() - type.parameters - type.history - 2;
    auto bound = differenceBound(type.residual, type.kind == RelationKind::GreaterEqual,
                                 parameterFirst, type.parameters);
    if (!bound || bound->coordinates.size() > 3)
        return {};
    uint32_t splitInput = 0;
    for (uint32_t endpoint = 0; endpoint < binding.split; ++endpoint)
        splitInput += type.spaces[endpoint]->stateSize;
    for (auto& [left, right] : bound->coordinates) {
        if (left >= splitInput && right < splitInput)
            std::swap(left, right);
        if (left >= splitInput || right < splitInput || right >= parameterFirst)
            return {};
    }
    return bound;
}
bool indexingWins(const BindingDomain& binding, uint32_t dimensions) {
    constexpr uint64_t ProbeCost = 12;
    const uint64_t neighbors = dimensions == 3 ? 27 : dimensions == 2 ? 9 : 3;
    return binding.count >= ProbeCost * (uint64_t(binding.left) * neighbors + binding.right);
}
} // namespace

bool canDeferCandidateDomain(const CompiledPlan& p, uint32_t set, const BindingDomain& binding) {
    if (p.dynamicTopology || p.policy.execution != ExecutionMode::Auto ||
        p.policy.mode == SolveMode::Colored || p.policy.weighting == JacobiWeighting::Static ||
        !p.policy.spatialCandidates)
        return false;
    auto bound = candidateBound(p, set, binding);
    return bound && indexingWins(binding, uint32_t(bound->coordinates.size()));
}

void lowerCandidateDomains(CompiledPlan& p) {
    // Region ownership and dynamic endpoints have their own scheduling contracts.
    // This lowering consumes only global, static Jacobi snapshots.
    if (p.dynamicTopology || !p.local.empty() || p.policy.execution != ExecutionMode::Auto)
        return;
    const uint32_t relations = uint32_t(p.statistics.relations);
    auto work = words(p.buffers[size_t(BufferRole::RelationWork)]);
    std::vector<bool> jacobi(relations);
    std::vector<uint32_t> all;
    for (const auto& batch : p.solve)
        if (batch.color < 0)
            for (uint32_t k = 0; k < batch.count; ++k) {
                uint32_t id = work[batch.first + k];
                jacobi[id] = true;
                all.push_back(id);
            }
    if (all.empty() && p.deferredJacobiDomains.empty()) return;
    std::vector<Domain> domains;
    for (uint32_t set = 0; set < p.relations.size(); ++set) {
        const auto& layout = p.relations[set];
        for (uint32_t domain = 0; domain < p.bindings[set].domains.size(); ++domain) {
            const auto& binding = p.bindings[set].domains[domain];
            auto bound = candidateBound(p, set, binding);
            if (!bound)
                continue;
            const bool deferred = std::any_of(
                p.deferredJacobiDomains.begin(), p.deferredJacobiDomains.end(),
                [&](const DeferredJacobiDomain& candidate) {
                    return candidate.set == set && candidate.domain == domain;
                });
            bool valid = true;
            for (uint32_t row = 0; valid && !deferred && row < binding.count; ++row)
                valid = jacobi[layout.first + binding.first + row];
            if (!valid) continue;
            domains.push_back({set, layout.first + binding.first, binding.count, 0, 0, 0, 0, 0,
                               binding, std::move(*bound)});
        }
    }
    const bool active = p.policy.weighting == JacobiWeighting::Active ||
                        (p.policy.weighting == JacobiWeighting::Auto && !domains.empty());
    // A proof enables indexing; it does not establish that indexing is cheaper.
    // Hash probes involve integer indirection, linked-list reads and an extra
    // dispatch. Small, cheap domains benefit more from a parallel activity scan.
    domains.erase(std::remove_if(domains.begin(), domains.end(), [&](const Domain& d) {
        return !p.policy.spatialCandidates ||
               !indexingWins(d.binding, uint32_t(d.bound.coordinates.size()));
    }), domains.end());
    if (domains.empty() && !active) return;

    struct TypeQueue { uint32_t type, first, count, capacity, lanes; };
    std::vector<TypeQueue> typed;
    std::set<uint32_t> types;
    std::vector<uint32_t> typeCounts(p.types.size());
    for (auto id : all) ++typeCounts[p.relationType(id)];
    for (const auto& deferred : p.deferredJacobiDomains) {
        const auto& binding = p.bindings[deferred.set].domains[deferred.domain];
        typeCounts[p.relations[deferred.set].type] += binding.count;
    }
    // Logical domains remain exact, while their active execution representation
    // has linear capacity. Dense activity falls back to a full domain traversal.
    constexpr uint64_t QueueRowsPerMember = 64;
    std::vector<uint64_t> domainCounts(p.types.size()), domainCapacities(p.types.size());
    for (const auto& d : domains) {
        const auto type = p.relations[d.set].type;
        domainCounts[type] += d.count;
        domainCapacities[type] +=
            std::min<uint64_t>(d.count, QueueRowsPerMember * (uint64_t(d.binding.left) + d.binding.right));
    }
    uint64_t logicalQueueCount64 = 0, queueCapacity64 = 0;
    uint32_t solveLanes = 0;
    for (uint32_t type = 0; type < typeCounts.size(); ++type) {
        auto count = typeCounts[type];
        if (!count) continue;
        if (domainCounts[type] > count)
            throw std::logic_error("Candidate domain accounting exceeds type rows");
        const uint64_t capacity =
            uint64_t(count) - domainCounts[type] + domainCapacities[type];
        if (!capacity || capacity > UINT32_MAX || queueCapacity64 + capacity > UINT32_MAX)
            throw std::overflow_error("Candidate queue capacity exceeds 32-bit addressing");
        // Keep enough independent groups for a wide GPU. The previous 32-group
        // ceiling serialized long active queues even when many SMs were idle.
        // This is a bounded execution budget, never a cap on queued relations.
        constexpr uint64_t SolveGroups = 128;
        auto width =
            uint32_t(std::min(SolveGroups * 128, ((capacity + 127) / 128) * 128));
        typed.push_back(
            {type, uint32_t(queueCapacity64), count, uint32_t(capacity), width});
        types.insert(type);
        logicalQueueCount64 += count;
        queueCapacity64 += capacity;
        solveLanes += width;
    }
    if (logicalQueueCount64 > UINT32_MAX)
        throw std::overflow_error("Candidate logical work exceeds 32-bit addressing");
    const uint32_t logicalQueueCount = uint32_t(logicalQueueCount64);
    const uint32_t queueCapacity = uint32_t(queueCapacity64);

    // The solve publishes its queue side separately: gather may reset the next
    // side while other workgroups still consume the completed queue for cleanup.
    // Overflow is side-specific so the next iteration can rebuild from the full
    // logical domain without reading a truncated predecessor.
    constexpr uint32_t counters = 2;
    uint64_t storage = counters + 2 * typed.size();
    auto reserve = [&](uint64_t n) {
        auto first = storage;
        storage += n;
        if (storage > UINT32_MAX) throw std::overflow_error("Candidate storage exceeds 32-bit addressing");
        return uint32_t(first);
    };
    const uint32_t overflows = reserve(2);
    const uint32_t queues = reserve(uint64_t(queueCapacity) * 2);
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
        const bool sameFeatures = d.binding.left == d.binding.right &&
            std::all_of(d.bound.coordinates.begin(), d.bound.coordinates.end(), [&](const auto& inputs) {
                return coordinate(p, d, inputs.first) == coordinate(p, d, inputs.second);
            });
        d.leftCells = sameFeatures ? d.cells : reserve(uint64_t(d.binding.left) * 3);
        p.statistics.candidateRelations += d.count;
    }
    p.statistics.candidateDomains = uint32_t(domains.size());
    p.statistics.activeJacobiRelations = active ? logicalQueueCount : 0;
    p.statistics.candidateQueueCapacity = queueCapacity;
    reserveZero(p.buffers[size_t(BufferRole::Candidates)], storage);
    reserveZero(p.buffers[size_t(BufferRole::ActiveDegrees)], p.statistics.variables);
    auto add = [&](const std::string& label, const std::string& source, uint32_t count, std::vector<Batch>& stages) {
        stages.push_back({uint32_t(p.kernels.size()), 0, count});
        p.kernels.push_back({label, source});
    };
    const uint32_t lanes = std::min(4096u, uint32_t((uint64_t(logicalQueueCount) + 127) / 128) * 128);
    std::ostringstream activity;
    activity << stateAccess(false);
    for (auto type : types)
        activity << relationActivityFunction(*p.types[type], p.typeReadOnly[type], p.typeEndpointMode[type],
                                             "activity" + std::to_string(type), active).source;
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        const auto& t = *p.types[q.type];
        activity << "void enqueue" << q.type << "(uint id){uint side=x_candidates[0],at=atomicAdd(x_candidates["
                 << counters + slot << "u+side*" << typed.size() << "u],1u);if(at<" << q.capacity
                 << "u)x_candidates[" << queues + q.first << "u+side*" << queueCapacity
                 << "u+at]=id;else atomicOr(x_candidates[" << overflows << "u+side],1u);}\n";
        activity << "void consider" << q.type << "(uint id){bool participates=activity" << q.type
                 << "(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration);";
        // Ordinary multirow work must still clear its CSR contributions when
        // disabled, so only guarded scalar inequalities are compacted away.
        if (t.rows == 1 && t.kind != RelationKind::Equality)
            activity << "if(!participates)return;";
        activity << "enqueue" << q.type << "(id);}\n";
    }
    // Each domain query enumerates a logical row at most once. A nonzero
    // multiplier remains in the previous queue, so partition by multiplier
    // state instead of atomically deduplicating overlapping work. Both sources
    // consume the same Jacobi snapshot; only the later solve changes multipliers.
    std::vector<std::string> mappedActivity(p.types.size());
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        if (p.typeEndpointMode[q.type] <= 0 ||
            std::none_of(domains.begin(), domains.end(), [&](const Domain& d) { return p.relations[d.set].type == q.type; }))
            continue;
        std::ostringstream mapped;
        mapped << relationActivityFunction(*p.types[q.type], p.typeReadOnly[q.type], p.typeEndpointMode[q.type],
                                            "mappedActivity", active, true, true).source;
        mapped << "void considerNew(uint id,uint left,uint right,float residual){"
                  "if(step.iteration!=0u&&x_lambda[relation(id).l]!=0.0)return;"
                  "if(!mappedActivity(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration,left,right,residual))return;"
                  "enqueue" << q.type << "(id);}\n";
        mappedActivity[q.type] = mapped.str();
    }
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
            source << fieldAccess(false) << emitGlsl(d.bound.squaredRadius, "boundValue")
                   << "void reduceBound(uint i){float x[" << t.inputSize()
                   << "],r[1];Relation boundRelation=relation(" << d.first << "u+i);";
            for (uint32_t k = 0; k < t.parameters; ++k)
                source << "x[" << firstParameter + k << "]=loadParameter(boundRelation," << k << "u);";
            source << "boundValue(x,r);if(isnan(r[0])||isinf(r[0])||r[0]<1e-20||r[0]>1e20)"
                      "atomicOr(x_candidates[" << d.header + 1 << "u],1u);else atomicMax(x_candidates["
                   << d.header << "u],floatBitsToUint(r[0]));}"
                      "void main(){";
            if (p.relationFieldModes[d.set].mask & CompiledPlan::UniformParameters)
                source << "Relation firstRelation=relation(" << d.first
                       << "u);if((x_fieldModes[firstRelation.u]&2u)!=0u){"
                          "if(invocation()==0u)reduceBound(0u);return;}";
            source << "for(uint i=invocation();i<" << d.count << "u;i+=step.count)reduceBound(i);}";
            add("Reduce candidate bound", source.str(), lanes, p.candidateBounds);
        }
    }
    std::vector<Batch> stages;
    std::ostringstream indexed;
    indexed << "bool indexed(uint id){return false";
    for (const auto& d : domains) indexed << "||(id>=" << d.first << "u&&id-" << d.first << "u<" << d.count << "u)";
    indexed << ";}\n";
    const std::string rebuild =
        "bool rebuildCandidates(){uint side=x_candidates[0];return step.iteration!=0u&&"
        "x_candidates[" + std::to_string(overflows) + "u+1u-side]!=0u;}\n";
    std::ostringstream reset;
    reset << indexed.str() << "void resetCandidates(uint iteration){uint i=invocation();if(i>=step.count)return;"
             "if(i==0u){uint side=1u-x_candidates[0];"
             "x_candidates[0]=side;for(uint k=0u;k<" << typed.size()
          << "u;++k)x_candidates[" << counters << "u+side*" << typed.size()
          << "u+k]=0u;x_candidates[" << overflows << "u+side]=0u;}";
    if (active)
        reset << "for(uint j=i;j<" << p.statistics.variables << "u;j+=step.count)x_activeDegrees[j]=0u;";
    for (const auto& d : domains) {
        reset << "if(i==0u)x_candidates[" << d.header + 2 << "u]=x_candidates[" << d.header + 1 << "u];"
              << "for(uint j=i;j<" << d.buckets << "u;j+=step.count)x_candidates[" << d.heads << "u+j]=0xffffffffu;";
    }
    // Every nonzero indexed multiplier remains in the completed active queue.
    // At a substep boundary clear that queue, not the entire Cartesian domain.
    // Enabled fields cannot change inside a submission. The next tick therefore
    // starts with zero multipliers even if enablement/parameters have changed.
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        if (std::none_of(domains.begin(), domains.end(), [&](const Domain& d) { return p.relations[d.set].type == q.type; }))
            continue;
        reset << "if(iteration==0u){uint side=x_candidates[1],count=x_candidates["
              << counters + slot << "u+side*" << typed.size() << "u];count=min(count," << q.capacity
              << "u);for(uint k=i;k<count;k+=step.count){uint id=x_candidates[" << queues + q.first
              << "u+side*" << queueCapacity
              << "u+k];if(indexed(id))x_lambda[relation(id).l]=0.0;}}";
    }
    reset << "}";
    if (p.apply.size() == 1) {
        add("Initialize Jacobi candidates", reset.str() + "void main(){resetCandidates(1u);}", lanes, p.prepareCandidates);
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
        const auto& layout = p.relations[d.set];
        const auto& relationType = *p.types[layout.type];
        const uint32_t firstParameter = relationType.inputSize() - relationType.parameters - 2;
        source << rebuild << domainSource(p, d)
               << "void main(){if(rebuildCandidates())return;uint member=invocation();";
        // Cache the feature-to-cell map once per member and snapshot. Identical
        // affine feature maps share the same column, including self-products.
        if (d.leftCells != d.cells)
            source << "if(member<" << d.binding.left << "u){ivec3 c;if(!cellOf(feature(member,true),c))"
                      "atomicOr(x_candidates[" << d.header + 2 << "u],1u);else{uint at=" << d.leftCells
                   << "u+member*3u;x_candidates[at]=uint(c.x);x_candidates[at+1u]=uint(c.y);"
                      "x_candidates[at+2u]=uint(c.z);}}";
        source << "if(member>=" << d.binding.right
               << "u)return;ivec3 c;if(!cellOf(feature(member,false),c)){atomicOr(x_candidates["
               << d.header + 2 << "u],1u);return;}uint at=" << d.cells << "u+member*3u;"
                  "x_candidates[at]=uint(c.x);x_candidates[at+1u]=uint(c.y);x_candidates[at+2u]=uint(c.z);"
                  "x_candidates[" << d.next << "u+member]=atomicExchange(x_candidates[" << d.heads << "u+bucket(c)],member);}";
        add("Index candidate domain", source.str(), std::max(d.binding.left, d.binding.right), stages);
        source.str(""); source.clear();
        const auto dimensions = d.bound.coordinates.size();
        const uint32_t fullNeighbors = dimensions == 3 ? 27 : dimensions == 2 ? 9 : 3;
        // Identical feature maps on an upper domain permit one orientation of
        // each cell pair. Canonicalize member IDs before recovering the logical
        // row; the relation itself need not be symmetric. This is an index-domain
        // proof, not an assumption about the residual or the other input fields.
        const bool halfNeighborhood = d.leftCells == d.cells &&
            (d.binding.map == BindingDomain::Map::Upper || d.binding.map == BindingDomain::Map::UpperDiagonal);
        const uint32_t neighborFirst = halfNeighborhood ? fullNeighbors / 2 : 0;
        const uint32_t neighbors = fullNeighbors - neighborFirst;
        auto type = layout.type;
        source << activity.str() << rebuild << domainSource(p, d);
        source << emitGlsl(d.bound.squaredRadius, "candidateRadius")
               << "float candidateResidual(uint id,float distanceSquared){";
        if (p.relationFieldModes[d.set].mask & CompiledPlan::UniformParameters)
            source << "if((x_fieldModes[" << layout.modes << "u]&2u)!=0u&&x_candidates["
                   << d.header + 1 << "u]==0u)return "
                   << (relationType.kind == RelationKind::GreaterEqual
                           ? "distanceSquared-uintBitsToFloat(x_candidates[" +
                                 std::to_string(d.header) + "u]);"
                           : "uintBitsToFloat(x_candidates[" + std::to_string(d.header) +
                                 "u])-distanceSquared;");
        source << "Relation r=relation(id);float x[" << relationType.inputSize() << "],radius[1];";
        for (uint32_t k = 0; k < relationType.parameters; ++k)
            source << "x[" << firstParameter + k << "]=loadParameter(r," << k << "u);";
        source << "candidateRadius(x,radius);return "
               << (relationType.kind == RelationKind::GreaterEqual
                       ? "distanceSquared-radius[0]"
                       : "radius[0]-distanceSquared")
               << ";}\n";
        // The query already owns the two member indices. Feed them into the
        // same predicate generator instead of inverting rowOf's triangle again.
        if (!mappedActivity[type].empty())
            source << mappedActivity[type];
        else
            source << "void considerNew(uint id,uint left,uint right,float residual){"
                      "if(step.iteration==0u||x_lambda[relation(id).l]==0.0)consider"
                   << type << "(id);}\n";
        // Apply the proven feature bound before touching row-indexed fields.
        // The same 25% radius margin as cellOf covers float reassociation; this
        // is only a conservative rejection, never a replacement for the formula.
        source << "void queryDomain(){if(rebuildCandidates())return;uint lane=invocation(),i=lane/"
               << neighbors << "u,neighbor=lane%" << neighbors
               << "u;if(i>=" << d.binding.left << "u)return;vec3 leftFeature=feature(i,true);"
                  "if(x_candidates[" << d.header + 2 << "u]!=0u){"
                  "for(uint j=neighbor;j<" << d.binding.right << "u;j+=" << neighbors
               << "u)if(accepts(i,j)){uint id=rowOf(i,j);vec3 delta=leftFeature-feature(j,false);"
                  "float distanceSquared=dot(delta,delta);considerNew(id,i,j,candidateResidual(id,distanceSquared));}return;}"
                  "float bound=1.5625*uintBitsToFloat(x_candidates[" << d.header << "u]);"
                  "uint featureAt=" << d.leftCells << "u+i*3u;ivec3 c=ivec3(x_candidates[featureAt],"
                  "x_candidates[featureAt+1u],x_candidates[featureAt+2u]);"
                  "uint offsetIndex=neighbor+" << neighborFirst << "u;"
                  "ivec3 offset=ivec3(int(offsetIndex%3u)-1,";
        source << (dimensions >= 2 ? "int((offsetIndex/3u)%3u)-1" : "0") << ","
               << (dimensions >= 3 ? "int(offsetIndex/9u)-1" : "0") << ");"
                  "ivec3 cell=c+offset;uint j=x_candidates["
               << d.heads << "u+bucket(cell)];while(j!=0xffffffffu){if("
               << (halfNeighborhood ? "neighbor!=0u||accepts(i,j)" : "accepts(i,j)")
               << "){uint at=" << d.cells << "u+j*3u;"
                  "ivec3 actual=ivec3(x_candidates[at],x_candidates[at+1u],x_candidates[at+2u]);"
                  "if(all(equal(cell,actual))){vec3 delta=leftFeature-feature(j,false);"
                  "float distanceSquared=dot(delta,delta);if(distanceSquared<=bound){";
        if (halfNeighborhood)
            source << "uint left=min(i,j),right=max(i,j),id=rowOf(left,right);"
                      "considerNew(id,left,right,candidateResidual(id,distanceSquared));";
        else
            source << "uint id=rowOf(i,j);considerNew(id,i,j,candidateResidual(id,distanceSquared));";
        source << "}}}j=x_candidates["
               << d.next << "u+j];}}";
        if (uint64_t(d.binding.left) * neighbors > UINT32_MAX)
            throw std::overflow_error("Candidate query dispatch exceeds 32-bit addressing");
        if (firstQuery == UINT32_MAX) {
            firstQuery = uint32_t(p.kernels.size());
            firstQueryBody = source.str().substr(activity.str().size() + rebuild.size());
        }
        add("Query candidate domain", source.str() + "void main(){queryDomain();}", d.binding.left * neighbors, stages);
    }
    std::vector<std::vector<uint32_t>> ordinaryByType(p.types.size());
    auto selected = [&](uint32_t id) {
        return std::any_of(domains.begin(), domains.end(), [&](const Domain& domain) {
            return id >= domain.first && id - domain.first < domain.count;
        });
    };
    for (auto id : all)
        if (!selected(id))
            ordinaryByType[p.relationType(id)].push_back(id);
    uint32_t ordinaryFirst = uint32_t(work.size());
    for (const auto& rows : ordinaryByType) work.insert(work.end(), rows.begin(), rows.end());
    uint64_t ordinaryCount64 = 0;
    for (const auto& rows : ordinaryByType) ordinaryCount64 += rows.size();
    uint64_t fallbackCount = ordinaryCount64;
    for (const auto& d : domains) fallbackCount += d.count;
    if (fallbackCount != logicalQueueCount)
        throw std::logic_error("Candidate fallback domain does not cover logical work");
    std::ostringstream fallbackRows;
    fallbackRows << "bool fallbackRow(uint lane,out uint id){";
    if (ordinaryCount64) {
        fallbackRows << "if(lane<" << ordinaryCount64 << "u){id=x_relationWork[" << ordinaryFirst
                     << "u+lane];return true;}lane-=" << ordinaryCount64 << "u;";
    }
    for (const auto& d : domains)
        fallbackRows << "if(lane<" << d.count << "u){id=" << d.first
                     << "u+lane;return true;}lane-=" << d.count << "u;";
    fallbackRows << "return false;}\n";
    std::ostringstream fallbackCollect;
    fallbackCollect << fallbackRows.str()
                    << "void considerFallback(uint id){switch(relation(id).type){";
    for (auto type : types)
        fallbackCollect << "case " << type << "u:consider" << type << "(id);break;";
    fallbackCollect << "}}\n";
    auto& workBuffer = p.buffers[size_t(BufferRole::RelationWork)].initial;
    workBuffer.resize(work.size() * 4);
    std::memcpy(workBuffer.data(), work.data(), workBuffer.size());
    p.buffers[size_t(BufferRole::RelationWork)].words = work.size();
    std::ostringstream collect;
    collect << activity.str() << indexed.str() << rebuild << fallbackCollect.str()
            << "shared uint activityCount,activityBase;\nvoid collectActivity(){uint i=invocation();"
               "if(i>=step.count)return;if(rebuildCandidates()){for(uint k=i;k<"
            << logicalQueueCount
            << "u;k+=step.count){uint id;if(fallbackRow(k,id))considerFallback(id);}return;}";
    uint64_t ordinaryLanes = 0;
    uint32_t rowFirst = ordinaryFirst;
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        auto count = uint32_t(ordinaryByType[q.type].size());
        if (!count) continue;
        uint64_t width = (uint64_t(count) + 127) / 128 * 128;
        collect << "if(i>=" << ordinaryLanes << "u&&i<" << ordinaryLanes + width
                << "u){uint k=i-" << ordinaryLanes << "u;";
        if (count < 128) {
            collect << "if(k<" << count << "u)consider" << q.type << "(x_relationWork[" << rowFirst << "u+k]);";
        } else {
            // Padded type ranges own whole workgroups, including tail lanes.
            // All lanes reach these barriers; predicate early returns only exit
            // the called function. Reserve once per nonempty workgroup.
            collect << "if(gl_LocalInvocationIndex==0u)activityCount=0u;barrier();"
                       "uint id=0u,rank=0u;bool append=false;if(k<" << count
                    << "u){id=x_relationWork[" << rowFirst << "u+k];bool participates=activity" << q.type
                    << "(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration);append="
                    << (p.types[q.type]->rows == 1 && p.types[q.type]->kind != RelationKind::Equality ? "participates" : "true")
                    << ";}if(append)rank=atomicAdd(activityCount,1u);barrier();"
                       "if(activityCount!=0u){uint side=x_candidates[0];if(gl_LocalInvocationIndex==0u)"
                       "activityBase=atomicAdd(x_candidates[" << counters + slot << "u+side*" << typed.size()
                    << "u],activityCount);if(gl_LocalInvocationIndex==0u&&activityBase+activityCount>"
                    << q.capacity << "u)atomicOr(x_candidates[" << overflows
                    << "u+side],1u);barrier();if(append&&activityBase+rank<" << q.capacity
                    << "u)x_candidates[" << queues + q.first << "u+side*" << queueCapacity
                    << "u+activityBase+rank]=id;}";
        }
        collect << "}";
        ordinaryLanes += width;
        rowFirst += count;
    }
    if (ordinaryLanes > UINT32_MAX)
        throw std::overflow_error("Candidate activity dispatch exceeds 32-bit addressing");
    collect << "if(i>=step.count||step.iteration==0u)return;uint side=1u-x_candidates[0];";
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        if (std::none_of(domains.begin(), domains.end(), [&](const Domain& d) { return p.relations[d.set].type == q.type; }))
            continue;
        collect << "{uint count=x_candidates[" << counters + slot << "u+side*" << typed.size() << "u];"
                   "count=min(count," << q.capacity << "u);for(uint k=i;k<count;k+=step.count){"
                   "uint id=x_candidates[" << queues + q.first << "u+side*" << queueCapacity
                << "u+k];if(indexed(id)&&x_lambda[relation(id).l]!=0.0)consider" << q.type << "(id);}}";
    }
    collect << "}";
    if (firstQuery != UINT32_MAX) {
        p.kernels[firstQuery].source = collect.str() + firstQueryBody +
                                      "void main(){collectActivity();queryDomain();}";
        for (auto& batch : stages)
            if (batch.kernel == firstQuery)
                batch.count = std::max(batch.count, uint32_t(ordinaryLanes));
    } else {
        add("Collect Jacobi activity", collect.str() + "void main(){collectActivity();}", uint32_t(ordinaryLanes), stages);
    }
    std::vector<std::pair<uint32_t, KernelFunction>> solve, fallbackActivity;
    for (auto type : types) {
        const auto& t = *p.types[type];
        solve.emplace_back(type, relationFunction(t, p.typeReadOnly[type], p.typeEndpointMode[type], true,
                            t.rows == 1 && t.kind != RelationKind::Equality,
                            p.statistics.directJacobiRelations != 0, false, "candidateSolve" + std::to_string(type), active, true));
        fallbackActivity.emplace_back(
            type, relationActivityFunction(t, p.typeReadOnly[type], p.typeEndpointMode[type],
                                           "fallbackActivity" + std::to_string(type), false));
    }
    std::ostringstream source;
    source << stateAccess(false);
    for (const auto& item : solve) source << item.second.source;
    for (const auto& item : fallbackActivity) source << item.second.source;
    source << fallbackRows.str()
           << "void solveFallback(uint id){switch(relation(id).type){";
    for (auto type : types) {
        const auto& t = *p.types[type];
        source << "case " << type << "u:";
        if (t.rows == 1 && t.kind != RelationKind::Equality)
            source << "if(fallbackActivity" << type
                   << "(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration))"
                      "candidateSolve" << type
                   << "(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration);";
        else
            source << "fallbackActivity" << type
                   << "(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration);"
                      "candidateSolve" << type
                   << "(id,step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration);";
        source << "break;";
    }
    source << "}}\nvoid main(){uint lane=invocation(),side=x_candidates[0];"
              "if(lane==0u)x_candidates[1]=side;if(x_candidates["
           << overflows << "u+side]!=0u){if(lane<step.count)for(uint k=lane;k<"
           << logicalQueueCount
           << "u;k+=step.count){uint id;if(fallbackRow(k,id))solveFallback(id);}return;}";
    uint32_t laneFirst = 0;
    // Whole workgroups consume one mathematical type. Interleaving unlike active
    // rows would serialize branches and inflate the per-warp register footprint.
    for (uint32_t slot = 0; slot < typed.size(); ++slot) {
        const auto& q = typed[slot];
        source << "if(lane>=" << laneFirst << "u&&lane<" << laneFirst + q.lanes
               << "u){uint count=x_candidates[" << counters + slot << "u+side*" << typed.size()
               << "u];count=min(count," << q.capacity << "u);for(uint i=lane-" << laneFirst
               << "u;i<count;i+=" << q.lanes << "u)candidateSolve" << q.type
               << "(x_candidates[" << queues + q.first << "u+side*" << queueCapacity
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
