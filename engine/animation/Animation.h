#pragma once
#include "core/Math.h"
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace afterlight::animation {
// Metres, Y up, +Z forward. Rigid transforms; a pose is parent-local.
struct Transform {
    vec3 position{0};
    quat rotation{1, 0, 0, 0};
};
Transform compose(const Transform&, const Transform&);
Transform relative(const Transform&, const Transform& space);
Transform interpolate(const Transform&, const Transform&, float);
struct Joint {
    std::string name;
    int parent = -1;
    Transform rest;
};
using Pose = std::vector<Transform>;
class Skeleton {
    std::vector<Joint> joints_;

  public:
    explicit Skeleton(std::vector<Joint>);
    const std::vector<Joint>& joints() const {
        return joints_;
    }
    size_t size() const {
        return joints_.size();
    }
    Pose restPose() const;
    Pose toModel(const Pose&) const;
    Pose toLocal(const Pose&) const;
    void restoreBones(Pose& model) const;
};
struct TrajectoryPoint {
    float time = 0;
    Transform transform; // World space, relative time in seconds.
    vec3 velocity{0};
};
struct Input {
    vec3 desiredVelocity{0}; // World space.
    vec3 facing{0, 0, 1};
    std::string action = "Idle";
    float speedScale = 1;
    std::vector<TrajectoryPoint> trajectory; // Optional authored/planned future.
};
struct Contact {
    unsigned joint = 0;
    float weight = 0;
};
struct Event {
    std::string name;
    float value = 0;
};
struct Output {
    Pose localPose;
    Transform rootMotion; // Delta in the current actor-root frame.
    std::vector<Contact> contacts;
    std::vector<Event> events;
};
// Asset-declared presentation controls. This first version supports enum values;
// it is deliberately not an untyped gameplay blackboard.
struct EnumOption {
    std::string value, label;
};
struct EnumAttribute {
    std::string key, label, defaultValue;
    std::vector<EnumOption> options;
};
using AttributeValues = std::map<std::string, std::string>;
struct Context {
    float dt;
    Transform root; // Actual root after gameplay/physics, never a predicted root.
    const Skeleton& skeleton;
    const Pose& pose;
    const Input& input;
    const AttributeValues& attributes;
};
// Peer implementations: neural sequence, clip graph, motion matching, etc.
// Weights/databases belong to immutable assets; playback state belongs to a solver instance.
class Solver {
  public:
    virtual ~Solver() = default;
    virtual void reset(const Context&) = 0;
    virtual void evaluate(const Context&, Output&) = 0;
};
class Asset {
  public:
    virtual ~Asset() = default;
    virtual std::shared_ptr<const Skeleton> skeleton() const = 0;
    virtual std::unique_ptr<Solver> createSolver() const = 0;
    virtual std::string solverLabel() const {
        return "Custom";
    }
    virtual std::vector<EnumAttribute> attributes() const {
        return {};
    }
};
class Instance {
    std::shared_ptr<const Skeleton> skeleton_;
    std::unique_ptr<Solver> solver_;
    Output output_;
    std::vector<EnumAttribute> schema_;
    AttributeValues attributes_;
    std::string solverLabel_;
    bool reset_ = true;

  public:
    Instance(std::shared_ptr<const Skeleton>, std::unique_ptr<Solver>, std::vector<EnumAttribute> schema = {},
             std::string solverLabel = "Custom");
    explicit Instance(const Asset&);
    void setAttribute(const std::string& key, const std::string& value);
    const std::vector<EnumAttribute>& schema() const {
        return schema_;
    }
    const AttributeValues& attributes() const {
        return attributes_;
    }
    const std::string& solverLabel() const {
        return solverLabel_;
    }
    void setSolver(std::unique_ptr<Solver>); // Preserve pose, reset the new solver's history.
    void reset();
    const Output& evaluate(float dt, Transform actualRoot, const Input&);
    const Skeleton& skeleton() const {
        return *skeleton_;
    }
    const Output& output() const {
        return output_;
    }
};
// Generic pose post-process usable by any solver. Chain must follow direct parent links.
void solveFabrik(const Skeleton&, Pose& model, const std::vector<unsigned>& chain, vec3 target, quat rotation,
                 unsigned iterations = 10, float tolerance = .001f, const vec3* pole = nullptr,
                 float poleWeight = 1);
} // namespace afterlight::animation
