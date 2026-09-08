#include "render/graph/RenderGraph.h"
#include <iostream>

// compile() is a pure function of the declarations, so the resource contract is testable
// without a device: what a pass says it does to the contents is the only input to
// dependencies, culling, validation and storage.
using namespace afterlight::rg;

static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> static void rejects(F&& f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}
static ResourceId image(Registry& registry, const char* name, Lifetime lifetime) {
    Declaration declaration;
    declaration.name = name;
    declaration.lifetime = lifetime;
    return registry.declare(std::move(declaration));
}

// A version nobody consumes before it is replaced takes its producer with it, whether the
// resource is transient or carried across frames.
static void culling() {
    Registry registry;
    auto scratch = image(registry, "scratch", Lifetime::Transient);
    auto blend = image(registry, "blend", Lifetime::Transient);
    auto carried = image(registry, "carried", Lifetime::Persistent);
    RenderGraph graph(registry);
    graph.add("replaced transient").overwrite(scratch, Access::Compute);
    graph.add("replaced persistent").overwrite(carried, Access::Compute);
    graph.add("produce").overwrite(scratch, Access::Compute);
    graph.add("consume").read(scratch, Access::Compute).overwrite(blend, Access::Compute);
    graph.add("final").read(blend, Access::Compute).overwrite(carried, Access::Compute);
    graph.compile(64, 64);
    check(!graph.alive(0) && !graph.alive(1), "A version nothing consumes must cull its producer");
    check(graph.alive(2) && graph.alive(3) && graph.alive(4) && graph.livePasses() == 3,
          "The chain reaching contents that outlive the frame must survive");
}

// Modify says the old contents have to reach the pass, so it is an edge to the previous
// producer rather than a fresh start.
static void modifyChain() {
    Registry registry;
    auto accumulator = image(registry, "accumulator", Lifetime::Persistent);
    RenderGraph graph(registry);
    graph.add("seed").overwrite(accumulator, Access::Compute);
    graph.add("unrelated").overwrite(image(registry, "unused", Lifetime::Transient), Access::Compute);
    graph.add("refine").modify(accumulator, Access::Compute);
    graph.compile(64, 64);
    check(graph.alive(0) && graph.alive(2) && !graph.alive(1),
          "Modify must keep the producer of the contents it preserves");
}

// The one thing declarations can get wrong that nothing downstream would notice.
static void undefinedContents() {
    auto build = [](bool loadAttachment) {
        Registry registry;
        auto undefined = image(registry, "undefined", Lifetime::Transient);
        auto carried = image(registry, "carried", Lifetime::Persistent);
        RenderGraph graph(registry);
        auto pass = graph.add("consumer");
        if (loadAttachment)
            pass.color(undefined);
        else
            pass.read(undefined, Access::Compute);
        pass.overwrite(carried, Access::Compute);
        graph.compile(64, 64);
    };
    rejects([&] { build(false); }, "Reading a transient nothing produced must be rejected");
    rejects([&] { build(true); }, "Loading an attachment nothing produced must be rejected");

    Registry registry;
    auto history = image(registry, "history", Lifetime::History);
    auto carried = image(registry, "carried", Lifetime::Persistent);
    RenderGraph graph(registry);
    graph.add("write current").overwrite(history, Access::Compute);
    graph.add("read previous").read(previous(history), Access::Compute).overwrite(carried, Access::Compute);
    graph.compile(64, 64);
    check(graph.livePasses() == 2, "History halves must be independent contents");
}

// Storage is a consequence of the same liveness that drove culling.
static void storage() {
    Registry registry;
    auto first = image(registry, "first", Lifetime::Transient);
    auto second = image(registry, "second", Lifetime::Transient);
    auto absent = image(registry, "absent", Lifetime::Transient);
    auto carried = image(registry, "carried", Lifetime::Persistent);
    RenderGraph graph(registry);
    graph.add("open").overwrite(first, Access::Compute);
    graph.add("close").read(first, Access::Compute).overwrite(carried, Access::Compute);
    graph.add("reopen").overwrite(second, Access::Compute);
    graph.add("reclose").read(second, Access::Compute).modify(carried, Access::Compute);
    graph.compile(64, 64);
    const auto& residency = graph.residency();
    check(graph.aliasedResources() == 1 && residency[second.index].root == first,
          "Transients whose live ranges do not overlap must share one allocation");
    check(!residency[absent.index].needed && residency[first.index].needed,
          "A transient no live pass touches must hold no memory");
    check(residency[carried.index].needed && residency[carried.index].root == carried,
          "Contents that cross the frame boundary always need their own storage");
}

