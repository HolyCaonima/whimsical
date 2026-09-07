// Bone restoration and FABRIK adapted from AI4AnimationPy, Meta Platforms, Inc. and affiliates.
// Those adaptations are CC BY-NC 4.0; see external/AI4AnimationPy/LICENSE and docs/animation.md.
#include "Animation.h"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace afterlight::animation {
Transform compose(const Transform& a, const Transform& b) {
    return {a.position + a.rotation * b.position, glm::normalize(a.rotation * b.rotation)};
}
Transform relative(const Transform& a, const Transform& space) {
    auto inv = glm::conjugate(space.rotation);
    return {inv * (a.position - space.position), glm::normalize(inv * a.rotation)};
}
Transform interpolate(const Transform& a, const Transform& b, float w) {
    return {glm::mix(a.position, b.position, w), glm::normalize(glm::slerp(a.rotation, b.rotation, w))};
}
Skeleton::Skeleton(std::vector<Joint> joints) : joints_(std::move(joints)) {
    if (joints_.empty())
        throw std::invalid_argument("Empty animation skeleton");
    std::unordered_set<std::string> names;
    for (size_t i = 0; i < size(); ++i) {
        const auto& j = joints_[i];
        if (j.parent < -1 || j.parent >= int(i) || !names.insert(j.name).second)
            throw std::invalid_argument("Skeleton requires unique names and parent-before-child order");
    }
}
Pose Skeleton::restPose() const {
    Pose p;
    p.reserve(size());
    for (const auto& j : joints_)
        p.push_back(j.rest);
    return p;
}
Pose Skeleton::toModel(const Pose& local) const {
    if (local.size() != size())
        throw std::invalid_argument("Pose/skeleton mismatch");
    Pose result(local);
    for (size_t i = 0; i < size(); ++i)
        if (joints_[i].parent >= 0)
            result[i] = compose(result[joints_[i].parent], local[i]);
    return result;
}
Pose Skeleton::toLocal(const Pose& model) const {
    if (model.size() != size())
        throw std::invalid_argument("Pose/skeleton mismatch");
    Pose result(model);
    for (size_t i = 0; i < size(); ++i)
        if (joints_[i].parent >= 0)
            result[i] = relative(model[i], model[joints_[i].parent]);
    return result;
}
void Skeleton::restoreBones(Pose& model) const {
    // Actor.RestoreBoneLengths/Alignments, with hierarchical length restoration.
    for (size_t i = 0; i < size(); ++i) {
        int p = joints_[i].parent;
        if (p < 0)
            continue;
        vec3 d = model[i].position - model[p].position;
        float length = glm::length(d);
        if (length > 1e-6f)
            model[i].position = model[p].position + d * (glm::length(joints_[i].rest.position) / length);
    }
    for (size_t i = 0; i < size(); ++i) {
        int child = -1, count = 0;
        for (size_t j = i + 1; j < size(); ++j)
            if (joints_[j].parent == int(i)) {
                child = int(j);
                ++count;
            }
        if (count != 1)
            continue;
        vec3 from = model[i].rotation * joints_[child].rest.position;
        vec3 to = model[child].position - model[i].position;
        if (glm::length(from) > 1e-6f && glm::length(to) > 1e-6f)
            model[i].rotation =
                glm::normalize(glm::rotation(glm::normalize(from), glm::normalize(to)) * model[i].rotation);
    }
}
Instance::Instance(std::shared_ptr<const Skeleton> skeleton, std::unique_ptr<Solver> solver,
                   std::vector<EnumAttribute> schema, std::string solverLabel)
    : skeleton_(std::move(skeleton)), solver_(std::move(solver)), schema_(std::move(schema)),
      solverLabel_(std::move(solverLabel)) {
    if (!skeleton_ || !solver_)
        throw std::invalid_argument("Animation needs a skeleton and solver");
    for (const auto& attribute : schema_) {
        if (attributes_.count(attribute.key))
            throw std::invalid_argument("Duplicate animation attribute: " + attribute.key);
        std::unordered_set<std::string> values;
        for (const auto& option : attribute.options)
            if (!values.insert(option.value).second)
                throw std::invalid_argument("Duplicate animation attribute option: " + option.value);
        setAttribute(attribute.key, attribute.defaultValue);
    }
    reset();
}
Instance::Instance(const Asset& asset)
    : Instance(asset.skeleton(), asset.createSolver(), asset.attributes(), asset.solverLabel()) {}
