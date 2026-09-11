#include "physics/dynamics/runtime/Instance.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <cstring>
using namespace whimsical;
using namespace whimsical::dynamics;
static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
static void near(double a, double b) {
    check(std::abs(a - b) < 2e-4, "Numerical result differs from analytic solution");
}
static std::shared_ptr<const std::vector<VariableRef>> column(uint32_t count, bool repeated = false) {
    auto result = std::make_shared<std::vector<VariableRef>>(count);
    for (uint32_t i = 0; i < count; ++i)
        (*result)[i] = {0, repeated ? 0 : i};
    return result;
}
static void candidateDomainChecks(rc::RenderCore& core, SpaceRef scalar) {
    // Two focused groups: changes within an iteration sequence, then invalidation
    // and range fallback across submissions. All relations are user mathematics.
    constexpr uint32_t count = 128;
    Model model;
    std::vector<float> initial(count);
    for (uint32_t i = 0; i < count; ++i) initial[i] = float(i * 16);
    initial[1] = .125f;
    VariableSet values;
    values.space = scalar;
    values.count = count;
    values.initial = Field::dense(count, 1, initial);
    auto variables = model.variables(values);
    Object collection;
    collection.kind = Object::Kind::Collection;
    collection.count = count;
    collection.dofs["u"] = EndpointSource::collection(variables, 0, count);
    auto object = model.object(collection);
    RelationBuilder bound("quadratic difference", {scalar, scalar}, 1);
    auto delta = bound.endpoint(0)[0] - bound.endpoint(1)[0];
    auto boundType = std::make_shared<RelationType>(*bound.finish(
        {delta * delta - bound.parameter(0) * bound.parameter(0)}, RelationKind::GreaterEqual));
    boundType->objects = {{"u"}, {"u"}};
    RelationSet domain;
    domain.type = boundType;
    domain.pairs = std::vector<PairBinding>{{object, object, PairBinding::Self::Undirected, false}};
    domain.count = count * (count - 1) / 2;
    domain.parameters = Field::uniform(domain.count, {1});
    auto relation = model.relations(domain);
    RelationBuilder target("scalar target", {scalar}, 1);
    RelationSet drive;
    drive.type = target.finish({target.endpoint(0)[0] - target.parameter(0)});
    drive.count = 1;
    drive.endpoints = {EndpointSource::object({variables, 2})};
    drive.parameters = Field::uniform(1, {-1.5f});
    drive.enabled = Field::uniform(1, {0});
    auto driven = model.relations(drive);
    auto commit = model.commit();
    SolverPolicy policy;
    policy.mode = SolveMode::Jacobi;
    policy.substeps = 1;
    policy.iterations = 2;
    policy.weighting = JacobiWeighting::Active;
    auto indexedPlan = Compiler().compile(commit.snapshot, policy);
    policy.spatialCandidates = false;
    auto fullPlan = Compiler().compile(commit.snapshot, policy);
    check(indexedPlan->statistics.candidateDomains == 1 && fullPlan->statistics.candidateDomains == 0,
          "Candidate checks must exercise indexed and full-domain execution");
    Instance indexed(core, indexedPlan), full(core, fullPlan);
    auto compare = [&](uint64_t tick, const std::vector<float>& q) {
        TickInput input;
        input.tick = tick;
        input.modelVersion = commit.snapshot.version;
        input.writes = {{StateField::Value, variables, 0, q},
                        {StateField::Velocity, variables, 0, std::vector<float>(count)}};
        for (auto* instance : {&indexed, &full}) {
            instance->step(input);
            const auto& done = instance->wait();
            check(!done.invalidEvaluations && !done.singularSystems, "Candidate diagnostics");
        }
        auto a = indexed.read(StateField::Value, variables, 0, count);
        auto b = full.read(StateField::Value, variables, 0, count);
        for (uint32_t i = 0; i < count; ++i) near(a[i], b[i]);
        return a;
    };
    // First correction makes the separation 4.0625, outside adjacent 1.25-wide
    // cells. The second must still release part of the nonzero multiplier.
    auto q = compare(1, initial);
    const double firstGap = (.125 + 1 / .125) / 2;
    near(q[1] - q[0], (firstGap + 1 / firstGap) / 2);
    // The target moves a previously distant endpoint into a new cell after the
    // first Jacobi snapshot. Reusing that first index would miss its correction.
    model.patch(FieldKind::RelationEnabled, driven, 0, {1});
    commit = model.commit();
    indexed.apply(commit); full.apply(commit);
    q = compare(2, initial);
    check(q[2] > -1.45f, "Iteration index missed a newly violated relation");

    // A single logical row widens the domain bound. Runtime must invalidate the
    // cached GPU reduction without changing the plan, row IDs or other parameters.
    model.patch(FieldKind::RelationEnabled, driven, 0, {0});
    model.patch(FieldKind::Parameters, relation, 0, {4});
    commit = model.commit();
    indexed.apply(commit); full.apply(commit);
    initial[1] = 3;
    q = compare(3, initial);
    check(q[1] - q[0] > 3.99f, "Parameter patch left a stale candidate bound");
    // Keep the same finite residual, but exceed the safe coordinate-to-cell range.
    // The indexed plan must run the complete logical domain, not discard it.
    for (auto& value : initial) value += 2000000.f;
    q = compare(4, initial);
    check(q[1] - q[0] >= 3.875f, "Unsafe cell coordinates did not fall back to the full domain");
    std::cout << "Candidate iteration/lifetime and bound/fallback checks passed\n";
}
int main() {
    try {
        Expression e(2);
        auto x = e.input(0), y = e.input(1);
        auto f = e.finish(sin(x) * y + x * x);
        auto j = f.jacobian({.3, 2});
        near(j[0], 2 * std::cos(.3) + .6);
        near(j[1], std::sin(.3));
        auto space = Space::euclidean(1);
        RelationBuilder builder("aliased sum", {space, space}, 1);
        auto type = builder.finish({builder.endpoint(0)[0] + builder.endpoint(1)[0] - builder.parameter(0)});
        rc::RenderCore core({true, false, nullptr});
        for (auto mode : {SolveMode::Colored, SolveMode::Jacobi}) {
            Model model;
            VariableSet variables;
            variables.name = "scalars";
            variables.space = space;
            variables.count = 100000;
            variables.initial = Field::uniform(variables.count, {0});
            auto id = model.variables(variables);
            RelationSet relations;
            relations.name = "sums";
            relations.type = type;
            relations.count = variables.count;
            relations.endpoints = {column(variables.count), column(variables.count)};
            relations.parameters = Field::uniform(relations.count, {4});
            model.relations(relations);
            auto commit = model.commit();
            SolverPolicy policy;
            policy.mode = mode;
            policy.substeps = policy.iterations = 1;
            auto plan = Compiler().compile(commit.snapshot, policy);
            Instance instance(core, plan);
            instance.step({1, commit.snapshot.version});
            auto completed = instance.wait();
            check(!completed.invalidEvaluations && !completed.singularSystems,
                  "GPU diagnostics reported a solve error");
            for (auto value : instance.read(StateField::Value, id, 99996, 4))
                near(value, 2);
            auto snapshot = instance.publish();
            model.patch(FieldKind::Parameters, 0, 99999, {6});
            commit = model.commit();
            instance.apply(commit);
            instance.step({2, commit.snapshot.version});
            instance.wait();
            near(instance.read(StateField::Value, id, 99999, 1)[0], 3);
            {
                rg::Registry registry;
                rg::Declaration input;
                input.name = "Published scalars";
                input.kind = rg::Kind::Buffer;
                input.byteSize = 400000;
                input.lifetime = rg::Lifetime::Imported;
                input.view = {rg::BindingType::Storage, true, {}, "Snapshot", "float values[];"};
                auto source = registry.declare(input);
                input.name = "Snapshot readback";
                input.view = {};
                input.lifetime = rg::Lifetime::Persistent;
                input.handover = rg::Access::Host;
                auto target = registry.declare(input);
                rc::GraphContext consumer(core, registry);
                snapshot.import(consumer, source, StateField::Value);
                snapshot = {};
                consumer.readback(source, target);
                consumer.compile();
                consumer.record();
                consumer.submit();
                consumer.wait();
                auto data = consumer.readbackData(target);
                float retained;
                std::memcpy(&retained, data.data() + 399996, 4);
                near(retained, 2);
            }
            model.appendVariables(id, 1, {9}, {0}, {1});
            commit = model.commit();
            instance.install(Compiler().compile(commit.snapshot, policy));
            auto tail = instance.read(StateField::Value, id, 99999, 2);
            near(tail[0], 3);
            near(tail[1], 9);
            std::cout << "100000 aliased relations, mode=" << int(mode) << ", buffers=" << BufferCount
                      << ", kernels=" << plan->kernels.size() << ", batches=" << plan->solve.size() << "\n";
        }
        Model star;
        VariableSet v;
        v.name = "shared scalar";
        v.space = space;
        v.count = 1;
        v.initial = Field::uniform(1, {0});
        star.variables(v);
        RelationSet r;
        r.name = "shared relations";
        r.type = type;
        r.count = 10000;
        r.endpoints = {column(r.count, true), column(r.count, true)};
        r.parameters = Field::uniform(r.count, {4});
        star.relations(r);
        SolverPolicy p;
        p.substeps = p.iterations = 1;
        p.colorBudget = 2;
        auto commit = star.commit();
        auto plan = Compiler().compile(commit.snapshot, p);
        check(plan->statistics.jacobiRelations == 9998, "Hybrid did not bound coloring");
        Instance instance(core, plan);
        instance.step({1, commit.snapshot.version});
        instance.wait();
        near(instance.read(StateField::Value, 0, 0, 1)[0], 2);
        Model dynamic;
        v.count = 100001;
        v.initial = Field::uniform(v.count, {0});
        dynamic.variables(v);
        r.dynamicEndpoints = true;
        r.endpoints = {column(r.count), column(r.count)};
        dynamic.relations(r);
        auto dynamicCommit = dynamic.commit();
        auto dynamicPlan = Compiler().compile(dynamicCommit.snapshot, p);
        Instance moving(core, dynamicPlan);
        moving.step({1, dynamicCommit.snapshot.version});
        moving.wait();
        near(moving.read(StateField::Value, 0, 9999, 1)[0], 2);
        TickInput next;
        next.tick = 2;
        next.modelVersion = dynamicCommit.snapshot.version;
        next.writes = {{StateField::Value, 0, 0, {0}}, {StateField::Velocity, 0, 0, {0}}};
        next.endpoints.push_back({0, 0, {*column(r.count, true), *column(r.count, true)}});
        moving.step(next);
        auto done = moving.wait();
        near(moving.read(StateField::Value, 0, 0, 1)[0], 2);
        check(!done.invalidEvaluations && !done.singularSystems, "Dynamic incidence solve failed");
        Model block;
        auto r2 = Space::euclidean(2);
        VariableSet pair;
        pair.name = "pair";
        pair.space = r2;
        pair.count = 1;
        pair.initial = Field::uniform(1, {0, 0});
        pair.inverseMetric = Field::uniform(1, {2, .5f, .5f, 1});
        block.variables(pair);
        RelationBuilder coupled("coupled rows", {r2}, 0, 1, 2);
        auto q = coupled.endpoint(0);
        RelationSet equations;
        equations.name = "equations";
        equations.type =
            coupled.finish({q[0] + q[1] - 4, q[0] - q[1] - 2}, RelationKind::Equality, {coupled.state(0) + 1});
        equations.count = 1;
        equations.endpoints = {column(1)};
        block.relations(equations);
        auto blockCommit = block.commit();
        Instance coupledSolve(core, Compiler().compile(blockCommit.snapshot, p));
        coupledSolve.step({1, blockCommit.snapshot.version});
        coupledSolve.wait();
        auto solution = coupledSolve.read(StateField::Value, 0, 0, 1);
        near(solution[0], 3);
        near(solution[1], 1);
        near(coupledSolve.read(StateField::History, 0, 0, 1)[0], 1);
        candidateDomainChecks(core, space);
        check(core.errors() == 0, "Vulkan validation errors");
        std::cout << "Dynamics focused checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