// A pass's declarations are its own. Two builders open at once used to interleave into one
// shared array, so each pass ended up holding whichever declarations happened to land in
// its slice — the wrong resources synchronised, and the wrong pass allowed to touch them.
static void ownership() {
    Registry registry;
    auto early = image(registry, "early", Lifetime::Transient);
    auto late = image(registry, "late", Lifetime::Transient);
    auto carried = image(registry, "carried", Lifetime::Persistent);
    RenderGraph graph(registry);
    auto producer = graph.add("produce early");
    auto consumer = graph.add("consume early");
    // Declared out of order on purpose: the second pass is named first.
    consumer.read(early, Access::Compute).overwrite(late, Access::Compute);
    producer.overwrite(early, Access::Compute);
    graph.add("finish").read(late, Access::Compute).overwrite(carried, Access::Compute);
    graph.compile(64, 64);
    check(graph.livePasses() == 3, "A pass declared while another builder is open must still be linked");
    graph.checkDeclared(0, early);
    graph.checkDeclared(1, late);
    rejects([&] { graph.checkDeclared(0, late); },
            "A pass must not inherit the declarations of another");
    rejects([&] { graph.checkDeclared(1, carried); },
            "A pass must not inherit the declarations of another");
}

// Contents a pass reaches through a resource it does name. A ray query walks the top-level
// structure into the bottom level and has no name for it, so the resource says so once
// instead of every pass repeating it — and forgetting leaves the query unordered against
// the refit that fed it.
static void reached() {
    Registry registry;
    Declaration bottom;
    bottom.name = "blas";
    bottom.kind = Kind::AccelerationStructure;
    bottom.lifetime = Lifetime::Imported;
    auto blas = registry.declare(std::move(bottom));
    Declaration top;
    top.name = "tlas";
    top.kind = Kind::AccelerationStructure;
    top.lifetime = Lifetime::Imported;
    top.reaches = {blas};
    auto tlas = registry.declare(std::move(top));
    auto shaded = image(registry, "shaded", Lifetime::Persistent);
    RenderGraph graph(registry);
    graph.add("refit").modify(blas, Access::Build);
    graph.add("build").overwrite(tlas, Access::Build);
    graph.add("trace").read(tlas, Access::Trace).overwrite(shaded, Access::Compute);
    graph.compile(64, 64);
    check(graph.livePasses() == 3, "Naming the structure a pass walks must reach the level below it");
    graph.checkDeclared(2, blas);
}

// A resource the frame hands to a consumer outside the graph keeps its producer without
// anything pretending to be a side effect.
static void handover() {
    Registry registry;
    auto shaded = image(registry, "shaded", Lifetime::Transient);
    Declaration presented;
    presented.name = "swapchain";
    presented.lifetime = Lifetime::Imported;
    presented.handover = Access::Present;
    auto swapchain = registry.declare(std::move(presented));
    RenderGraph graph(registry);
    graph.add("shade").overwrite(shaded, Access::Compute);
    graph.add("blit").read(shaded, Access::Transfer).overwrite(swapchain, Access::Transfer);
    graph.compile(64, 64);
    check(graph.livePasses() == 2, "A handover must keep the pass that produced the contents");
    graph.checkDeclared(1, swapchain);
    rejects([&] { graph.checkDeclared(0, swapchain); },
            "Touching a resource a pass never declared must be rejected");
}

int main() {
    try {
        culling();
        modifyChain();
        undefinedContents();
        storage();
        ownership();
        reached();
        handover();
        std::cout << "Render graph contract verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
