#pragma once
#include "physics/dynamics/compiler/CompiledPlan.h"
#include "renderCore/RenderCore.h"
#include "State.h"

namespace whimsical::dynamics {
class PublishedState {
    struct Storage;
    std::shared_ptr<Storage> storage_;
    friend class Instance;

  public:
    CompletedState completed;
    PlanRef plan;
    // GPU-owner-thread operation; target is a read-only Imported storage buffer.
    // The consumer retains the snapshot until it unbinds/replaces the import.
    void import(rc::GraphContext&, rg::ResourceId, StateField) const;
};

// GPU-owner-thread object. It neither owns an application thread nor talks to World,
// scripts or rendering. Numeric input is batched; readback is explicit and ranged.
class Instance {
    struct Storage;
    std::unique_ptr<Storage> storage_;
    rc::RenderCore& core_;
    ModelSnapshot model_;
    CompletedState completed_, submitted_;
    bool pending_ = false;
    std::vector<StateRange> submittedReads_;
    std::vector<std::vector<float>> samples_;
    void idle() const;
    void complete();

  public:
    Instance(rc::RenderCore&, PlanRef);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    const CompiledPlan& plan() const;
    const CompletedState& completed() const {
        return completed_;
    }
    bool pending() const {
        return pending_;
    }
    void step(const TickInput&);
    bool poll();
    std::vector<std::vector<float>> takeSamples() {
        return std::move(samples_);
    }
    const CompletedState& wait();
    // Numeric-only commit retains programs/layout/state. Topology changes use install.
    void apply(const ModelCommit&);
    // Explicit completion boundary. Retained state migrates GPU-to-GPU by stable
    // set/index identities; changed relation endpoints reset only their history.
    void install(PlanRef);
    std::vector<float> read(StateField, SetId, uint32_t first, uint32_t count);
    // Multiple disjoint ranges share one GPU submission and completion boundary.
    std::vector<std::vector<float>> read(const std::vector<StateRange>&);
    // Explicit immutable GPU copy; subsequent ticks cannot overwrite reader state.
    // RenderCore must outlive snapshots and consuming contexts.
    PublishedState publish();
    std::optional<GpuProfile> takeProfile();
};
} // namespace whimsical::dynamics
