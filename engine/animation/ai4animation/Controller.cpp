// Adapted from AI4AnimationPy (Meta Platforms, Inc. and affiliates), CC BY-NC 4.0.
// See external/AI4AnimationPy/LICENSE and docs/animation.md for source mapping and changes.
#include "Controller.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace whimsical::animation::ai4animation {
static vec3 unit(vec3 v) {
    float l = glm::length(v);
    return l > 1e-6f ? v / l : vec3(0);
}
static quat look(vec3 z, vec3 y = {0, 1, 0}) {
    // Project noisy neural axes onto a rigid frame before publishing to engine consumers.
    z = unit(z);
    vec3 x = unit(glm::cross(y, z));
    y = glm::cross(z, x);
    if (glm::length(x) < .5f)
        throw std::runtime_error("Neural model emitted degenerate rotation axes");
    return glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
}
static float smooth(float a, float b, float dt, float rate) {
    float v = glm::mix(a, b, 1 - std::exp(-dt * rate));
    return std::abs(v - b) < .01f ? b : v;
}
static vec3 smooth(vec3 a, vec3 b, float dt, float rate) {
    vec3 v = glm::mix(a, b, 1 - std::exp(-dt * rate));
    return glm::distance(v, b) < .01f ? b : v;
}
static void feed(std::vector<float>& x, vec3 v) {
    x.insert(x.end(), {v.x, v.y, v.z});
}
static void feedXZ(std::vector<float>& x, vec3 v) {
    x.insert(x.end(), {v.x, v.z});
}
std::vector<float> encodePose(const Pose& model, const std::vector<vec3>& velocities) {
    std::vector<float> x;
    x.reserve(model.size() * 12);
    for (const auto& p : model)
        feed(x, p.position);
    for (const auto& p : model)
        feed(x, p.rotation * vec3(0, 0, 1));
    for (const auto& p : model)
        feed(x, p.rotation * vec3(0, 1, 0));
    for (auto v : velocities)
        feed(x, v);
    return x;
}
Sequence decodeSequence(const std::vector<float>& y, unsigned joints, unsigned samples, float window,
                        const Transform& root) {
    size_t stride = 3 + 12 * size_t(joints);
    if (samples < 2 || window <= 0 || y.size() != samples * stride)
        throw std::invalid_argument("Neural sequence shape mismatch");
    Sequence s;
    s.window = window;
    vec3 accumulated(0);
    for (unsigned i = 0; i < samples; ++i) {
        const float* row = y.data() + i * stride;
        auto read = [&](size_t offset) { return vec3(row[offset], row[offset + 1], row[offset + 2]); };
        vec3 delta = read(0);
        if (i)
            accumulated += delta; // Upstream uses additive deltas in the prediction root frame.
        SequenceFrame f;
        f.root = compose(root, {{accumulated.x, 0, accumulated.z},
                                glm::angleAxis(glm::radians(accumulated.y), vec3(0, 1, 0))});
        f.rootVelocity = f.root.rotation * vec3(delta.x, 0, delta.z) * (float(samples - 1) / window);
        for (unsigned j = 0; j < joints; ++j) {
            Transform pose{read(3 + 3 * j), look(read(3 + 3 * j + 3 * joints), read(3 + 3 * j + 6 * joints))};
            f.joints.push_back(compose(f.root, pose));
            f.velocities.push_back(f.root.rotation * read(3 + 3 * j + 9 * joints));
        }
        s.frames.push_back(std::move(f));
    }
    return s;
}
SequenceFrame Sequence::sample(float ahead) const {
    float index = std::clamp((age + ahead) / window, 0.f, 1.f) * float(frames.size() - 1);
    size_t a = size_t(std::floor(index)), b = size_t(std::ceil(index));
    float w = index - float(a);
    SequenceFrame result;
    result.root = interpolate(frames[a].root, frames[b].root, w);
    result.rootVelocity = glm::mix(frames[a].rootVelocity, frames[b].rootVelocity, w);
    for (size_t j = 0; j < frames[a].joints.size(); ++j) {
        result.joints.push_back(interpolate(frames[a].joints[j], frames[b].joints[j], w));
        result.velocities.push_back(glm::mix(frames[a].velocities[j], frames[b].velocities[j], w));
    }
    for (size_t j = 0; j < frames[a].contacts.size(); ++j)
        result.contacts.push_back(glm::mix(frames[a].contacts[j], frames[b].contacts[j], w));
    return result;
}
float Sequence::length() const {
    float v = 0;
    for (size_t i = 1; i < frames.size(); ++i)
        v += glm::distance(frames[i - 1].root.position, frames[i].root.position);
    return v;
}
float Sequence::rootLock() const {
    float sum = 0;
    size_t count = 0;
    for (const auto& f : frames)
        for (float v : f.contacts) {
            sum += v;
            ++count;
        }
    return count && sum / float(count) > .75f ? 1.f : 0.f;
}
void Sequence::rebase(const Transform& from, const Transform& to) {
    quat delta = to.rotation * glm::conjugate(from.rotation);
    for (auto& f : frames) {
        f.root = compose(to, relative(f.root, from));
        f.rootVelocity = delta * f.rootVelocity;
        for (auto& p : f.joints)
            p = compose(to, relative(p, from));
        for (auto& v : f.velocities)
            v = delta * v;
    }
}
Controller::Controller(std::shared_ptr<const ControllerAsset> asset) : asset_(std::move(asset)) {
    if (!asset_)
        throw std::invalid_argument("Missing AI4Animation asset");
    asset_->validate();
}
void Controller::reset(const Context& c) {
    if (c.skeleton.size() != asset_->skeleton->size())
        throw std::invalid_argument("Controller skeleton mismatch");
    for (size_t i = 0; i < c.skeleton.size(); ++i)
        if (c.skeleton.joints()[i].name != asset_->skeleton->joints()[i].name ||
            c.skeleton.joints()[i].parent != asset_->skeleton->joints()[i].parent)
            throw std::invalid_argument("Controller bone order mismatch");
    current_ = {};
    previous_ = {};
    elapsed_ = 0;
    timescale_ = 1;
    synchronization_ = 0;
    expectedRoot_ = c.root;
    velocities_.assign(c.skeleton.size(), vec3(0));
    simulation_.resize(asset_->samples);
    for (unsigned i = 0; i < asset_->samples; ++i)
        simulation_[i] = {float(i) * asset_->window / float(asset_->samples - 1), c.root, vec3(0)};
    control_ = simulation_;
    auto model = c.skeleton.toModel(c.pose);
    footTargets_.clear();
    footBaselines_.clear();
    footReach_.clear();
    for (const auto& f : asset_->feet) {
        auto p = compose(c.root, model[f.chain.back()]);
        footTargets_.push_back(p);
        footBaselines_.push_back(p.position.y);
        footReach_.push_back(f.ankleFoot < 0 ? 0
                                             : glm::distance(p.position, footTargets_[f.ankleFoot].position));
    }
}
void Controller::control(const Context& c) {
    const auto& a = *asset_;
    vec3 velocity = c.input.desiredVelocity * c.input.speedScale;
    vec3 direction = c.input.facing;
    direction.y = 0;
    if (glm::length(direction) < 1e-6f)
        direction =
            glm::length(velocity) > 1e-6f ? velocity : simulation_[0].transform.rotation * vec3(0, 0, 1);
    auto rotation = look(direction);
    auto& first = simulation_[0];
    first.velocity = smooth(first.velocity, velocity, c.dt, 10);
    first.transform.position =
        glm::mix(first.transform.position, c.root.position, synchronization_) + first.velocity * c.dt;
    first.transform.rotation = glm::slerp(first.transform.rotation, rotation, 1 - std::exp(-c.dt * 10));
    float step = a.window / float(a.samples - 1);
    for (unsigned i = 1; i < a.samples; ++i) {
        float ratio = float(i) / float(a.samples - 1);
        simulation_[i].velocity = smooth(simulation_[i - 1].velocity, velocity, step, ratio * 10);
        simulation_[i].transform = {simulation_[i - 1].transform.position + simulation_[i].velocity * step,
                                    glm::slerp(first.transform.rotation, rotation, ratio)};
    }
    control_ = simulation_;
    if (!c.input.trajectory.empty()) {
        const auto& path = c.input.trajectory;
        for (size_t i = 1; i < path.size(); ++i)
            if (path[i].time <= path[i - 1].time)
                throw std::invalid_argument("Trajectory times must increase");
        for (auto& p : control_) {
            auto it = std::lower_bound(path.begin(), path.end(), p.time,
                                       [](const TrajectoryPoint& q, float t) { return q.time < t; });
            if (it == path.begin()) {
                p.transform = it->transform;
                p.velocity = it->velocity;
            } else if (it == path.end()) {
                p.transform = path.back().transform;
                p.velocity = path.back().velocity;
            } else {
                auto& before = *(it - 1);
                float w = (p.time - before.time) / (it->time - before.time);
                p.transform = interpolate(before.transform, it->transform, w);
                p.velocity = glm::mix(before.velocity, it->velocity, w);
            }
        }
        simulation_ = control_;
    }
    if (current_.frames.empty())
        return;
    for (unsigned i = 0; i < a.samples; ++i)
        control_[i].transform =
            interpolate(simulation_[i].transform, current_.frames[i].root, a.trajectoryCorrection);
    for (unsigned i = 0; i < a.samples; ++i) {
        vec3 sum(0);
        float time = 0;
        for (unsigned j = i; j < a.samples; ++j) {
            sum += control_[j].transform.position - c.root.position;
            time += control_[j].time;
        }
        control_[i].velocity = glm::mix(sum / time, current_.frames[i].rootVelocity, a.trajectoryCorrection);
    }
}
void Controller::predict(const Context& c) {
    const auto& a = *asset_;
    auto model = c.skeleton.toModel(c.pose);
    std::vector<vec3> localVelocities;
    for (auto v : velocities_)
        localVelocities.push_back(glm::conjugate(c.root.rotation) * v);
    auto poseFeatures = encodePose(model, localVelocities), x = poseFeatures;
    if (!a.poseAxes) {
        x.clear();
        for (const auto& p : model)
            feed(x, p.position);
        for (auto v : localVelocities)
            feed(x, v);
    }
    for (const auto& p : control_)
        feedXZ(x, relative(p.transform, c.root).position);
    for (const auto& p : control_)
        feedXZ(x, relative(p.transform, c.root).rotation * vec3(0, 0, 1));
    for (const auto& p : control_)
        feedXZ(x, glm::conjugate(c.root.rotation) * p.velocity);
    const auto& guidance = a.guidance(c);
    for (auto p : guidance)
        feed(x, p);
    auto sequence = decodeSequence(a.network->run(x), unsigned(model.size()), a.samples, a.window, c.root);
    auto post = poseFeatures;
    for (int feature = 0; feature < 3; ++feature)
        for (unsigned i = 1; i < a.samples; ++i)
            for (unsigned j : a.contactJoints) {
                const auto& frame = sequence.frames[i];
                auto now = compose(c.root, model[j]);
                if (feature == 0)
                    post.push_back(glm::distance(now.position, frame.joints[j].position));
                else if (feature == 1) {
                    float cosine =
                        std::clamp(std::abs(glm::dot(now.rotation, frame.joints[j].rotation)), 0.f, 1.f);
                    post.push_back(glm::degrees(2 * std::acos(cosine)));
                } else
                    post.push_back(glm::distance(velocities_[j], frame.velocities[j]));
            }
    auto contacts = a.postprocessor->run(post);
    for (unsigned i = 0; i < a.samples; ++i)
        for (size_t j = 0; j < a.contactJoints.size(); ++j)
            sequence.frames[i].contacts.push_back(
                std::pow(std::clamp(contacts[i * a.contactJoints.size() + j], 0.f, 1.f), a.contactPower));
    previous_ = current_.frames.empty() ? sequence : std::move(current_);
    current_ = std::move(sequence);
    elapsed_ = 0;
}
void Controller::feet(const Context& c, Pose& world, const std::vector<float>& contacts) {
    for (size_t i = 0; i < asset_->feet.size(); ++i) {
        const auto& f = asset_->feet[i];
        float weight = contacts[f.contact];
        auto& target = footTargets_[i];
        vec3 locked = target.position;
        if (f.grounded)
            locked.y = std::max(glm::mix(locked.y, footBaselines_[i], weight), footBaselines_[i]);
        auto tip = world[f.chain.back()];
        target.position = glm::mix(tip.position, locked, weight);
        target.rotation = f.grounded ? glm::slerp(tip.rotation, target.rotation, .5f * weight) : tip.rotation;
        if (f.ankleFoot >= 0) {
            vec3 ankle = footTargets_[f.ankleFoot].position, d = target.position - ankle;
            d.y = 0;
            float height = target.position.y, vertical = height - ankle.y;
            d = glm::length(d) > 1e-6f ? unit(d) : vec3(1, 0, 0);
            target.position =
                ankle + std::sqrt(std::max(footReach_[i] * footReach_[i] - vertical * vertical, 0.f)) * d;
            target.position.y = height;
        }
        vec3 pole;
        const vec3* polePtr = nullptr;
        if (f.grounded && f.chain.size() > 2) {
            auto knee = world[f.chain[1]];
            pole = knee.position + knee.rotation * vec3(0, 0, 1);
            polePtr = &pole;
        }
        solveFabrik(c.skeleton, world, f.chain, target.position, target.rotation, 1, .001f, polePtr);
    }
}
void Controller::evaluate(const Context& c, Output& out) {
    // Reconcile cached predictions to physics feedback. This avoids accumulating blocked root motion.
    quat correction = c.root.rotation * glm::conjugate(expectedRoot_.rotation);
    current_.rebase(expectedRoot_, c.root);
    previous_.rebase(expectedRoot_, c.root);
    for (auto& p : simulation_) {
        p.transform = compose(c.root, relative(p.transform, expectedRoot_));
        p.velocity = correction * p.velocity;
    }
    for (auto& v : velocities_)
        v = correction * v;
    control(c);
    if (current_.frames.empty() || elapsed_ + 1e-6f >= 1 / asset_->predictionRate)
        predict(c);
    float required = (glm::distance(c.root.position, simulation_[0].transform.position)) / asset_->window;
    for (size_t i = 1; i < simulation_.size(); ++i)
        required += glm::distance(simulation_[i - 1].transform.position, simulation_[i].transform.position) /
                    asset_->window;
    float predicted = current_.length() / asset_->window;
    bool moving = required > .1f && predicted > .1f;
    timescale_ = std::clamp(smooth(timescale_, moving ? required / predicted : 1, c.dt, 5),
                            asset_->minTimescale, asset_->maxTimescale);
    synchronization_ = smooth(synchronization_, moving ? 1.f : 0.f, c.dt, 5);
    float dt = c.dt * timescale_, blend = std::clamp(elapsed_ * asset_->predictionRate, 0.f, 1.f);
    auto prev = previous_.sample(dt), next = current_.sample(dt);
    Transform root = interpolate(interpolate(prev.root, next.root, blend), c.root, current_.rootLock());
    Pose world = c.skeleton.toModel(c.pose);
    for (size_t i = 0; i < world.size(); ++i) {
        world[i] = compose(c.root, world[i]);
        velocities_[i] = glm::mix(prev.velocities[i], next.velocities[i], blend);
        world[i].position = glm::mix(world[i].position + velocities_[i] * dt,
                                     glm::mix(prev.joints[i].position, next.joints[i].position, blend), .5f);
        world[i].rotation = glm::slerp(prev.joints[i].rotation, next.joints[i].rotation, blend);
    }
    std::vector<float> contacts;
    for (size_t i = 0; i < next.contacts.size(); ++i) {
        contacts.push_back(glm::mix(prev.contacts[i], next.contacts[i], blend));
        out.contacts.push_back({asset_->contactJoints[i], contacts.back()});
    }
    c.skeleton.restoreBones(world);
    feet(c, world, contacts);
    for (auto& p : world)
        p = relative(p, root);
    out.localPose = c.skeleton.toLocal(world);
    out.rootMotion = relative(root, c.root);
    expectedRoot_ = root;
    elapsed_ += c.dt;
    current_.age += dt;
    previous_.age += dt;
}
} // namespace whimsical::animation::ai4animation
