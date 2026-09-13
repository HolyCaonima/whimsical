#include "physics/dynamics/compiler/FormulaGlsl.h"
#include "Schedule.h"
#include "SumRelations.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace whimsical::dynamics {
// Kernel generation is local to the physics compiler; RenderCore never sees spaces,
// constraints, multiplier state, coloring, or the numerical method.
std::string incidenceKernel(const RelationType&, const std::vector<bool>&, int32_t endpointMode, bool scatter);
namespace {
constexpr const char* Names[] = {"q",
                                 "oldq",
                                 "velocity",
                                 "metric",
                                 "fieldModes",
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
                                 "regionRanges", "regionState", "localOffsets", "stateWrites",
                                 "candidates", "activeDegrees", "epochData", "epochOutput", "sumData"};
static_assert(sizeof(Names) / sizeof(*Names) == BufferCount);
// Bounded transient input table; larger updates retain the direct upload path.
constexpr uint32_t StateWriteCapacity = 4096;
bool integer(BufferRole r) {
    return r == BufferRole::Variables || r == BufferRole::VariableWork || r == BufferRole::Relations ||
           r == BufferRole::Endpoints || r == BufferRole::RelationWork || r == BufferRole::AdjacencyOffsets ||
           r == BufferRole::Contributions || r == BufferRole::AdjacencyEntries || r == BufferRole::Diagnostics ||
           r == BufferRole::ScanScratch || r == BufferRole::AdjacencyCursors || r == BufferRole::RegionRanges ||
           r == BufferRole::RegionState || r == BufferRole::LocalOffsets || r == BufferRole::StateWrites ||
           r == BufferRole::Candidates || r == BufferRole::ActiveDegrees || r == BufferRole::FieldModes ||
           r == BufferRole::EpochData || r == BufferRole::SumData;
}
uint32_t checked(size_t n) {
    if (n > UINT32_MAX)
        throw std::overflow_error("Dynamics plan exceeds 32-bit element addressing");
    return uint32_t(n);
}
uint32_t bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
template <class T> uint32_t append(BufferData& target, const std::vector<T>& values) {
    static_assert(sizeof(T) == 4);
    if (target.wordCount() != target.initial.size() / 4)
        throw std::logic_error("Cannot append CPU data after deferred buffer initialization");
    auto first = checked(target.wordCount());
    if (uint64_t(first) + values.size() > UINT32_MAX)
        throw std::overflow_error("Dynamics buffer element capacity");
    auto offset = target.initial.size();
    target.initial.resize(offset + values.size() * 4);
    if (!values.empty())
        std::memcpy(target.initial.data() + offset, values.data(), values.size() * 4);
    target.words = target.initial.size() / 4;
    return first;
}
uint32_t reserve(BufferData& target, uint64_t words, std::optional<uint32_t> fill = {}) {
    const auto first = checked(target.wordCount());
    if (uint64_t(first) + words > UINT32_MAX)
        throw std::overflow_error("Dynamics buffer element capacity");
    target.words = uint64_t(first) + words;
    if (fill && words)
        target.fills.push_back({first, words, *fill});
    return first;
}
uint32_t field(BufferData& target, const Field& values, bool deferUniform = false) {
    if (deferUniform) {
        const auto first = reserve(target, uint64_t(values.count()) * values.width());
        const auto& uniform = values.uniformValue();
        for (uint32_t c = 0; c < values.width(); ++c) {
            uint32_t value;
            std::memcpy(&value, &uniform[c], sizeof(value));
            target.fills.push_back({uint64_t(first) + uint64_t(c) * values.count(), values.count(), value});
        }
        return first;
    }
    return append(target, values.columnMajor());
}
void packField(BufferData& buffer) {
    const auto count = buffer.wordCount();
    if (count < BufferData::PageWords * 2 || buffer.fills.empty())
        return;
    const auto pages = uint32_t((count + BufferData::PageWords - 1) / BufferData::PageWords);
    std::vector<uint32_t> packed(pages);
    std::map<uint32_t, uint32_t> constants;
    auto constantPage = [&](uint32_t page, uint32_t value) {
        auto [at, inserted] = constants.emplace(value, uint32_t(packed.size()));
        if (inserted)
            packed.push_back(value);
        packed[page] = at->second | BufferData::UniformPage;
    };
    size_t fillIndex = 0;
    for (uint32_t page = 0; page < pages; ++page) {
        if (packed.size() + BufferData::PageWords >= BufferData::UniformPage)
            return; // Dense addressing can represent the larger physical span.
        const uint64_t first = uint64_t(page) * BufferData::PageWords;
        const auto end = std::min(count, first + BufferData::PageWords);
        while (fillIndex < buffer.fills.size() &&
               buffer.fills[fillIndex].first + buffer.fills[fillIndex].count <= first)
            ++fillIndex;
        if (fillIndex < buffer.fills.size()) {
            const auto& fill = buffer.fills[fillIndex];
            if (fill.first <= first && fill.first + fill.count >= end) {
                constantPage(page, fill.value);
                continue;
            }
        }
        std::array<uint32_t, BufferData::PageWords> values{};
        const auto initialEnd = std::min(end, uint64_t(buffer.initial.size() / 4));
        if (initialEnd > first)
            std::memcpy(values.data(), buffer.initial.data() + first * 4, (initialEnd - first) * 4);
        for (size_t i = fillIndex; i < buffer.fills.size() && buffer.fills[i].first < end; ++i) {
            const auto& part = buffer.fills[i];
            std::fill(values.begin() + (std::max(first, part.first) - first),
                      values.begin() + (std::min(end, part.first + part.count) - first), part.value);
        }
        const bool constant = std::all_of(values.begin() + 1, values.begin() + (end - first),
                                         [&](uint32_t value) { return value == values[0]; });
        if (constant)
            constantPage(page, values[0]);
        else {
            packed[page] = uint32_t(packed.size());
            packed.insert(packed.end(), values.begin(), values.end());
        }
    }
    if (packed.size() * 2 >= count)
        return;
    buffer.initial.resize(packed.size() * 4);
    std::memcpy(buffer.initial.data(), packed.data(), buffer.initial.size());
    buffer.words = packed.size();
    buffer.fills.clear();
    buffer.paged = true;
}
// Per-row scheduling state is a color byte. Everything else is an immutable
// projection of the logical set range, not a Cartesian array of instance structs.
class InstanceRows {
    const CompiledPlan& plan_;
    struct Segment {
        uint32_t first = 0, count = 0, set = 0;
        std::vector<int8_t> colors;
        std::vector<uint32_t> endpoints, contributions, contributionStrides;
        uint32_t deferredEndpoint = 0;
    };
    std::vector<Segment> segments_;
    uint32_t size_ = 0, materialized_ = 0, currentSegment_ = 0;
    int8_t deferredColor_ = -1;

