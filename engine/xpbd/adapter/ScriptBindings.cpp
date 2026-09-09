#include "ScriptBindings.h"
#include "assets/Json.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <type_traits>
namespace whimsical::xpbd {
namespace {
uint32_t integer(duk_context* c, int index) {
    auto v = duk_get_number(c, index);
    if (!std::isfinite(v) || v < 0 || v > UINT32_MAX || std::floor(v) != v)
        throw std::invalid_argument("Expected an unsigned 32-bit integer");
    return uint32_t(v);
}
std::string string(duk_context* c, int index) {
    if (!duk_is_string(c, index))
        throw std::invalid_argument("Expected a string");
    return duk_get_string(c, index);
}
Json json(duk_context* c, int index) {
    duk_dup(c, index);
    duk_json_encode(c, -1);
    auto result = Json::parse(string(c, -1));
    duk_pop(c);
    return result;
}
void push(duk_context* c, const Json& value) {
    auto text = value.dump();
    duk_push_lstring(c, text.data(), text.size());
    duk_json_decode(c, -1);
}
template <class T> std::vector<T> array(duk_context* c, int index) {
    index = duk_normalize_index(c, index);
    std::vector<T> result;
    if (duk_is_undefined(c, index) || duk_is_null(c, index))
        return result;
    if (duk_is_buffer_data(c, index)) {
        duk_size_t bytes = 0;
        auto data = duk_get_buffer_data(c, index, &bytes);
        if (bytes % sizeof(T))
            throw std::invalid_argument("Typed buffer has an incomplete element");
        result.resize(bytes / sizeof(T));
        if (bytes)
            std::memcpy(result.data(), data, bytes);
        return result;
    }
    if (!duk_is_array(c, index))
        throw std::invalid_argument("Expected an array or a typed buffer");
    auto n = duk_get_length(c, index);
    result.reserve(n);
    for (duk_uarridx_t i = 0; i < n; ++i) {
        duk_get_prop_index(c, index, i);
        if constexpr (std::is_same_v<T, uint32_t>)
            result.push_back(integer(c, -1));
        else {
            auto v = duk_get_number(c, -1);
            if (!std::isfinite(v))
                throw std::invalid_argument("Array element must be finite");
            result.push_back(T(v));
        }
        duk_pop(c);
    }
    return result;
}
template <class T> std::vector<T> property(duk_context* c, int object, const char* name) {
    duk_get_prop_string(c, object, name);
    auto result = array<T>(c, -1);
    duk_pop(c);
    return result;
}
uint32_t number(duk_context* c, int object, const char* name, uint32_t fallback = 0) {
    duk_get_prop_string(c, object, name);
    auto result = duk_is_undefined(c, -1) ? fallback : integer(c, -1);
    duk_pop(c);
    return result;
}
bool boolean(duk_context* c, int object, const char* name) {
    duk_get_prop_string(c, object, name);
    bool value = duk_get_boolean(c, -1) != 0;
    duk_pop(c);
    return value;
}
Field field(duk_context* c, int object, const char* name, uint32_t count, uint32_t width,
            bool required = false) {
    auto values = property<float>(c, object, name);
    if (values.empty() && !required)
        return {};
    if (values.size() == width)
        return Field::uniform(count, std::move(values));
    return Field::dense(count, width, values);
}
std::vector<std::vector<VariableRef>> endpoints(duk_context* c, int object) {
    duk_get_prop_string(c, object, "endpoints");
    auto list = duk_normalize_index(c, -1);
    if (!duk_is_array(c, list))
        throw std::invalid_argument("Expected endpoint columns");
    std::vector<std::vector<VariableRef>> result(duk_get_length(c, list));
    for (uint32_t e = 0; e < result.size(); ++e) {
        duk_get_prop_index(c, list, e);
        auto ids = property<uint32_t>(c, -1, "indices"), sets = property<uint32_t>(c, -1, "sets");
        auto set = number(c, -1, "set");
        if (!sets.empty() && sets.size() != ids.size())
            throw std::invalid_argument("Endpoint set/index column sizes differ");
        result[e].resize(ids.size());
        for (uint32_t i = 0; i < ids.size(); ++i)
            result[e][i] = {sets.empty() ? set : sets[i], ids[i]};
        duk_pop(c);
    }
    duk_pop(c);
    return result;
}
Formula formula(const Json& j) {
    Formula f;
    f.inputs = j.at("inputs").uint();
    const std::vector<std::string> names = {"constant", "input", "add", "sub",   "mul",  "div",
                                            "neg",      "sqrt",  "sin", "cos",   "exp",  "log",
                                            "abs",      "min",   "max", "atan2", "less", "select"};
    for (const auto& row : j.at("nodes").elements()) {
        auto name = row.at(0).string();
        auto found = std::find(names.begin(), names.end(), name);
        if (found == names.end())
            throw std::invalid_argument("Unknown mathematical operation: " + name);
        MathNode n{MathOp(found - names.begin())};
        const auto& values = row.elements();
        if (n.op == MathOp::Constant)
            n.value = float(row.at(1).number());
        else {
            if (values.size() > 1)
                n.a = row.at(1).uint();
            if (values.size() > 2)
                n.b = row.at(2).uint();
            if (values.size() > 3)
                n.c = row.at(3).uint();
        }
        f.nodes.push_back(n);
    }
    for (const auto& value : j.at("outputs").elements())
        f.outputs.push_back(value.uint());
    f.validate();
    return f;
}
StateField stateField(const std::string& name) {
    if (name == "value")
        return StateField::Value;
    if (name == "velocity")
        return StateField::Velocity;
    if (name == "acceleration")
        return StateField::Acceleration;
    if (name == "history")
        return StateField::History;
    if (name == "multiplier")
        return StateField::Multiplier;
    throw std::invalid_argument("Unknown state field: " + name);
}
FieldKind modelField(const std::string& name) {
    if (name == "inverseMetric")
        return FieldKind::InverseMetric;
    if (name == "variableEnabled")
        return FieldKind::VariableEnabled;
    if (name == "parameters")
        return FieldKind::Parameters;
    if (name == "compliance")
        return FieldKind::Compliance;
    if (name == "relationEnabled")
        return FieldKind::RelationEnabled;
    throw std::invalid_argument("Unknown model field: " + name);
}
} // namespace
ScriptBindings::ScriptBindings(duk_context* c, std::shared_ptr<Mailbox> mailbox)
    : mailbox_(std::move(mailbox)) {
    duk_push_heap_stash(c);
    duk_push_pointer(c, this);
    duk_put_prop_string(c, -2, "xpbd");
    duk_pop(c);
    duk_get_global_string(c, "Engine");
    duk_push_object(c);
    const char* names[] = {"space",
                           "relation",
                           "model",
                           "variables",
                           "relations",
                           "patch",
                           "compile",
                           "step",
                           "read",
                           "poll",
                           "destroy",
                           "appendVariables",
                           "appendRelations",
                           "replaceEndpoints",
                           "spaceInfo"};
    for (int i = 0; i < 15; ++i) {
        duk_push_c_function(c, call, DUK_VARARGS);
        duk_set_magic(c, -1, i);
        duk_put_prop_string(c, -2, names[i]);
    }
    duk_put_prop_string(c, -2, "xpbd");
    duk_pop(c);
    const char* source =
#include "ScriptMath.inc"
        ;
    if (duk_peval_string(c, source) != 0) {
        auto error = std::string(duk_safe_to_string(c, -1));
        duk_pop(c);
        throw std::runtime_error(error);
    }
    duk_pop(c);
}
ScriptBindings::~ScriptBindings() {
    for (auto& pair : models_)
        pair.second->channel->retire();
}
duk_ret_t ScriptBindings::call(duk_context* c) {
    duk_push_heap_stash(c);
    duk_get_prop_string(c, -1, "xpbd");
    auto self = static_cast<ScriptBindings*>(duk_get_pointer(c, -1));
    duk_pop_2(c);
    try {
        return self->dispatch(c, duk_get_current_magic(c));
    } catch (const std::exception& error) {
        duk_push_error_object(c, DUK_ERR_ERROR, "%s", error.what());
    }
    duk_throw_raw(c);
}
__declspec(noinline) duk_ret_t ScriptBindings::dispatch(duk_context* c, int op) {
    if (op == 0) {
        SpaceRef space;
        if (duk_is_number(c, 0))
            space = Space::euclidean(integer(c, 0));
        else {
            auto def = json(c, 0);
            if (def.contains("kind") && def.at("kind").string() == "rotation")
                space = Space::rotation();
            else {
                auto s = std::make_shared<Space>();
                s->name = def.at("name").string();
                s->stateSize = def.at("stateSize").uint();
                s->tangentSize = def.at("tangentSize").uint();
                s->retract = formula(def.at("retract"));
                s->difference = formula(def.at("difference"));
                s->validate();
                space = s;
            }
        }
        duk_push_uint(c, uint32_t(spaces_.size()));
        spaces_.push_back(std::move(space));
        return 1;
    }
    if (op == 14) {
        const auto& s = *spaces_.at(integer(c, 0));
        push(c, {{"stateSize", s.stateSize}, {"tangentSize", s.tangentSize}});
        return 1;
    }
    if (op == 1) {
        auto def = json(c, 0);
        auto t = std::make_shared<RelationType>();
        t->name = def.at("name").string();
        for (auto& id : def.at("spaces").elements())
            t->spaces.push_back(spaces_.at(id.uint()));
        t->parameters = def.contains("parameters") ? def.at("parameters").uint() : 0;
        t->history = def.contains("history") ? def.at("history").uint() : 0;
        t->rows = def.contains("rows") ? def.at("rows").uint() : 1;
        if (def.contains("domain")) {
            auto domain = def.at("domain").string();
            if (domain == "nonnegative")
                t->domain = Domain::NonNegative;
            else if (domain == "nonpositive")
                t->domain = Domain::NonPositive;
            else if (domain == "projected")
                t->domain = Domain::Projected;
            else if (domain != "equality")
                throw std::invalid_argument("Unknown multiplier domain");
        }
        t->residual = formula(def.at("residual"));
        if (def.contains("projection"))
            t->projection = formula(def.at("projection"));
        if (def.contains("update"))
            t->update = formula(def.at("update"));
        t->validate();
        duk_push_uint(c, uint32_t(types_.size()));
        types_.push_back(std::move(t));
        return 1;
    }
    if (op == 2) {
        auto entry = std::make_unique<Entry>();
        entry->channel = mailbox_->attach();
        auto id = ++next_;
        models_.emplace(id, std::move(entry));
        duk_push_uint(c, id);
        return 1;
    }
    auto id = integer(c, 0);
    auto& e = *models_.at(id);
    if (op == 10) {
        e.channel->retire();
        models_.erase(id);
        return 0;
    }
    if (op == 9) {
        auto reply = e.channel->receive();
        if (!reply) {
            if (e.channel->retired())
                throw std::runtime_error("XPBD GPU service has stopped");
            duk_push_null(c);
            return 1;
        }
        if (!reply->error.empty())
            e.needsPlan = true;
        else if (e.pendingOperation == Operation::Install || e.pendingOperation == Operation::Apply)
            e.needsPlan = false;
        auto& s = reply->completed;
        e.version = s.modelVersion;
        push(c, {{"model", double(s.model)},
                 {"modelVersion", double(s.modelVersion)},
                 {"tick", double(s.tick)},
                 {"time", s.time},
                 {"gpuMilliseconds", s.gpuMilliseconds},
                 {"invalidEvaluations", s.invalidEvaluations},
                 {"singularSystems", s.singularSystems},
                 {"error", reply->error}});
        duk_push_fixed_buffer(c, reply->values.size() * 4);
        duk_size_t size = 0;
        auto buffer = duk_get_buffer(c, -1, &size);
        if (size)
            std::memcpy(buffer, reply->values.data(), size);
        duk_push_buffer_object(c, -1, 0, size, DUK_BUFOBJ_FLOAT32ARRAY);
        duk_remove(c, -2);
        duk_put_prop_string(c, -2, "values");
        return 1;
    }
    if (op == 3) {
        VariableSet set;
        set.space = spaces_.at(integer(c, 1));
        set.count = number(c, 2, "count");
        set.name = "variables";
        set.readOnly = boolean(c, 2, "readOnly");
        set.initial = field(c, 2, "initial", set.count, set.space->stateSize, true);
        set.velocity = field(c, 2, "velocity", set.count, set.space->tangentSize);
        set.inverseMetric =
            field(c, 2, "inverseMetric", set.count, set.space->tangentSize * set.space->tangentSize);
        set.enabled = field(c, 2, "enabled", set.count, 1);
        duk_push_uint(c, e.model.variables(std::move(set)));
        return 1;
    }
    if (op == 4) {
        RelationSet set;
        set.type = types_.at(integer(c, 1));
        set.name = "relations";
        set.dynamicEndpoints = boolean(c, 2, "dynamicEndpoints");
        auto columns = endpoints(c, 2);
        if (columns.empty())
            throw std::invalid_argument("Relationship needs endpoints");
        set.count = uint32_t(columns[0].size());
        for (auto& column : columns)
            set.endpoints.push_back(std::make_shared<const std::vector<VariableRef>>(std::move(column)));
        set.parameters = field(c, 2, "parameters", set.count, set.type->parameters);
        set.compliance = field(c, 2, "compliance", set.count, set.type->rows);
        set.initialHistory = field(c, 2, "history", set.count, set.type->history);
        set.enabled = field(c, 2, "enabled", set.count, 1);
        duk_push_uint(c, e.model.relations(std::move(set)));
        return 1;
    }
    if (op == 5) {
        e.model.patch(modelField(string(c, 1)), integer(c, 2), integer(c, 3), array<float>(c, 4));
        return 0;
    }
    if (op == 11) {
        e.model.appendVariables(integer(c, 1), number(c, 2, "count"), property<float>(c, 2, "initial"),
                                property<float>(c, 2, "velocity"), property<float>(c, 2, "inverseMetric"));
        return 0;
    }
    if (op == 12) {
        e.model.appendRelations(integer(c, 1), endpoints(c, 2), property<float>(c, 2, "parameters"),
                                property<float>(c, 2, "compliance"), property<float>(c, 2, "history"));
        return 0;
    }
    if (op == 13) {
        auto columns = endpoints(c, 3);
        if (columns.size() != 1)
            throw std::invalid_argument("Replace one endpoint column at a time");
        e.model.replaceEndpoints(integer(c, 1), integer(c, 2), std::move(columns[0]));
        return 0;
    }
    if (e.channel->busy())
        throw std::runtime_error("XPBD instance is busy; poll its reply before submitting another operation");
    Request request{};
    if (op == 6) {
        if (duk_is_object(c, 1)) {
            auto p = json(c, 1);
            if (p.contains("substeps"))
                e.policy.substeps = p.at("substeps").uint();
            if (p.contains("iterations"))
                e.policy.iterations = p.at("iterations").uint();
            if (p.contains("colorBudget"))
                e.policy.colorBudget = p.at("colorBudget").uint();
            if (p.contains("relaxation"))
                e.policy.relaxation = float(p.at("relaxation").number());
            if (p.contains("mode")) {
                auto mode = p.at("mode").string();
                if (mode == "colored")
                    e.policy.mode = SolveMode::Colored;
                else if (mode == "jacobi")
                    e.policy.mode = SolveMode::Jacobi;
                else if (mode == "hybrid")
                    e.policy.mode = SolveMode::Hybrid;
                else
                    throw std::invalid_argument("Unknown solver mode");
            }
            e.needsPlan = true;
        }
        request.commit = e.model.commit();
        e.needsPlan = e.needsPlan || request.commit.changes.topology;
        if (e.needsPlan) {
            request.operation = Operation::Install;
            request.plan = Compiler().compile(request.commit.snapshot, e.policy);
        } else
            request.operation = Operation::Apply;
    } else if (op == 7) {
        request.operation = Operation::Step;
        request.tick.tick = integer(c, 1);
        request.tick.dt = float(duk_get_number(c, 2));
        request.tick.modelVersion = e.version;
        if (duk_is_array(c, 3))
            for (uint32_t i = 0; i < duk_get_length(c, 3); ++i) {
                duk_get_prop_index(c, 3, i);
                StateWrite write;
                duk_get_prop_string(c, -1, "field");
                write.field = stateField(string(c, -1));
                duk_pop(c);
                write.set = number(c, -1, "set");
                write.first = number(c, -1, "first");
                write.values = property<float>(c, -1, "values");
                request.tick.writes.push_back(std::move(write));
                duk_pop(c);
            }
        if (duk_is_array(c, 4))
            for (uint32_t i = 0; i < duk_get_length(c, 4); ++i) {
                duk_get_prop_index(c, 4, i);
                TickInput::Endpoints input;
                input.set = number(c, -1, "set");
                input.first = number(c, -1, "first");
                input.columns = endpoints(c, -1);
                request.tick.endpoints.push_back(std::move(input));
                duk_pop(c);
            }
    } else if (op == 8) {
        request.operation = Operation::Read;
        request.field = stateField(string(c, 1));
        request.set = integer(c, 2);
        request.first = integer(c, 3);
        request.count = integer(c, 4);
    } else
        throw std::logic_error("Unknown XPBD operation");
    e.pendingOperation = request.operation;
    e.channel->submit(std::move(request));
    return 0;
}
} // namespace whimsical::xpbd
