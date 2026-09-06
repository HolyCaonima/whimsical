#pragma once
#include "animation/Animation.h"
#include "OnnxModel.h"
#include <map>

namespace afterlight::animation::ai4animation {
struct Foot {
    std::vector<unsigned> chain;
    unsigned contact = 0;
    // Biped toe chains constrain their reach relative to the preceding ankle chain.
    int ankleFoot = -1;
    bool grounded = true;
};
struct ControllerAsset {
    struct GuidanceRule {
        std::string action, attribute, guidance;
        std::map<std::string, std::string> choices;
    };
    std::vector<EnumAttribute> attributes;
    std::vector<GuidanceRule> actions;
    std::shared_ptr<const Skeleton> skeleton;
    std::shared_ptr<const OnnxModel> network, postprocessor;
    std::map<std::string, std::vector<vec3>> guidances;
    std::vector<unsigned> contactJoints;
    std::vector<Foot> feet;
    unsigned samples = 16;
    bool poseAxes = true;
    float window = .5f, predictionRate = 10, contactPower = 3;
    float trajectoryCorrection = .25f, minTimescale = 1, maxTimescale = 1.5f;
    static std::shared_ptr<const ControllerAsset> load(const std::filesystem::path& directory);
    void validate() const;
    const std::vector<vec3>& guidance(const Context&) const;
};
std::shared_ptr<const Asset> loadAsset(const std::filesystem::path& controllerPath);
struct SequenceFrame {
    Transform root;
    vec3 rootVelocity{0};
    Pose joints; // World space.
    std::vector<vec3> velocities;
    std::vector<float> contacts;
};
struct Sequence {
    float age = 0, window = .5f;
    std::vector<SequenceFrame> frames;
    SequenceFrame sample(float ahead) const;
    float length() const;
    float rootLock() const;
    void rebase(const Transform& from, const Transform& to);
};
// Native port of locomotion Program/Sequence/LegIK. No dependency on World, JS or renderer.
class Controller final : public Solver {
    std::shared_ptr<const ControllerAsset> asset_;
    Sequence current_, previous_;
    std::vector<TrajectoryPoint> simulation_, control_;
    std::vector<vec3> velocities_;
    std::vector<Transform> footTargets_;
    std::vector<float> footBaselines_, footReach_;
    Transform expectedRoot_;
    float elapsed_ = 0, timescale_ = 1, synchronization_ = 0;
    void control(const Context&);
    void predict(const Context&);
    void feet(const Context&, Pose& world, const std::vector<float>& contacts);

  public:
    explicit Controller(std::shared_ptr<const ControllerAsset>);
    void reset(const Context&) override;
    void evaluate(const Context&, Output&) override;
};
// Explicitly shared feature ABI for exporter parity checks and controller use.
std::vector<float> encodePose(const Pose& model, const std::vector<vec3>& localVelocities);
Sequence decodeSequence(const std::vector<float>&, unsigned joints, unsigned samples, float window,
                        const Transform& root);
} // namespace afterlight::animation::ai4animation