    uint32_t locate(uint32_t id) {
        if (currentSegment_ < segments_.size()) {
            const auto& current = segments_[currentSegment_];
            if (id >= current.first && id - current.first < current.count)
                return currentSegment_;
        }
        auto found = std::upper_bound(
            segments_.begin(), segments_.end(), id,
            [](uint32_t row, const Segment& segment) { return row < segment.first; });
        currentSegment_ = uint32_t(std::prev(found) - segments_.begin());
        return currentSegment_;
    }
  public:
    struct Row {
        uint32_t id, set, local, type;
        int8_t& color;
        uint32_t arity;
    };
    InstanceRows(const CompiledPlan& plan, uint32_t count) : plan_(plan), size_(count) {
        for (uint32_t set = 0; set < plan.relations.size(); ++set)
            for (uint32_t domain = 0; domain < plan.bindings[set].domains.size(); ++domain) {
                const auto& binding = plan.bindings[set].domains[domain];
                Segment segment{plan.relations[set].first + binding.first, binding.count, set};
                const bool deferred = std::any_of(
                    plan.deferredJacobiDomains.begin(), plan.deferredJacobiDomains.end(),
                    [&](const DeferredJacobiDomain& candidate) {
                        return candidate.set == set && candidate.domain == domain;
                    });
                if (!deferred) {
                    segment.colors.assign(binding.count, -1);
                    segment.endpoints.resize(binding.count);
                    segment.contributions.resize(binding.count);
                    segment.contributionStrides.resize(binding.count);
                    materialized_ += binding.count;
                }
                segments_.push_back(std::move(segment));
            }
    }
    uint32_t size() const { return size_; }
    uint32_t materializedSize() const { return materialized_; }
    bool deferred(uint32_t id) {
        return segments_[locate(id)].colors.empty();
    }
    void set(uint32_t id, uint32_t column, uint32_t value) {
        auto& segment = segments_[locate(id)];
        if (segment.colors.empty())
            throw std::logic_error("Deferred relation metadata is affine");
        auto row = id - segment.first;
        if (column == 0)
            segment.endpoints[row] = value;
        else if (column == 5)
            segment.contributions[row] = value;
        else if (column == 7)
            segment.contributionStrides[row] = value;
        else
            throw std::logic_error("Unsupported mutable relation metadata column");
    }
    void setDomainEndpoint(uint32_t set, uint32_t domain, uint32_t value) {
        const auto& binding = plan_.bindings[set].domains[domain];
        if (!binding.count)
            return;
        const auto first = plan_.relations[set].first + binding.first;
        auto& segment = segments_[locate(first)];
        if (segment.first != first)
            throw std::logic_error("Binding domain metadata range mismatch");
        if (segment.colors.empty())
            segment.deferredEndpoint = value;
        else
            std::fill(segment.endpoints.begin(), segment.endpoints.end(), value);
    }
    void packMetadata(BufferData& buffer, std::vector<MetadataRange<10>>& ranges) const {
        std::vector<uint8_t> packed;
        auto values = [&](const Segment& segment, uint32_t row) {
            const auto& layout = plan_.relations[segment.set];
            const auto local = segment.first - layout.first + row;
            const auto endpoint = segment.colors.empty() ? segment.deferredEndpoint : segment.endpoints[row];
            const auto contribution = segment.colors.empty() ? 0u : segment.contributions[row];
            const auto contributionStride =
                segment.colors.empty() ? 0u : segment.contributionStrides[row];
            return std::array<uint32_t, 10>{
                endpoint, layout.parameters + local, layout.compliance + local,
                layout.history + local, layout.multipliers + local, contribution,
                layout.count, contributionStride, layout.type, layout.modes};
        };
        for (const auto& segment : segments_) {
            if (!segment.count)
                continue;
            MetadataRange<10> metadata;
            metadata.first = segment.first;
            metadata.count = segment.count;
            metadata.base = values(segment, 0);
            metadata.affine = segment.count >= 128;
            if (segment.count > 1) {
                metadata.stride = values(segment, 1);
                for (uint32_t column = 0; column < metadata.stride.size(); ++column)
                    metadata.stride[column] -= metadata.base[column];
            }
            // Deferred segments are affine by construction: their endpoint
            // descriptor is uniform and every logical row field above is either
            // uniform or base + row. Materialized segments still prove the table.
            for (uint32_t row = 2;
                 metadata.affine && !segment.colors.empty() && row < segment.count; ++row) {
                const auto rowValues = values(segment, row);
                for (uint32_t column = 0; column < metadata.stride.size(); ++column)
                    metadata.affine = metadata.affine &&
                        rowValues[column] == metadata.base[column] + row * metadata.stride[column];
            }
            metadata.offset = checked(packed.size() / 4);
            if (!metadata.affine)
                for (uint32_t row = 0; row < segment.count; ++row) {
                    const auto rowValues = values(segment, row);
                    const auto* first = reinterpret_cast<const uint8_t*>(rowValues.data());
                    packed.insert(packed.end(), first, first + sizeof(rowValues));
                }
            if (!metadata.affine && !ranges.empty() && !ranges.back().affine &&
                ranges.back().first + ranges.back().count == metadata.first)
                ranges.back().count += metadata.count;
            else
                ranges.push_back(metadata);
        }
        buffer.initial = std::move(packed);
        buffer.words = buffer.initial.size() / 4;
        buffer.fills.clear();
    }
    Row at(uint32_t id, uint32_t segment) {
        const auto& range = segments_[segment];
        const auto& layout = plan_.relations[range.set];
        auto& color = range.colors.empty() ? deferredColor_ : segments_[segment].colors[id - range.first];
        return {id, range.set, id - layout.first, layout.type, color,
                uint32_t(plan_.types[layout.type]->spaces.size())};
    }
    Row operator[](uint32_t id) {
        return at(id, locate(id));
    }
    struct Iterator {
        InstanceRows& rows;
        uint32_t segment, row;
        bool operator!=(const Iterator& other) const {
            return segment != other.segment || row != other.row;
        }
        Row operator*() const {
            return rows.at(rows.segments_[segment].first + row, segment);
        }
        Iterator& operator++() {
            ++row;
            while (segment < rows.segments_.size() &&
                   (rows.segments_[segment].colors.empty() ||
                    row >= rows.segments_[segment].count)) {
                ++segment;
                row = 0;
            }
            return *this;
        }
    };
    Iterator begin() {
        uint32_t segment = 0;
        while (segment < segments_.size() && segments_[segment].colors.empty())
            ++segment;
        return {*this, segment, 0};
    }
    Iterator end() { return {*this, uint32_t(segments_.size()), 0}; }
};
template <size_t Width, class Layout>
void packMetadata(BufferData& buffer, const std::vector<Layout>& layouts,
                  std::vector<MetadataRange<Width>>& ranges) {
    std::vector<uint8_t> packed;
    for (const auto& layout : layouts) {
        if (!layout.count) continue;
        MetadataRange<Width> m;
        m.first = layout.first;
        m.count = layout.count;
        const auto* begin = buffer.initial.data() + size_t(layout.first) * Width * 4;
        // Small sets stay in the table to bound generated code per metadata row.
        m.affine = layout.count >= 128;
        std::memcpy(m.base.data(), begin, Width * 4);
        if (layout.count > 1) {
            std::memcpy(m.stride.data(), begin + Width * 4, Width * 4);
            for (size_t c = 0; c < Width; ++c) m.stride[c] -= m.base[c];
        }
        for (uint32_t row = 2; m.affine && row < layout.count; ++row) {
            std::array<uint32_t, Width> values;
            std::memcpy(values.data(), begin + size_t(row) * Width * 4, Width * 4);
            for (size_t c = 0; c < Width; ++c)
                m.affine = m.affine && values[c] == m.base[c] + row * m.stride[c];
        }
        m.offset = checked(packed.size() / 4);
        if (!m.affine)
            packed.insert(packed.end(), begin, begin + size_t(layout.count) * Width * 4);
        if (!m.affine && !ranges.empty() && !ranges.back().affine)
            ranges.back().count += m.count;
        else
            ranges.push_back(m);
    }
    buffer.initial = std::move(packed);
    buffer.words = buffer.initial.size() / 4;
    buffer.fills.clear();
}
template <size_t Width>
void metadataAccess(std::ostringstream& source, const std::vector<MetadataRange<Width>>& ranges,
                    const char* type, const char* buffer) {
    // A balanced range dispatch keeps table and arithmetic leaves interchangeable.
    auto emit = [&](auto&& self, size_t first, size_t end) -> void {
        if (end - first > 1) {
            auto middle = first + (end - first) / 2;
            source << "if(id<" << ranges[middle].first << "u){";
            self(self, first, middle);
            source << "}else{";
            self(self, middle, end);
            source << "}";
            return;
        }
        const auto& m = ranges[first];
        source << "uint row=id-" << m.first << "u;";
        if (!m.affine)
            source << "uint k=" << m.offset << "u+row*" << Width << "u;";
        source << "return " << type << "(";
        for (size_t c = 0; c < Width; ++c) {
            if (c) source << ",";
            if (!m.affine)
                source << buffer << "[k+" << c << "u]";
            else {
                source << m.base[c] << "u";
                if (m.stride[c]) source << "+row*" << m.stride[c] << "u";
            }
        }
        source << ",id);";
    };
    if (!ranges.empty())
        emit(emit, 0, ranges.size());
    else {
        source << "return " << type << "(";
        for (size_t c = 0; c < Width; ++c) source << "0u,";
        source << "id);";
    }
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
    if (t.summedObject() >= 0) {
        s += ":sum" + std::to_string(t.summedObject()) + ":split" + std::to_string(t.objects[0].size());
        auto expression = [&](const Formula& f, const std::string& name) {
            const auto program = splitSums(f, t);
            return (program.terms.outputs.empty() ? std::string{} : emitGlsl(program.terms, name + "Terms")) +
                   emitGlsl(program.outer, name + "Outer");
        };
        s += expression(t.residual, "r");
        if (t.update) s += expression(*t.update, "u");
    } else {
        s += emitGlsl(t.residual, "r");
        if (t.update) s += emitGlsl(*t.update, "u");
    }
    return s;
}
} // namespace
uint32_t CompiledPlan::relationSet(uint32_t relation) const {
    auto found = std::upper_bound(relations.begin(), relations.end(), relation,
        [](uint32_t id, const RelationLayout& layout) { return id < layout.first; });
    return uint32_t(std::prev(found) - relations.begin());
}
uint32_t CompiledPlan::relationType(uint32_t relation) const {
    return relations[relationSet(relation)].type;
}
uint32_t CompiledPlan::variableSet(uint32_t variable) const {
    auto found = std::upper_bound(variables.begin(), variables.end(), variable,
        [](uint32_t id, const VariableLayout& layout) { return id < layout.first; });
    return uint32_t(std::prev(found) - variables.begin());
}
bool CompiledPlan::variableReadOnly(uint32_t variable) const {
    return model.data->variables[variableSet(variable)].readOnly;
}
uint32_t CompiledPlan::endpoint(uint32_t relation, uint32_t slot) const {
    const auto set = relationSet(relation);
    auto ref = bindings[set].at(relation - relations[set].first, slot);
    return variables[ref.set].first + ref.index;
}
uint32_t CompiledPlan::activeTangent(uint32_t type, uint32_t slot) const {
    return typeReadOnly[type][slot] ? 0 : types[type]->spaces[slot]->tangentSize;
}
uint32_t CompiledPlan::activeTangent(uint32_t type) const {
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < types[type]->spaces.size(); ++slot)
        count += activeTangent(type, slot);
    return count;
}
std::string CompiledPlan::interface() const {
    std::ostringstream source;
    for (auto role : {BufferRole::Parameters, BufferRole::Compliance, BufferRole::RelationEnabled}) {
        const auto& buffer = buffers[size_t(role)];
        source << "float read_" << buffer.name << "(uint word){";
        if (buffer.paged)
            source << "uint page=floatBitsToUint(x_" << buffer.name << "[word/"
                   << BufferData::PageWords << "u]);return x_" << buffer.name
                   << "[(page&0x7fffffffu)+((page&0x80000000u)!=0u?0u:word%"
                   << BufferData::PageWords << "u)];}\n";
        else
            source << "return x_" << buffer.name << "[word];}\n";
    }
    auto anyMode = [](const std::vector<FieldModeLayout>& fields, uint32_t mode) {
        return std::any_of(fields.begin(), fields.end(),
                           [&](const FieldModeLayout& field) { return (field.mask & mode) != 0; });
    };
    source << "#define DYNAMICS_UNIFORM_VARIABLE_ENABLED "
           << anyMode(variableFieldModes, UniformEnabled) << "\n"
           << "#define DYNAMICS_IDENTITY_METRIC " << anyMode(variableFieldModes, IdentityMetric) << "\n"
           << "#define DYNAMICS_UNIFORM_RELATION_ENABLED "
           << anyMode(relationFieldModes, UniformEnabled) << "\n"
           << "#define DYNAMICS_UNIFORM_PARAMETERS "
           << anyMode(relationFieldModes, UniformParameters) << "\n"
           << "#define DYNAMICS_UNIFORM_COMPLIANCE "
           << anyMode(relationFieldModes, UniformCompliance) << "\n";
    source << R"(
layout(local_size_x=128) in;
layout(push_constant) uniform Step {
    uint first; uint count; float h; float time;
    uint tick; uint iteration; float relaxation; uint reserved;
} step;
uint invocation() { return gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * 8388480u; }
struct Variable {uint q; uint v; uint m; uint stride; uint flags; uint u; uint id;};
Variable variable(uint id) {
)";
    metadataAccess(source, variableMetadata, "Variable", "x_variables");
    source << R"(
}
struct Relation {uint e; uint p; uint a; uint h; uint l; uint c; uint stride; uint cs; uint type; uint u; uint id;};
Relation relation(uint id) {
)";
    metadataAccess(source, relationMetadata, "Relation", "x_relations");
    source << R"(
}
uint trianglePrefix(uint i,uint n,bool diagonal){
    uint b=2u*n-i-(diagonal?0u:2u)+1u;
    return (i&1u)==0u?(i/2u)*b:i*(b/2u);
}
uint linearEndpoint(Relation r,uint slot){
    uint at=r.e&0x7fffffffu,field=at+5u+2u*slot;
    return x_endpoints[field]+(r.id-x_endpoints[at+1u])*x_endpoints[field+1u];
}
uvec2 bindingMembers(uint row,uint n,uint mode){
    uint i=row,j=row;
    if(mode==1u){i=row/n;j=row%n;}
    else if(mode==2u){i=row/(n-1u);j=row%(n-1u);if(j>=i)++j;}
    else if(mode>=3u){
        bool diagonal=mode==4u;
        float b=2.0*float(n)+(diagonal?1.0:-1.0);
        i=min(n-1u,uint(max(0.0,floor((b-sqrt(max(0.0,b*b-8.0*float(row))))*0.5))));
        while(i>0u&&trianglePrefix(i,n,diagonal)>row)--i;
        while(i+1u<n&&trianglePrefix(i+1u,n,diagonal)<=row)++i;
        j=i+(diagonal?0u:1u)+row-trianglePrefix(i,n,diagonal);
    }
    return uvec2(i,j);
}
uint endpoint(Relation r,uint slot,int mapping){
    if(mapping==0||(mapping<0&&(r.e&0x80000000u)==0u))return x_endpoints[r.e+slot];
    uint at=r.e&0x7fffffffu,mode=mapping<0?x_endpoints[at]:uint(mapping-1),row=r.id-x_endpoints[at+1u];
    uvec2 members=bindingMembers(row,x_endpoints[at+3u],mode);
    uint member=slot<x_endpoints[at+4u]?members.x:members.y,field=at+5u+2u*slot;
    return x_endpoints[field]+member*x_endpoints[field+1u];
}
)";
    return source.str();
}
PlanRef Compiler::compile(const ModelSnapshot& model, const SolverPolicy& policy) const {
    const auto started = std::chrono::steady_clock::now();
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
    auto allUniform = [](const auto& sets, auto field) {
        return std::all_of(sets.begin(), sets.end(),
                           [&](const auto& set) { return (set.*field).isUniform(); });
    };
    const bool uniformValues = allUniform(model.data->variables, &VariableSet::initial);
    const bool uniformVelocity = allUniform(model.data->variables, &VariableSet::velocity);
    const bool uniformMetric = allUniform(model.data->variables, &VariableSet::inverseMetric);
    const bool uniformVariableEnabled = allUniform(model.data->variables, &VariableSet::enabled);
    const bool uniformParameters = allUniform(model.data->relations, &RelationSet::parameters);
    const bool uniformCompliance = allUniform(model.data->relations, &RelationSet::compliance);
    const bool uniformHistory = allUniform(model.data->relations, &RelationSet::initialHistory);
    const bool uniformRelationEnabled = allUniform(model.data->relations, &RelationSet::enabled);
    std::map<std::string, uint32_t> spaceIds, typeIds;
    std::vector<std::vector<uint32_t>> variableGroups;
    std::vector<bool> writable;
    std::vector<uint32_t> variableMeta;
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
        uint32_t modeMask = 0;
        if (set.enabled.isUniform()) {
            modeMask |= CompiledPlan::UniformEnabled;
            if (set.enabled.uniformValue()[0] != 0.0f)
                modeMask |= CompiledPlan::EnabledValue;
        }
        bool identity = set.inverseMetric.isUniform();
        for (uint32_t row = 0; identity && row < set.space->tangentSize; ++row)
            for (uint32_t column = 0; identity && column < set.space->tangentSize; ++column)
                identity = set.inverseMetric.uniformValue()[row * set.space->tangentSize + column] ==
                           (row == column ? 1.0f : 0.0f);
        if (identity)
            modeMask |= CompiledPlan::IdentityMetric;
        const auto modes = append(
            buffer(BufferRole::FieldModes),
            std::vector<uint32_t>{modeMask,
                                  bits(set.enabled.isUniform() ? set.enabled.uniformValue()[0] : 0.0f)});
        VariableLayout layout{checked(writable.size()),
                              set.count,
                              space,
                              field(buffer(BufferRole::Values), set.initial, uniformValues),
                              field(buffer(BufferRole::Velocity), set.velocity, uniformVelocity),
                              field(buffer(BufferRole::Metric), set.inverseMetric, uniformMetric),
                              modes};
        p->variables.push_back(layout);
        p->variableFieldModes.push_back({modes, modeMask});
        field(buffer(BufferRole::VariableEnabled), set.enabled, uniformVariableEnabled);
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
                                                     layout.metric + i, set.count, set.readOnly ? 1u : 0u,
                                                     layout.modes});
            variableGroups[space].push_back(layout.first + i);
            writable.push_back(!set.readOnly);
        }
    }
    append(buffer(BufferRole::Variables), variableMeta);
    p->statistics.variables = writable.size();
    buffer(BufferRole::Previous) = buffer(BufferRole::Values);
    buffer(BufferRole::Previous).name = Names[size_t(BufferRole::Previous)];
    reserve(buffer(BufferRole::Acceleration), buffer(BufferRole::Velocity).wordCount(), 0u);
    // Resolve named fields and access effects before choosing numerical kernels.
    // Scheduling traverses the domains without making a second endpoint graph.
    uint64_t relationCount = 0;
    for (const auto& set : model.data->relations) relationCount += set.count;
    checked(relationCount);
    uint32_t relationFirst = 0;
    for (uint32_t setId = 0; setId < model.data->relations.size(); ++setId) {
        const auto& set = model.data->relations[setId];
        set.type->validate();
        p->summedRelations = p->summedRelations || set.type->summedObject() >= 0;
        p->bindings.push_back(analyzeBindings(*model.data, set));
        const auto& binding = p->bindings.back();
        p->statistics.bindingDomains += binding.domains.size();
        int32_t endpointMode = binding.domains.empty() ? 0 : binding.domains[0].endpointMode(set.dynamicEndpoints);
        for (const auto& domain : binding.domains)
            if (domain.endpointMode(set.dynamicEndpoints) != endpointMode)
                endpointMode = -1;
        auto key = signature(*set.type) + ":map" + std::to_string(endpointMode);
        for (bool readOnly : binding.readOnly)
            key += readOnly ? ":r" : ":w";
        auto it = typeIds.find(key);
        uint32_t type;
        if (it == typeIds.end()) {
            type = checked(p->types.size());
            typeIds.emplace(key, type);
            p->types.push_back(set.type);
            p->typeReadOnly.push_back(binding.readOnly);
            p->typeEndpointMode.push_back(endpointMode);
        } else
            type = it->second;
        uint32_t modeMask = 0;
        if (set.enabled.isUniform()) {
            modeMask |= CompiledPlan::UniformEnabled;
            if (set.enabled.uniformValue()[0] != 0.0f)
                modeMask |= CompiledPlan::EnabledValue;
        }
        if (set.parameters.isUniform())
            modeMask |= CompiledPlan::UniformParameters;
        if (set.compliance.isUniform()) {
            modeMask |= CompiledPlan::UniformCompliance;
            if (std::all_of(set.compliance.uniformValue().begin(),
                            set.compliance.uniformValue().end(),
                            [](float value) { return value == 0.0f; }))
                modeMask |= CompiledPlan::ZeroCompliance;
        }
        std::vector<uint32_t> modeWords{
            modeMask, bits(set.enabled.isUniform() ? set.enabled.uniformValue()[0] : 0.0f)};
        for (uint32_t c = 0; c < set.type->parameters; ++c)
            modeWords.push_back(
                bits(set.parameters.isUniform() ? set.parameters.uniformValue()[c] : 0.0f));
        for (uint32_t c = 0; c < set.type->rows; ++c)
            modeWords.push_back(
                bits(set.compliance.isUniform() ? set.compliance.uniformValue()[c] : 0.0f));
        const auto modes = append(buffer(BufferRole::FieldModes), modeWords);
        RelationLayout layout{
            relationFirst,
            set.count,
            type,
            field(buffer(BufferRole::Parameters), set.parameters, uniformParameters),
            field(buffer(BufferRole::Compliance), set.compliance, uniformCompliance),
            field(buffer(BufferRole::History), set.initialHistory, uniformHistory),
            reserve(buffer(BufferRole::Multipliers), uint64_t(set.count) * set.type->rows, 0u),
            modes,
            0};
        p->dynamicTopology = p->dynamicTopology || set.dynamicEndpoints;
        if (set.dynamicEndpoints && policy.mode == SolveMode::Colored)
            throw std::invalid_argument("Dynamic endpoint sets require Hybrid or Jacobi policy");
        p->relations.push_back(layout);
        p->relationFieldModes.push_back({modes, modeMask});
        field(buffer(BufferRole::RelationEnabled), set.enabled, uniformRelationEnabled);
        std::vector<uint32_t> endpointSpaces;
        for (const auto& space : set.type->spaces) {
            auto found = spaceIds.find(signature(*space));
            endpointSpaces.push_back(found == spaceIds.end() ? UINT32_MAX : found->second);
        }
        // Validate the image of each reference column, not every tuple of the
        // product. Affine columns are monotone and retain a single space/set.
        for (const auto& domain : binding.domains) {
            for (uint32_t e = 0; e < domain.fields.size(); ++e) {
                const auto& source = domain.fields[e];
                const auto [first, end] = domain.memberRange(e);
                auto validate = [&](uint32_t member) {
                    const auto ref = source.at(member);
                    if (ref.set >= p->variables.size() || ref.index >= p->variables[ref.set].count)
                        throw std::invalid_argument("Relation references a missing variable: " + set.name);
                    if (p->variables[ref.set].space != endpointSpaces[e])
                        throw std::invalid_argument("Relation endpoint space mismatch: " + set.name);
                };
                const bool affine = source.kind != EndpointSource::Kind::Explicit &&
                    (source.broadcast() || first == end ||
                     uint64_t(source.first.index) + uint64_t(end - 1) * source.stride <= UINT32_MAX);
                if (!affine) {
                    for (uint32_t member = first; member < end; ++member) validate(member);
                } else if (first != end) {
                    validate(first);
                    validate(end - 1);
                }
            }
        }
        for (uint32_t e = 0; e < binding.readOnly.size(); ++e)
            if (binding.readOnly[e])
                p->statistics.eliminatedDerivativeColumns += uint64_t(set.count) * set.type->spaces[e]->tangentSize;
        if (set.compliance.isUniform()) {
            for (auto value : set.compliance.uniformValue())
                if (value < 0)
                    throw std::invalid_argument("Compliance must be nonnegative: " + set.name);
        } else {
            for (uint32_t row = 0; row < set.count; ++row)
                for (uint32_t r = 0; r < set.type->rows; ++r)
                    if (set.compliance.at(row, r) < 0)
                        throw std::invalid_argument("Compliance must be nonnegative: " + set.name);
        }
        p->statistics.endpointReferences += uint64_t(set.count) * set.type->spaces.size();
        relationFirst += set.count;
    }
    if (p->summedRelations && (policy.mode == SolveMode::Colored || p->dynamicTopology))
        throw std::invalid_argument("Collection sums currently require static Hybrid or Jacobi execution");
    const bool activeSums = p->summedRelations && policy.weighting != JacobiWeighting::Static;
    for (uint32_t set = 0; set < p->relations.size(); ++set)
        for (uint32_t domain = 0; domain < p->bindings[set].domains.size(); ++domain)
            if (canDeferCandidateDomain(*p, set, p->bindings[set].domains[domain]))
                p->deferredJacobiDomains.push_back({set, domain});
    InstanceRows instances(*p, relationFirst);
    // The existing incidence walk also proves whether alias merging is needed
    // in a mathematical kernel. Never infer this from initial dynamic endpoints.
    std::vector<bool> distinctWritableEndpoints(p->types.size(), !p->dynamicTopology);
    for (const auto& deferred : p->deferredJacobiDomains)
        distinctWritableEndpoints[p->relations[deferred.set].type] = false;
    auto endpointId = [&](const InstanceRows::Row& instance, uint32_t slot) {
        auto ref = p->bindings[instance.set].at(instance.local, slot);
        return p->variables[ref.set].first + ref.index;
    };
    std::vector<uint32_t> endpointScratch;
    for (const auto& type : p->types)
        endpointScratch.resize(std::max(endpointScratch.size(), type->spaces.size()));
    BindingDomain::Cursor bindingCursor;
    auto writableEndpoints = [&](const InstanceRows::Row& instance, auto&& visit) {
        if (p->types[instance.type]->summedObject() >= 0) return;
        const auto& domain = p->bindings[instance.set].domain(instance.local);
        const auto members = bindingCursor.seek(domain, instance.local - domain.first);
        for (uint32_t endpoint = 0; endpoint < instance.arity; ++endpoint) {
            const auto ref = domain.fields[endpoint].at(endpoint < domain.split ? members.first : members.second);
            const auto id = p->variables[ref.set].first + ref.index;
            bool repeated = false;
            for (uint32_t before = 0; before < endpoint; ++before)
                repeated = repeated || endpointScratch[before] == id;
            endpointScratch[endpoint] = id;
            if (writable[id] && repeated)
                distinctWritableEndpoints[instance.type] = false;
            if (writable[id] && !repeated)
                visit(id);
        }
    };
    std::vector<uint32_t> coloringOrder;
    if (policy.mode != SolveMode::Jacobi && p->deferredJacobiDomains.empty()) {
        // Stable buckets retain the exact priority/row order without sorting a
        // Cartesian number of identical type priorities against one another.
        std::map<std::array<uint32_t, 4>, std::vector<uint32_t>> priorities;
        std::vector<std::array<uint32_t, 4>> typePriority;
        for (uint32_t type = 0; type < p->types.size(); ++type)
            typePriority.push_back({0, UINT32_MAX - p->types[type]->rows,
                UINT32_MAX - uint32_t(p->types[type]->residual.nodes.size()), UINT32_MAX - p->activeTangent(type)});
        coloringOrder.reserve(instances.materializedSize());
        for (const auto& instance : instances) {
            if (p->types[instance.type]->summedObject() >= 0) continue;
            if (model.data->relations[instance.set].dynamicEndpoints)
                continue;
            uint32_t endpoints = 0;
            writableEndpoints(instance, [&](uint32_t) { ++endpoints; });
            auto priority = typePriority[instance.type];
            priority[0] = endpoints;
            priorities[priority].push_back(instance.id);
        }
        for (const auto& [priority, rows] : priorities)
            coloringOrder.insert(coloringOrder.end(), rows.begin(), rows.end());
    }
    // A color consumes one slot at every writable endpoint. Place relations with
    // fewer slots first, then prefer numerically heavier work when slot cost ties.
    // This maximizes useful candidate work under the user's color upper bound.
    std::vector<uint64_t> used(writable.size());
    const uint64_t allowed =
        policy.colorBudget == 64 ? UINT64_MAX : ((uint64_t(1) << policy.colorBudget) - 1);
    for (auto id : coloringOrder) {
        auto instance = instances[id];
        uint64_t conflict = 0;
        writableEndpoints(instance, [&](uint32_t variable) { conflict |= used[variable]; });
        const auto available = allowed & ~conflict;
        if (!available) {
            if (policy.mode == SolveMode::Colored)
                throw std::runtime_error(
                    "Color budget exhausted; choose Hybrid/Jacobi or increase the budget");
            continue;
        }
        uint32_t color = 0;
        while (!(available & (uint64_t(1) << color)))
            ++color;
        instance.color = int8_t(color);
        p->statistics.colors = std::max(p->statistics.colors, color + 1);
        writableEndpoints(instance,
                          [&](uint32_t variable) { used[variable] |= uint64_t(1) << color; });
    }
    p->statistics.candidateColors = p->statistics.colors;
    if (policy.mode == SolveMode::Hybrid && !p->dynamicTopology && p->statistics.colors > 1) {
        std::vector<uint32_t> incidence(writable.size());
        bool hasOverflow = false, sparseInequalityOverflow = true;
        for (const auto& instance : instances) {
            uint32_t endpoints = 0;
            writableEndpoints(instance, [&](uint32_t variable) {
                ++incidence[variable];
                ++endpoints;
            });
            hasOverflow = hasOverflow || (endpoints && instance.color < 0);
            if (endpoints && instance.color < 0)
                sparseInequalityOverflow =
                    sparseInequalityOverflow && p->types[instance.type]->rows == 1 &&
                    p->types[instance.type]->kind != RelationKind::Equality;
        }
        if (hasOverflow) {
            // Gather is already mandatory. Tail colors then add graph-wide
            // synchronization while doing work the Jacobi path can absorb. Retain
            // the shortest prefix that still updates at least half of every
            // variable's static incidence in place; colorBudget remains an upper
            // bound, and models that cannot reach this coverage keep every color.
            std::vector<std::vector<uint32_t>> colorIncidence(p->statistics.colors);
            for (const auto& instance : instances)
                if (instance.color >= 0)
                    writableEndpoints(instance, [&](uint32_t variable) {
                        colorIncidence[uint32_t(instance.color)].push_back(variable);
                    });
            std::vector<uint32_t> covered(writable.size());
            uint32_t selected = p->statistics.colors;
            bool coverageReached = false;
            for (uint32_t color = 0; color < p->statistics.colors; ++color) {
                for (auto variable : colorIncidence[color])
                    ++covered[variable];
                bool sufficient = true;
                for (uint32_t variable = 0; variable < incidence.size(); ++variable)
                    if (uint64_t(covered[variable]) * 2 < incidence[variable]) {
                        sufficient = false;
                        break;
                    }
                if (sufficient) {
                    selected = color + 1;
                    coverageReached = true;
                    break;
                }
            }
            // A bounded prefix that cannot cover half of every endpoint gives no
            // useful in-place convergence floor. If the overflow is entirely made
            // of scalar inequalities, their feasibility guard and sparse direct
            // accumulation make a coherent Jacobi phase cheaper than graph-wide
            // barriers around a weak colored prefix.
            if (!coverageReached && sparseInequalityOverflow &&
                policy.execution == ExecutionMode::Auto)
                selected = 0;
            if (selected < p->statistics.colors) {
                for (auto instance : instances)
                    if (instance.color >= int32_t(selected))
                        instance.color = -1;
                p->statistics.colors = selected;
            }
        }
    }
    // Scalar inequalities often reject most candidates before differentiation.
    // For static Jacobi work, accumulate those sparse corrections directly into
    // each writable variable. Equality blocks and dynamic topology retain the
    // ordered per-relation contribution table.
    std::vector<bool> directJacobi(p->types.size());
    if (!p->dynamicTopology && policy.execution == ExecutionMode::Auto)
        for (uint32_t type = 0; type < p->types.size(); ++type)
            directJacobi[type] =
                p->types[type]->rows == 1 && p->types[type]->kind != RelationKind::Equality;
    for (uint32_t type = 0; type < p->types.size(); ++type)
        if (p->types[type]->summedObject() >= 0) directJacobi[type] = true;
    std::vector<uint32_t> degree(writable.size()), gatheredDegree(writable.size());
    std::vector<bool> directVariables(writable.size());
    for (const auto& instance : instances)
        if (instance.color >= 0)
            ++p->statistics.coloredRelations;
        else {
            writableEndpoints(instance, [&](uint32_t id) {
                ++degree[id];
                if (directJacobi[instance.type])
                    directVariables[id] = true;
                else
                    ++gatheredDegree[id];
            });
            ++p->statistics.jacobiRelations;
            p->statistics.directJacobiRelations += directJacobi[instance.type];
        }
    for (const auto& deferred : p->deferredJacobiDomains) {
        const auto& domain = p->bindings[deferred.set].domains[deferred.domain];
        const auto type = p->relations[deferred.set].type;
        p->statistics.jacobiRelations += domain.count;
        p->statistics.directJacobiRelations += domain.count;
        for (uint32_t slot = 0; slot < domain.fields.size(); ++slot) {
            const auto& source = domain.fields[slot];
            const auto [first, end] = domain.memberRange(slot);
            for (uint32_t member = first; member < end; ++member) {
                const auto ref = source.at(member);
                const auto id = p->variables[ref.set].first + ref.index;
                if (writable[id]) {
                    directVariables[id] = true;
                    degree[id] = std::max(degree[id], 1u);
                }
                if (source.broadcast())
                    break;
            }
        }
        if (!directJacobi[type])
            throw std::logic_error("Deferred candidate domains require sparse scalar Jacobi work");
    }
    auto summed = lowerSumRelations(*p, degree);
    std::vector<bool> sumApplies(writable.size());
    for (const auto& domain : summed)
        for (auto id : domain.appliedVariables) sumApplies[id] = true;
    for (const auto& domain : summed)
        for (auto [id, count] : domain.degrees) {
            degree[id] = checked(uint64_t(degree[id]) + count);
            directVariables[id] = true;
        }
    reserve(buffer(BufferRole::Diagnostics), 2, 0u);
    p->statistics.relations = instances.size();
    std::vector<uint32_t> offsets(writable.size() + 1);
    for (size_t i = 0; i < gatheredDegree.size(); ++i)
        offsets[i + 1] = checked(uint64_t(offsets[i]) + gatheredDegree[i]);
    std::vector<uint32_t> entries(size_t(offsets.back()) * 2), cursor = offsets;
    if (p->statistics.directJacobiRelations) {
        reserve(buffer(BufferRole::Contributions), buffer(BufferRole::Velocity).wordCount(), 0u);
        append(buffer(BufferRole::AdjacencyCursors), degree);
    }
    for (const auto& in : instances)
        if (in.color < 0 && directJacobi[in.type]) {
            uint32_t maximumDegree = 1;
            writableEndpoints(
                in, [&](uint32_t id) { maximumDegree = std::max(maximumDegree, degree[id]); });
            instances.set(in.id, 7, maximumDegree);
        }
    std::vector<std::vector<uint32_t>> gatheredRows(p->relations.size());
    for (const auto& in : instances)
        if (in.color < 0 && !directJacobi[in.type])
            gatheredRows[in.set].push_back(in.id);
    for (uint32_t setId = 0; setId < p->relations.size(); ++setId) {
        const auto& layout = p->relations[setId];
        const auto count = uint32_t(gatheredRows[setId].size());
        if (!count)
            continue;
        auto base = reserve(buffer(BufferRole::Contributions),
                            uint64_t(count) * p->activeTangent(layout.type), 0u);
        uint32_t index = 0;
        for (auto id : gatheredRows[setId]) {
            const auto in = instances[id];
            auto rowBase = base + index++;
            uint32_t local = 0;
            for (uint32_t e = 0; e < in.arity; ++e) {
                auto variable = endpointId(in, e);
                bool repeated = false;
                for (uint32_t before = 0; before < e; ++before)
                    repeated = repeated || endpointId(in, before) == variable;
                if (writable[variable] && !repeated) {
                    auto at = cursor[variable]++;
                    entries[size_t(at) * 2] = rowBase + local * count;
                    entries[size_t(at) * 2 + 1] = count;
                }
                local += p->activeTangent(layout.type, e);
            }
            instances.set(in.id, 5, rowBase);
            instances.set(in.id, 7, count);
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
        if (p->statistics.jacobiRelations) {
            std::vector<uint32_t> gather;
            for (auto variable : variableGroups[space])
                if (writable[variable] && !sumApplies[variable] && (p->dynamicTopology || degree[variable]))
                    gather.push_back(variable);
            if (!gather.empty())
                p->apply.push_back(
                    {operation("Gather " + s.name,
                               variableFunction(
                                   s, "apply", "gather" + suffix,
                                   std::any_of(variableGroups[space].begin(), variableGroups[space].end(),
                                               [&](uint32_t id) { return directVariables[id]; }))),
                     append(buffer(BufferRole::VariableWork), gather), checked(gather.size())});
        }
    }
    std::map<std::pair<int32_t, uint32_t>, std::vector<uint32_t>> groups;
    for (const auto& in : instances)
        if (p->types[in.type]->summedObject() < 0)
            groups[{in.color < 0 ? int32_t(policy.colorBudget) : in.color, in.type}].push_back(in.id);
    std::map<std::pair<uint32_t, bool>, uint32_t> solveKernels;
    std::vector<Batch> sumActivity;
    for (const auto& group : groups) {
        auto type = group.first.second;
        bool jacobi = group.first.first == int32_t(policy.colorBudget);
        auto key = std::make_pair(type, jacobi);
        auto found = solveKernels.find(key);
        uint32_t program;
        if (found == solveKernels.end()) {
            program = operation(std::string(jacobi ? "Jacobi " : "Colored ") + p->types[type]->name,
                                relationFunction(*p->types[type], p->typeReadOnly[type],
                                                 p->typeEndpointMode[type], jacobi,
                                                 jacobi && directJacobi[type],
                                                 p->statistics.directJacobiRelations != 0, false,
                                                 std::string(jacobi ? "jacobi" : "colored") +
                                                     std::to_string(type), jacobi && activeSums, false,
                                                 distinctWritableEndpoints[type]));
            solveKernels.emplace(key, program);
        } else
            program = found->second;
        p->solve.push_back({program, append(buffer(BufferRole::RelationWork), group.second),
                            checked(group.second.size()), jacobi ? -1 : group.first.first});
        if (jacobi && activeSums) {
            auto batch = p->solve.back();
            batch.kernel = operation("Count active relation members", relationActivityFunction(
                *p->types[type], p->typeReadOnly[type], p->typeEndpointMode[type], "activeMembers"));
            sumActivity.push_back(batch);
        }
        if (p->dynamicTopology && jacobi) {
            auto batch = p->solve.back();
            batch.kernel =
                kernel("Count incidence " + p->types[type]->name, incidenceKernel(*p->types[type], p->typeReadOnly[type], p->typeEndpointMode[type], false));
            p->countIncidence.push_back(batch);
            batch.kernel =
                kernel("Scatter incidence " + p->types[type]->name, incidenceKernel(*p->types[type], p->typeReadOnly[type], p->typeEndpointMode[type], true));
            p->scatterIncidence.push_back(batch);
        }
    }
    auto relationDispatch = [&](bool jacobi) {
        std::vector<std::pair<uint32_t, KernelFunction>> typed;
        for (const auto& [key, program] : solveKernels)
            if (key.second == jacobi)
                typed.emplace_back(key.first, functions[program]);
        if (typed.size() < 2)
            return UINT32_MAX;
        auto label = std::string(jacobi ? "Jacobi" : "Colored");
        return operation(label + " relations",
                         relationDispatchFunction(typed, jacobi, label + "Dispatch"));
    };
    p->coloredDispatchKernel = relationDispatch(false);
    p->jacobiDispatchKernel = relationDispatch(true);
    std::vector<Batch> sumStages;
    if (activeSums) {
        reserve(buffer(BufferRole::ActiveDegrees), writable.size(), 0u);
        sumStages.push_back({kernel("Reset active sum incidence",
            "void main(){uint i=invocation();if(i<" + std::to_string(writable.size()) +
            "u)x_activeDegrees[i]=0u;}"), 0, checked(writable.size())});
    }
    std::vector<Batch> sumRefinement, sumFinalRefinement;
    for (const auto& domain : summed) {
        const auto& binding = p->bindings[domain.set].domains[domain.domain];
        const auto first = p->relations[domain.set].first + binding.first;
        for (const auto& [source, count] : domain.bounds)
            p->candidateBounds.push_back({kernel("Bound summed expression", source), 0, count});
        for (const auto& [source, count] : domain.index)
            sumStages.push_back({kernel("Index summed expression", source), 0, count});
        if (!domain.activity.empty())
            sumActivity.push_back({kernel(domain.gather.empty() ? "Count active sum members" : "Linearize summed relation",
                domain.activity), first, domain.activityCount ? domain.activityCount : binding.count});
        p->solve.push_back({kernel("Solve summed relation", domain.solve), first,
            domain.solveCount ? domain.solveCount : binding.count});
        if (!domain.gather.empty()) {
            p->solve.push_back({kernel("Gather summed linearization", domain.gather), first, domain.gatherCount});
            sumRefinement.push_back({kernel("Refine summed linear system", domain.refine), first, domain.solveCount});
            sumRefinement.push_back(p->solve.back());
            sumFinalRefinement.push_back(sumRefinement[sumRefinement.size()-2]);
            auto final = p->solve.back();
            if (!domain.finalGather.empty()) final.kernel = kernel("Gather and apply summed operator",domain.finalGather);
            sumFinalRefinement.push_back(final);
        }
        if (!domain.update.empty())
            p->update.push_back({kernel("Commit summed relation", domain.update), first, binding.count});
    }
    // Sparse linear sweeps reuse the same Jacobian and total tangent correction.
    // They improve the block solve while keeping the nonlinear iteration and
    // integration budgets supplied by the caller intact.
    for (uint32_t sweep = 1; sweep < 4; ++sweep) {
        const auto& stages = sweep == 3 ? sumFinalRefinement : sumRefinement;
        p->solve.insert(p->solve.end(), stages.begin(), stages.end());
    }
    // Colored work changes the snapshot. Build indices and count dependencies
    // after that work, immediately before the common Jacobi solve/apply stage.
    for (const auto& domain : summed)
        for (const auto& [source, count] : domain.assembly)
            sumStages.push_back({kernel("Transpose summed incidence", source), 0, count});
    sumStages.insert(sumStages.end(), sumActivity.begin(), sumActivity.end());
    p->solve.insert(std::find_if(p->solve.begin(), p->solve.end(), [](const Batch& batch) {
        return batch.color < 0;
    }), sumStages.begin(), sumStages.end());
    for (uint32_t type = 0; type < p->types.size(); ++type)
        if (p->types[type]->update && p->types[type]->summedObject() < 0) {
            std::vector<uint32_t> work;
            for (const auto& in : instances)
                if (in.type == type)
                    work.push_back(in.id);
            p->update.push_back(
                {operation("Commit " + p->types[type]->name,
                           relationFunction(*p->types[type], p->typeReadOnly[type],
                                            p->typeEndpointMode[type], false, false, false, true,
                                            "commit" + std::to_string(type))),
                 append(buffer(BufferRole::RelationWork), work), checked(work.size())});
        }
    const std::array<BufferRole, 4> stateFields = {
        BufferRole::Values, BufferRole::Velocity, BufferRole::Acceleration, BufferRole::History};
    uint64_t stateWords = 0;
    for (const auto& field : stateFields)
        stateWords += buffer(field).wordCount();
    const auto stateWriteCapacity = uint32_t(std::min<uint64_t>(stateWords, StateWriteCapacity));
    reserve(buffer(BufferRole::StateWrites), std::max(1u, stateWriteCapacity * 3));
    p->stateWriteKernel = kernel("Scatter Dynamics state", R"(
void main(){uint i=invocation();if(i>=step.count)return;uint record=(step.first+i)*3u;
uint field=x_stateWrites[record],at=x_stateWrites[record+1u];
float value=uintBitsToFloat(x_stateWrites[record+2u]);
if(field==0u)x_q[at]=value;else if(field==1u)x_velocity[at]=value;
else if(field==2u)x_acceleration[at]=value;else x_history[at]=value;}
)");
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
    lowerCandidateDomains(*p);
    // Physical endpoint storage is selected last. Affine object fields with large
    // domains remain compact, and the GPU computes their addresses on demand.
    std::vector<uint32_t> endpointData;
    constexpr uint32_t Indirect = 0x80000000u;
    for (uint32_t setId = 0; setId < p->relations.size(); ++setId) {
        auto& layout = p->relations[setId];
        layout.endpoints = checked(endpointData.size());
        const auto arity = uint32_t(p->types[layout.type]->spaces.size());
        for (uint32_t domainId = 0; domainId < p->bindings[setId].domains.size(); ++domainId) {
            const auto& domain = p->bindings[setId].domains[domainId];
            if (domain.summedObject >= 0) {
                // Member maps and unique-variable incidence live in SumData.
                instances.setDomainEndpoint(setId, domainId, 0);
                continue;
            }
            const auto base = checked(endpointData.size());
            const bool compact = domain.endpointMode(model.data->relations[setId].dynamicEndpoints) > 0;
            if (compact) {
                endpointData.insert(endpointData.end(), {uint32_t(domain.map), layout.first + domain.first,
                    domain.left, domain.right, domain.split});
                for (const auto& source : domain.fields) {
                    endpointData.push_back(p->variables[source.first.set].first + source.first.index);
                    endpointData.push_back(source.broadcast() ? 0 : source.stride);
                }
                p->statistics.implicitEndpointReferences += uint64_t(domain.count) * arity;
                instances.setDomainEndpoint(setId, domainId, base | Indirect);
            } else {
                for (uint32_t row = 0; row < domain.count; ++row) {
                    const auto address = checked(endpointData.size());
                    for (uint32_t slot = 0; slot < arity; ++slot) {
                        const auto ref = domain.at(row, slot);
                        endpointData.push_back(p->variables[ref.set].first + ref.index);
                    }
                    instances.set(layout.first + domain.first + row, 0, address);
                }
            }
            if (endpointData.size() >= Indirect)
                throw std::overflow_error("Endpoint storage exceeds tagged address capacity");
        }
    }
    append(buffer(BufferRole::Endpoints), endpointData);
    p->statistics.endpointStorageWords = endpointData.size();
    // Scheduling has finished inspecting row metadata. Select physical storage
    // now, without constraining the logical domain or its patch/read contract.
    instances.packMetadata(buffer(BufferRole::Relations), p->relationMetadata);
    packMetadata(buffer(BufferRole::Variables), p->variables, p->variableMetadata);
    for (auto role : {BufferRole::Parameters, BufferRole::Compliance, BufferRole::RelationEnabled})
        packField(buffer(role));
    for (auto& b : p->buffers) {
        if (!b.wordCount())
            reserve(b, 1, 0u);
        p->statistics.storageBytes += b.byteSize();
    }
    p->statistics.compileMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return p;
}
} // namespace whimsical::dynamics
