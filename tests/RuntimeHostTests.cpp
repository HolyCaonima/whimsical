#include "assets/EngineAssets.h"
#include "scripting/RuntimeHost.h"
#include "scene/ScenePersistence.h"
#include "uiCore/UiCore.h"
#include <RmlUi/Core.h>
#include <fstream>
#include <iostream>

using namespace afterlight;
namespace fs = std::filesystem;
static void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
template <class F> static void rejects(F action, const char* why) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, why);
}
static void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
static AssetRef save(AssetManager& assets, const char* path, const char* type, const std::string& payload) {
    AssetHeader header;
    header.id = newPersistentId();
    header.name = type;
    header.type = type;
    return assets.save(AssetPath(path), header, payload);
}
static fs::path fixture() {
    auto root = fs::path(AFTERLIGHT_ROOT) / "build" / ("runtime-host-" + newPersistentId());
    auto project = Project::create(root / "Host", "Runtime host contract fixture");
    fs::create_directories(root / "Target/shaders");
    fs::copy_file(fs::path(AFTERLIGHT_ROOT) / "Projects/EntityID/Content/shaders/Standard.asset",
                  root / "Target/shaders/Standard.asset");
    fs::create_directories(project.content() / "shaders");
    fs::copy_file(root / "Target/shaders/Standard.asset", project.content() / "shaders/Standard.asset");
    AssetManager assets(project.content());
    assets.mount("/Target", root / "Target");
    registerEngineAssets(assets);
    save(assets, "/Game/settings", "Data",
         Json{{"owner", "host"}, {"target", (root / "Target").generic_u8string()}}.dump());
    save(assets, "/Target/settings", "Data", R"({"owner":"scene"})");
    save(assets, "/Game/Picking", "RenderTarget", R"({"width":0,"height":0,"format":"R32Uint"})");
    std::ifstream script(fs::path(AFTERLIGHT_ROOT) / "tests/RuntimeHostFixture.js");
    save(assets, "/Game/Host", "Script", std::string(std::istreambuf_iterator<char>(script), {}));
    auto simulation = save(assets, "/Target/Simulation", "Script", R"JS(
        var simulationTicks = 0, sceneUiReads = 0;
        function initialize() {
            if (Engine.readJson('/Game/settings').owner !== 'scene') throw Error('scene origin');
            var d = Engine.ui.createDocument('<rml><head/><body id="scene-ui" style="font-family:LatoLatin;"><button id="check">Scene</button></body></rml>');
            d.getElementById('check').on('click', function() {
                if (Engine.readJson('/Game/settings').owner !== 'scene') throw Error('scene UI origin');
                sceneUiReads++;
            });
        }
        function fixedUpdate(dt, input) {
            simulationTicks++;
            Engine.setSceneData({ticks:simulationTicks,inputX:input.x,inputWidth:input.width});
        }
    )JS");
    SceneDocument map;
    MaterialDefinition material;
    material.shader = assets.reference(AssetPath("/Target/shaders/Standard"));
    material.properties = {{"baseColor", Json::array({.1, .5, .8})}};
    map.materials.push_back(material);
    map.scripts.push_back(simulation);
    map.entities.push_back(SceneEntity::fromJson(Json::parse(
        R"({"id":"11111111111111111111111111111111","name":"Near","components":{"transform":{"position":[0,0,3]},"render":{"scale":[1.5,1.5,1.5],"material":0}}})")));
    map.entities.push_back(SceneEntity::fromJson(Json::parse(
        R"({"id":"22222222222222222222222222222222","name":"Far","components":{"transform":{"position":[0,0,0]},"render":{"scale":[4,4,0.5],"material":0}}})")));
    map.entities.push_back(SceneEntity::fromJson(Json::parse(
        R"({"id":"33333333333333333333333333333333","name":"Light","components":{"transform":{"position":[2,3,4]},"light":{"type":"point","color":[1,1,1],"intensity":1000,"radius":0.1}}})")));
    save(assets, "/Target/Maps/Test", "Map", map.json().dump());
    write(root / "Host/.project", Json{{"version", 1},
                                       {"id", project.id()},
                                       {"name", project.name()},
                                       {"startupMap", Json()},
                                       {"scripts", Json::array()},
                                       {"hostScripts", Json::array({"/Game/Host"})},
                                       {"startupMode", "load"}}
                                      .dump());
    write(fs::path(AFTERLIGHT_ROOT) / "build/runtime-host-fixture.txt", (root / "Host").generic_u8string());
    return root;
}
int main() {
    try {
        auto root = fixture();
        Project project(root / "Host");
        AssetManager assets(project.content());
        assets.mount("/Engine", fs::path(AFTERLIGHT_ROOT) / "engine/Content", false);
        registerEngineAssets(assets);
        World world;
        ui::UiCore ui(assets.mounts());
        RuntimeHost host(world, assets, &ui);
        std::string uiError;
        host.setLogSink([&](const std::string& error) { uiError = error; });
        host.initialize(project);
        host.processRequests();
        // File selection is a generic host service; project inspection is independent and read-only.
        host.setOpenFileDialog([&](const OpenFileDialogOptions& options) -> std::optional<std::string> {
            check(options.title == "Import image" && options.initialDirectory == "C:/Assets" &&
                      options.filters.size() == 1 && options.filters[0].pattern == "*.png;*.jpg",
                  "File dialog options reach the host without project-specific policy");
            return (project.root() / ".project").generic_u8string();
        });
        host.executeHost(R"JS(
            var pickedFile=Engine.files.openDialog({title:'Import image',initialDirectory:'C:/Assets',filters:[{name:'Images',pattern:'*.png;*.jpg'}]});
            var inspectedProject=Engine.project.read(pickedFile);
            if(inspectedProject.name!=='Runtime host contract fixture'||inspectedProject.startupMap!==''||inspectedProject.hostScripts[0]!=='/Game/Host')throw Error('Project metadata');
            if(!Engine.content.mounts().some(function(m){return m.mount==='/Game'&&m.root===inspectedProject.content;}))throw Error('Mount root metadata');
            var invalidProject=false;try{Engine.project.read(pickedFile+'/missing');}catch(e){invalidProject=true;}
            if(!invalidProject)throw Error('Invalid project accepted');
        )JS");
        host.setOpenFileDialog([](const OpenFileDialogOptions&) -> std::optional<std::string> { return {}; });
        host.executeHost("if(Engine.files.openDialog({})!==null)throw Error('Picker cancellation');");
        auto* document = ui.context().GetDocument("host-ui");
        auto near = world.findObject("11111111111111111111111111111111");
        check(document && near && !host.running(),
              "Data load keeps the host UI and leaves simulation stopped");
        auto original = host.captureScene();
        check(original.entities.size() == 3 && world.registry().entities().size() == 4,
              "Foreign transient ID writer is rendered but excluded from authored capture");
        Input input;
        input.width = 640;
        input.height = 400;
        input.mouseX = 320;
        input.mouseY = 200;
        auto frame = world.snapshot(input, 1, 0, 0, false, &host.view);
        check(frame.viewport.x == 160 && frame.viewport.width == 320 && frame.camera.yaw == 0 &&
                  original.camera.yaw != frame.camera.yaw && frame.entityIDOutputs.size() == 1,
              "The view overrides presentation without editing the map camera");
        host.tick(.1f, input);
        check(world.resources.data == Json::object() && host.simulationTime() == 0,
              "Host ticks do not advance target gameplay or its clock");
        host.executeHost(R"JS(
            expect(hostTicks === 1 && hostChanges === 1, 'host callbacks continue');
            expect(typeof simulationTicks === 'undefined', 'data load never executes scene JS');
            expect(Engine.view.pixel(100,100,640,400) === null, 'outside viewport');
            var p = Engine.view.pixel(320,200,640,400,80,60);
            expect(p.x === 40 && p.y === 30, 'fixed RT coordinate mapping');
            var e = Engine.findEntity('11111111111111111111111111111111');
            expect(Engine.entity(e).name === 'Near', 'identity inspection');
            expect(Engine.components(e).authored.render.material === 0, 'authored component values');
            expect(Engine.componentTypes().some(function(c){return c.name === 'drawEntityID';}), 'catalog enumeration');
            Engine.rename(e, 'Renamed');
            var r = Engine.scene.resources();
            r.materials[0].properties.baseColor = [0.9,0.15,0.05];
            Engine.scene.resources({materials:r.materials});
        )JS");
        check(world.get<Identity>(near).name == "Renamed", "Live identity edit");
        auto edited = host.captureScene();
        check(edited.materials[0].properties.at("baseColor") == Json::array({.9f, .15f, .05f}),
              "Generic material edit");
        host.executeHost("var saved=Engine.scene.save('/Target/Maps/Saved','Saved');");
        check(assets.load<SceneAsset>(AssetPath("/Target/Maps/Saved"))->scene.json() == edited.json(),
              "Cross-Content save excludes host resources and preserves authored data");
        auto beforeFailure = world.registry().entities();
        auto broken = edited;
        broken.materials[0].shader = {newPersistentId(), AssetPath("/Target/missing")};
        rejects([&] { host.restoreScene(broken, world.mapAsset()); },
                "Invalid dependencies must reject replacement");
        check(beforeFailure == world.registry().entities() && ui.context().GetDocument("host-ui") == document,
              "Failed scene preparation preserves World and host UI");
        host.executeHost("Engine.simulation.play(); Engine.simulation.pause(true);");
        check(host.running() && host.paused(), "Queued simulation commands preserve call order");
        host.pause(false);
        host.tick(.1f, input);
        check(world.resources.data.at("ticks").uint() == 1 &&
                  world.resources.data.at("inputX").uint() == 160 &&
                  world.resources.data.at("inputWidth").uint() == 320,
              "Scene input uses the displayed view and ticks once");
        document->GetElementById("origin")->DispatchEvent("click", {});
        ui.context().GetDocument("scene-ui")->GetElementById("check")->DispatchEvent("click", {});
        host.execute("if(sceneUiReads !== 1) throw Error('scene UI callback');");
        host.executeHost("expect(uiReads === 1, 'host UI callback'); expect(typeof simulationTicks === "
                         "'undefined', 'separate realms');");
        check(uiError.empty(), "UI callbacks resolve each realm's own Content");
        host.pause(true);
        auto pausedTime = host.simulationTime();
        host.tick(.1f, input);
        check(world.resources.data.at("ticks").uint() == 1 && host.simulationTime() == pausedTime,
              "Pause stops target simulation and its clock");
        host.executeHost("expect(hostTicks === 3, 'host continues while simulation is paused'); "
                         "Engine.simulation.stop();");
        check(!host.running() && !world.registry().contains(near) &&
                  host.captureScene().json() == edited.json(),
              "Stop restores the pre-Play authored scene and invalidates old entity handles");
        check(ui.context().GetDocument("host-ui") == document && !ui.context().GetDocument("scene-ui"),
              "Stopping scene scripts closes only their documents");
        host.executeHost(R"JS(
            var before = Engine.scene.capture();
            var temporaryMaterial = Engine.scene.addMaterial({shader:Engine.asset('/Game/shaders/Standard'),properties:{},textures:{}},false);
            var keptMaterial = Engine.scene.addMaterial(Engine.scene.resources().materials[0]);
            var authored = Engine.create({components:{transform:{},render:{material:keptMaterial}}});
            var portable = Engine.scene.capture().document;
            expect(portable.materials.length === 2, 'exclude foreign temporary material');
            var persistentId = Engine.entity(authored).id;
            expect(portable.entities.filter(function(e){return e.id === persistentId;})[0].components.render.material === 1,
                   'compact serialized material indices');
            expect(Engine.component(authored,'render').material === keptMaterial, 'capture leaves runtime indices unchanged');
            Engine.setMaterial(authored,temporaryMaterial);
            var rejected = false;
            try { Engine.scene.capture(); } catch (error) { rejected = true; }
            expect(rejected, 'authored entities may not reference transient materials');
            Engine.setMaterial(authored,keptMaterial);
            Engine.scene.save('/Target/Maps/MaterialTest','Material test');
            rejected = false;
            try { Engine.scene.resources({materials:[]}); } catch (error) { rejected = true; }
            expect(rejected && Engine.scene.resources().materials.length === 3, 'failed resource edit keeps live table');
            Engine.destroy(authored);
            var child = Engine.create({name:'temporary child',components:{transform:{}}});
            var parent = Engine.create({persistent:false,components:{transform:{}}});
            Engine.parent(child,parent);
            expect(Engine.entity(child).persistent && !Engine.entity(child).effectivePersistent, 'transient hierarchy');
            expect(Engine.scene.capture().document.entities.length === 3, 'exclude transient subtree');
            Engine.scene.restore(before);
        )JS");
        check(host.captureScene().json() == edited.json(), "Document restore preserves authored values");
        // Low-level realms execute callbacks only. Native simulation remains an explicit host operation.
        ScriptRuntime isolated(world, assets);
        isolated.start(assets.mounts()->source("/Target"), {});
        isolated.execute("var calls=0; function fixedUpdate(){calls++;}");
        isolated.tick(.1f, input);
        isolated.execute("if(calls!==1)throw Error('realm callback');");
        std::cout << "Runtime host / authoring / view contracts PASS\nFixture: " << (root / "Host") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
