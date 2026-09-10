#include "DynamicsSystem.h"
#include "core/World.h"
#include <algorithm>
#include <cmath>

namespace whimsical::dynamics {
struct SceneInstance {
    SceneModel document;
    std::unique_ptr<Model> model;
    PlanRef plan;
    std::shared_ptr<Channel> channel;
    CompletedState completed;
    std::vector<StateWrite> writes;
    std::vector<SampleRange> observe, reading;
    std::vector<Sample> samples;
    uint64_t samplesTick = 0;
    Operation pending = Operation::Install;
    bool installed = false, dirty = false, paused = false, oneStep = false;
    float stepDt = 1.f / 60.f, debt = 0;
    std::string error;
    ~SceneInstance() {
        if (channel)
            channel->retire();
    }
};
namespace {
SceneInstance& instance(SceneStorage& s, Entity e) {
    return *s.registry.get<DynamicsModel>(e).instance;
}
uint32_t width(const SceneInstance& s, SetId set) {
    return s.model->snapshot().data->variables.at(set).space->stateSize;
}
void checkRange(const SceneInstance& s, SampleRange r) {
    auto count = s.model->snapshot().data->variables.at(r.set).count;
    if (r.first > count || r.count > count - r.first)
        throw std::out_of_range("Dynamics sample range");
}
std::vector<SampleRange> mergeRanges(std::vector<SampleRange> ranges) {
    std::sort(ranges.begin(), ranges.end(),
              [](auto a, auto b) { return std::tie(a.set, a.first) < std::tie(b.set, b.first); });
    std::vector<SampleRange> out;
    for (auto r : ranges) {
        if (!r.count)
            continue;
        if (!out.empty() && out.back().set == r.set && r.first <= out.back().first + out.back().count)
            out.back().count = std::max(out.back().count, r.first + r.count - out.back().first);
        else
            out.push_back(r);
    }
    return out;
}
Entity source(const World& w, Entity e, const DynamicsBinding& b) {
    if (!b.model.empty())
        return w.findObject(b.model);
    for (auto p = e; p;) {
        if (w.has<DynamicsModel>(p))
            return p;
        auto t = w.registry().tryGet<Transform>(p);
        p = t ? t->parent : 0;
    }
    return 0;
}
const Sample* findSample(const SceneInstance& s, SampleRange r) {
    auto it = std::upper_bound(s.samples.begin(), s.samples.end(), r, [](auto key, const Sample& value) {
        return std::tie(key.set, key.first) < std::tie(value.range.set, value.range.first);
    });
    if (it == s.samples.begin())
        return nullptr;
    --it;
    return it->range.set == r.set && r.first >= it->range.first &&
                   uint64_t(r.first) + r.count <= uint64_t(it->range.first) + it->range.count
               ? &*it
               : nullptr;
}
std::vector<float> sample(const SceneInstance& s, SampleRange r) {
    checkRange(s, r);
    auto n = width(s, r.set);
    if (!s.samplesTick)
        return s.model->snapshot().data->variables.at(r.set).initial.slice(r.first, r.count);
    if (auto v = findSample(s, r)) {
        auto begin = v->values.begin() + size_t(r.first - v->range.first) * n;
        return {begin, begin + size_t(r.count) * n};
    }
    throw std::out_of_range("Range is not in the completed Dynamics sample; declare observe ranges");
}
std::vector<double> poseValues(const TransformPose& p) {
    return {p.position.x, p.position.y, p.position.z, p.rotation.w, p.rotation.x,
            p.rotation.y, p.rotation.z, p.scale.x,    p.scale.y,    p.scale.z};
}
void submit(SceneInstance& s, Request request) {
    s.pending = request.operation;
    s.channel->submit(std::move(request));
}
void receive(SceneInstance& s) {
    if (!s.channel)
        return;
    auto reply = s.channel->receive();
    if (!reply) {
        if (s.channel->retired())
            s.error = "Dynamics GPU service has stopped";
        return;
    }
    s.completed = reply->completed;
    if (!reply->error.empty()) {
        s.error = reply->error;
        return;
    }
    if (s.pending == Operation::Install)
        s.installed = true;
    if (s.pending == Operation::Read) {
        s.samples.clear();
        for (size_t i = 0; i < s.reading.size(); ++i)
            s.samples.push_back({s.reading[i], std::move(reply->samples.at(i))});
        s.samplesTick = s.completed.tick;
        s.reading.clear();
    }
}
} // namespace
void DynamicsSystem::update(World& world, float dt) {
    auto entities = storage_.registry.view<DynamicsModel>();
    auto bindings = storage_.registry.view<DynamicsBinding, Transform>();
    std::map<Entity, std::vector<Entity>> byModel;
    for (auto e : bindings)
        if (world.enabled(e)) {
            auto& b = storage_.registry.get<DynamicsBinding>(e);
            auto owner = source(world, e, b);
            if (owner && world.has<DynamicsModel>(owner))
                byModel[owner].push_back(e);
        }
    for (auto e : entities) {
        auto& s = instance(storage_, e);
        try {
            const bool enabled = world.enabled(e);
            if (enabled && !s.paused)
                s.debt = std::min(s.stepDt, s.debt + dt);
            else
                s.debt = 0;
            bool finishedStep = s.channel && s.channel->busy() && s.pending == Operation::Step;
            receive(s);
            if (!s.error.empty())
                continue;
            if (s.channel && s.channel->busy())
                continue;
            if (finishedStep) {
                auto ranges = s.observe;
                for (auto binding : byModel[e]) {
                    auto& b = storage_.registry.get<DynamicsBinding>(binding);
                    if (!b.input)
                        for (auto v : b.variables)
                            ranges.push_back({v.set, v.index, 1});
                }
                for (auto r : ranges)
                    checkRange(s, r);
                s.reading = mergeRanges(std::move(ranges));
            }
            if (!s.reading.empty()) {
                Request request{};
                request.operation = Operation::Read;
                for (auto r : s.reading)
                    request.ranges.push_back({StateField::Value, r.set, r.first, r.count});
                submit(s, std::move(request));
                continue;
            }
            if (!enabled)
                continue;
            if (!s.channel)
                s.channel = mailbox_->attach();
            if (!s.installed) {
                Request request{};
                request.operation = Operation::Install;
                request.plan = s.plan;
                submit(s, std::move(request));
                continue;
            }
            if (s.dirty) {
                Request request{};
                request.operation = Operation::Apply;
                request.commit = s.model->commit();
                s.dirty = false;
                submit(s, std::move(request));
                continue;
            }
            if (!s.oneStep && (s.paused || s.debt < s.stepDt))
                continue;
            Request request{};
            request.operation = Operation::Step;
            request.tick = {s.completed.tick + 1, s.completed.modelVersion, s.stepDt};
            request.tick.writes = std::move(s.writes);
            for (auto binding : byModel[e]) {
                auto& b = storage_.registry.get<DynamicsBinding>(binding);
                if (!b.input)
                    continue;
                auto v = b.variables.front();
                checkRange(s, {v.set, v.index, 1});
                auto result = b.mapping.evaluate(poseValues(storage_.registry.get<Transform>(binding).local));
                auto& set = s.model->snapshot().data->variables.at(v.set);
                auto n = b.field == StateField::Value ? set.space->stateSize : set.space->tangentSize;
                if (result.size() != n)
                    throw std::invalid_argument("Dynamics input mapping width differs from target space");
                request.tick.writes.push_back({b.field, v.set, v.index, {result.begin(), result.end()}});
            }
            s.oneStep = false;
            s.debt = 0;
            submit(s, std::move(request));
        } catch (const std::exception& error) {
            s.error = error.what();
        }
    }
    auto batch = world.changes();
    for (const auto& [owner, list] : byModel) {
        auto& s = instance(storage_, owner);
        if (!world.enabled(owner) || !s.error.empty())
            continue;
        for (auto e : list) {
            auto& b = storage_.registry.get<DynamicsBinding>(e);
            if (b.input)
                continue;
            try {
                auto modelId = s.model->snapshot().model;
                if (!b.initialized || b.sampledModel != modelId || b.sampledTick != s.samplesTick) {
                    std::vector<double> input;
                    bool available = true;
                    for (auto v : b.variables) {
                        // Newly attached outputs join the next sample.
                        if (s.samplesTick && !findSample(s, {v.set, v.index, 1})) {
                            available = false;
                            break;
                        }
                        auto values = sample(s, {v.set, v.index, 1});
                        input.insert(input.end(), values.begin(), values.end());
                    }
                    if (!available)
                        continue;
                    auto p = b.mapping.evaluate(input);
                    b.target = {vec3(float(p[0]), float(p[1]), float(p[2])),
                                quat(float(p[3]), float(p[4]), float(p[5]), float(p[6])),
                                vec3(float(p[7]), float(p[8]), float(p[9]))};
                    b.from = b.initialized ? storage_.registry.get<Transform>(e).local : b.target;
                    b.age = 0;
                    b.initialized = true;
                    b.sampledModel = modelId;
                    b.sampledTick = s.samplesTick;
                }
                b.age += dt;
                float blend = b.interpolation > 0 ? std::min(1.f, b.age / b.interpolation) : 1.f;
                blend = blend * blend * (3 - 2 * blend);
                transforms_.setDrivenLocal(e,
                                           {glm::mix(b.from.position, b.target.position, blend),
                                            glm::slerp(b.from.rotation, b.target.rotation, blend),
                                            glm::mix(b.from.scale, b.target.scale, blend)},
                                           typeid(DynamicsBinding));
            } catch (const std::exception& error) {
                s.error = error.what();
                break;
            }
        }
    }
    batch.commit();
}
Json DynamicsSystem::state(Entity e) const {
    const auto& s = instance(storage_, e);
    const auto& c = s.completed;
    return {{"tick", double(c.tick)},
            {"time", c.time},
            {"sampleTick", double(s.samplesTick)},
            {"gpuMilliseconds", c.gpuMilliseconds},
            {"invalidEvaluations", c.invalidEvaluations},
            {"singularSystems", c.singularSystems},
            {"ready", s.installed},
            {"paused", s.paused},
            {"error", s.error}};
}
std::vector<float> DynamicsSystem::values(Entity e, SetId set, uint32_t first, uint32_t count) const {
    return sample(instance(storage_, e), {set, first, count});
}
void DynamicsSystem::control(Entity e, bool paused, bool step) {
    auto& s = instance(storage_, e);
    s.oneStep = s.oneStep || step;
    if (s.paused != paused) {
        auto batch = Changes::Batch(storage_.changes);
        s.paused = paused;
        storage_.changes.mark<DynamicsModel>(e);
        batch.commit();
    }
}
void DynamicsSystem::write(Entity e, StateWrite write) {
    auto& s = instance(storage_, e);
    // Repeated UI updates replace the same pending range, without growing a queue while paused.
    for (auto& pending : s.writes)
        if (pending.field == write.field && pending.set == write.set && pending.first == write.first &&
            pending.values.size() == write.values.size()) {
            pending = std::move(write);
            return;
        }
    s.writes.push_back(std::move(write));
}
void DynamicsSystem::patch(Entity e, FieldKind field, SetId set, uint32_t first,
                           const std::vector<float>& values) {
    auto& s = instance(storage_, e);
    auto batch = Changes::Batch(storage_.changes);
    s.model->patch(field, set, first, values);
    s.dirty = true;
    storage_.changes.mark<DynamicsModel>(e);
    batch.commit();
}
void registerSceneComponents(ComponentCatalog& catalog) {
    auto model = component<DynamicsModel, SceneModel>("dynamics");
    model.decode = [](const Json& j) -> std::any { return SceneModel{j}; };
    model.encode = [](const std::any& v) { return std::any_cast<const SceneModel&>(v).value; };
    model.prepare = [](const World&, Entity e, const std::any& value, AssetManager&) -> PreparedComponent {
        auto s = std::make_shared<SceneInstance>();
        s->document = std::any_cast<const SceneModel&>(value);
        auto& j = s->document.value;
        s->model = modelFromDocument(j.at("model"));
        j["model"] = Json(); // Runtime stores paged model data, not a second JSON copy.
        if (j.contains("stepTime"))
            s->stepDt = float(j.at("stepTime").number());
        if (!std::isfinite(s->stepDt) || s->stepDt <= 0)
            throw std::invalid_argument("Dynamics stepTime must be positive");
        if (j.contains("paused"))
            s->paused = j.at("paused").boolean();
        if (j.contains("observe"))
            for (const auto& r : j.at("observe").elements()) {
                SampleRange range{r.at("set").uint(), r.at("first").uint(), r.at("count").uint()};
                checkRange(*s, range);
                s->observe.push_back(range);
            }
        s->plan = Compiler().compile(s->model->commit().snapshot,
                                     solverPolicy(j.contains("policy") ? j.at("policy") : Json::object()));
        return [e, s](ComponentAccess& a) {
            auto& r = a.storage.registry;
            if (auto c = r.tryGet<DynamicsModel>(e))
                c->instance = s;
            else
                r.emplace<DynamicsModel>(e, DynamicsModel{s});
            a.storage.changes.mark<DynamicsModel>(e);
        };
    };
    model.inspect = [](const World& w, Entity e) -> std::optional<std::any> {
        auto& s = *w.get<DynamicsModel>(e).instance;
        auto j = s.document.value;
        j["model"] = document(s.model->snapshot());
        j["paused"] = s.paused;
        return SceneModel{j};
    };
    model.erase = [](ComponentAccess& a, Entity e) { a.storage.registry.remove<DynamicsModel>(e); };
    catalog.add(std::move(model));
    auto binding = component<DynamicsBinding, SceneBinding>("dynamicsBinding");
    binding.dependencies = {{"transform", OnDependencyRemoval::Cascade}};
    binding.decode = [](const Json& j) -> std::any { return SceneBinding{j}; };
    binding.encode = [](const std::any& v) { return std::any_cast<const SceneBinding&>(v).value; };
    binding.validate = [](const ComponentSet& set, const std::any& v) {
        if (set.contains("rootMotion") &&
            std::any_cast<const SceneBinding&>(v).value.at("direction").string() == "output")
            throw std::invalid_argument("Root motion and Dynamics cannot both drive a local transform");
    };
    binding.prepare = [](const World& w, Entity e, const std::any& value,
                         AssetManager&) -> PreparedComponent {
        DynamicsBinding b;
        b.document = std::any_cast<const SceneBinding&>(value);
        auto& j = b.document.value;
        if (j.contains("model"))
            b.model = j.at("model").string();
        if (!b.model.empty() && !w.findObject(b.model))
            throw std::invalid_argument("Missing Dynamics model entity");
        auto direction = j.at("direction").string();
        if (direction != "input" && direction != "output")
            throw std::invalid_argument("Unknown Dynamics binding direction");
        b.input = direction == "input";
        b.mapping = formula(j.at("mapping"));
        if (j.contains("interpolation"))
            b.interpolation = float(j.at("interpolation").number());
        if (!std::isfinite(b.interpolation) || b.interpolation < 0)
            throw std::invalid_argument("Interpolation time must be nonnegative");
        for (const auto& v : j.at("variables").elements())
            b.variables.push_back({v.at("set").uint(), v.at("index").uint()});
        if (b.input) {
            if (b.variables.size() != 1 || b.mapping.inputs != 10)
                throw std::invalid_argument("Input binding maps local pose to one variable");
            if (j.contains("field"))
                b.field = stateField(j.at("field").string());
            if (b.field == StateField::History)
                throw std::invalid_argument("Input binding requires a variable field");
        } else if (b.mapping.outputs.size() != 10 || b.variables.empty())
            throw std::invalid_argument("Output binding maps variable states to a local pose of ten scalars");
        return [e, b](ComponentAccess& a) {
            auto& r = a.storage.registry;
            if (auto old = r.tryGet<DynamicsBinding>(e); old && !old->input)
                a.world.transforms.release(e, typeid(DynamicsBinding));
            if (!b.input)
                a.world.transforms.claim(e, typeid(DynamicsBinding));
            if (auto old = r.tryGet<DynamicsBinding>(e))
                *old = b;
            else
                r.emplace<DynamicsBinding>(e, b);
            a.storage.changes.mark<DynamicsBinding>(e);
        };
    };
    binding.inspect = [](const World& w, Entity e) -> std::optional<std::any> {
        return w.get<DynamicsBinding>(e).document;
    };
    binding.erase = [](ComponentAccess& a, Entity e) {
        if (!a.storage.registry.get<DynamicsBinding>(e).input)
            a.world.transforms.release(e, typeid(DynamicsBinding));
        a.storage.registry.remove<DynamicsBinding>(e);
    };
    catalog.add(std::move(binding));
}
} // namespace whimsical::dynamics
