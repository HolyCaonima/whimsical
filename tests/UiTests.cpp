#include "TestEntities.h"
#include "TestProject.h"
#include "scripting/ScriptRuntime.h"
#include "uiCore/UiCore.h"
#include "ui/EngineUi.h"
#include "core/EngineSettings.h"
#include "render/Renderer.h"
#include "core/FrameMailbox.h"
#include <RmlUi/Core.h>
#include <iostream>
#include <fstream>

using namespace afterlight;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
static void ownership() {
    auto root = std::filesystem::path(AFTERLIGHT_ROOT) / "build" / "ui-project-tests" / newPersistentId();
    AssetManager assets(Project::create(root, "Empty UI host").content());
    assets.mount("/Engine", std::filesystem::path(AFTERLIGHT_ROOT) / "engine/Content", false);
    registerEngineAssets(assets);
    ui::UiCore ui(assets.mounts());
    World world;
    ui::EngineUi diagnostics(ui);
    ScriptRuntime scripts(world, assets, &ui);
    scripts.initialize();
    ConsoleRegistry variables;
    EngineSettings settings(variables, false);
    Frame frame;
    settings.decorate(frame);
    diagnostics.sync(frame, RenderStatistics{});
    check(diagnostics.snapshot(frame)->draws.empty(),
          "An empty project must not inherit gameplay panels or visible diagnostics");
    auto* stats = ui.context().GetDocument("engine-statistics");
    check(!stats->GetElementById("minimap") && !stats->GetElementById("animation"),
          "Engine documents must contain no game-specific controls");
    scripts.execute("var own=Engine.ui.createDocument('<rml><head/><body id=\"own\" "
                    "style=\"width:80px;height:60px;background-color:#fff;\"/></rml>').show();");
    auto* own = ui.context().GetDocument("own");
    for (bool hud : {false, true})
        for (bool enabled : {false, true}) {
            variables.set("r.Hud", hud ? "true" : "false", CVarSource::Console);
            variables.set("r.Stats", enabled ? "true" : "false", CVarSource::Console);
            settings.decorate(frame);
            scripts.setHudEnabled(frame.hudEnabled);
            diagnostics.sync(frame, RenderStatistics{});
            check(own->IsVisible() == hud && stats->IsVisible() == enabled,
                  "Project HUD and engine statistics must have independent visibility");
        }
    frame.hudEnabled = frame.statsEnabled = false;
    scripts.setHudEnabled(false);
    frame.console.open = true;
    diagnostics.sync(frame, RenderStatistics{});
    check(ui.context().GetDocument("engine-console")->IsVisible(),
          "Console remains available with both overlays disabled");
    frame.console.open = false;
    frame.physicsDebug = true;
    frame.physicsLines.push_back({{0, 0, 0}, {1, 0, 0}, {1, 0, 0}});
    diagnostics.sync(frame, RenderStatistics{});
    check(!diagnostics.snapshot(frame)->draws.empty(),
          "Physics diagnostics must not depend on either UI visibility switch");
}
int main() {
    std::cout.setf(std::ios::unitbuf);
    try {
        ownership();
        ui::UiCore ui(testAssets().mounts());
        World world;
        ScriptRuntime scripts(world, testAssets(), &ui);
        std::string error;
        scripts.setLogSink([&](const auto& text) { error = text; });
        scripts.execute(R"JS(
var clicks=0,changes=0;
var doc=Engine.ui.createDocument('<rml><head><style>body{font-family:LatoLatin;width:500px;height:320px;background-color:#102030;} button{position:absolute;left:40px;top:40px;width:120px;height:40px;} input{position:absolute;left:40px;top:100px;width:180px;height:30px;} #clip{position:absolute;left:260px;top:40px;width:80px;height:40px;overflow:hidden;background-color:#223344;} #clip div{width:200px;height:100px;background-color:#ffffff;}</style></head><body><button id="action">Action</button><input id="edit" type="text"/><div id="clip"><div/></div></body></rml>').show();
var button=doc.getElementById('action');
var token=button.on('click',function(e){
    if(e.target.getAttribute('id')!=='action')throw Error('event target');
    clicks++; Engine.status('UI','clicked'); button.setText('Clicked');
});
doc.getElementById('edit').on('change',function(e){changes++;});
if(doc.querySelectorAll('button').length!==1)throw Error('selector list');
button.setClass('active',true).setAttribute('data-test','yes').setProperty('color','#ff8080');
if(!button.hasClass('active')||button.getAttribute('data-test')!=='yes')throw Error('DOM mutation');
var child=doc.appendChild('div').setAttribute('id','dynamic').setText('<unsafe>&text');
doc.appendChild('img').setAttribute('src','../textures/Honeybud/wood_baseColor.png').setProperty('width','32px').setProperty('height','32px');
if(doc.querySelector('#dynamic').getInnerRML().indexOf('&lt;')<0)throw Error('text must escape markup');
child.remove();
var stale=false;try{child.setText('gone');}catch(e){stale=true;}if(!stale)throw Error('stale element');
)JS");
        auto retained = ui.snapshot();
        check(!retained->draws.empty(), "RmlUi must produce geometry");
        bool font = false, clip = false, image = false;
        for (const auto& draw : retained->draws) {
            font = font || bool(draw.texture);
            image = image || (draw.texture && draw.texture->width == 1024 && draw.texture->height == 1024);
            clip = clip || (draw.scissor[2] < retained->width && draw.scissor[3] < retained->height);
        }
        check(font && clip, "Snapshot must include font textures and scissor state");
        check(image, "Relative PNG resources must decode through WIC");
        Input click;
        click.mouseX = 70;
        click.mouseY = 60;
        click.leftPressed = true;
        click.wheel = 1;
        scripts.processUiInput(click);
        check(click.pointerCaptured && !click.leftPressed && click.wheel == 0,
              "UI click/wheel must not reach gameplay");
        check(world.gameplay.state == "UI" && world.gameplay.message == "clicked",
              "JS UI callback must mutate actual gameplay");
        scripts.execute("if(clicks!==1)throw Error('click count'); button.off(token);");
        Input again;
        again.mouseX = 70;
        again.mouseY = 60;
        again.leftPressed = true;
        scripts.processUiInput(again);
        scripts.execute("if(clicks!==1)throw Error('unsubscribe'); doc.getElementById('edit').focus();");
        Input typing;
        typing.text = U"Hello UI";
        typing.keys['W'] = true;
        typing.pressed['W'] = true;
        scripts.processUiInput(typing);
        check(typing.keyboardCaptured && !typing.keys['W'], "Text focus must own keyboard input");
        scripts.execute("if(doc.getElementById('edit').getValue()!=='Hello UI')throw Error('text input');");
        scripts.execute("doc.getElementById('edit').setValue('');");
        Input edits;
        using Event = ui::InputEvent;
        edits.uiEvents = {{Event::Type::Text, 'A'},  {Event::Type::KeyDown, 8}, {Event::Type::KeyUp, 8},
                          {Event::Type::Text, 'B'},  {Event::Type::Text, 'C'},  {Event::Type::KeyDown, 8},
                          {Event::Type::KeyDown, 8}, {Event::Type::KeyUp, 8}};
        scripts.processUiInput(edits);
        scripts.execute(
            "if(doc.getElementById('edit').getValue()!=='')throw Error('ordered editing and key repeats');");
        Input focusLost;
        focusLost.focused = false;
        scripts.processUiInput(focusLost);
        scripts.execute("doc.hide();");
        Input ground;
        ground.mouseX = 700;
        ground.mouseY = 500;
        ground.leftPressed = true;
        scripts.processUiInput(ground);
        check(!ground.pointerCaptured && ground.leftPressed, "Hidden documents must release game input");
        scripts.execute("doc.show(true);");
        Input outside;
        outside.mouseX = 900;
        outside.mouseY = 600;
        outside.leftPressed = true;
        scripts.processUiInput(outside);
        check(outside.pointerCaptured, "Modal document must block clicks outside its bounds");
        scripts.execute(R"JS(
doc.show();
var self=button.on('click',function(){button.off(self);button.remove();});
)JS");
        Input remove;
        remove.mouseX = 70;
        remove.mouseY = 60;
        remove.leftPressed = true;
        scripts.processUiInput(remove);
        ui.snapshot();
        scripts.execute(
            "var expired=false;try{button.setText('bad');}catch(e){expired=true;}if(!expired)throw "
            "Error('removed callback target'); doc.close();");
        check(ui.snapshot()->draws.empty(), "Closing the last document must clear the overlay");
        check(!retained->draws.front().geometry->vertices.empty(),
              "Old immutable snapshot must survive document destruction");
        FrameMailbox mailbox;
        Frame first;
        first.ui = retained;
        mailbox.publish(std::move(first));
        Frame second;
        second.ui = ui.snapshot();
        mailbox.publish(std::move(second));
        FrameRef acquired;
        uint64_t seen = 0;
        mailbox.acquire(acquired, seen);
        check(acquired->ui->draws.empty(), "Dropped UI frames must not replay stale draws");
        check(error.empty(), "JS event callback raised an unexpected error");
        // A second context must neither steal the first one's file base nor shut down global RmlUi.
        {
            ui::UiCore peer(testAssets().mounts());
            auto* doc = peer.createDocument(
                "<rml><head/><body style='width:10px;height:10px;background-color:#fff;'/></rml>");
            doc->Show();
            check(!peer.snapshot()->draws.empty(), "Independent UI context");
        }
        scripts.initialize(testProject());
        scripts.execute(R"JS(
var probe = Engine.create({name:'Spatial query',components:{transform:{position:[30,1,30]},render:{scale:[2,2,4],material:0},collider:{shape:{type:'box',halfExtents:[1.0,1.0,2.0]},blocking:true,pickable:true}}});
Engine.collider(probe, {shape:'box',halfExtents:{x:1,y:1,z:2},layer:8,blocking:true});
var frozen = Engine.physicsBodies({mask:8,blockingOnly:true});
if (frozen.length !== 1 || frozen[0].owner !== probe || frozen[0].min.x !== 29 || frozen[0].max.z !== 32)
    throw Error('body enumeration bounds');
Engine.transform(probe, {position:{x:40,y:1,z:30},rotation:{x:0,y:Math.sin(Math.PI/4),z:0,w:Math.cos(Math.PI/4)}});
var moved = Engine.physicsBodies({mask:8});
if (Math.abs(moved[0].min.x-38) > .001 || frozen[0].min.x !== 29)
    throw Error('rotated bounds or copied query results');
if (Engine.physicsBodies({mask:8,ignoreOwner:probe}).length !== 0) throw Error('body filter');
Engine.enabled(probe,false);
if (Engine.physicsBodies({mask:8}).length !== 0) throw Error('disabled body');
Engine.enabled(probe,true);
Engine.destroy(probe);
if (Engine.physicsBodies({mask:8}).length !== 0 || frozen[0].owner !== probe) throw Error('destroyed body');
var oldMap = GameplayHud.document.getElementById('map-obstacles').getInnerRML();
var wall = Engine.create({name:'Map wall',components:{transform:{position:[5,1,2]},render:{scale:[2,2,2],material:0},collider:{shape:{type:'box',halfExtents:[1.0,1.0,1.0]},blocking:true,pickable:true}}});
updateUI(.11);
if (GameplayHud.document.getElementById('map-obstacles').getInnerRML() === oldMap)
    throw Error('project map must track new obstacles without a simulation tick');
Engine.enabled(wall,false);
updateUI(.11);
if (GameplayHud.document.getElementById('map-obstacles').getInnerRML() !== oldMap)
    throw Error('project map must discard disabled obstacles');
Engine.destroy(wall);
Engine.animationAttribute(Locomotion.id,'locomotion.style','Zombie');
)JS");
        scripts.updateUi(.1f);
        scripts.execute("if(GameplayHud.document.getElementById('attributes').getInnerRML().indexOf('Zombie') < 0) "
                        "throw Error('paused UI refresh');");
        auto count = ui.context().GetNumDocuments();
        for (int i = 0; i < 3; ++i) {
            scripts.loadScene(world.mapAsset().path);
            ui.snapshot();
            check(ui.context().GetNumDocuments() == count,
                  "Map reload must replace project documents without leaking");
        }
        scripts.setHudEnabled(false);
        check(ui.snapshot()->draws.empty(), "Host HUD visibility must hide project UI");
        scripts.loadScene(world.mapAsset().path);
        check(ui.snapshot()->draws.empty(), "Reload must respect host visibility");
        std::cerr << "PASS: RmlUi geometry/fonts/clipping, JS DOM/events, capture, lifetime, mailbox and "
                     "scene reload\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
