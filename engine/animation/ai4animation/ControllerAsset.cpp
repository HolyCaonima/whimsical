#include "Controller.h"
#include <algorithm>
#include <stdexcept>

namespace afterlight::animation::ai4animation {
const std::vector<vec3>& ControllerAsset::guidance(const Context& c) const {
    auto rule = std::find_if(actions.begin(), actions.end(),
                             [&](const GuidanceRule& r) { return r.action == c.input.action; });
    if (rule == actions.end())
        throw std::invalid_argument("Unsupported animation action: " + c.input.action);
    return guidances.at(rule->attribute.empty() ? rule->guidance
                                                : rule->choices.at(c.attributes.at(rule->attribute)));
}
void ControllerAsset::validate() const {
    if (!skeleton || !network || !postprocessor || samples < 2 || window <= 0 || predictionRate <= 0 ||
        contactPower <= 0 || minTimescale <= 0 || maxTimescale < minTimescale || trajectoryCorrection < 0 ||
        trajectoryCorrection > 1)
        throw std::invalid_argument("Invalid AI4Animation controller asset");
    const auto n = skeleton->size(), k = contactJoints.size();
    if (network->inputSize() != (poseAxes ? 15 : 9) * n + 6 * samples ||
        network->outputSize() != samples * (3 + 12 * n) ||
        postprocessor->inputSize() != 12 * n + 3 * (samples - 1) * k ||
        postprocessor->outputSize() != samples * k)
        throw std::invalid_argument("ONNX models do not match the locomotion feature ABI");
    for (auto j : contactJoints)
        if (j >= n)
            throw std::invalid_argument("Invalid contact joint");
    for (size_t i = 0; i < feet.size(); ++i) {
        const auto& f = feet[i];
        if (f.chain.size() < 2 || f.contact >= k || f.ankleFoot < -1 || f.ankleFoot >= int(i))
            throw std::invalid_argument("Invalid foot binding");
        for (size_t j = 0; j < f.chain.size(); ++j)
            if (f.chain[j] >= n || (j && skeleton->joints()[f.chain[j]].parent != int(f.chain[j - 1])))
                throw std::invalid_argument("Foot IK requires a contiguous chain");
        if (f.chain.back() != contactJoints[f.contact])
            throw std::invalid_argument("Foot/contact mismatch");
    }
    if (guidances.empty())
        throw std::invalid_argument("Controller has no action guidances");
    for (const auto& g : guidances)
        if (g.second.size() != n)
            throw std::invalid_argument("Guidance bone count mismatch");
    for (const auto& rule : actions) {
        if (rule.attribute.empty()) {
            if (!guidances.count(rule.guidance))
                throw std::invalid_argument("Action references missing guidance");
        } else {
            auto descriptor = std::find_if(attributes.begin(), attributes.end(),
                                           [&](const EnumAttribute& a) { return a.key == rule.attribute; });
            if (descriptor == attributes.end())
                throw std::invalid_argument("Action references missing attribute");
            for (const auto& option : descriptor->options) {
                auto choice = rule.choices.find(option.value);
                if (choice == rule.choices.end() || !guidances.count(choice->second))
                    throw std::invalid_argument("Attribute option has no valid guidance binding");
            }
        }
    }
}
std::shared_ptr<const ControllerAsset>
ControllerAsset::decode(std::istream& file, std::shared_ptr<const OnnxModel> network,
                        std::shared_ptr<const OnnxModel> postprocessor) {
    auto read = [&](void* data, size_t size) {
        if (!file.read(static_cast<char*>(data), std::streamsize(size)))
            throw std::runtime_error("Truncated animation controller payload");
    };
    auto u = [&]() {
        uint32_t value;
        read(&value, 4);
        return value;
    };
    auto i = [&]() {
        int32_t value;
        read(&value, 4);
        return value;
    };
    auto f = [&]() {
        float value;
        read(&value, 4);
        return value;
    };
    auto string = [&]() {
        auto count = u();
        std::string value(count, '\0');
        read(value.data(), count);
        return value;
    };
    auto vector = [&]() {
        float x = f(), y = f(), z = f();
        return vec3(x, y, z);
    };
    char magic[4];
    read(magic, 4);
    if (std::string(magic, 4) != "A4C2")
        throw std::runtime_error("Unsupported controller asset version");
    auto a = std::make_shared<ControllerAsset>();
    auto count = u();
    a->samples = u();
    a->poseAxes = u() != 0;
    a->window = f();
    a->predictionRate = f();
    a->contactPower = f();
    a->trajectoryCorrection = f();
    a->minTimescale = f();
    a->maxTimescale = f();
    std::vector<Joint> joints;
    for (uint32_t j = 0; j < count; ++j) {
        Joint joint;
        joint.name = string();
        joint.parent = i();
        joint.rest.position = vector();
        float x = f(), y = f(), z = f(), w = f();
        joint.rest.rotation = glm::normalize(quat(w, x, y, z));
        joints.push_back(joint);
    }
    a->skeleton = std::make_shared<Skeleton>(std::move(joints));
    auto contacts = u();
    for (uint32_t j = 0; j < contacts; ++j)
        a->contactJoints.push_back(u());
    auto feet = u();
    for (uint32_t j = 0; j < feet; ++j) {
        Foot foot;
        foot.contact = u();
        foot.ankleFoot = i();
        foot.grounded = u() != 0;
        auto size = u();
        for (uint32_t k = 0; k < size; ++k)
            foot.chain.push_back(u());
        a->feet.push_back(foot);
    }
    auto guidances = u();
    for (uint32_t j = 0; j < guidances; ++j) {
        auto name = string();
        std::vector<vec3> positions;
        for (uint32_t k = 0; k < count; ++k)
            positions.push_back(vector());
        a->guidances.emplace(name, std::move(positions));
    }
    auto attributes = u();
    for (uint32_t j = 0; j < attributes; ++j) {
        EnumAttribute attribute;
        attribute.key = string();
        attribute.label = string();
        attribute.defaultValue = string();
        auto options = u();
        for (uint32_t k = 0; k < options; ++k) {
            EnumOption option;
            option.value = string();
            option.label = string();
            attribute.options.push_back(std::move(option));
        }
        a->attributes.push_back(std::move(attribute));
    }
    auto actions = u();
    for (uint32_t j = 0; j < actions; ++j) {
        GuidanceRule rule;
        rule.action = string();
        rule.attribute = string();
        rule.guidance = string();
        auto choices = u();
        for (uint32_t k = 0; k < choices; ++k) {
            auto value = string();
            auto guidance = string();
            rule.choices.emplace(std::move(value), std::move(guidance));
        }
        a->actions.push_back(std::move(rule));
    }
    a->network = std::move(network);
    a->postprocessor = std::move(postprocessor);
    a->validate();
    return a;
}
std::unique_ptr<Solver> ControllerResource::createSolver() const {
    return std::make_unique<Controller>(data);
}
} // namespace afterlight::animation::ai4animation
