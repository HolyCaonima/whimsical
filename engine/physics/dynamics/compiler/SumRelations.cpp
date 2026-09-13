#include "SumRelations.h"
#include "FormulaGlsl.h"
#include "Schedule.h"
#include "CandidateIndex.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace whimsical::dynamics {
namespace {
uint32_t operands(MathOp op) {
    if (op == MathOp::Constant || op == MathOp::Input) return 0;
    if ((op >= MathOp::Negate && op <= MathOp::Abs) || op == MathOp::Sum) return 1;
    return op == MathOp::Select ? 3 : 2;
}
std::string u(uint32_t value) { return std::to_string(value) + "u"; }
void array(std::ostringstream& s, const std::string& name, uint32_t size) {
    s << "float " << name << "[" << std::max(1u, size) << "];\n";
}
uint32_t append(CompiledPlan& p, const std::vector<uint32_t>& words) {
    auto& b = p.buffers[size_t(BufferRole::SumData)];
    const auto first = b.initial.size() / 4;
    if (first + words.size() > UINT32_MAX) throw std::overflow_error("Sum metadata exceeds 32-bit addressing");
    b.initial.resize((first + words.size()) * 4);
    if (!words.empty()) std::memcpy(b.initial.data() + first * 4, words.data(), words.size() * 4);
    b.words = first + words.size();
    return uint32_t(first);
}
// The original binding domain also defines the extent of a sum. In particular,
// a triangular binding produces a partial sum, including empty boundary sums.
std::vector<std::pair<uint32_t, uint32_t>> anchors(const BindingDomain& d, uint32_t member) {
    const uint32_t n = d.count;
    switch (d.map) {
    case BindingDomain::Map::Directed: return {{0, member}, {std::min(n, member + 1), n}};
    case BindingDomain::Map::Upper:
        return d.summedObject == 1 ? std::vector<std::pair<uint32_t,uint32_t>>{{0, member}} :
                                                    std::vector<std::pair<uint32_t,uint32_t>>{{member + 1, n}};
    case BindingDomain::Map::UpperDiagonal:
        return d.summedObject == 1 ? std::vector<std::pair<uint32_t,uint32_t>>{{0, member + 1}} :
                                                    std::vector<std::pair<uint32_t,uint32_t>>{{member, n}};
    default: return {{0, n}};
    }
}
}

SumExpression splitSums(const Formula& f, const RelationType& type) {
    const auto axis = type.summedObject();
    uint32_t first = 0, end = 0;
    for (uint32_t slot = 0; slot < type.spaces.size(); ++slot) {
        const bool reduced = int32_t(slot < type.objects[0].size() ? 0 : 1) == axis;
        if (reduced) end += type.spaces[slot]->stateSize;
        else if (axis == 1) first += type.spaces[slot]->stateSize;
    }
    end += first;
    SumExpression result;
    result.terms.inputs = f.inputs;
    std::map<uint32_t, uint32_t> termIds;
    for (uint32_t i = 0; i < f.nodes.size(); ++i)
        if (f.nodes[i].op == MathOp::Sum) termIds.emplace(i, uint32_t(termIds.size()));
    auto copy = [&](Formula& target, bool outer) {
        std::vector<uint32_t> map(f.nodes.size(), UINT32_MAX);
        std::function<uint32_t(uint32_t)> visit = [&](uint32_t id) -> uint32_t {
            if (map[id] != UINT32_MAX) return map[id];
            auto node = f.nodes[id];
            if (node.op == MathOp::Sum) {
                if (!outer) throw std::invalid_argument("Nested collection sums are not supported");
                node = {MathOp::Input, f.inputs + termIds.at(id)};
            } else {
                if (outer && node.op == MathOp::Input && node.a >= first && node.a < end)
                    throw std::invalid_argument("A summed object may only be read inside its sum");
                const auto n = operands(node.op);
                if (n > 0) node.a = visit(node.a);
                if (n > 1) node.b = visit(node.b);
                if (n > 2) node.c = visit(node.c);
            }
            map[id] = uint32_t(target.nodes.size());
            target.nodes.push_back(node);
            return map[id];
        };
        if (outer) for (auto id : f.outputs) target.outputs.push_back(visit(id));
        else for (const auto& [id, index] : termIds) target.outputs.push_back(visit(f.nodes[id].a));
    };
    copy(result.terms, false);
    result.outer.inputs = f.inputs + uint32_t(termIds.size());
    copy(result.outer, true);
    return result;
}

namespace {
struct Generator {
    CompiledPlan& p;
    const BindingDomain& d;
    const RelationType& t;
    uint32_t set, axis, n, M, S = 1, T = 1, QA = 0, Q = 0;
    std::vector<uint32_t> input, projection, qoffset, fixed, varying;
    std::vector<bool> readOnly;
    uint32_t records = 0, recordCount = 0;
    std::string access;
    std::vector<std::pair<uint32_t, uint32_t>> degrees;
    std::optional<DifferenceBound> support;
    uint32_t header = 0, heads = 0, cells = 0, buckets = 0, lists = 0, memberRecords = 0, anchorFeatures = 0;
    static constexpr uint32_t Capacity = 128;
    static constexpr uint32_t LinearTeam = 32;
    uint32_t operatorTeam = 8;
    bool fastRecords = false;
    // Snapshot-local blocked sparse matrix. Row headers are compact columns;
    // member coefficients are columns within a row. Its transpose has a CSR
    // column for every writable variable and is reused by all linear sweeps.
    // Small transposed blocks fit a 16-byte record: linearization scatters the
    // whole block, and the transpose multiply consumes that same block.
    bool materialized = false;
    // A reduced scalar DAG and its consumers stay together until storage is
    // chosen. Equal injective index maps provide the reverse traversal without
    // assembling another incidence structure.
    std::optional<DifferenceFactor> factor;
    float factorScale = 0;
    uint32_t operatorRows = 0, gradients = 0;
    bool listedMembers = true, cachedGradients = false;
    uint32_t linearization = 0;
    uint32_t transpose = 0, cursors = 0, inverseEntries = 0, inverseVariables = 0;
    uint32_t variableCount = 0;
    uint32_t linearHeader() const { return 2 + M*M + 3*M; }
    uint32_t linearEntry() const { return 3 + M*T; }
    uint32_t linearColumns() const { return linearHeader() + (Capacity + uint32_t(fixed.size()))*linearEntry(); }
    std::string inverseAccess() const {
        const auto width = 1 + M*T;
        return "uint inverseWord(uint entry,uint column){return " + u(inverseEntries) +
            (width <= 4 ? "+entry*" + u(width) + "+column;" :
                "+column*" + u(d.count*(Capacity + uint32_t(fixed.size()))) + "+entry;") + "}\n";
    }
    std::vector<std::pair<std::string, uint32_t>> bounds, index, assembly;

