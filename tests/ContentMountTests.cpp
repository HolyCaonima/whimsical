#include "assets/EngineAssets.h"
#include "scene/ScenePersistence.h"
#include "scripting/RuntimeHost.h"
#include "uiCore/UiCore.h"
#include <RmlUi/Core.h>
#include <fstream>
#include <iostream>
using namespace afterlight;
namespace fs = std::filesystem;
static void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> static void rejects(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "Expected a Content boundary error");
}
static AssetHeader header(const char* id, const char* type) {
    AssetHeader h;
    h.id = id;
    h.name = type;
    h.type = type;
    return h;
}
static void bytes(const fs::path& file, const std::string& data) {
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary);
    stream << data;
}
static void fixture(const fs::path& root, int value) {
    auto write = [&](const char* name, const AssetHeader& h, const std::string& data) {
        bytes(root / (std::string(name) + ".asset"), "ALAS1\n" + h.json().dump() + "\n" + data);
    };
    auto h = header("11111111111111111111111111111111", "Data");
    write("value", h, Json{{"value", value}}.dump());
    AssetRef local{h.id, AssetPath("/Game/value")};
    h = header("22222222222222222222222222222222", "Data");
    h.metadata = {{"target", local.json()}};
    write("link", h, Json{{"target", local.json()}}.dump());
    h = header("33333333333333333333333333333333", "Binary");
    h.storage = PayloadStorage::External;
    h.source = "raw/blob.bin";
    bytes(root / h.source, "external bytes");
    write("blob", h, "");
    h = header("44444444444444444444444444444444", "Script");
    write("script", h, "var executed = true; function initialize() { Engine.readJson('/Game/value'); }");
    SceneDocument map;
    map.scripts.push_back({h.id, AssetPath("/Game/script")});
    write("map", header("55555555555555555555555555555555", "Map"), map.json().dump());
    bytes(root / "UI/panel.rml", "<rml><head><link type=\"text/rcss\" href=\"/Game/UI/style.rcss\"/></head>"
                                 "<body><div id=\"box\"></div></body></rml>");
    bytes(root / "UI/style.rcss", "#box { display: block; width: " + std::to_string(value * 10) +
                                      "px; height: 10px; background-color: red; }");
}
int main() {
    try {
        auto root = fs::path(AFTERLIGHT_ROOT) / "build" / ("content-mounts-" + newPersistentId());
        fixture(root / "a", 1);
        fixture(root / "b", 2);
        AssetManager assets;
        registerEngineAssets(assets);
        World world;
        RuntimeHost scripts(world, assets);
        scripts.execute(
            "var roots = " +
            Json{{"a", (root / "a").generic_u8string()}, {"b", (root / "b").generic_u8string()}}.dump() +
            ";");
        std::ifstream example(fs::path(AFTERLIGHT_ROOT) / "tests/ContentMountsExample.js");
        std::string source((std::istreambuf_iterator<char>(example)), {});
        scripts.execute(source, "ContentMountsExample.js");
        auto a = assets.reference(AssetPath("/A/value"));
        auto b = assets.reference(AssetPath("/B/value"));
        auto old = assets.load<DataAsset>(a);
        assets.scan("/B");
        check(old == assets.load<DataAsset>(a), "Rescanning B must preserve A's cache");
        auto link = assets.read(AssetPath("/B/link"));
        assets.write(link.ref, link.header, link.payload);
        auto stored = ContentMounts::read(assets.file("/B/link.asset"));
        check(stored.find("/B/") == std::string::npos && stored.find(b.source) == std::string::npos &&
                  stored.find("/Game/value") != std::string::npos,
              "Persistent refs must be local and portable");
        check(ContentMounts::read(assets.file("/B/raw/blob.bin")) == "changed external bytes",
              "External write must use the same mapping");
        check(ContentMounts::read(assets.file("/A/raw/blob.bin")) == "external bytes",
              "External writes must stay in their source");
        auto badHeader = assets.descriptor(AssetPath("/B/link"));
        badHeader.metadata = {{"target", a.json()}};
        rejects([&] { assets.save(link.ref, badHeader, link.payload); });
        // Decoder code is subject to the same scope, even without a JSON payload.
        assets.registerLoader("Dependency",
                              [](AssetManager& manager, const AssetHeader&, const std::string& path) {
                                  (void)manager.load(AssetPath(path));
                                  return std::make_shared<BinaryAsset>();
                              });
        auto dependency = header("66666666666666666666666666666666", "Dependency");
        rejects([&] { assets.save(AssetPath("/B/dependency"), dependency, "/A/value"); });
        assets.save(AssetPath("/B/dependency"), dependency, "/Game/value");
        (void)assets.load(AssetPath("/B/dependency"));
        // Failed scans and mounts preserve active namespaces.
        bytes(root / "b/duplicate.asset", ContentMounts::read(assets.file("/B/value.asset")));
        rejects([&] { assets.scan("/B"); });
        check(assets.load<DataAsset>(b)->data.at("value").uint() == 20, "Failed scan must retain old index");
        fs::remove(root / "b/duplicate.asset");
        rejects([&] { assets.mount("/Overlap", root / "b/raw"); });
        auto escape = assets.descriptor(AssetPath("/B/blob"));
        escape.source = "../a/raw/blob.bin";
        rejects([&] { assets.save(AssetPath("/B/blob"), escape, ""); });
        // Delayed Map dependencies keep their source, and mixed-source scenes cannot be saved.
        auto map = assets.load<SceneAsset>(AssetPath("/B/map"));
        check(map->scene.scripts.front().source == b.source, "Map refs retain origin after decoding");
        ScenePersistence::load(world, assets, AssetPath("/B/map"));
        rejects([&] { ScenePersistence::save(world, assets, AssetPath("/A/copy"), "Mixed"); });
        ScenePersistence::save(world, assets, AssetPath("/B/map"), "B map");
        // UI uses the same source identities for documents and their /Game dependencies.
        ui::UiCore ui(assets.mounts());
        auto* adoc = ui.loadDocument("/A/UI/panel.rml");
        auto* bdoc = ui.loadDocument("/B/UI/panel.rml");
        adoc->Show();
        bdoc->Show();
        ui.context().Update();
        check(adoc->GetElementById("box")->GetOffsetWidth() == 10 &&
                  bdoc->GetElementById("box")->GetOffsetWidth() == 20,
              "UI /Game CSS resolves within its document");
        auto uiFile = assets.file("/B/UI/panel.rml");
        rejects([&] { assets.mounts()->relative(uiFile, "/A/UI/style.rcss"); });
        rejects([&] { assets.mounts()->relative(uiFile, "../../a/UI/style.rcss"); });
        auto uri = ui.resolve("/B/UI/panel.rml");
        auto previous = assets.load<DataAsset>(b);
        assets.unmount("/B");
        rejects([&] { ui.loadDocument(uri); });
        rejects([&] { assets.load(map->scene.scripts.front()); });
        check(previous->data.at("value").uint() == 20, "Live immutable values survive unmount");
        bytes(root / "b/UI/style.rcss", "#box { display: block; width: 30px; height: 10px; }");
        assets.mount("/B", root / "b");
        auto* next = ui.loadDocument("/B/UI/panel.rml");
        next->Show();
        ui.context().Update();
        check(next->GetElementById("box")->GetOffsetWidth() == 30 && uri != ui.resolve("/B/UI/panel.rml"),
              "Remount must not reuse old UI dependency caches");
        rejects([&] { assets.resolve(b); });
        // Explicit scene execution works from a non-/Game mount.
        scripts.loadScene(assets.reference(AssetPath("/B/map")));
        scripts.execute("if (executed !== true) throw Error('explicit scene starts its scripts');"
                        "Engine.saveScene('/Game/map', 'B map');");
        std::cout << "Content namespace, persistence, lifecycle and UI example passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