void Instance::setAttribute(const std::string& key, const std::string& value) {
    auto attribute =
        std::find_if(schema_.begin(), schema_.end(), [&](const EnumAttribute& a) { return a.key == key; });
    if (attribute == schema_.end())
        throw std::invalid_argument("Unsupported animation attribute: " + key);
    auto option = std::find_if(attribute->options.begin(), attribute->options.end(),
                               [&](const EnumOption& o) { return o.value == value; });
    if (option == attribute->options.end())
        throw std::invalid_argument("Unsupported value for animation attribute " + key + ": " + value);
    attributes_[key] = value;
}
void Instance::setAttributes(const AttributeValues& values) {
    AttributeValues draft;
    for (const auto& attribute : schema_)
        draft[attribute.key] = attribute.defaultValue;
    for (const auto& [key, value] : values) {
        auto attribute =
            std::find_if(schema_.begin(), schema_.end(), [&](const auto& a) { return a.key == key; });
        if (attribute == schema_.end() ||
            std::none_of(attribute->options.begin(), attribute->options.end(),
                         [&](const auto& option) { return option.value == value; }))
            throw std::invalid_argument("Unsupported animation attribute: " + key + "=" + value);
        draft[key] = value;
    }
    attributes_ = std::move(draft);
}
void Instance::setSolver(std::unique_ptr<Solver> solver) {
    if (!solver)
        throw std::invalid_argument("Missing animation solver");
    solver_ = std::move(solver);
    reset_ = true;
}
void Instance::reset() {
    output_ = {};
    output_.localPose = skeleton_->restPose();
    reset_ = true;
}
Output Instance::prepare(float dt, Transform root, const Input& input) {
    if (!std::isfinite(dt) || dt <= 0)
        throw std::invalid_argument("Animation dt must be positive");
    Context c{dt, root, *skeleton_, output_.localPose, input, attributes_};
    try {
        if (reset_) {
            solver_->reset(c);
            reset_ = false;
        }
        Output next;
        solver_->evaluate(c, next);
        if (next.localPose.size() != skeleton_->size())
            throw std::runtime_error("Solver produced wrong pose size");
        auto valid = [](const Transform& p) {
            float q = glm::dot(p.rotation, p.rotation);
            if (!std::isfinite(glm::length(p.position)) || !std::isfinite(q) || std::abs(q - 1) > .001f)
                throw std::runtime_error("Solver produced non-rigid pose");
        };
        valid(next.rootMotion);
        for (const auto& p : next.localPose)
            valid(p);
        return next;
    } catch (...) {
        reset_ = true; // Solver history is rebuilt from the last accepted pose on retry.
        throw;
    }
}
const Output& Instance::evaluate(float dt, Transform root, const Input& input) {
    accept(prepare(dt, root, input));
    return output_;
}
void solveFabrik(const Skeleton& skeleton, Pose& model, const std::vector<unsigned>& chain, vec3 target,
                 quat rotation, unsigned iterations, float tolerance, const vec3* pole, float poleWeight) {
    if (chain.size() < 2)
        throw std::invalid_argument("IK chain requires at least two joints");
    std::vector<vec3> positions;
    std::vector<float> lengths(chain.size(), 0);
    for (size_t i = 0; i < chain.size(); ++i) {
        if (chain[i] >= model.size() || (i && skeleton.joints()[chain[i]].parent != int(chain[i - 1])))
            throw std::invalid_argument("IK chain is not contiguous");
        positions.push_back(model[chain[i]].position);
        if (i)
            lengths[i] = glm::distance(positions[i - 1], positions[i]);
    }
    auto direction = [](vec3 v) {
        float l = glm::length(v);
        return l > 1e-6f ? v / l : vec3(0);
    };
    for (unsigned k = 0; k < iterations; ++k) {
        positions.back() = target;
        for (size_t i = chain.size() - 2; i > 0; --i)
            positions[i] = positions[i + 1] + lengths[i + 1] * direction(positions[i] - positions[i + 1]);
        for (size_t i = 1; i < chain.size(); ++i)
            positions[i] = positions[i - 1] + lengths[i] * direction(positions[i] - positions[i - 1]);
        if (pole && poleWeight > 0) {
            vec3 axis = direction(target - positions[0]);
            vec3 p = *pole - positions[0];
            p = direction(p - glm::dot(p, axis) * axis);
            for (size_t i = 1; i + 1 < chain.size(); ++i) {
                vec3 v = positions[i] - positions[0], q = direction(v - glm::dot(v, axis) * axis);
                if (glm::length(p) * glm::length(q) * glm::length(axis) < .5f)
                    continue;
                float angle = std::atan2(glm::dot(axis, glm::cross(q, p)), glm::dot(q, p));
                positions[i] = positions[0] + glm::angleAxis(angle * poleWeight, axis) * v;
            }
        }
        if (glm::distance(positions.back(), target) < tolerance)
            break;
    }
    // Apply via local transforms so non-chain descendants follow the corrected joints.
    Pose local = skeleton.toLocal(model);
    for (size_t i = 0; i < chain.size(); ++i) {
        unsigned joint = chain[i];
        Transform value = model[joint];
        value.position = positions[i];
        if (i + 1 == chain.size())
            value.rotation = rotation;
        else {
            vec3 from = value.rotation * skeleton.joints()[chain[i + 1]].rest.position;
            vec3 to = positions[i + 1] - positions[i];
            if (glm::length(from) > 1e-6f && glm::length(to) > 1e-6f)
                value.rotation =
                    glm::normalize(glm::rotation(glm::normalize(from), glm::normalize(to)) * value.rotation);
        }
        int parent = skeleton.joints()[joint].parent;
        local[joint] = parent < 0 ? value : relative(value, model[parent]);
        model = skeleton.toModel(local);
    }
}
} // namespace afterlight::animation
