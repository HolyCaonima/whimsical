#include "TestProject.h"
#include "scene/ScenePersistence.h"
#include "scripting/ScriptRuntime.h"
#include "core/FrameMailbox.h"
#include <fstream>
#include <iostream>
#include <thread>

using namespace afterlight;
namespace fs = std::filesystem;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> static void rejects(F&& f, const char* message) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
static AssetHeader header(const std::string& type) {
    AssetHeader h;
    h.id = newPersistentId();
    h.type = type;
    h.name = "Test";
    return h;
}
// Test fixture authoring intentionally bypasses the registry to exercise corrupt files.
static void fixture(const fs::path& file, const AssetHeader& h, const std::string& payload = "") {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "ALAS1\n" << h.json().dump() << '\n' << payload;
}
static void payloadStorage(const fs::path& directory) {
    AssetManager assets{Project::create(directory, "Payload storage")};
    auto inlineHeader = header("Binary");
    auto inlineRef = assets.save(AssetPath("/Game/Inline"), inlineHeader, "payload bytes");
    fs::create_directories(directory / "Content/blobs");
    std::ofstream(directory / "Content/blobs/weights.onnx", std::ios::binary) << "payload bytes";
    auto external = header("Binary"); // Same decoder, storage chosen by the envelope.
    external.storage = PayloadStorage::External;
    external.source = "../blobs/weights.onnx";
    AssetPath wrapper("/Game/headers/model");
    auto ref = assets.save(wrapper, external, "");
    check(assets.load<BinaryAsset>(inlineRef)->bytes == assets.load<BinaryAsset>(ref)->bytes,
          "Inline and external storage must use the same type decoder");
    check(assets.load<BinaryAsset>(ref)->reference().path == wrapper,
          "Loaded assets must retain their .asset locator, never the payload location");
    std::ifstream file(directory / "Content/headers/model.asset", std::ios::binary);
    std::string line;
    std::getline(file, line);
    std::getline(file, line);
    check(file.peek() == EOF && Json::parse(line).at("source").string() == external.source,
          "External .asset must contain only its descriptor and relative payload address");
    file.close();
    assets.scan();
    check(assets.size() == 2, "Raw payload files must never enter the asset registry");
    rejects([&] { assets.load(AssetPath("/Game/blobs/weights")); },
            "An extensionless virtual locator must not fall back to an ONNX payload");
    rejects([&] { assets.reference(AssetPath("/Game/blobs/weights")); },
            "Payload files cannot acquire asset references");
    rejects([&] { AssetPath direct("/Game/blobs/weights.onnx"); },
            "Virtual asset paths cannot target a payload extension");
    rejects(
        [&] {
            AssetRef::fromJson({{"id", ref.id}, {"path", "/Game/blobs/weights.onnx"}});
        },
        "Serialized asset references cannot point directly to a payload");
    rejects([&] { assets.save(wrapper, external, "inline bytes"); },
            "External storage cannot also contain an inline payload");
    auto ambiguous = inlineHeader.json();
    ambiguous["source"] = "blobs/weights.onnx";
    rejects([&] { AssetHeader::fromJson(ambiguous); },
            "Inline storage cannot declare an external payload address");
    fixture(directory / "Content/headers/model.asset", external, "unexpected bytes");
    assets.clearCache();
    rejects([&] { assets.load(wrapper); }, "Load must reject an external envelope with inline bytes");
    fixture(directory / "Content/headers/model.asset", external);
    fs::rename(directory / "Content/blobs/weights.onnx", directory / "Content/blobs/moved.onnx");
    external.source = "../blobs/moved.onnx";
    check(assets.save(wrapper, external, "").id == ref.id &&
              assets.load<BinaryAsset>(ref)->bytes == "payload bytes",
          "Moving payload changes only its address in the .asset; all asset references stay stable");
    auto dataHeader = header("Data");
    dataHeader.storage = PayloadStorage::External;
    dataHeader.source = "blobs/moved.onnx";
    rejects([&] { assets.save(AssetPath("/Game/BadData"), dataHeader, ""); },
            "External payload must be validated by its asset type decoder on save");
    check(!fs::exists(directory / "Content/BadData.asset"), "Invalid payload must not commit its header");
    fs::remove(directory / "Content/headers/model.asset");
    assets.scan();
    rejects([&] { assets.load(ref); }, "Removing the .asset must break the reference even if payload exists");
    registerEngineAssets(assets);
    std::ofstream(directory / "Content/blobs/empty.scene") << SceneDocument{}.json().dump();
    auto mapHeader = header("Map");
    mapHeader.storage = PayloadStorage::External;
    mapHeader.source = "blobs/empty.scene";
    AssetPath mapPath("/Game/ImportedMap");
    assets.save(mapPath, mapHeader, "");
    World imported;
    ScenePersistence::load(imported, assets, mapPath);
    auto saved = ScenePersistence::save(imported, assets, mapPath, "Authored Map");
    check(saved.id == mapHeader.id && assets.descriptor(mapPath).storage == PayloadStorage::Inline,
          "Scene saving must write its document inline and preserve the Map identity");
}
static void registry(const fs::path& directory) {
    auto project = Project::create(directory, "Portable test project");
    check(Project(directory / ".project").id() == project.id(), "Project identity must survive reopen");
    rejects([&] { Project::create(directory, "Overwrite"); },
            "Project create must not overwrite a descriptor");
    AssetManager assets(project);
    registerEngineAssets(assets);
    check(assets.size() == 0 && project.startupMap().empty(),
          "Empty project must open without a startup Map");
    World emptyWorld;
    ScriptRuntime emptyScripts(emptyWorld, assets);
    emptyScripts.initialize();
    emptyScripts.tick(1.f / 60, {});
    for (const char* invalid : {"models/a", "/Game", "/Game/../a", "/Game/a.asset", "/Game/a/", "/Game/a//b",
                                "/Game/a\\b", "/Game/C:/a"})
        rejects([&] { AssetPath path(invalid); }, "Reject noncanonical virtual paths");
    auto h = header("Data");
    AssetPath path("/Game/Test/value");
    auto reference = assets.save(path, h, "{\"value\":17}");
    auto first = assets.load<DataAsset>(path);
    check(first->data.at("value").uint() == 17 && first == assets.load<DataAsset>(reference),
          "Cache must share immutable typed resources");
    rejects([&] { assets.load<ScriptAsset>(path); }, "Typed access must reject mismatches");
    rejects([&] { assets.load(AssetPath("/Game/Missing")); }, "Missing assets must report failure");
    assets.save(path, h, "{\"value\":18}");
    check(first->data.at("value").uint() == 17 && assets.load<DataAsset>(path)->data.at("value").uint() == 18,
          "Save must preserve old resource generations");
    rejects([&] { assets.save(AssetPath("/Game/Test/VALUE"), header("Data"), "{}"); },
            "Case aliases must not overwrite a registered file on Windows");
    rejects([&] { assets.save(path, h, "invalid json"); },
            "Failed validation must not replace committed asset");
    check(assets.load<DataAsset>(path)->data.at("value").uint() == 18,
          "Failed save must retain cached content");
    fs::rename(directory / "Content/Test/value.asset", directory / "Content/Test/renamed.asset");
    assets.scan();
    check(assets.resolve(reference).path == AssetPath("/Game/Test/renamed"),
          "Persistent reference must survive an asset move");
    check(assets.load<DataAsset>(reference)->data.at("value").uint() == 18,
          "Old path hints must resolve by ID");
    fixture(directory / "Content/duplicate.asset", h, "{}");
    rejects([&] { assets.scan(); }, "Duplicate asset IDs must be rejected");
    check(assets.load<DataAsset>(reference)->data.at("value").uint() == 18,
          "Failed scan must preserve previous registry");
    fs::remove(directory / "Content/duplicate.asset");
    auto external = header("OnnxModel");
    external.storage = PayloadStorage::External;
    external.source = "../outside.onnx";
    std::ofstream(directory / "outside.onnx") << "not an ONNX model";
    fixture(directory / "Content/escape.asset", external);
    rejects([&] { assets.scan(); }, "Header-only source must remain inside Content");
    fs::remove(directory / "Content/escape.asset");
    external.source = "missing.onnx";
    fixture(directory / "Content/missing.asset", external);
    rejects([&] { assets.scan(); }, "Missing external payload must fail registration");
    fs::remove(directory / "Content/missing.asset");
    auto unsupported = header("Data");
    unsupported.version = 2;
    fixture(directory / "Content/version.asset", unsupported, "{}");
    rejects([&] { assets.scan(); }, "Unknown envelope version must not load");
    fs::remove(directory / "Content/version.asset");
    assets.registerLoader("Cycle", [](AssetManager& manager, const AssetHeader&, const std::string& bytes) {
        (void)manager.load(AssetPath(bytes));
        return std::make_shared<DataAsset>();
    });
    fixture(directory / "Content/cycleA.asset", header("Cycle"), "/Game/cycleB");
    fixture(directory / "Content/cycleB.asset", header("Cycle"), "/Game/cycleA");
    assets.scan();
    rejects([&] { assets.load(AssetPath("/Game/cycleA")); },
            "Dependency cycles must fail instead of recursing");
    rejects([&] { assets.load(AssetPath("/Game/cycleA")); },
            "Failed loads must release their in-progress marks");
    bool wrongThread = false;
    std::thread other([&] {
        try {
            assets.load(reference);
        } catch (const std::logic_error&) {
            wrongThread = true;
        }
    });
    other.join();
    check(wrongThread, "Registry mutation/access must remain on its owner thread");
    assets.clearCache();
    check(first->data.at("value").uint() == 17, "Cache eviction cannot invalidate live references");
    SceneDocument empty;
    assets.save(AssetPath("/Game/EmptyMap"), header("Map"), empty.json().dump());
    auto loader =
        assets.save(AssetPath("/Game/EntryScript"), header("Script"), "Engine.loadScene('/Game/EmptyMap');");
    empty.scripts.push_back(loader);
    assets.save(AssetPath("/Game/EntryMap"), header("Map"), empty.json().dump());
    emptyScripts.loadScene(AssetPath("/Game/EntryMap"));
    check(emptyWorld.mapAsset().path == AssetPath("/Game/EntryMap"),
          "Top-level scripts must not replace their VM during the script loading batch");
    emptyScripts.tick(1.f / 60, {});
    check(emptyWorld.mapAsset().path == AssetPath("/Game/EmptyMap"),
          "Top-level scene request must run at the next owner boundary");
}
static void scene(const fs::path& directory) {
    // Relocation test: no runtime content access may depend on the repository's game path.
    fs::copy(fs::path(AFTERLIGHT_ROOT) / "game", directory, fs::copy_options::recursive);
    AssetManager assets{Project(directory)};
    registerEngineAssets(assets);
    auto path = assets.project().startupMap();
    auto original = assets.load<SceneAsset>(path);
    check(original->header().storage == PayloadStorage::Inline, "Rain Court Map must have an inline payload");
    check(original->scene.objects.size() == 48 && original->scene.lights.size() == 8,
          "Rain Court migration must preserve authored content");
    auto human = assets.load<animation::ai4animation::ControllerResource>(
        AssetPath("/Game/animations/ai4animation/biped/controller"));
    check(human->data->network == assets.load<animation::ai4animation::OnnxModel>(
                                      AssetPath("/Game/animations/ai4animation/biped/network")),
          "Controller dependencies must use the shared registry");
    check(human->data->network->header().storage == PayloadStorage::External,
          "ONNX references must use header-only assets");
    World world;
    ScriptRuntime scripts(world, assets);
    scripts.initialize();
    check(ScenePersistence::capture(world, assets).json() == original->scene.json(),
          "Load/capture must round-trip the authored Map exactly");
    auto oldPlayer = world.playerId;
    auto oldBody = world.entity(oldPlayer).physical;
    auto objectPath = world.objectPath(oldPlayer);
    check(ObjectPath(objectPath.string()).object == world.entity(oldPlayer).persistentId,
          "Object path must identify Map and Object");
    check(!world.resolveObject(ObjectPath(AssetPath("/Game/OtherMap"), objectPath.object)),
          "Object IDs must be scoped by Map path");
    auto dead = world.spawn("Temporary", Shape::Box, {0, 10, 0}, {1, 1, 1}, 0, false, false);
    auto deletedId = world.entity(dead).persistentId;
    world.sceneReferences["temporary"] = deletedId;
    world.destroy(dead);
    check(!world.sceneReferences.count("temporary"), "Destroy must remove named references to dead Objects");
    auto prop = world.spawn("Duplicate name", Shape::Box, {1, 10, 0}, {1, 2, 3}, 0, false, false);
    auto other = world.spawn("Duplicate name", Shape::Box, {2, 10, 0}, {1, 1, 1}, 1, false, false);
    world.setPose(prop, {3, 10, 1}, .4f, 2);
    world.setVisualPose(prop, {.1f, .2f, .3f}, {1, 2, 1});
    world.setVisible(prop, false);
    world.setEnabled(other, false);
    world.setAnimationJoints(prop, {{{0, 1, 0}, quat(1, 0, 0, 0)}});
    world.addAnimationCollider(prop, 0, ColliderShape::box(vec3(.1f)), {{0, .2f, 0}, quat(1, 0, 0, 0)},
                               false);
    world.addAnimationCollider(oldPlayer, 0, ColliderShape::capsule(.1f, .3f), {}, false);
    world.setAnimationAttribute(oldPlayer, "locomotion.style", "Zombie");
    world.navigation.planeTolerance = .03f;
    scripts.execute("Interactions.execute(Engine.sceneObject('beacon')); "
                    "Interactions.execute(Engine.sceneObject('cache'));");
    check(!world.sceneData.at("power").boolean(), "Interaction must update explicit persistent state");
    auto expected = ScenePersistence::capture(world, assets).json();
    AssetPath saved("/Game/Maps/Saved");
    auto savedRef = scripts.saveScene(saved, "Saved Rain Court");
    check(savedRef.id != original->header().id && world.objectPath(oldPlayer).object == objectPath.object,
          "Save As must create Map identity and retain Object identities");
    check(!world.resolveObject(objectPath), "Save As must update virtual Object path namespace");
    auto savedObjectPath = world.objectPath(oldPlayer);
    check(scripts.saveScene(saved, "Saved Rain Court").id == savedRef.id,
          "Saving in place must preserve Map identity");
    auto before = world.snapshot({}, 1, 0, 0);
    FrameMailbox mailbox;
    mailbox.publish(before);
    FrameRef delivered;
    uint64_t cursor = 0;
    mailbox.acquire(delivered, cursor);
    auto mesh = delivered->skins[0].mesh;
    scripts.loadScene(saved);
    check(world.playerId != oldPlayer && !world.physics().contains(oldBody),
          "Reload must invalidate old Entity and physics identities");
    rejects([&] { world.entity(oldPlayer); }, "A stale Entity must never target a newly loaded Object");
    check(world.resolveObject(savedObjectPath) == world.playerId && !world.findObject(deletedId),
          "Object identity must survive while deleted objects stay absent");
    check(ScenePersistence::capture(world, assets).json() == expected,
          "All persistent components and gameplay data must round-trip");
    auto loaded = world.snapshot({}, 2, 0, 0);
    check(loaded.delta.base == before.delta.revision && !loaded.delta.structural.empty(),
          "Map replacement must preserve render delta continuity");
    mailbox.publish(loaded);
    mailbox.publish(world.snapshot({}, 3, 0, 0));
    mailbox.acquire(delivered, cursor);
    check(delivered->resetHistory && !delivered->delta.structural.empty(),
          "Dropped scene-load frame must preserve structural changes and history reset");
    check(mesh->vertices.size() > 0, "Published skin resources must outlive cache generations");
    scripts.execute("Interactions.execute(Engine.sceneObject('beacon'));");
    check(world.sceneData.at("power").boolean(),
          "Fresh gameplay bindings must toggle the restored state, not stale closures");
    scripts.execute("Engine.loadScene('/Game/Maps/Saved');");
    check(!world.sceneData.at("power").boolean(),
          "Queued JS load must replace scene after returning from JS");
    scripts.tick(1.f / 60, {});
    check(world.snapshot({}, 4, 0, 0).skins.size() == 2, "Reloaded animation instances must run and render");
    auto bad = original->scene;
    bad.objects[0].id = bad.objects[1].id;
    rejects([&] { bad.json(); }, "Duplicate Object IDs must be rejected");
    bad = original->scene;
    bad.player = newPersistentId();
    rejects([&] { bad.json(); }, "Dangling persistent references must be rejected");
    bad = original->scene;
    for (auto& o : bad.objects)
        if (o.animation) {
            o.animation->asset.id = newPersistentId();
            break;
        }
    auto badHeader = header("Map");
    fixture(directory / "Content/Maps/Broken.asset", badHeader, bad.json().dump());
    assets.scan();
    auto stateBefore = ScenePersistence::capture(world, assets).json();
    auto entityBefore = world.playerId;
    rejects([&] { scripts.loadScene(AssetPath("/Game/Maps/Broken")); },
            "Missing dependency must reject scene loading");
    check(world.playerId == entityBefore && ScenePersistence::capture(world, assets).json() == stateBefore,
          "Dependency failure must leave live scene untouched");
    // Reopen the copied project and load the saved asset using a different registry/World.
    AssetManager reopened{Project(directory / ".project")};
    registerEngineAssets(reopened);
    World second;
    ScenePersistence::load(second, reopened, saved);
    check(ScenePersistence::capture(second, reopened).json() == expected,
          "Scene must load independently from a relocated project directory");
    world.clearScene();
    check(world.physics().size() == 0 && world.playerId == 0,
          "Unload must release all bodies and animation attachments");
    SceneDocument empty;
    auto emptyHeader = header("Map");
    AssetPath emptyPath("/Game/Maps/Empty");
    reopened.save(emptyPath, emptyHeader, empty.json().dump());
    ScenePersistence::load(second, reopened, emptyPath);
    check(second.physics().size() == 0 && second.snapshot({}, 5, 0, 0).skins.empty(),
          "Empty Maps must load and publish a clean scene");
}
int main() {
    try {
        auto directory = fs::path(AFTERLIGHT_ROOT) / "build" / ("asset-scene-tests-" + newPersistentId());
        payloadStorage(directory / "payloads");
        registry(directory / "empty");
        scene(directory / "relocated");
        std::cout << "Project, asset registry, relocation and Scene Save/Load tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
