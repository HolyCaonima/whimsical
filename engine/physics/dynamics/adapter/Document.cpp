#include "Document.h"
#include <algorithm>
#include <stdexcept>
namespace whimsical::dynamics {
Formula formula(const Json& j) {
    Formula f;
    f.inputs = j.at("inputs").uint();
    const std::vector<std::string> names = {"constant", "input", "add", "sub",   "mul",  "div",
                                            "neg",      "sqrt",  "sin", "cos",   "exp",  "log",
                                            "abs",      "min",   "max", "atan2", "less", "select", "sum"};
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
Json document(const Formula& f) {
    static const char* names[] = {"constant", "input", "add", "sub", "mul", "div", "neg",   "sqrt", "sin",
                                  "cos",      "exp",   "log", "abs", "min", "max", "atan2", "less", "select", "sum"};
    Json nodes = Json::array(), outputs = Json::array();
    for (const auto& n : f.nodes) {
        auto row = Json::array({names[uint32_t(n.op)]});
        if (n.op == MathOp::Constant)
            row.push(n.value);
        else {
            row.push(n.a);
            if (n.op != MathOp::Input && !(n.op >= MathOp::Negate && n.op <= MathOp::Abs))
                row.push(n.b);
            if (n.op == MathOp::Select)
                row.push(n.c);
        }
        nodes.push(std::move(row));
    }
    for (auto o : f.outputs)
        outputs.push(o);
    return {{"inputs", f.inputs}, {"nodes", nodes}, {"outputs", outputs}};
}
namespace {
Json floats(const std::vector<float>& v) {
    auto j = Json::array();
    for (auto x : v)
        j.push(x);
    return j;
}
std::vector<float> floats(const Json& j) {
    std::vector<float> v;
    for (const auto& x : j.elements())
        v.push_back(float(x.number()));
    return v;
}
Json fieldDocument(const Field& f) {
    // Keep a constant column compact; export dense data only for nonuniform fields.
    if (!f.width() || !f.count())
        return Json::array();
    auto first = f.slice(0, 1);
    bool uniform = true;
    for (uint32_t i = 1; i < f.count() && uniform; ++i)
        for (uint32_t c = 0; c < f.width(); ++c)
            if (f.at(i, c) != first[c]) {
                uniform = false;
                break;
            }
    return floats(uniform ? first : f.slice(0, f.count()));
}
Field fieldFrom(const Json& j, uint32_t count, uint32_t width) {
    auto v = floats(j);
    if (v.empty())
        return {};
    return v.size() == width ? Field::uniform(count, std::move(v)) : Field::dense(count, width, v);
}
Json endpointDocument(const EndpointSource& source) {
    if (source.kind == EndpointSource::Kind::Object)
        return {{"kind", "object"}, {"set", source.first.set}, {"index", source.first.index}};
    if (source.kind == EndpointSource::Kind::Collection)
        return {{"kind", "collection"},
                {"set", source.first.set},
                {"first", source.first.index},
                {"count", source.count},
                {"stride", source.stride}};
    auto sets = Json::array(), indices = Json::array();
    for (auto value : *source.references) {
        sets.push(value.set);
        indices.push(value.index);
    }
    return {{"sets", sets}, {"indices", indices}};
}
EndpointSource endpointFrom(const Json& value) {
    if (value.contains("kind")) {
        auto kind = value.at("kind").string();
        if (kind == "object")
            return EndpointSource::object({value.at("set").uint(), value.at("index").uint()});
        if (kind == "collection")
            return EndpointSource::collection(value.at("set").uint(), value.at("first").uint(),
                                              value.at("count").uint(), value.at("stride").uint());
        throw std::invalid_argument("Unknown endpoint source kind");
    }
    auto refs = std::make_shared<std::vector<VariableRef>>();
    auto& sets = value.at("sets").elements();
    auto& indices = value.at("indices").elements();
    if (sets.size() != indices.size())
        throw std::invalid_argument("Endpoint column sizes differ");
    for (size_t i = 0; i < sets.size(); ++i)
        refs->push_back({sets[i].uint(), indices[i].uint()});
    return EndpointSource(std::move(refs));
}
} // namespace
Object objectFromDocument(const Json& j) {
    Object object;
    object.name = j.at("name").string();
    auto kind = j.at("kind").string();
    if (kind != "single" && kind != "collection")
        throw std::invalid_argument("Object kind must be single or collection");
    object.kind = kind == "single" ? Object::Kind::Single : Object::Kind::Collection;
    object.count = j.at("count").uint();
    for (const auto& field : j.at("dofs").elements())
        if (!object.dofs.emplace(field.at("name").string(), endpointFrom(field.at("source"))).second)
            throw std::invalid_argument("Duplicate object DOF field");
    if (j.contains("member"))
        object.member = Object::Member{j.at("member").at(0).uint(), j.at("member").at(1).uint()};
    return object;
}
PairBinding pairFromDocument(const Json& j) {
    PairBinding pair;
    pair.a = j.at("a").uint();
    pair.b = j.at("b").uint();
    if (j.contains("self")) {
        const auto rule = j.at("self").string();
        if (rule != "directed" && rule != "undirected")
            throw std::invalid_argument("Self-pair rule must be directed or undirected");
        pair.self = rule == "directed" ? PairBinding::Self::Directed : PairBinding::Self::Undirected;
        pair.includeSelf = j.at("includeSelf").boolean();
    }
    return pair;
}
Json document(const ModelSnapshot& snapshot) {
    Json spaces = Json::array(), types = Json::array(), variables = Json::array(), relations = Json::array();
    Json objects = Json::array();
    for (const auto& object : snapshot.data->objects) {
        auto fields = Json::array();
        for (const auto& [name, source] : object.dofs)
            fields.push({{"name", name}, {"source", endpointDocument(source)}});
        Json value = {{"name", object.name}, {"kind", object.kind == Object::Kind::Single ? "single" : "collection"},
                      {"count", object.count}, {"dofs", fields}};
        if (object.member)
            value["member"] = Json::array({object.member->object, object.member->index});
        objects.push(std::move(value));
    }
    std::map<const Space*, uint32_t> spaceIds;
    auto spaceId = [&](const SpaceRef& s) {
        auto it = spaceIds.find(s.get());
        if (it != spaceIds.end())
            return it->second;
        auto id = uint32_t(spaceIds.size());
        spaceIds.emplace(s.get(), id);
        spaces.push({{"name", s->name},
                     {"stateSize", s->stateSize},
                     {"tangentSize", s->tangentSize},
                     {"retract", document(s->retract)},
                     {"difference", document(s->difference)}});
        return id;
    };
    for (const auto& v : snapshot.data->variables)
        variables.push({{"name", v.name},
                        {"space", spaceId(v.space)},
                        {"count", v.count},
                        {"readOnly", v.readOnly},
                        {"initial", fieldDocument(v.initial)},
                        {"velocity", fieldDocument(v.velocity)},
                        {"inverseMetric", fieldDocument(v.inverseMetric)},
                        {"enabled", fieldDocument(v.enabled)}});
    std::map<const RelationType*, uint32_t> typeIds;
    for (const auto& r : snapshot.data->relations) {
        auto [it, added] = typeIds.emplace(r.type.get(), uint32_t(typeIds.size()));
        if (added) {
            const auto& t = *r.type;
            auto ids = Json::array();
            for (const auto& s : t.spaces)
                ids.push(spaceId(s));
            Json type = {{"name", t.name},
                         {"spaces", ids},
                         {"parameters", t.parameters},
                         {"history", t.history},
                         {"rows", t.rows},
                         {"kind", uint32_t(t.kind)},
                         {"residual", document(t.residual)}};
            if (t.update)
                type["update"] = document(*t.update);
            if (!t.objects.empty()) {
                auto schemas = Json::array();
                for (const auto& fields : t.objects) {
                    auto names = Json::array();
                    for (const auto& name : fields)
                        names.push(name);
                    schemas.push(std::move(names));
                }
                type["objects"] = std::move(schemas);
            }
            types.push(std::move(type));
        }
        auto endpoints = Json::array();
        for (const auto& source : r.endpoints)
            endpoints.push(endpointDocument(source));
        Json relation = {{"name", r.name},
                        {"type", it->second},
                        {"count", r.count},
                        {"dynamicEndpoints", r.dynamicEndpoints},
                        {"endpoints", endpoints},
                        {"parameters", fieldDocument(r.parameters)},
                        {"compliance", fieldDocument(r.compliance)},
                        {"history", fieldDocument(r.initialHistory)},
                        {"enabled", fieldDocument(r.enabled)}};
        if (r.pairs) {
            auto pairs = Json::array();
            for (const auto& pair : *r.pairs) {
                Json binding = {{"a", pair.a}, {"b", pair.b}};
                if (pair.self != PairBinding::Self::Unspecified) {
                    binding["self"] = pair.self == PairBinding::Self::Directed ? "directed" : "undirected";
                    binding["includeSelf"] = pair.includeSelf;
                }
                pairs.push(std::move(binding));
            }
            relation["pairs"] = std::move(pairs);
        }
        relations.push(std::move(relation));
    }
    return {{"spaces", spaces}, {"types", types}, {"variables", variables}, {"objects", objects}, {"relations", relations}};
}
std::unique_ptr<Model> modelFromDocument(const Json& j) {
    std::vector<SpaceRef> spaces;
    std::vector<RelationRef> types;
    for (const auto& v : j.at("spaces").elements()) {
        auto s = std::make_shared<Space>();
        s->name = v.at("name").string();
        s->stateSize = v.at("stateSize").uint();
        s->tangentSize = v.at("tangentSize").uint();
        s->retract = formula(v.at("retract"));
        s->difference = formula(v.at("difference"));
        s->validate();
        spaces.push_back(s);
    }
    for (const auto& v : j.at("types").elements()) {
        auto t = std::make_shared<RelationType>();
        t->name = v.at("name").string();
        for (const auto& id : v.at("spaces").elements())
            t->spaces.push_back(spaces.at(id.uint()));
        t->parameters = v.at("parameters").uint();
        t->history = v.at("history").uint();
        t->rows = v.at("rows").uint();
        auto kind = v.at("kind").uint();
        if (kind > 2)
            throw std::invalid_argument("Unknown residual condition");
        t->kind = RelationKind(kind);
        t->residual = formula(v.at("residual"));
        if (v.contains("update"))
            t->update = formula(v.at("update"));
        if (v.contains("objects"))
            for (const auto& fields : v.at("objects").elements()) {
                t->objects.emplace_back();
                for (const auto& name : fields.elements())
                    t->objects.back().push_back(name.string());
            }
        t->validate();
        types.push_back(t);
    }
    auto model = std::make_unique<Model>();
    for (const auto& v : j.at("variables").elements()) {
        VariableSet s;
        s.name = v.at("name").string();
        s.space = spaces.at(v.at("space").uint());
        s.count = v.at("count").uint();
        s.readOnly = v.at("readOnly").boolean();
        s.initial = fieldFrom(v.at("initial"), s.count, s.space->stateSize);
        s.velocity = fieldFrom(v.at("velocity"), s.count, s.space->tangentSize);
        s.inverseMetric =
            fieldFrom(v.at("inverseMetric"), s.count, s.space->tangentSize * s.space->tangentSize);
        s.enabled = fieldFrom(v.at("enabled"), s.count, 1);
        model->variables(std::move(s));
    }
    if (j.contains("objects"))
        for (const auto& v : j.at("objects").elements()) {
            auto object = objectFromDocument(v);
            if (object.member)
                model->member(object.member->object, object.member->index);
            else
                model->object(std::move(object));
        }
    for (const auto& v : j.at("relations").elements()) {
        RelationSet s;
        s.name = v.at("name").string();
        s.type = types.at(v.at("type").uint());
        s.count = v.at("count").uint();
        s.dynamicEndpoints = v.at("dynamicEndpoints").boolean();
        for (const auto& source : v.at("endpoints").elements())
            s.endpoints.push_back(endpointFrom(source));
        if (v.contains("pairs")) {
            s.pairs.emplace();
            for (const auto& pair : v.at("pairs").elements())
                s.pairs->push_back(pairFromDocument(pair));
        }
        s.parameters = fieldFrom(v.at("parameters"), s.count, s.type->parameters);
        s.compliance = fieldFrom(v.at("compliance"), s.count, s.type->rows);
        s.initialHistory = fieldFrom(v.at("history"), s.count, s.type->history);
        s.enabled = fieldFrom(v.at("enabled"), s.count, 1);
        model->relations(std::move(s));
    }
    return model;
}
SolverPolicy solverPolicy(const Json& j) {
    SolverPolicy p;
    if (j.contains("substeps"))
        p.substeps = j.at("substeps").uint();
    if (j.contains("iterations"))
        p.iterations = j.at("iterations").uint();
    if (j.contains("colorBudget"))
        p.colorBudget = j.at("colorBudget").uint();
    if (j.contains("relaxation"))
        p.relaxation = float(j.at("relaxation").number());
    if (j.contains("mode")) {
        auto m = j.at("mode").string();
        if (m == "hybrid")
            p.mode = SolveMode::Hybrid;
        else if (m == "colored")
            p.mode = SolveMode::Colored;
        else if (m == "jacobi")
            p.mode = SolveMode::Jacobi;
        else
            throw std::invalid_argument("Unknown solver mode");
    }
    if (j.contains("execution")) {
        auto m = j.at("execution").string();
        if (m == "auto")
            p.execution = ExecutionMode::Auto;
        else if (m == "global")
            p.execution = ExecutionMode::Global;
        else
            throw std::invalid_argument("Unknown solver execution mode");
    }
    return p;
}
} // namespace whimsical::dynamics
