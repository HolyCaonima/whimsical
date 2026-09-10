#pragma once
#include "ecs/Systems.h"
#include "physics/dynamics/adapter/Mailbox.h"
#include "physics/dynamics/adapter/Document.h"

namespace whimsical {
class World;
class ComponentCatalog;
namespace dynamics {
// One component owns a whole batch, independently of the number of variables or relations.
struct SceneModel {
    Json value;
};
struct SceneBinding {
    Json value;
};
struct SampleRange {
    SetId set;
    uint32_t first, count;
};
struct Sample {
    SampleRange range;
    std::vector<float> values;
};
struct SceneInstance;
struct DynamicsModel {
    using Ownership = SystemComponent;
    std::shared_ptr<SceneInstance> instance;
};
struct DynamicsBinding {
    using Ownership = SystemComponent;
    SceneBinding document;
    std::string model;
    bool input = false;
    std::vector<VariableRef> variables;
    Formula mapping;
    StateField field = StateField::Value;
    float interpolation = 0, age = 0;
    uint64_t sampledModel = 0, sampledTick = 0;
    bool initialized = false;
    TransformPose from, target;
};
class DynamicsSystem {
    SceneStorage& storage_;
    TransformSystem& transforms_;
    std::shared_ptr<Mailbox> mailbox_ = std::make_shared<Mailbox>();

  public:
    DynamicsSystem(SceneStorage& s, TransformSystem& t) : storage_(s), transforms_(t) {}
    const std::shared_ptr<Mailbox>& mailbox() const {
        return mailbox_;
    }
    bool update(World&, float dt); // Returns whether GPU completions were consumed.
    Json state(Entity) const;
    std::vector<float> values(Entity, SetId, uint32_t first, uint32_t count) const;
    void control(Entity, bool paused, bool step);
    void write(Entity, StateWrite);
    void patch(Entity, FieldKind, SetId, uint32_t first, const std::vector<float>&);
};
void registerSceneComponents(ComponentCatalog&);
} // namespace dynamics
} // namespace whimsical
