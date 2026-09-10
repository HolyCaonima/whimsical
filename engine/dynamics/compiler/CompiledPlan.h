#pragma once
#include "dynamics/model/Model.h"
#include <array>

namespace whimsical::dynamics {
enum class SolveMode { Colored, Jacobi, Hybrid };
enum class ExecutionMode { Auto, Global };
struct SolverPolicy {
    uint32_t substeps = 4, iterations = 4;
    SolveMode mode = SolveMode::Hybrid;
    uint32_t colorBudget = 32; // bounded coloring; Hybrid sends overflow to gather Jacobi
    float relaxation = 1;
    ExecutionMode execution = ExecutionMode::Auto;
};
enum class BufferRole : uint32_t {
    Values,
    Previous,
    Velocity,
    Metric,
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
    Count
};
constexpr size_t BufferCount = size_t(BufferRole::Count);
struct BufferData {
    const char* name;
    bool integers;
    std::vector<uint8_t> initial;
};
struct VariableLayout {
    uint32_t first, count, space, values, velocity, metric;
};
struct RelationLayout {
    uint32_t first, count, type, parameters, compliance, history, multipliers;
    uint32_t endpoints = 0;
};
struct Kernel {
    std::string name, source; // local_size and main; runtime supplies the resource interface
};
struct Batch {
    uint32_t kernel, first, count;
    int32_t color = -1;
};
struct PlanStatistics {
    uint64_t variables = 0, relations = 0, endpointReferences = 0;
    uint64_t coloredRelations = 0, jacobiRelations = 0, incidenceEntries = 0;
    uint32_t colors = 0;
    uint64_t storageBytes = 0;
    uint32_t components = 0, localRegions = 0;
    uint32_t localSharedBytes = 0;
    uint64_t localVariables = 0, localRelations = 0;
    uint64_t referenceDispatches = 0, dispatches = 0; // per tick, excluding topology/transfers
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
    std::vector<Kernel> kernels;
    std::vector<Batch> predict, solve, apply, recover, update;
    // Closed regions execute the same substep/iteration schedule inside a workgroup.
    std::vector<Batch> local;
    uint32_t resetKernel = 0;
    uint32_t multiplierCount = 0;
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