    Generator(CompiledPlan& plan, uint32_t setId, uint32_t domain)
        : p(plan), d(p.bindings[setId].domains[domain]), t(*p.types[p.relations[setId].type]),
          set(setId), axis(uint32_t(d.summedObject)), n(t.inputSize()), M(t.rows),
          readOnly(p.bindings[setId].readOnly) {
        uint32_t at = 0;
        for (uint32_t slot = 0; slot < t.spaces.size(); ++slot) {
            input.push_back(at); at += t.spaces[slot]->stateSize;
            S = std::max(S, t.spaces[slot]->stateSize); T = std::max(T, t.spaces[slot]->tangentSize);
            (uint32_t(slot < d.split ? 0 : 1) == axis ? varying : fixed).push_back(slot);
        }
        qoffset.assign(t.spaces.size(), UINT32_MAX);
        for (const auto* slots : {&fixed, &varying}) {
            for (auto slot : *slots) if (!readOnly[slot]) {
                qoffset[slot] = Q;
                for (uint32_t c = 0; c < t.spaces[slot]->stateSize; ++c) projection.push_back(input[slot] + c);
                Q += t.spaces[slot]->stateSize;
            }
            if (slots == &fixed) QA = Q;
        }
        std::ostringstream s;
        for (uint32_t slot = 0; slot < d.fields.size(); ++slot) {
            const auto& field = d.fields[slot];
            s << "uint field" << slot << "(uint member){return ";
            if (field.kind == EndpointSource::Kind::Explicit) {
                std::vector<uint32_t> ids;
                for (const auto& ref : *field.references) ids.push_back(p.variables[ref.set].first + ref.index);
                s << "x_sumData[" << append(p, ids) << "u+member]";
            } else {
                s << p.variables[field.first.set].first + field.first.index << "u";
                if (!field.broadcast()) s << "+member*" << field.stride << "u";
            }
            s << ";}\n";
        }
        s << "bool accepts(uint anchor,uint member){return ";
        const std::string left = axis == 1 ? "anchor" : "member", right = axis == 1 ? "member" : "anchor";
        if (d.map == BindingDomain::Map::Directed) s << left << "!=" << right;
        else if (d.map == BindingDomain::Map::Upper) s << left << "<" << right;
        else if (d.map == BindingDomain::Map::UpperDiagonal) s << left << "<=" << right;
        else s << "true";
        s << ";}\nvoid loadInputs(Relation r,uint anchor,uint member,out float x[" << n << "]){";
        for (uint32_t slot = 0; slot < t.spaces.size(); ++slot) {
            const bool reduced = uint32_t(slot < d.split ? 0 : 1) == axis;
            // member == extent is the outer expression: no bound member is read.
            if (reduced) s << "if(member<" << extent() << "u){";
            s << "{Variable v=variable(field" << slot << "(" << (reduced ? "member" : "anchor") << "));";
            for (uint32_t c = 0; c < t.spaces[slot]->stateSize; ++c)
                s << "x[" << input[slot] + c << "]=loadValue(v," << c << "u);";
            s << "}";
            if (reduced) {
                s << "}else{";
                for (uint32_t c = 0; c < t.spaces[slot]->stateSize; ++c) s << "x[" << input[slot] + c << "]=0.0;";
                s << "}";
            }
        }
        for (uint32_t c = 0; c < t.parameters; ++c) s << "x[" << t.stateSize() + c << "]=loadParameter(r," << c << "u);";
        for (uint32_t c = 0; c < t.history; ++c) s << "x[" << t.stateSize() + t.parameters + c << "]=loadHistory(r," << c << "u);";
        s << "x[" << n - 2 << "]=step.h;x[" << n - 1 << "]=step.time;}\n";
        access = s.str();
        analyzeOperator();
        if (factor) {
            for (uint32_t member = 0; member < d.count; ++member) {
                const auto ref = d.fields[fixed[0]].at(member);
                degrees.emplace_back(p.variables[ref.set].first + ref.index, d.count);
            }
        } else prepareIncidence();
        prepareIndex();
        if (factor) {
            // Cached rows distribute consecutive entries across short teams;
            // streaming queries distribute the independent spatial cells.
            if (!listedMembers) operatorTeam = 32;
            operatorRows = append(p, std::vector<uint32_t>(size_t(d.count)*(6+T)));
            const uint64_t words = uint64_t((d.count+128/operatorTeam-1)/(128/operatorTeam))*128*Capacity/operatorTeam*T;
            cachedGradients = listedMembers && p.buffers[size_t(BufferRole::SumData)].wordCount() + words <= 24ull*1024*1024;
            if (cachedGradients) gradients = append(p, std::vector<uint32_t>(size_t(words)));
            return;
        }
        if (support && fastRecords && recordCount &&
            p.policy.weighting != JacobiWeighting::Static && p.policy.execution == ExecutionMode::Auto) {
            const auto R = uint32_t(splitSums(t.residual, t).terms.outputs.size());
            const uint64_t linearShared = Capacity * std::max(R*(1+QA), M*M) + std::max(1u,M*QA) + M*(QA+R) + M + 1;
            const uint64_t shared = std::max<uint64_t>({linearShared, 128ull*(M+1), 128ull*T});
            const uint64_t words = uint64_t(d.count) *
                (linearHeader() + uint64_t(Capacity + fixed.size()) * linearEntry());
            constexpr uint64_t WordBudget = 24u * 1024u * 1024u;
            variableCount = degrees.empty() ? 0 : degrees.back().first + 1;
            const uint64_t inverseWords = uint64_t(d.count)*(Capacity + fixed.size())*(1 + M*T) +
                3ull*(variableCount + 1) + degrees.size();
            if (shared <= 2048 && p.buffers[size_t(BufferRole::SumData)].wordCount() + words + inverseWords <= WordBudget) {
                linearization = append(p, std::vector<uint32_t>(size_t(words)));
                materialized = true;
                prepareTranspose();
            }
        }
    }
    uint32_t extent() const { return axis == 0 ? d.left : d.right; }
    void analyzeOperator();
    std::string operatorSource() const;
    std::string operatorLinearize() const;
    std::string operatorSolve(bool refine) const;
    std::string operatorGather() const;
    std::string operatorReduce(uint32_t columns, bool degree = false) const;
    std::string operatorMembers(const std::string& body) const;
    void prepareIncidence() {
        struct Entry { uint32_t member, slot; };
        std::map<uint32_t, std::vector<Entry>> entries;
        std::map<uint32_t, std::vector<std::pair<uint32_t,uint32_t>>> ranges;
        for (auto slot : varying) if (!readOnly[slot])
            for (uint32_t member = 0; member < extent(); ++member) {
                const auto ref = d.fields[slot].at(member);
                const auto id = p.variables[ref.set].first + ref.index;
                entries[id].push_back({member, slot});
                const auto intervals = anchors(d, member);
                ranges[id].insert(ranges[id].end(), intervals.begin(), intervals.end());
            }
        for (auto slot : fixed) if (!readOnly[slot])
            for (uint32_t anchor = 0; anchor < d.count; ++anchor) {
                const auto ref = d.fields[slot].at(anchor);
                ranges[p.variables[ref.set].first + ref.index].push_back({anchor, anchor + 1});
            }
        std::vector<uint32_t> table;
        std::vector<uint32_t> recordOfMember(extent());
        fastRecords = entries.empty() || entries.size() == extent();
        for (const auto& [id, refs] : entries) {
            fastRecords = fastRecords && refs.size() == 1;
            if (refs.size() == 1) recordOfMember[refs[0].member] = uint32_t(table.size() / 4);
            std::vector<uint32_t> values;
            for (const auto& ref : refs) { values.push_back(ref.member); values.push_back(ref.slot); }
            const auto offset = append(p, values);
            table.insert(table.end(), {id, offset, uint32_t(refs.size()), refs[0].slot});
        }
        records = append(p, table); recordCount = uint32_t(entries.size());
        if (fastRecords && recordCount) memberRecords = append(p, recordOfMember);
        for (auto& [id, intervals] : ranges) {
            std::sort(intervals.begin(), intervals.end());
            uint32_t count = 0, end = 0;
            for (auto [a, b] : intervals) if (b > end) { count += b - std::max(a, end); end = b; }
            if (count) degrees.push_back({id, count});
        }
    }
    std::string transforms() const;
    void prepareIndex();
    std::string gridSource() const;
    std::string generate(const Formula&, bool update, bool activity = false, bool refine = false) const;
    std::string solveAndProject() const;
    std::string linearize() const;
    std::string cachedSolve(bool refine) const;
    std::string cachedGather() const;
    void prepareTranspose();
    std::string linearAccess() const;
};

std::string Generator::linearAccess() const {
    // Subgroup lane numbering need not match local invocation numbering. With
    // 128 invocations, exactly four 32-lane subgroups proves all lanes are active
    // at entry. More subgroups can contain holes; use local IDs and shared memory.
    return "uint sumLocal(){\n#ifdef DYNAMICS_SUBGROUP32\n"
        "if(gl_NumSubgroups==4u)return gl_SubgroupID*32u+gl_SubgroupInvocationID;\n#endif\n"
        "return gl_LocalInvocationID.x;}\n"
        "uint sumInvocation(){return invocation()-gl_LocalInvocationID.x+sumLocal();}\n"
        "uint linearWord(uint anchor,uint column){return " + u(linearization) +
        "+(column<" + u(linearHeader()) + "?column*" + u(d.count) + "+anchor:" +
        u(linearHeader()*d.count) + "+anchor*" + u(linearColumns()-linearHeader()) +
        "+(column-" + u(linearHeader()) + ")%" + u(linearEntry()) + "*" +
        u(Capacity + uint32_t(fixed.size())) + "+(column-" + u(linearHeader()) + ")/" +
        u(linearEntry()) + ");}\n" + inverseAccess();
}

void Generator::analyzeOperator() {
    // Prove C_i = a * sum_j f(||q_i-q_j||^2, p_i) + b(p_i), with
    // identity tangent maps and reciprocal, injective endpoint indices. Then
    // g_ij = 2*a*f'*(q_i-q_j), J_ii = sum_j g_ij, J_ij = -g_ij.
    // The two matrix actions can be composed from these expressions without
    // assembling a second sparse orientation or storing weighted blocks.
    if (!p.policy.spatialCandidates || p.policy.execution != ExecutionMode::Auto ||
        p.policy.weighting == JacobiWeighting::Static || M != 1 || fixed.size() != 1 ||
        varying.size() != 1 || readOnly[fixed[0]] || readOnly[varying[0]] ||
        d.left != d.right || d.count != extent() || uint64_t(d.count)*extent() < 4096 ||
        (d.map != BindingDomain::Map::Product && d.map != BindingDomain::Map::Directed)) return;
    const auto& a = d.fields[fixed[0]];
    const auto& b = d.fields[varying[0]];
    if (a.kind != EndpointSource::Kind::Collection || b.kind != a.kind ||
        !(a.first == b.first) || a.stride != b.stride || !a.stride) return;
    const auto& space = *t.spaces[fixed[0]];
    if (S != T || T > 3 || t.spaces[varying[0]] != t.spaces[fixed[0]]) return;
    std::vector<uint32_t> tangent(T);
    std::iota(tangent.begin(), tangent.end(), S);
    const auto map = constantJacobian(space.retract, tangent);
    if (!map) return;
    for (uint32_t r=0;r<T;++r) for(uint32_t c=0;c<T;++c)
        if ((*map)[r*T+c] != float(r==c)) return;
    const auto program = splitSums(t.residual, t);
    if (program.terms.outputs.size() != 1) return;
    const auto bound = supportBound(program.terms, t.stateSize(), t.parameters);
    if (!bound || bound->coordinates.size() != T) return;
    std::vector<std::pair<uint32_t,uint32_t>> coordinates;
    for (uint32_t c=0;c<T;++c) coordinates.emplace_back(input[fixed[0]]+c,input[varying[0]]+c);
    std::vector<uint32_t> outerProjection(projection.begin(), projection.begin()+QA);
    outerProjection.push_back(n);
    const auto outer = constantJacobian(program.outer, outerProjection);
    if (!outer || !std::isfinite(outer->back()) || outer->back() == 0) return;
    for (uint32_t c=0;c<QA;++c) if ((*outer)[c]!=0) return;
    factor = factorDifferenceNorm(program.terms,t.stateSize(),coordinates);
    if (factor) factorScale = outer->back();
}

std::string Generator::operatorSource() const {
    std::ostringstream s;
    s << "#define REDUCTION_TEAM " << operatorTeam << "u\n";
    s << stateAccess(StateStorage::Global) << access << gridSource() << linearAccess();
    const auto& field = d.fields[fixed[0]];
    const auto& layout = p.variables[field.first.set];
    s << "Variable mappedVariable(uint member){uint i=" << field.first.index << "u+member*" << field.stride
      << "u;return Variable(" << layout.values << "u+i," << layout.velocity << "u+i," << layout.metric << "u+i,"
      << layout.count << "u,0u," << layout.modes << "u," << layout.first << "u+i);}\n";
    // Row columns: activity, scalar diagonal, residual RHS, snapshot multiplier,
    // multiplier increment, active degree, and the summed fixed gradient.
    s << "uint rowWord(uint row,uint column){return " << operatorRows << "u+column*" << d.count << "u+row;}\n";
    // Each 128-entry tile follows invocation order, including short teams from
    // different rows. A warp reads adjacent coefficients rather than row strides.
    s << "uint gradientWord(uint row,uint entry,uint column){return " << gradients
      << "u+(row/" << 128/operatorTeam << "u)*" << Capacity*128/operatorTeam*T
      << "u+(entry/REDUCTION_TEAM)*" << 128*T << "u+column*128u+(row%"
      << 128/operatorTeam << "u)*REDUCTION_TEAM+entry%REDUCTION_TEAM;}\n";
    s << "float rowValue(uint row,uint column){return uintBitsToFloat(x_sumData[rowWord(row,column)]);}\n"
         "void rowStore(uint row,uint column,float value){x_sumData[rowWord(row,column)]=floatBitsToUint(value);}\n";
    s << emitGlslDerivative(factor->scalar,"scalarTerm",{factor->argument});
    s << emitGlsl(splitSums(t.residual,t).outer,"outerValue");
    s << "void term(uint anchor,float distance2,out float value,out float coefficient){Relation r=relation(step.first+anchor);";
    array(s,"x",n+1);array(s,"y",1);array(s,"dy",1);
    for (uint32_t c=0;c<t.parameters;++c) s << "x[" << t.stateSize()+c << "]=loadParameter(r," << c << "u);";
    for (uint32_t c=0;c<t.history;++c) s << "x[" << t.stateSize()+t.parameters+c << "]=loadHistory(r," << c << "u);";
    s << "x[" << n-2 << "]=step.h;x[" << n-1 << "]=step.time;x[" << n << "]=distance2;scalarTerm(x,y,dy);"
         "value=y[0];coefficient=dy[0]*2.0*" << std::setprecision(9) << factorScale << ";}\n";
    s << "vec3 point(uint member){Variable v=mappedVariable(member);return vec3(";
    for (uint32_t c=0;c<3;++c) s << (c?",":"") << (c<T?"loadValue(v,"+u(c)+")":"0.0");
    s << ");}\nvec3 displacement(uint member){Variable v=mappedVariable(member);if(!variableEnabled(v))return vec3(0);return vec3(";
    for (uint32_t c=0;c<3;++c) s << (c?",":"") << (c<T?"uintBitsToFloat(x_contributions[v.v+"+u(c)+"*v.stride])":"0.0");
    s << ");}\nvec3 metric(uint member,vec3 value){Variable v=mappedVariable(member);if(!variableEnabled(v))return vec3(0);if(identityMetric(v))return value;return vec3(";
    for (uint32_t c=0;c<3;++c) {
        if(c)s << ",";
        if(c>=T){s << "0.0";continue;}
        for(uint32_t k=0;k<T;++k)s << (k?"+":"") << "x_metric[v.m+" << c*T+k << "u*v.stride]*value[" << k << "]";
    }
    s << ");}\nvec3 diagonal(uint anchor){return vec3(";
    for(uint32_t c=0;c<3;++c)s << (c?",":"") << (c<T?"rowValue(anchor,"+u(6+c)+")":"0.0");
    s << ");}\n";
    // Lists and direct traversal implement the same candidate relation. The
    // latter bounds scratch storage for large domains and overflowing rows.
    s << "struct MemberCursor{uint at,cell,member;ivec3 center;bool indexed;};\n"
         "MemberCursor members(uint anchor,uint lane){MemberCursor c;c.at=lane;c.indexed=x_sumData[" << header+2
      << "u]==0u;c.cell=lane;c.member=0xffffffffu;uint at=" << cells << "u+anchor*8u;"
         "c.center=ivec3(x_sumData[at],x_sumData[at+1u],x_sumData[at+2u]);return c;}\n"
         "bool nextMember(uint anchor,inout MemberCursor c,out uint member,out uint entry){entry=0xffffffffu;"
         "if(!c.indexed){while(c.at<" << d.count << "u){member=c.at;c.at+=REDUCTION_TEAM;if(accepts(anchor,member))return true;}return false;}"
         "for(;;){while(c.member!=0xffffffffu){member=c.member;uint at=" << cells
      << "u+member*8u;c.member=x_sumData[at+3u];uint code=c.cell-REDUCTION_TEAM;ivec3 offset=ivec3(0);";
    for (uint32_t c=0;c<T;++c) s << "offset[" << c << "]=int(code%3u)-1;code/=3u;";
    s << "if(all(equal(c.center+offset,ivec3(x_sumData[at],x_sumData[at+1u],x_sumData[at+2u])))"
         "&&accepts(anchor,member))return true;}if(c.cell>=" << (T==3?27:T==2?9:3) << "u)return false;"
         "uint code=c.cell;ivec3 offset=ivec3(0);";
    for (uint32_t c=0;c<T;++c) s << "offset[" << c << "]=int(code%3u)-1;code/=3u;";
    s << "c.member=x_sumData[" << heads << "u+bucket(c.center+offset)];c.cell+=REDUCTION_TEAM;}}\n";
    s << "vec3 gradient(uint anchor,uint member,uint entry,vec3 center){";
    if(cachedGradients) {
        s << "if(entry!=0xffffffffu){return vec3(";
        for(uint32_t c=0;c<3;++c)s << (c?",":"") << (c<T?"uintBitsToFloat(x_sumData[gradientWord(anchor,entry,"+u(c)+")])":"0.0");
        s << ");}";
    }
    s << "vec3 delta=center-point(member);float value,k;term(anchor,dot(delta,delta),value,k);return delta*k;}\n";
    return s.str();
}

std::string Generator::operatorMembers(const std::string& body) const {
    const auto direct = "MemberCursor cursor=members(anchor,lane);uint member,entry;"
        "while(nextMember(anchor,cursor,member,entry)){" + body + "}";
    if (!listedMembers) return direct;
    return "uint count=x_sumData[" + u(lists) + "+anchor*" + u(Capacity+1) + "];if(count<=" + u(Capacity) +
        "){for(uint entry=lane;entry<count;entry+=REDUCTION_TEAM){uint member=x_sumData[" + u(lists) +
        "+anchor*" + u(Capacity+1) + "+1u+entry];" + body + "}}else{" + direct + "}";
}

std::string Generator::operatorReduce(uint32_t columns, bool degree) const {
    std::ostringstream s;
    s << "\n#ifdef DYNAMICS_SUBGROUP32\nif(gl_NumSubgroups==4u){for(uint offset=REDUCTION_TEAM/2u;offset>0u;offset/=2u){";
    for(uint32_t c=0;c<columns;++c)s << "{float other=subgroupShuffleDown(total[" << c << "],offset);if(lane<offset)total[" << c << "]+=other;}";
    if(degree)s << "uint other=subgroupShuffleDown(degree,offset);if(lane<offset)degree=max(degree,other);";
    s << "}}else\n#endif\n{";
    for(uint32_t c=0;c<columns;++c)s << "partial[" << c*128 << "u+local]=total[" << c << "];";
    if(degree)s << "degreePartial[local]=degree;";
    s << "barrier();for(uint offset=REDUCTION_TEAM/2u;offset>0u;offset/=2u){if(lane<offset){";
    for(uint32_t c=0;c<columns;++c)s << "partial[" << c*128 << "u+local]+=partial[" << c*128 << "u+local+offset];";
    if(degree)s << "degreePartial[local]=max(degreePartial[local],degreePartial[local+offset]);";
    s << "}barrier();}";
    for(uint32_t c=0;c<columns;++c)s << "total[" << c << "]=partial[" << c*128 << "u+local];";
    if(degree)s << "degree=degreePartial[local];";
    s << "}";
    return s.str();
}

std::string Generator::operatorLinearize() const {
    std::ostringstream s;
    s << operatorSource() << "shared float partial[" << 128*(T+2) << "];shared uint live[128/REDUCTION_TEAM];"
         "void main(){uint local=sumLocal(),lane=local%REDUCTION_TEAM,team=local/REDUCTION_TEAM,anchor=sumInvocation()/REDUCTION_TEAM;"
         "bool valid=anchor<" << d.count << "u;float total[" << T+2 << "];";
    for(uint32_t c=0;c<T+2;++c)s << "total[" << c << "]=0.0;";
    std::ostringstream body;
    body << "vec3 delta=a-point(member);float value,k;term(anchor,dot(delta,delta),value,k);";
    body << "vec3 g=delta*k;total[0]+=value;";
    if(cachedGradients)for(uint32_t c=0;c<T;++c)body << "if(entry!=0xffffffffu)x_sumData[gradientWord(anchor,entry," << c << "u)]=floatBitsToUint(g[" << c << "]);";
    for(uint32_t c=0;c<T;++c)body << "total[" << 1+c << "]+=g[" << c << "];";
    body << "if(member!=anchor)total[" << T+1 << "]+=dot(g,metric(member,g));";
    s << "if(valid){vec3 a=point(anchor);" << operatorMembers(body.str()) << "}" << operatorReduce(T+2);
    s << "if(lane==0u){live[team]=0u;if(valid){Relation r=relation(step.first+anchor);"
         "if(step.iteration==0u)storeMultiplier(r,0u,0.0);rowStore(anchor,4u,0.0);rowStore(anchor,0u,0.0);";
    for(uint32_t c=0;c<T;++c)s << "rowStore(anchor," << 6+c << "u,total[" << c+1 << "]);";
    s << "if(relationEnabled(r)){";
    array(s,"x",n);array(s,"outerX",n+1);array(s,"c",1);
    s << "loadInputs(r,anchor," << extent() << "u,x);for(uint k=0u;k<" << n << "u;++k)outerX[k]=x[k];"
         "outerX[" << n << "]=total[0];outerValue(outerX,c);if(isnan(c[0])||isinf(c[0]))invalidEvaluation();else{";
    if(t.kind!=RelationKind::Equality)s << "if(!(c[0]" << (t.kind==RelationKind::GreaterEqual?">=":"<=") << "0.0&&loadMultiplier(r,0u)==0.0))";
    s << "{live[team]=1u;rowStore(anchor,0u,1.0);vec3 g=diagonal(anchor);"
         "float alpha=loadCompliance(r,0u," << 2+t.parameters << "u)/(step.h*step.h);"
         "rowStore(anchor,1u,total[" << T+1 << "]+dot(g,metric(anchor,g))+alpha);"
         "rowStore(anchor,2u,-c[0]-alpha*loadMultiplier(r,0u));rowStore(anchor,3u,loadMultiplier(r,0u));"
         "if(any(notEqual(g,vec3(0)))&&variableEnabled(mappedVariable(anchor)))"
         "atomicAdd(x_activeDegrees[mappedVariable(anchor).id],1u);}}}}}barrier();"
         "if(valid&&live[team]!=0u){vec3 a=point(anchor);";
    s << operatorMembers("if(member==anchor)continue;vec3 g=gradient(anchor,member,entry,a);"
         "if(any(notEqual(g,vec3(0)))&&variableEnabled(mappedVariable(member)))atomicAdd(x_activeDegrees[mappedVariable(member).id],1u);") << "}}";
    return s.str();
}

std::string Generator::operatorSolve(bool refine) const {
    std::ostringstream s;
    s << operatorSource() << "shared float partial[128];shared uint degreePartial[128];"
         "void main(){uint local=sumLocal(),lane=local%REDUCTION_TEAM,anchor=sumInvocation()/REDUCTION_TEAM;bool valid=anchor<"
      << d.count << "u;uint degree=1u;float total[1];total[0]=0.0;"
         "if(valid&&rowValue(anchor,0u)!=0.0){vec3 a=point(anchor);"
         "if(lane==0u){vec3 g=diagonal(anchor);total[0]+=dot(g,displacement(anchor));";
    if(!refine)s << "if(any(notEqual(g,vec3(0)))&&variableEnabled(mappedVariable(anchor)))degree=max(degree,x_activeDegrees[mappedVariable(anchor).id]);";
    s << "}";
    std::ostringstream body;
    body <<
         "if(member==anchor)continue;vec3 g=gradient(anchor,member,entry,a);"
         "total[0]-=dot(g,displacement(member));";
    if(!refine)body << "if(any(notEqual(g,vec3(0)))&&variableEnabled(mappedVariable(member)))degree=max(degree,x_activeDegrees[mappedVariable(member).id]);";
    s << operatorMembers(body.str()) << "}" << operatorReduce(1,!refine) << "if(lane!=0u||!valid)return;rowStore(anchor,4u,0.0);"
         "if(rowValue(anchor,0u)==0.0)return;";
    // Preserve the existing single-sweep behavior of overflowing cached rows.
    if (refine && listedMembers) s << "if(x_sumData[" << lists << "u+anchor*" << Capacity+1 << "u]>" << Capacity << "u)return;";
    if(refine)s << "degree=x_sumData[rowWord(anchor,5u)];";
    else s << "x_sumData[rowWord(anchor,5u)]=degree;";
    s << "Relation r=relation(step.first+anchor);float a[1],rhs[1],dl[1],nextLambda[1];"
         "a[0]=rowValue(anchor,1u);rhs[0]=rowValue(anchor,2u)-total[0]-loadCompliance(r,0u," << 2+t.parameters
      << "u)/(step.h*step.h)*(loadMultiplier(r,0u)-rowValue(anchor,3u));" << solveAndProject()
      << "rowStore(anchor,4u,dl[0]);}";
    return s.str();
}

std::string Generator::operatorGather() const {
    // Own the output variable: B_i*(J_ii*dl_i - sum_j g_ji*dl_j).
    // Reciprocal incidence follows from the maps, but coefficient reciprocity
    // also needs equal row parameters. Field modes prove that at execution;
    // otherwise evaluate the reverse row's parameters/history explicitly.
    std::ostringstream s;
    s << operatorSource() << "shared float partial[" << 128*T << "];void main(){uint local=sumLocal(),lane=local%REDUCTION_TEAM,"
         "anchor=sumInvocation()/REDUCTION_TEAM;bool valid=anchor<" << d.count << "u;float total[" << T << "];";
    for(uint32_t c=0;c<T;++c)s << "total[" << c << "]=0.0;";
    s << "if(valid){vec3 a=point(anchor),sum=vec3(0);float own=rowValue(anchor,4u);if(lane==0u&&own!=0.0)sum=diagonal(anchor)*own;"
         "Relation r=relation(step.first+anchor);";
    std::ostringstream body;
    body << "if(member==anchor)continue;float dl=rowValue(member,4u);if(dl==0.0)continue;"
         "vec3 g;";
    if(t.history==0)body << "if((x_fieldModes[r.u]&" << CompiledPlan::UniformParameters << "u)!=0u)g=gradient(anchor,member,entry,a);else";
    body << "{vec3 delta=a-point(member);float value,k;term(member,dot(delta,delta),value,k);g=delta*k;}sum+=g*dl;";
    s << operatorMembers(body.str());
    for(uint32_t c=0;c<T;++c)s << "total[" << c << "]=sum[" << c << "];";
    s << "}" << operatorReduce(T) << "if(lane==0u&&valid){vec3 sum=metric(anchor,vec3(";
    for(uint32_t c=0;c<3;++c)s << (c?",":"") << (c<T?"total["+std::to_string(c)+"]":"0.0");
    s << "));Variable v=mappedVariable(anchor);";
    for(uint32_t c=0;c<T;++c)s << "{uint at=v.v+" << c << "u*v.stride;x_contributions[at]=floatBitsToUint(uintBitsToFloat(x_contributions[at])+sum[" << c << "]);}";
    s << "}}";
    return s.str();
}

void Generator::prepareTranspose() {
    // Invert the candidate incidence, independently of numerical activity.
    // It lives as long as the certified candidate lists. Linearization writes
    // the two matrix orientations directly using the resulting entry map.
    std::vector<std::pair<uint32_t,uint32_t>> levels;
    for (uint32_t count = variableCount + 1;; count = (count + 127) / 128) {
        levels.push_back({append(p, std::vector<uint32_t>(count)), count});
        if (count == 1) break;
    }
    transpose = levels.front().first;
    cursors = append(p, std::vector<uint32_t>(variableCount));
    const uint32_t entryCapacity = d.count*(Capacity + uint32_t(fixed.size()));
    inverseEntries = append(p, std::vector<uint32_t>(size_t(entryCapacity)*(1 + M*T)));
    std::vector<uint32_t> ids;
    for (auto [id, count] : degrees) ids.push_back(id);
    inverseVariables = append(p, ids);
    const std::string rebuild = "if(x_sumData[" + u(header+3) + "]==0u)return;";
    index.push_back({"void main(){" + rebuild + "uint i=invocation();if(i<" + u(variableCount + 1) +
        ")x_sumData[" + u(transpose) + "+i]=0u;if(i<" + u(variableCount) +
        ")x_sumData[" + u(cursors) + "+i]=0u;}", variableCount + 1});
    const auto varyingSlot = *std::find_if(varying.begin(), varying.end(), [&](uint32_t slot) { return !readOnly[slot]; });
    std::ostringstream lookup;
    lookup << access << "uint entryVariable(uint anchor,uint k,uint count){if(k<count)return field"
           << varyingSlot << "(x_sumData[" << lists << "u+anchor*" << Capacity+1 << "u+1u+k]);switch(k-count){";
    for (uint32_t f=0;f<fixed.size();++f) if (!readOnly[fixed[f]])
        lookup << "case " << f << "u:return field" << fixed[f] << "(anchor);";
    lookup << "}return 0xffffffffu;}\n";
    const std::string loop = lookup.str() + "void main(){" + rebuild +
        "uint anchor=invocation()/32u,lane=gl_LocalInvocationID.x%32u;if(anchor>=" + u(d.count) +
        ")return;uint count=x_sumData[" + u(lists) + "+anchor*" + u(Capacity+1) + "];if(count>" +
        u(Capacity) + ")return;for(uint k=lane;k<count+" + u(uint32_t(fixed.size())) +
        ";k+=32u){uint id=entryVariable(anchor,k,count);if(id==0xffffffffu)continue;";
    // loadInputs in the field-map source uses the common state accessors.
    index.push_back({stateAccess(StateStorage::Global) + loop +
        "atomicAdd(x_sumData[" + u(transpose) + "+id],1u);}}", d.count*32});
    for (size_t level = 0; level + 1 < levels.size(); ++level) {
        const auto [first, count] = levels[level];
        std::ostringstream s;
        s << "shared uint partial[128];void main(){" << rebuild << "uint i=invocation(),lane=gl_LocalInvocationID.x;"
             "uint value=i<" << count << "u?x_sumData[" << first << "u+i]:0u;partial[lane]=value;barrier();"
             "for(uint offset=1u;offset<128u;offset*=2u){uint add=lane>=offset?partial[lane-offset]:0u;"
             "barrier();partial[lane]+=add;barrier();}if(i<" << count << "u)x_sumData[" << first
          << "u+i]=partial[lane]-value;if(lane==127u&&i-lane<" << count << "u)x_sumData["
          << levels[level+1].first << "u+i/128u]=partial[lane];}";
        assembly.push_back({s.str(), count});
    }
    for (int level = int(levels.size()) - 3; level >= 0; --level) {
        const auto [first, count] = levels[level];
        assembly.push_back({"void main(){" + rebuild + "uint i=invocation();if(i<" + u(count) + ")x_sumData[" +
            u(first) + "+i]+=x_sumData[" + u(levels[level+1].first) + "+i/128u];}", count});
    }
    assembly.push_back({stateAccess(StateStorage::Global) + linearAccess() + loop +
        "uint at=x_sumData[" + u(transpose) + "+id]+atomicAdd(x_sumData[" + u(cursors) +
        "+id],1u);x_sumData[inverseWord(at,0u)]=anchor;x_sumData[linearWord(anchor," +
        u(linearHeader()+2+M*T) + "+k*" + u(linearEntry()) + ")]=at;}}", d.count*32});
}

std::string Generator::gridSource() const {
    std::ostringstream s;
    s << candidateHash(buckets);
    // Queries chase member IDs in arbitrary order. Keep each member's key,
    // link and projected feature in one cache line, independently of user SoA.
    s << "void insertMember(uint member,ivec3 c,vec3 f){uint at=" << cells << "u+member*8u;"
         "x_sumData[at]=uint(c.x);x_sumData[at+1u]=uint(c.y);x_sumData[at+2u]=uint(c.z);"
         "x_sumData[at+3u]=atomicExchange(x_sumData[" << heads << "u+bucket(c)],member);"
         "x_sumData[at+4u]=floatBitsToUint(f.x);x_sumData[at+5u]=floatBitsToUint(f.y);"
         "x_sumData[at+6u]=floatBitsToUint(f.z);}\n";
    s << "vec3 feature(uint member,bool reduced){return reduced?vec3(";
    for (uint32_t side = 0; side < 2; ++side) {
        if (side) s << "):vec3(";
        for (uint32_t c = 0; c < 3; ++c) {
            if (c) s << ",";
            if (c >= support->coordinates.size()) { s << "0.0"; continue; }
            auto column = side ? support->coordinates[c].first : support->coordinates[c].second;
            uint32_t slot = 0;
            while (column >= t.spaces[slot]->stateSize) column -= t.spaces[slot++]->stateSize;
            s << "loadValue(variable(field" << slot << "(member))," << column << "u)";
        }
    }
    s << ");}\nbool cellOf(vec3 f,out ivec3 c){vec3 v=f/(1.0502*sqrt(uintBitsToFloat(x_sumData["
      << header << "u])));if(any(isnan(v))||any(isinf(v))||any(greaterThan(abs(v),vec3(262144.0)))"
         "||any(greaterThan(abs(f),vec3(1e15))))return false;c=ivec3(floor(v));return true;}\n"
         "uint memberCount(uint anchor){uint count=x_sumData[" << lists << "u+anchor*" << Capacity+1
      << "u];return count>" << Capacity << "u?" << extent() << "u:count;}\n"
         "uint memberAt(uint anchor,uint k){uint at=" << lists << "u+anchor*" << Capacity+1
      << "u;return x_sumData[at]>" << Capacity << "u?k:x_sumData[at+1u+k];}\n";
    return s.str();
}

void Generator::prepareIndex() {
    if (!p.policy.spatialCandidates || uint64_t(d.count)*extent() < 4096) return;
    const auto terms = splitSums(t.residual, t).terms;
    support = supportBound(terms, t.stateSize(), t.parameters);
    if (!support || support->coordinates.size() > 3) { support.reset(); return; }
    uint32_t splitInput = 0;
    for (uint32_t slot = 0; slot < d.split; ++slot) splitInput += t.spaces[slot]->stateSize;
    for (auto& [a, b] : support->coordinates) {
        if (a >= splitInput && b < splitInput) std::swap(a, b);
        if (a >= splitInput || b < splitInput || b >= t.stateSize()) { support.reset(); return; }
        if (axis == 0) std::swap(a, b);
    }
    header = append(p, std::vector<uint32_t>(5));
    buckets = 1;
    while (uint64_t(buckets) < uint64_t(extent())*2) {
        if (buckets >= (1u<<30)) throw std::overflow_error("Sum index bucket capacity");
        buckets *= 2;
    }
    heads = append(p, std::vector<uint32_t>(buckets));
    cells = append(p, std::vector<uint32_t>(size_t(extent())*8));
    if (!factor) anchorFeatures = append(p, std::vector<uint32_t>(size_t(d.count)*3));
    // Storage is a lowering decision. Large domains retain the spatial index
    // and stream members instead of reserving Capacity entries for every row.
    listedMembers = !factor || p.buffers[size_t(BufferRole::SumData)].wordCount() +
        uint64_t(d.count)*(Capacity+1) <= 24ull*1024*1024;
    if (listedMembers) lists = append(p, std::vector<uint32_t>(size_t(d.count)*(Capacity+1)));
    const auto source = stateAccess(StateStorage::Global) + access + gridSource();
    bounds.push_back({"void main(){if(invocation()==0u){x_sumData["+u(header)+"]=0u;x_sumData["+
        u(header+1)+"]=0u;}}", 1});
    std::ostringstream s;
    s << stateAccess(StateStorage::Global) << emitGlsl(support->squaredRadius,"boundValue")
      << "void main(){uint i=invocation();if(i>=" << d.count << "u)return;Relation r=relation("
      << p.relations[set].first+d.first << "u+i);float x[" << n << "],value[1];";
    for (uint32_t k = 0; k < t.parameters; ++k) s << "x[" << t.stateSize()+k << "]=loadParameter(r," << k << "u);";
    s << "boundValue(x,value);if(isnan(value[0])||isinf(value[0])||value[0]<1e-20||value[0]>1e20)"
         "atomicOr(x_sumData[" << header+1 << "u],1u);else atomicMax(x_sumData[" << header
      << "u],floatBitsToUint(value[0]));}";
    bounds.push_back({s.str(),d.count}); s.str(""); s.clear();
    // A 5% enlarged support is reusable while both endpoint projections move
    // at most 2.4% of the original radius. The triangle inequality leaves a
    // rounding margin. This certifies candidates only; values and AD are fresh.
    s << "void main(){if(invocation()==0u)x_sumData[" << header+3 << "u]=uint(x_sumData["
      << header+2 << "u]!=0u||x_sumData[" << header+1 << "u]!=0u||x_sumData[" << header+4
      << "u]!=x_sumData[" << header << "u]);}";
    index.push_back({s.str(),1}); s.str(""); s.clear();
    s << source << "bool moved(vec3 f,uint at){vec3 previous=vec3(uintBitsToFloat(x_sumData[at]),"
         "uintBitsToFloat(x_sumData[at+1u]),uintBitsToFloat(x_sumData[at+2u]));vec3 delta=f-previous;"
         "return !(dot(delta,delta)<=0.000576*uintBitsToFloat(x_sumData[" << header << "u]));}"
         "void main(){uint i=invocation();if(x_sumData[" << header+2 << "u]!=0u||x_sumData["
      << header+1 << "u]!=0u||x_sumData[" << header+4 << "u]!=x_sumData[" << header
      << "u])return;bool changed=false;if(i<" << extent() << "u)changed=moved(feature(i,true),"
      << cells+4 << "u+i*8u);";
    if (!factor) s << "if(i<" << d.count << "u)changed=changed||moved(feature(i,false),"
      << anchorFeatures << "u+i*3u);";
    s << "if(changed)atomicOr(x_sumData[" << header+3 << "u],1u);}";
    index.push_back({s.str(),std::max(d.count,extent())}); s.str(""); s.clear();
    s << "void main(){if(x_sumData[" << header+3 << "u]==0u)return;uint i=invocation();if(i<"
      << buckets << "u)x_sumData[" << heads << "u+i]=0xffffffffu;if(i==0u){x_sumData["
      << header+2 << "u]=x_sumData[" << header+1 << "u];x_sumData[" << header+4 << "u]=x_sumData[" << header << "u];}}";
    index.push_back({s.str(),buckets}); s.str(""); s.clear();
    s << source << "void main(){if(x_sumData[" << header+3 << "u]==0u)return;uint member=invocation();if(member>=" << extent()
      << "u||x_sumData[" << header+1 << "u]!=0u)return;vec3 f=feature(member,true);ivec3 c;if(!cellOf(f,c)){"
         "atomicOr(x_sumData[" << header+2 << "u],1u);return;}insertMember(member,c,f);}";
    index.push_back({s.str(),extent()}); s.str(""); s.clear();
    ++p.statistics.candidateDomains;
    p.statistics.candidateRelations += uint64_t(d.count)*extent();
    if (!listedMembers) return;
    // Independent cells of one query need not be walked serially by one lane.
    // Four query teams share a workgroup; barriers never depend on row validity.
    constexpr uint32_t Team = 32, Teams = 128 / Team;
    uint32_t cellCount = 1;
    for (size_t c = 0; c < support->coordinates.size(); ++c) cellCount *= 3;
    s << source << "shared uint queryCount[" << Teams << "],queryMembers[" << Teams*Capacity
      << "];void main(){if(x_sumData[" << header+3 << "u]==0u)return;uint anchor=invocation()/" << Team << "u,lane=gl_LocalInvocationID.x%"
      << Team << "u,team=gl_LocalInvocationID.x/" << Team << "u;bool valid=anchor<" << d.count
      << "u;vec3 f=vec3(0);ivec3 center=ivec3(0);bool indexed=false;"
         "if(valid){f=feature(anchor,false);indexed=x_sumData[" << header+2
      << "u]==0u&&cellOf(f,center);}if(lane==0u)queryCount[team]=0u;barrier();"
         "if(indexed&&lane<" << cellCount << "u){uint code=lane;ivec3 offset=ivec3(0);";
    for (uint32_t c = 0; c < support->coordinates.size(); ++c)
        s << "offset[" << c << "]=int(code%3u)-1;code/=3u;";
    s << "ivec3 cell=center+offset;uint member=x_sumData[" << heads
      << "u+bucket(cell)];while(member!=0xffffffffu){uint p=" << cells
      << "u+member*8u;if(all(equal(cell,ivec3(x_sumData[p],x_sumData[p+1u],x_sumData[p+2u])))"
         "&&accepts(anchor,member)){vec3 delta=f-vec3(uintBitsToFloat(x_sumData[p+4u]),"
         "uintBitsToFloat(x_sumData[p+5u]),uintBitsToFloat(x_sumData[p+6u]));"
         "if(dot(delta,delta)<=1.1029*uintBitsToFloat(x_sumData["
      << header << "u])){uint entry=atomicAdd(queryCount[team],1u);if(entry<" << Capacity
      << "u)queryMembers[team*" << Capacity << "u+entry]=member;else break;}}member=x_sumData[p+3u];"
         "}}barrier();if(valid){uint count=queryCount[team],at=" << lists
      << "u+anchor*" << Capacity+1 << "u;if(lane==0u){x_sumData[at]=indexed?count:0xffffffffu;";
    if (!factor) s << "uint snapshot=" << anchorFeatures << "u+anchor*3u;x_sumData[snapshot]=floatBitsToUint(f.x);"
         "x_sumData[snapshot+1u]=floatBitsToUint(f.y);x_sumData[snapshot+2u]=floatBitsToUint(f.z);";
    s << "}if(indexed&&count<=" << Capacity << "u)for(uint k=lane;k<count;k+=" << Team
      << "u)x_sumData[at+1u+k]=queryMembers[team*" << Capacity << "u+k];}}";
    if (uint64_t(d.count)*Team > UINT32_MAX) throw std::overflow_error("Sum query dispatch capacity");
    index.push_back({s.str(),d.count*Team});
    p.statistics.candidateQueueCapacity += uint64_t(d.count)*Capacity;
}

std::string Generator::transforms() const {
    std::ostringstream s;
    for (uint32_t slot = 0; slot < t.spaces.size(); ++slot) if (!readOnly[slot]) {
        const auto& space = *t.spaces[slot];
        std::vector<uint32_t> columns(space.tangentSize);
        std::iota(columns.begin(), columns.end(), space.stateSize);
        s << emitGlslJacobian(space.retract, "tangent" + std::to_string(slot), columns);
    }
    s << "void transform(uint slot,Variable v,float g[" << M*S << "],out float j[" << M*T
      << "],out float w[" << M*T << "]){";
    s << "for(uint k=0u;k<" << M*T << "u;++k){j[k]=0.0;w[k]=0.0;}"
         "if(v.flags!=0u||!variableEnabled(v))return;switch(slot){";
    for (uint32_t slot = 0; slot < t.spaces.size(); ++slot) if (!readOnly[slot]) {
        const auto& space = *t.spaces[slot];
        s << "case " << slot << "u:{";
        array(s, "x", space.stateSize + space.tangentSize); array(s, "a", space.stateSize * space.tangentSize);
        for (uint32_t c=0;c<space.stateSize;++c) s << "x[" << c << "]=loadValue(v," << c << "u);";
        for (uint32_t c=0;c<space.tangentSize;++c) s << "x[" << space.stateSize+c << "]=0.0;";
        s << "tangent" << slot << "(x,a);";
        for (uint32_t row=0;row<M;++row) for(uint32_t c=0;c<space.tangentSize;++c) {
            s << "j[" << row*T+c << "]=0.0";
            for(uint32_t k=0;k<space.stateSize;++k) s << "+g[" << row*S+k << "]*a[" << k*space.tangentSize+c << "]";
            s << ";";
        }
        for (uint32_t row=0;row<M;++row) for(uint32_t c=0;c<space.tangentSize;++c) {
            s << "w[" << row*T+c << "]=identityMetric(v)?j[" << row*T+c << "]:0.0";
            for(uint32_t k=0;k<space.tangentSize;++k)
                s << "+x_metric[v.m+" << c*space.tangentSize+k << "u*v.stride]*j[" << row*T+k << "]";
            s << ";";
        }
        s << "break;}";
    }
    s << "}}\nvoid accumulate(float j[" << M*T << "],float w[" << M*T << "],inout float a[" << M*M << "]){";
    for(uint32_t row=0;row<M;++row) for(uint32_t col=0;col<M;++col) {
        s << "a[" << row*M+col << "]+=";
        for(uint32_t k=0;k<T;++k) s << (k?"+":"") << "j[" << row*T+k << "]*w[" << col*T+k << "]";
        s << ";";
    }
    s << "}\nvoid scatter(uint slot,Variable v,float w[" << M*T << "],float dl[" << M << "]){"
         "if(v.flags!=0u||!variableEnabled(v))return;uint width=0u;switch(slot){";
    for(uint32_t slot=0;slot<t.spaces.size();++slot)
        s << "case " << slot << "u:width=" << t.spaces[slot]->tangentSize << "u;break;";
    s << "}for(uint k=0u;k<width;++k){float value=0.0;";
    for(uint32_t row=0;row<M;++row) s << "value+=w[" << row*T << "u+k]*dl[" << row << "];";
    s << "addContribution(v.v+k*v.stride,value);}}\n";
    return s.str();
}

std::string Generator::generate(const Formula& formula, bool update, bool activity, bool refine) const {
    const auto program = splitSums(formula, t);
    const auto R = uint32_t(program.terms.outputs.size()), O = QA + R, outputs = uint32_t(formula.outputs.size());
    std::vector<uint32_t> outerProjection(projection.begin(), projection.begin()+QA);
    for(uint32_t k=0;k<R;++k) outerProjection.push_back(n+k);
    std::ostringstream s;
    s << stateAccess(StateStorage::Global) << access;
    const bool indexed = support && !update;
    const bool active = p.policy.weighting != JacobiWeighting::Static;
    if (indexed) s << gridSource();
    const bool cache = materialized && !update;
    if (cache) {
        s << linearAccess() << "bool canMaterialize(uint anchor){return x_sumData[" << lists << "u+anchor*"
          << Capacity+1 << "u]<=" << Capacity << "u;}\n"
             "void cacheEntry(uint anchor,uint k,uint id,uint slot,float j[" << M*T << "],float w[" << M*T
          << "]){uint column=" << linearHeader() << "u+k*" << linearEntry()
          << "u;x_sumData[linearWord(anchor,column)]=id;x_sumData[linearWord(anchor,column+1u)]=slot;";
        s << "uint at=x_sumData[linearWord(anchor,column+" << 2+M*T << "u)];";
        for (uint32_t k=0;k<M*T;++k) {
            s << "x_sumData[linearWord(anchor,column+" << k+2 << "u)]=floatBitsToUint(j[" << k << "]);";
            s << "x_sumData[inverseWord(at," << 1+k << "u)]=floatBitsToUint(w[" << k << "]);";
        }
        s << "}\nvoid clearEntry(uint anchor,uint k){uint column=" << linearHeader() << "u+k*" << linearEntry()
          << "u;x_sumData[linearWord(anchor,column)]=0xffffffffu;uint at=x_sumData[linearWord(anchor,column+" << 2+M*T << "u)];";
        for (uint32_t k=0;k<M*T;++k) s << "x_sumData[inverseWord(at," << 1+k << "u)]=0u;";
        s << "}\n";
    }
    if(R) s << (update ? emitGlsl(program.terms,"terms") : emitGlslDerivative(program.terms,"terms",projection));
    s << (update ? emitGlsl(program.outer,"outerValue") : emitGlslDerivative(program.outer,"outerValue",outerProjection));
    if(!update) {
        s << transforms();
        s << "bool influences(float j[" << M*T << "]){for(uint k=0u;k<" << M*T
          << "u;++k)if(j[k]!=0.0)return true;return false;}\n";
        s << "bool memberGradient(uint at,Relation r,uint anchor,float outerJ[" << std::max(1u,M*O)
          << "],out float g[" << M*S << "]){bool included=false;for(uint k=0u;k<" << M*S << "u;++k)g[k]=0.0;";
        if(R) {
            array(s,"x",n);array(s,"v",R);array(s,"d",R*Q);
            s << "uint first=x_sumData[at+1u],count=x_sumData[at+2u];for(uint k=0u;k<count;++k){"
                 "uint member=x_sumData[first+2u*k],slot=x_sumData[first+2u*k+1u];if(!accepts(anchor,member))continue;included=true;"
                 "loadInputs(r,anchor,member,x);terms(x,v,d);switch(slot){";
            for(auto slot:varying) if(!readOnly[slot]) {
                s << "case " << slot << "u:";
                for(uint32_t row=0;row<M;++row) for(uint32_t c=0;c<t.spaces[slot]->stateSize;++c)
                    for(uint32_t k=0;k<R;++k)
                        s << "g[" << row*S+c << "]+=outerJ[" << row*O+QA+k << "]*d[" << k*Q+qoffset[slot]+c << "];";
                s << "break;";
            }
            s << "}}";
        }
        s << "return included;}\n";
    }
    if (cache) s << "void " << (activity ? "streamedActivity" : "streamedSolve") << "(uint anchor){";
    else s << "void main(){uint anchor=invocation();if(anchor>=step.count)return;";
    s << "Relation r=relation(step.first+anchor);";
    if(!update) for(uint32_t row=0;row<M;++row) s << "if(step.iteration==0u)storeMultiplier(r," << row << "u,0.0);";
    s << "if(!relationEnabled(r))return;";
    array(s,"x",n);array(s,"outerX",n+R);array(s,"totals",R);array(s,"termValue",R);
    if(!update) {array(s,"termJ",R*Q);array(s,"totalJ",R*QA);}
    for(uint32_t k=0;k<R;++k) s << "totals[" << k << "]=0.0;";
    if(!update) for(uint32_t k=0;k<R*QA;++k) s << "totalJ[" << k << "]=0.0;";
    if(R) {
        s << "for(uint memberIndex=0u;memberIndex<" << (indexed?"memberCount(anchor)":u(extent()))
          << ";++memberIndex){uint member=" << (indexed?"memberAt(anchor,memberIndex)":"memberIndex")
          << ";if(!accepts(anchor,member))continue;loadInputs(r,anchor,member,x);terms(x,termValue"
          << (update?"":",termJ") << ");";
        for(uint32_t k=0;k<R;++k) {
            s << "totals[" << k << "]+=termValue[" << k << "];";
            if(!update) for(uint32_t c=0;c<QA;++c) s << "totalJ[" << k*QA+c << "]+=termJ[" << k*Q+c << "];";
        }
        s << "}";
    }
    s << "loadInputs(r,anchor," << extent() << "u,x);for(uint k=0u;k<" << n << "u;++k)outerX[k]=x[k];";
    for(uint32_t k=0;k<R;++k) s << "outerX[" << n+k << "]=totals[" << k << "];";
    array(s,"c",outputs);
    if(!update) array(s,"outerJ",M*O);
    s << "outerValue(outerX,c" << (update?"":",outerJ") << ");"
         "for(uint k=0u;k<" << outputs << "u;++k)if(isnan(c[k])||isinf(c[k])){invalidEvaluation();return;}";
    if(update) {
        for(uint32_t k=0;k<outputs;++k) s << "storeHistory(r," << k << "u,c[" << k << "]);";
        s << "}";return s.str();
    }
    if(M==1 && t.kind!=RelationKind::Equality)
        s << "if(c[0]" << (t.kind==RelationKind::GreaterEqual?">=":"<=") << "0.0&&loadMultiplier(r,0u)==0.0)return;";
    array(s,"fixedJ",M*QA);array(s,"g",M*S);array(s,"j",M*T);array(s,"w",M*T);
    array(s,"a",M*M);array(s,"rhs",M);array(s,"dl",M);array(s,"nextLambda",M);
    s << "uint degree=1u;";
    for(auto slot:fixed) if(!readOnly[slot]) {
        s << "uint id" << slot << "=field" << slot << "(anchor);";
        if (!active) s << "degree=max(degree,x_adjCursors[id" << slot << "]);";
        for(uint32_t row=0;row<M;++row) for(uint32_t c=0;c<t.spaces[slot]->stateSize;++c) {
            const auto col=qoffset[slot]+c;
            s << "fixedJ[" << row*QA+col << "]=outerJ[" << row*O+col << "]";
            for(uint32_t k=0;k<R;++k) s << "+outerJ[" << row*O+QA+k << "]*totalJ[" << k*QA+col << "]";
            s << ";";
        }
    }
    // Merge repeated fixed fields before forming J M^-1 J^T.
    for(auto slot:fixed) if(!readOnly[slot]) for(auto before:fixed) {
        if(before>=slot) break;
        if(readOnly[before]||t.spaces[before]->stateSize!=t.spaces[slot]->stateSize) continue;
        s << "if(id" << slot << "==id" << before << "){";
        for(uint32_t row=0;row<M;++row) for(uint32_t c=0;c<t.spaces[slot]->stateSize;++c)
            s << "fixedJ[" << row*QA+qoffset[before]+c << "]+=fixedJ[" << row*QA+qoffset[slot]+c << "];fixedJ[" << row*QA+qoffset[slot]+c << "]=0.0;";
        s << "}";
    }
    for(uint32_t row=0;row<M;++row) {
        s << "float alpha" << row << "=loadCompliance(r," << row << "u," << 2+t.parameters << "u)/(step.h*step.h);"
             "rhs[" << row << "]=-c[" << row << "]-alpha" << row << "*loadMultiplier(r," << row << "u);";
        for(uint32_t col=0;col<M;++col) s << "a[" << row*M+col << "]=" << (row==col?"alpha"+std::to_string(row):"0.0") << ";";
    }
    auto memberLoop = [&](bool scatter) {
        const bool compact = indexed && fastRecords && recordCount;
        s << "for(uint k=0u;k<" << (compact?"memberCount(anchor)":u(recordCount)) << ";++k){uint record=";
        if (compact) s << "x_sumData[" << memberRecords << "u+memberAt(anchor,k)]";
        else s << "k";
        s << ";uint at=" << records << "u+record*4u,id=x_sumData[at],slot=x_sumData[at+3u];"
             "if(!memberGradient(at,r,anchor,outerJ,g))continue;";
        if (!active) s << "degree=max(degree,x_adjCursors[id]);";
        bool first=true;
        for(auto fixedSlot:fixed) if(!readOnly[fixedSlot]) {
            s << (first?"if(":"else if(") << "id==id" << fixedSlot << "){"; first=false;
            if(!scatter) for(uint32_t row=0;row<M;++row) for(uint32_t c=0;c<t.spaces[fixedSlot]->stateSize;++c)
                s << "fixedJ[" << row*QA+qoffset[fixedSlot]+c << "]+=g[" << row*S+c << "];";
            s << "}";
        }
        if(!first) s << "else";
        s << "{Variable v=variable(id);transform(slot,v,g,j,w);";
        if (activity) s << "if(influences(j))atomicAdd(x_activeDegrees[id],1u);";
        else {
            if (active && !scatter) s << "if(influences(j))degree=max(degree,x_activeDegrees[id]);";
            s << (scatter?"scatter(slot,v,w,dl);":"accumulate(j,w,a);");
        }
        s << "}}";
    };
    auto fixedLoop = [&](bool scatter) {
        for(auto slot:fixed) if(!readOnly[slot]) {
            s << "if(true";
            for(auto before:fixed) if(before<slot&&!readOnly[before]) s << "&&id" << slot << "!=id" << before;
            s << "){for(uint k=0u;k<" << M*S << "u;++k)g[k]=0.0;";
            for(uint32_t row=0;row<M;++row) for(uint32_t c=0;c<t.spaces[slot]->stateSize;++c)
                s << "g[" << row*S+c << "]=fixedJ[" << row*QA+qoffset[slot]+c << "];";
            s << "Variable v=variable(id" << slot << ");transform(" << slot << "u,v,g,j,w);";
            if (activity) s << "if(influences(j))atomicAdd(x_activeDegrees[id" << slot << "],1u);";
            else {
                if (active && !scatter) s << "if(influences(j))degree=max(degree,x_activeDegrees[id" << slot << "]);";
                s << (scatter?"scatter("+u(slot)+",v,w,dl);":"accumulate(j,w,a);");
            }
            s << "}";
        }
    };
    memberLoop(false);fixedLoop(false);
    if (activity) { s << "}"; if (cache) s << linearize(); return s.str(); }
    s << solveAndProject();
    memberLoop(true);fixedLoop(true);s << "}";
    if (cache) s << cachedSolve(refine);
    return s.str();
}

std::string Generator::solveAndProject() const {
    std::ostringstream s;
    s << solveBlock(M);
    for(uint32_t row=0;row<M;++row) {
        std::string candidate="loadMultiplier(r,"+u(row)+")+rhs["+std::to_string(row)+"]*step.relaxation/float(degree)";
        if(t.kind==RelationKind::GreaterEqual) candidate="max(0.0,"+candidate+")";
        if(t.kind==RelationKind::LessEqual) candidate="min(0.0,"+candidate+")";
        s << "nextLambda[" << row << "]=" << candidate << ";dl[" << row << "]=nextLambda[" << row << "]-loadMultiplier(r," << row << "u);";
    }
    s << "for(uint k=0u;k<" << M << "u;++k)if(isnan(dl[k])||isinf(dl[k])||isnan(nextLambda[k])||isinf(nextLambda[k])){invalidEvaluation();return;}";
    for(uint32_t row=0;row<M;++row) s << "storeMultiplier(r," << row << "u,nextLambda[" << row << "]);";
    return s.str();
}

std::string Generator::linearize() const {
    const auto R = uint32_t(splitSums(t.residual, t).terms.outputs.size()), O = QA + R;
    const auto varyingSlot = *std::find_if(varying.begin(), varying.end(), [&](uint32_t slot) { return !readOnly[slot]; });
    const auto width = std::max(R*(1+QA), M*M);
    std::ostringstream s;
    // The injective member map proves there is one writer for each alias of a
    // fixed field. Keep integrand derivatives in registers across both reductions.
    s << "\nshared float partial[" << Capacity*width << "],fixedJ[" << std::max(1u,M*QA)
      << "],outerJ[" << M*O << "],residual[" << M << "];shared uint live;\n"
         "void main(){uint anchor=invocation()/" << Capacity << "u,lane=sumLocal();"
         "if(anchor>=" << d.count << "u)return;if(!canMaterialize(anchor)){"
         "if(lane==0u)streamedActivity(anchor);return;}Relation r=relation(step.first+anchor);"
         "if(lane==0u){x_sumData[linearWord(anchor,0u)]=0xffffffffu;";
    for (uint32_t row=0;row<M;++row)
        s << "if(step.iteration==0u)storeMultiplier(r," << row << "u,0.0);"
             "x_sumData[linearWord(anchor," << 1+M*M+M+row << "u)]=0u;";
    s << "}if(!relationEnabled(r))return;uint count=memberCount(anchor),member=0u;";
    array(s,"x",n);array(s,"termValue",R);array(s,"termJ",R*Q);
    s << "if(lane<count){member=memberAt(anchor,lane);loadInputs(r,anchor,member,x);terms(x,termValue,termJ);}";
    for (uint32_t k=0;k<R;++k) {
        s << "partial[" << k*Capacity << "u+lane]=lane<count?termValue[" << k << "]:0.0;";
        for (uint32_t c=0;c<QA;++c)
            s << "partial[" << (R+k*QA+c)*Capacity << "u+lane]=lane<count?termJ[" << k*Q+c << "]:0.0;";
    }
    auto reduce = [&](uint32_t columns) {
        s << "\n#ifdef DYNAMICS_SUBGROUP32\nif(gl_NumSubgroups==4u){barrier();"
             "for(uint offset=" << Capacity/2 << "u;offset>=32u;offset/=2u){if(lane<offset){";
        for (uint32_t k=0;k<columns;++k)
            s << "partial[" << k*Capacity << "u+lane]+=partial[" << k*Capacity << "u+lane+offset];";
        s << "}barrier();}";
        for (uint32_t k=0;k<columns;++k) {
            s << "{float value=partial[" << k*Capacity << "u+lane];"
                 "for(uint offset=16u;offset>0u;offset/=2u){float other=subgroupShuffleDown(value,offset);"
                 "if(lane%32u<offset)value+=other;}if(lane==0u)partial[" << k*Capacity << "u]=value;}";
        }
        s << "barrier();}else\n#endif\n";
        s << "{barrier();for(uint offset=" << Capacity/2 << "u;offset>0u;offset/=2u){if(lane<offset){";
        for (uint32_t k=0;k<columns;++k)
            s << "partial[" << k*Capacity << "u+lane]+=partial[" << k*Capacity << "u+lane+offset];";
        s << "}barrier();}}";
    };
    reduce(R*(1+QA));
    for (auto slot:fixed) if (!readOnly[slot])
        s << "uint id" << slot << "=field" << slot << "(anchor);";
    s << "if(lane==0u){";
    array(s,"outerX",n+R);array(s,"c",M);array(s,"localOuterJ",M*O);
    s << "loadInputs(r,anchor," << extent() << "u,x);for(uint k=0u;k<" << n << "u;++k)outerX[k]=x[k];";
    for (uint32_t k=0;k<R;++k) s << "outerX[" << n+k << "]=partial[" << k*Capacity << "];";
    s << "outerValue(outerX,c,localOuterJ);live=1u;"
         "for(uint k=0u;k<" << M << "u;++k){residual[k]=c[k];if(isnan(c[k])||isinf(c[k]))live=0u;}"
         "if(live==0u)invalidEvaluation();";
    if (M==1 && t.kind!=RelationKind::Equality)
        s << "if(c[0]" << (t.kind==RelationKind::GreaterEqual?">=":"<=")
          << "0.0&&loadMultiplier(r,0u)==0.0)live=0u;";
    for (uint32_t k=0;k<M*O;++k) s << "outerJ[" << k << "]=localOuterJ[" << k << "];";
    for (auto slot:fixed) if (!readOnly[slot])
        for (uint32_t row=0;row<M;++row) for (uint32_t c=0;c<t.spaces[slot]->stateSize;++c) {
            const auto col=qoffset[slot]+c;
            s << "fixedJ[" << row*QA+col << "]=localOuterJ[" << row*O+col << "]";
            for (uint32_t k=0;k<R;++k)
                s << "+localOuterJ[" << row*O+QA+k << "]*partial[" << (R+k*QA+col)*Capacity << "]";
            s << ";";
        }
    for (auto slot:fixed) if (!readOnly[slot]) for (auto before:fixed) {
        if (before>=slot) break;
        if (readOnly[before] || t.spaces[before]->stateSize!=t.spaces[slot]->stateSize) continue;
        s << "if(id" << slot << "==id" << before << "){";
        for (uint32_t row=0;row<M;++row) for (uint32_t c=0;c<t.spaces[slot]->stateSize;++c)
            s << "fixedJ[" << row*QA+qoffset[before]+c << "]+=fixedJ[" << row*QA+qoffset[slot]+c
              << "];fixedJ[" << row*QA+qoffset[slot]+c << "]=0.0;";
        s << "}";
    }
    s << "}barrier();if(live==0u)return;";
    array(s,"g",M*S);array(s,"j",M*T);array(s,"w",M*T);array(s,"a",M*M);
    s << "for(uint k=0u;k<" << M*M << "u;++k)a[k]=0.0;"
         "if(lane<count){uint id=field" << varyingSlot << "(member);"
         "for(uint k=0u;k<" << M*S << "u;++k)g[k]=0.0;";
    for (uint32_t row=0;row<M;++row) for (uint32_t c=0;c<t.spaces[varyingSlot]->stateSize;++c)
        for (uint32_t k=0;k<R;++k)
            s << "g[" << row*S+c << "]+=outerJ[" << row*O+QA+k << "]*termJ[" << k*Q+qoffset[varyingSlot]+c << "];";
    bool first=true;
    for (auto slot:fixed) if (!readOnly[slot]) {
        s << (first?"if(":"else if(") << "id==id" << slot << "){";first=false;
        for (uint32_t row=0;row<M;++row) for (uint32_t c=0;c<t.spaces[slot]->stateSize;++c)
            s << "fixedJ[" << row*QA+qoffset[slot]+c << "]+=g[" << row*S+c << "];";
        s << "clearEntry(anchor,lane);}";
    }
    if (!first) s << "else";
    s << "{transform(" << varyingSlot << "u,variable(id),g,j,w);accumulate(j,w,a);"
         "if(influences(j)){atomicAdd(x_activeDegrees[id],1u);cacheEntry(anchor,lane,id,"
      << varyingSlot << "u,j,w);}else clearEntry(anchor,lane);}}barrier();if(lane==0u){";
    for (uint32_t f=0;f<fixed.size();++f) {
        const auto slot=fixed[f];
        if (readOnly[slot]) {
            s << "x_sumData[linearWord(anchor," << linearHeader() << "u+(count+" << f << "u)*"
              << linearEntry() << "u)]=0xffffffffu;";
            continue;
        }
        s << "clearEntry(anchor,count+" << f << "u);";
        s << "if(true";
        for (auto before:fixed) if (before<slot && !readOnly[before]) s << "&&id" << slot << "!=id" << before;
        s << "){for(uint k=0u;k<" << M*S << "u;++k)g[k]=0.0;";
        for (uint32_t row=0;row<M;++row) for (uint32_t c=0;c<t.spaces[slot]->stateSize;++c)
            s << "g[" << row*S+c << "]=fixedJ[" << row*QA+qoffset[slot]+c << "];";
        s << "transform(" << slot << "u,variable(id" << slot << "),g,j,w);accumulate(j,w,a);"
             "if(influences(j)){atomicAdd(x_activeDegrees[id" << slot << "],1u);cacheEntry(anchor,count+"
          << f << "u,id" << slot << "," << slot << "u,j,w);}}";
    }
    s << "}";
    for (uint32_t k=0;k<M*M;++k) s << "partial[" << k*Capacity << "u+lane]=a[" << k << "];";
    reduce(M*M);
    s << "if(lane==0u){x_sumData[linearWord(anchor,0u)]=count+" << fixed.size() << "u;";
    for (uint32_t row=0;row<M;++row) {
        s << "x_sumData[linearWord(anchor," << 1+M*M+2*M+row << "u)]=floatBitsToUint(loadMultiplier(r,"
          << row << "u));";
        s << "float alpha" << row << "=loadCompliance(r," << row << "u," << 2+t.parameters
          << "u)/(step.h*step.h);x_sumData[linearWord(anchor," << 1+M*M+row << "u)]=floatBitsToUint("
             "-residual[" << row << "]-alpha" << row << "*loadMultiplier(r," << row << "u));";
        for (uint32_t col=0;col<M;++col)
            s << "x_sumData[linearWord(anchor," << 1+row*M+col << "u)]=floatBitsToUint(partial["
              << (row*M+col)*Capacity << "]" << (row==col?"+alpha"+std::to_string(row):"") << ");";
    }
    s << "}}\n";
    return s.str();
}

std::string Generator::cachedSolve(bool refine) const {
    std::ostringstream s;
    const auto team = LinearTeam;
    s << "\n";
    if (!refine) s << "shared uint degrees[128];";
    s << "shared float changes[" << 128*M << "];void main(){uint anchor=sumInvocation()/" << team << "u,"
         "local=sumLocal(),lane=local%" << team << "u;bool cached=false;if(anchor<" << d.count
      << "u)cached=canMaterialize(anchor);uint count=cached?x_sumData[linearWord(anchor,0u)]:0xffffffffu,degree=1u;";
    array(s,"change",M);
    s << "for(uint row=0u;row<" << M << "u;++row)change[row]=0.0;"
         "if(count!=0xffffffffu)for(uint k=lane;k<count;k+=" << team << "u){uint column=" << linearHeader()
      << "u+k*" << linearEntry() << "u,id=x_sumData[linearWord(anchor,column)];"
         "if(id!=0xffffffffu){";
    if (!refine) {
        s << "degree=max(degree,x_activeDegrees[id]);";
    }
    const bool uniformWidth = std::all_of(t.spaces.begin(), t.spaces.end(),
        [&](const SpaceRef& space) { return space->tangentSize == T; });
    s << "Variable v=variable(id);";
    if (!uniformWidth) {
        s << "uint slot=x_sumData[linearWord(anchor,column+1u)],width=0u;switch(slot){";
        for (uint32_t slot=0;slot<t.spaces.size();++slot)
            s << "case " << slot << "u:width=" << t.spaces[slot]->tangentSize << "u;break;";
        s << "}";
    }
    for (uint32_t c=0;c<T;++c) {
        s << (uniformWidth ? "{" : "if(width>"+u(c)+"){")
          << "float dx=uintBitsToFloat(x_contributions[v.v+" << c << "u*v.stride]);";
        for (uint32_t row=0;row<M;++row)
            s << "change[" << row << "]+=uintBitsToFloat(x_sumData[linearWord(anchor,column+"
              << 2+row*T+c << "u)])*dx;";
        s << "}";
    }
    s << "}}";
    s << "\n#ifdef DYNAMICS_SUBGROUP32\nif(gl_NumSubgroups==4u){for(uint offset=" << team/2 << "u;offset>0u;offset/=2u){";
    if (!refine) s << "uint otherDegree=subgroupShuffleDown(degree,offset);if(lane<offset)degree=max(degree,otherDegree);";
    for (uint32_t row=0;row<M;++row)
        s << "{float other=subgroupShuffleDown(change[" << row << "],offset);if(lane<offset)change[" << row << "]+=other;}";
    s << "}}else\n#endif\n{";
    if (!refine) s << "degrees[local]=degree;";
    for (uint32_t row=0;row<M;++row) s << "changes[" << row*128 << "u+local]=change[" << row << "];";
    s << "barrier();for(uint offset=" << team/2 << "u;offset>0u;offset/=2u){if(lane<offset){";
    if (!refine) s << "degrees[local]=max(degrees[local],degrees[local+offset]);";
    for (uint32_t row=0;row<M;++row)
        s << "changes[" << row*128 << "u+local]+=changes[" << row*128 << "u+local+offset];";
    s << "}barrier();}";
    if (!refine) s << "degree=degrees[local];";
    for (uint32_t row=0;row<M;++row) s << "change[" << row << "]=changes[" << row*128 << "u+local];";
    s << "}if(lane!=0u||anchor>=" << d.count << "u)return;if(!cached){"
      << (refine ? "" : "streamedSolve(anchor);") << "return;}"
         "if(count==0xffffffffu)return;";
    if (refine)
        s << "degree=x_sumData[linearWord(anchor," << linearHeader()-1 << "u)];";
    else
        s << "x_sumData[linearWord(anchor," << linearHeader()-1 << "u)]=degree;";
    s << "Relation r=relation(step.first+anchor);";
    array(s,"a",M*M);array(s,"rhs",M);array(s,"dl",M);array(s,"nextLambda",M);
    // A singular/invalid block returns before publishing a correction. Clear
    // the transient output here so the transpose gather cannot reuse old data.
    for (uint32_t k=0;k<M;++k)
        s << "x_sumData[linearWord(anchor," << 1+M*M+M+k << "u)]=0u;";
    for (uint32_t k=0;k<M*M;++k)
        s << "a[" << k << "]=uintBitsToFloat(x_sumData[linearWord(anchor," << 1+k << "u)]);";
    // Contributions hold the total tangent displacement since this snapshot.
    // Reuse J to solve b - J*dx - alpha*(lambda-lambda0), without re-evaluating
    // the nonlinear expression or changing the physical integration time step.
    for (uint32_t k=0;k<M;++k)
        s << "rhs[" << k << "]=uintBitsToFloat(x_sumData[linearWord(anchor," << 1+M*M+k
          << "u)])-change[" << k << "]-loadCompliance(r," << k << "u," << 2+t.parameters
          << "u)/(step.h*step.h)*(loadMultiplier(r," << k << "u)-uintBitsToFloat(x_sumData[linearWord(anchor,"
          << 1+M*M+2*M+k << "u)]));";
    s << solveAndProject();
    for (uint32_t k=0;k<M;++k)
        s << "x_sumData[linearWord(anchor," << 1+M*M+M+k << "u)]=floatBitsToUint(dl[" << k << "]);";
    s << "}\n";
    return s.str();
}

std::string Generator::cachedGather() const {
    std::ostringstream s;
    const auto team = LinearTeam;
    s << stateAccess(StateStorage::Global) << linearAccess()
      << "shared float partial[" << 128*T << "];void main(){uint i=sumInvocation()/" << team << "u,"
         "local=sumLocal(),lane=local%" << team << "u;bool valid=i<" << degrees.size()
      << "u;uint id=valid?x_sumData[" << inverseVariables << "u+i]:0u;";
    array(s,"sum",T);
    s << "for(uint c=0u;c<" << T << "u;++c)sum[c]=0.0;if(valid){uint end=x_sumData["
      << transpose << "u+id+1u];for(uint at=x_sumData[" << transpose << "u+id]+lane;at<end;at+=" << team << "u){"
         "uint anchor=x_sumData[inverseWord(at,0u)];";
    for (uint32_t row=0;row<M;++row) {
        s << "float dl" << row << "=uintBitsToFloat(x_sumData[linearWord(anchor," << 1+M*M+M+row << "u)]);";
        for (uint32_t c=0;c<T;++c)
            s << "if(dl" << row << "!=0.0)sum[" << c << "]+=uintBitsToFloat(x_sumData[inverseWord(at,"
              << 1+row*T+c << "u)])*dl" << row << ";";
    }
    s << "}}";
    s << "\n#ifdef DYNAMICS_SUBGROUP32\nif(gl_NumSubgroups==4u){for(uint offset=" << team/2 << "u;offset>0u;offset/=2u){";
    for (uint32_t c=0;c<T;++c)
        s << "{float other=subgroupShuffleDown(sum[" << c << "],offset);if(lane<offset)sum[" << c << "]+=other;}";
    s << "}}else\n#endif\n{";
    for (uint32_t c=0;c<T;++c) s << "partial[" << c*128 << "u+local]=sum[" << c << "];";
    s << "barrier();for(uint offset=" << team/2 << "u;offset>0u;offset/=2u){if(lane<offset){";
    for (uint32_t c=0;c<T;++c)
        s << "partial[" << c*128 << "u+local]+=partial[" << c*128 << "u+local+offset];";
    s << "}barrier();}";
    for (uint32_t c=0;c<T;++c) s << "sum[" << c << "]=partial[" << c*128 << "u+local];";
    s << "}if(lane==0u&&valid){Variable v=variable(id);";
    // A variable can have a narrower tangent than the widest slot in this domain.
    for (uint32_t c=0;c<T;++c) {
        s << "if(";
        bool first=true;
        for (const auto& layout:p.variables) if (p.spaces[layout.space]->tangentSize>c) {
            s << (first?"":"||") << "(id>=" << layout.first << "u&&id<" << layout.first+layout.count << "u)";
            first=false;
        }
        if (first) s << "false";
        s << "){uint at=v.v+" << c << "u*v.stride;x_contributions[at]=floatBitsToUint("
             "uintBitsToFloat(x_contributions[at])+sum[" << c << "]);}";
    }
    s << "}}\n";
    return s.str();
}
}

