#pragma once
#include "physics/dynamics/model/Model.h"
#include "BindingIR.h"
#include <algorithm>
#include <array>

namespace whimsical::dynamics {
enum class SolveMode { Colored, Jacobi, Hybrid };
enum class ExecutionMode { Auto, Global };
enum class JacobiWeighting { Auto, Static, Active };
struct SolverPolicy {
    uint32_t substeps = 4, iterations = 4;
    SolveMode mode = SolveMode::Hybrid;
    uint32_t colorBudget = 32; // bounded coloring; Hybrid sends overflow to gather Jacobi
    float relaxation = 1;
    ExecutionMode execution = ExecutionMode::Auto;
    // Controls for global static Auto plans; local/dynamic/Global execution keeps
    // its original numerical path. No additional modeling DSL input is required.
    JacobiWeighting weighting = JacobiWeighting::Auto;
    bool spatialCandidates = true; // permit cost-selected indexing
};
enum class BufferRole : uint32_t {
    Values,
    Previous,
    Velocity,
    Metric,
    FieldModes,
    Acceleration,
    Variables,
    VariableWork,
    Relations,
    Endpoints,
    Parameters,
    Compliance,
    History,
    Multipliers,
    VariableEnabled,
    RelationEnabled,
    RelationWork,
    Contributions,
    AdjacencyOffsets,
    AdjacencyEntries,
    Diagnostics,
    ScanScratch,
    AdjacencyCursors,
    RegionRanges,
    RegionState,
    LocalOffsets,
    StateWrites,
    Candidates,
    ActiveDegrees,
    Count
};
constexpr size_t BufferCount = size_t(BufferRole::Count);
struct BufferData {
    struct Fill {
        uint64_t first = 0, count = 0; // 32-bit words
        uint32_t value = 0;
    };
    const char* name;
    bool integers;
    std::vector<uint8_t> initial;
    uint64_t words = 0;
    std::vector<Fill> fills;
    uint64_t wordCount() const {
        return std::max<uint64_t>(words, initial.size() / 4);
    }
    uint64_t byteSize() const {
        return wordCount() * 4;
    }
};
struct VariableLayout {
    uint32_t first, count, space, values, velocity, metric, modes;
};
struct RelationLayout {
    uint32_t first, count, type, parameters, compliance, history, multipliers, modes;
    uint32_t endpoints = 0;
};
struct FieldModeLayout {
    uint32_t first = 0, mask = 0;
};
struct Kernel {
    std::string name, source; // local_size and main; runtime supplies the resource interface
};
struct Batch {
    uint32_t kernel, first, count;
    int32_t color = -1;
};
struct DeferredJacobiDomain {
    uint32_t set = 0, domain = 0;
};
struct PlanStatistics {
    uint64_t variables = 0, relations = 0, endpointReferences = 0;
    uint64_t coloredRelations = 0, jacobiRelations = 0, incidenceEntries = 0;
    uint64_t directJacobiRelations = 0;
    uint32_t candidateDomains = 0;
    uint64_t candidateRelations = 0, activeJacobiRelations = 0, candidateQueueCapacity = 0;
    uint32_t colors = 0, candidateColors = 0;
    uint64_t storageBytes = 0;
    uint32_t components = 0, localRegions = 0;
    uint32_t localSharedBytes = 0;
    uint64_t localVariables = 0, localRelations = 0;
    uint32_t colorWindows = 0;
    uint64_t colorWindowRegions = 0;
    uint64_t referenceDispatches = 0, dispatches = 0; // per tick, excluding topology/transfers
    uint64_t bindingDomains = 0, implicitEndpointReferences = 0, endpointStorageWords = 0;
    uint64_t eliminatedDerivativeColumns = 0;
    double compileMilliseconds = 0;
};
// Immutable row metadata can use arithmetic addressing independently of the
// logical domains and the mutable fields indexed by those domains.
template <size_t Width> struct MetadataRange {
    uint32_t first = 0, count = 0, offset = 0;
    bool affine = false;
    std::array<uint32_t, Width> base{}, stride{};
};
// No device, resource IDs or submission state. Instance count affects work tables
// and execution mapping, without a graph or GPU resource per variable/relation.
struct CompiledPlan {
    ModelSnapshot model;
    SolverPolicy policy;
    std::array<BufferData, BufferCount> buffers;
    std::vector<SpaceRef> spaces;
    std::vector<RelationRef> types;
    std::vector<VariableLayout> variables;
    std::vector<RelationLayout> relations;
    // Large, provably bounded logical domains can enter candidate lowering
    // without first materializing every relation row in CPU scheduling tables.
    std::vector<DeferredJacobiDomain> deferredJacobiDomains;
    // Final physical metadata layout. Logical row IDs and mutable fields keep
    // their public indexing; affine metadata columns become address arithmetic.
    using RelationMetadata = MetadataRange<10>;
    std::vector<RelationMetadata> relationMetadata;
    std::vector<MetadataRange<6>> variableMetadata;
    std::vector<FieldModeLayout> variableFieldModes, relationFieldModes;
    // High-level index maps survive analysis, scheduling and GPU lowering.
    std::vector<BindingIR> bindings;
    std::vector<std::vector<bool>> typeReadOnly;
    std::vector<int32_t> typeEndpointMode;
    uint32_t relationSet(uint32_t relation) const;
    uint32_t relationType(uint32_t relation) const;
    uint32_t variableSet(uint32_t variable) const;
    bool variableReadOnly(uint32_t variable) const;
    uint32_t endpoint(uint32_t relation, uint32_t slot) const;
    uint32_t activeTangent(uint32_t type, uint32_t slot) const;
    uint32_t activeTangent(uint32_t type) const;
    std::vector<Kernel> kernels;
    std::vector<Batch> predict, solve, apply, recover, update;
    std::vector<Batch> prepareCandidates; // once per tick, before prediction
    std::vector<Batch> candidateBounds; // cached until relation parameters change
    // Closed regions execute the same substep/iteration schedule inside a workgroup.
    std::vector<Batch> local;
    bool localPerSubstep = false;
    uint32_t stateWriteKernel = 0;
    // Type-dispatch kernels let Schedule collapse all mathematical relation types
    // in one dependency color without changing their model definitions.
    uint32_t coloredDispatchKernel = UINT32_MAX, jacobiDispatchKernel = UINT32_MAX;
    bool dynamicTopology = false;
    std::vector<Batch> countIncidence, scatterIncidence;
    struct ScanLevel {
        uint32_t first, count;
    };
    std::vector<ScanLevel> scanLevels;
    uint32_t resetTopologyKernel = 0, scanKernel = 0, addOffsetsKernel = 0;
    PlanStatistics statistics;
    // The native-independent kernel ABI. Values in push constants are per dispatch.
    static constexpr uint32_t ConstantBytes = 32;
    static constexpr uint32_t UniformEnabled = 1u;
    static constexpr uint32_t IdentityMetric = 2u;
    static constexpr uint32_t UniformParameters = 2u;
    static constexpr uint32_t UniformCompliance = 4u;
    static constexpr uint32_t EnabledValue = 8u;
    static constexpr uint32_t ZeroCompliance = 16u;
    std::string interface() const;
};
using PlanRef = std::shared_ptr<const CompiledPlan>;
struct StateMigration {
    // Triples (field, destination word, source word); q, velocity, history, acceleration.
    std::vector<uint32_t> words;
};
class Compiler {
  public:
    PlanRef compile(const ModelSnapshot&, const SolverPolicy& = {}) const;
    StateMigration migration(const CompiledPlan& previous, const CompiledPlan& next) const;
};
} // namespace whimsical::dynamics