std::vector<SumDomain> lowerSumRelations(CompiledPlan& p) {
    std::vector<SumDomain> result;
    for (uint32_t set = 0; set < p.relations.size(); ++set)
        for (uint32_t domain = 0; domain < p.bindings[set].domains.size(); ++domain) {
            if (p.bindings[set].domains[domain].summedObject < 0) continue;
            Generator generator(p, set, domain);
            SumDomain out{set, domain, generator.degrees, {}, {}};
            if (generator.factor) {
                out.solve = generator.operatorSolve(false);
                out.activity = generator.operatorLinearize();
                out.gather = generator.operatorGather();
                out.refine = generator.operatorSolve(true);
                out.solveCount = out.activityCount = out.gatherCount = generator.d.count * generator.operatorTeam;
            } else out.solve = generator.generate(generator.t.residual, false);
            if (generator.t.update) out.update = generator.generate(*generator.t.update, true);
            if (!generator.factor && p.policy.weighting != JacobiWeighting::Static)
                out.activity = generator.generate(generator.t.residual, false, true);
            if (generator.materialized) {
                out.solveCount = generator.d.count * Generator::LinearTeam;
                out.activityCount = generator.d.count * Generator::Capacity;
                out.gather = generator.cachedGather();
                out.refine = generator.generate(generator.t.residual, false, false, true);
                out.gatherCount = uint32_t(generator.degrees.size()) * Generator::LinearTeam;
            }
            out.bounds = std::move(generator.bounds);
            out.index = std::move(generator.index);
            out.assembly = std::move(generator.assembly);
            result.push_back(std::move(out));
        }
    return result;
}
}
