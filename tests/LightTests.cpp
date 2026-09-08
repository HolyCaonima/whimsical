#include "TestProject.h"
#include "core/World.h"
#include "scene/ScenePersistence.h"
#include "scripting/RuntimeHost.h"
#include <iostream>

using namespace afterlight;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void close(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual-expected) > tolerance*std::max(std::abs(expected), 1.e-10)) {
        std::cerr << message << ": " << actual << " vs " << expected << '\n';
        throw std::runtime_error(message);
    }
}
static vec2 uv(int i, int count) {
    return {float((i+.5)/count), float(std::fmod((i+.5)*.6180339887498949,1.))};
}
static double irradiance(const Light& light, vec3 p, vec3 n, int count = 65536) {
    double result = 0;
    for (int i = 0; i < count; ++i) {
        auto s = samplePhysicalLight(light,p,uv(i,count));
        check(std::isfinite(s.weight.x) && s.weight.x >= 0, "Finite nonnegative estimator");
        if (s.pdfSolidAngle > 0) {
            close(s.weight.x*s.pdfSolidAngle,s.radiance.x,1.e-5,"Le/pdfOmega identity");
            if (s.pdfArea > 0)
                close(s.pdfSolidAngle, s.pdfArea*s.distance*s.distance/std::abs(glm::dot(s.normal,-s.direction)),
                      1.e-5,"Area to solid angle Jacobian");
        }
        result += s.weight.x*std::max(glm::dot(n,s.direction),0.f);
    }
    return result/count;
}
static Light packed(LightComponent c) { return packLight(c,vec3(0),quat(1,0,0,0)); }
static void physics() {
    LightComponent c;
    c.color = {1,0,0};
    c.intensity = 100;
    c.radius = 0;
    auto l = packed(c);
    double e = irradiance(l,{0,0,2},{0,0,-1});
    close(e,100/(16*Pi),1.e-6,"Point radiant power / 4pi r^2");
    close(irradiance(l,{0,0,4},{0,0,-1})/e,.25,1.e-6,"Inverse square falloff");
    for (float radius : {.01f,.5f,1.f}) {
        c.radius = radius;
        close(irradiance(packed(c),{0,0,1.2f},{0,0,-1}),100/(4*Pi*1.2*1.2),.001,
              "Sphere radius changes shape, not power, including near field");
    }
    close(irradiance(packed(c),{0,0,0},{0,0,1}),0,0,"Inside opaque sphere emits no inward light");
    c.type = LightType::Rect; c.width = 2; c.height = 3;
    e = irradiance(packed(c),{0,0,100},{0,0,-1});
    close(e,100/(Pi*10000),.001,"Rect far field projected intensity");
    close(irradiance(packed(c),{0,0,-100},{0,0,1}),0,0,"Rect back face is dark");
    c.twoSided = true;
    close(irradiance(packed(c),{0,0,100},{0,0,-1}),e*.5,1.e-5,"Two-sided rect conserves total power");
    close(irradiance(packed(c),{0,0,-100},{0,0,1}),e*.5,1.e-5,"Two-sided rect back emission");
    c.type = LightType::Directional; c.angularRadius = 0;
    close(irradiance(packed(c),{0,0,0},{0,0,-1}),100,1.e-6,"Delta directional irradiance");
    for (float angle : {.00465f,.4f}) {
        c.angularRadius = angle;
        close(irradiance(packed(c),{0,0,0},{0,0,-1}),100,1.e-6,"Directional finite cone normalization");
        close(irradiance(packed(c),{5,8,2000},{0,0,-1}),100,1.e-6,"Directional distance invariance");
    }
    // Integrate flux through an enclosing sphere. This checks angular and surface
    // distributions together, including cap/cylinder mixture and spot disk cosine.
    c.twoSided = false; c.radius = .4f; c.angularRadius = .00465f;
    for (auto type : {LightType::Point,LightType::Rect,LightType::Capsule,LightType::Spot}) {
        c.type = type;
        for (float size : {0.f,2.f}) {
            if (type == LightType::Spot) c.radius = size == 0 ? 0 : .4f;
            if (type == LightType::Capsule) c.length = size;
            l = packed(c);
            double flux = 0;
            constexpr int directions = 4096, samples = 512;
            for (int i = 0; i < directions; ++i) {
                auto u = uv(i,directions);
                vec3 n = lightSphere(u.x,u.y);
                flux += irradiance(l,n*1000.f,-n,samples)*(4*Pi*1000000.0);
            }
            close(flux/directions,c.intensity,.008,"Integrated emitted power over enclosing sphere");
        }
    }
    c.type = LightType::Spot; c.radius = 0;
    close(irradiance(packed(c),{10,0,1},{-1,0,0}),0,0,"Spot outside cone is dark");
    c.innerAngle = c.outerAngle;
    l = packed(c);
    close(l.emission.y,2*Pi*(1-std::cos(c.outerAngle)),1.e-6,"Hard spot cone normalization");
    c.outerAngle = .0001f; c.innerAngle = 0;
    auto narrow = samplePhysicalLight(packed(c),vec3(0,0,1),vec2(.5f));
    check(std::isfinite(narrow.weight.x) && narrow.weight.x > 0,"Narrow spot avoids cosine cancellation");
    c.color = {1,2,3};
    auto tint = packed(c);
    close(tint.colorIntensity.x+tint.colorIntensity.y+tint.colorIntensity.z,1,1.e-6,"Color partitions total power");
    c.color *= 2;
    check(packed(c).colorIntensity == tint.colorIntensity,"RGB tint scale does not change power");
}
static void components() {
    World w;
    RuntimeHost scripts(w,testAssets());
    scripts.execute(R"JS(
var parent=Engine.create({name:'Rig',components:{transform:{position:[1,2,3],rotation:[0,.70710678,0,.70710678]}}});
['directional','spot','point','rect','capsule'].forEach(function(type) {
    var e=Engine.create({name:type,components:{transform:{position:[0,0,2]},light:{type:type,intensity:20,radius:.3,width:2,height:3,length:4,innerAngle:.2,outerAngle:.5,angularRadius:.01,twoSided:true}}});
    Engine.parent(e,parent,false);
    var light=Engine.component(e,'light'); light.intensity=40; Engine.setComponent(e,'light',light);
    if(Engine.component(e,'light').type!==type)throw Error('light type lost');
});
)JS");
    auto frame = w.snapshot({},0,0,0);
    check(frame.lights.size() == 5,"All five script-created components extracted");
    for (const auto& light : frame.lights) {
        check(glm::distance(vec3(light.positionRadius),vec3(3,2,3)) < .0001f,"Parent-space light position");
        check(glm::distance(vec3(light.directionType),vec3(1,0,0)) < .0001f,"Parent-space light emission axis");
        close(light.colorIntensity.w,40,0,"Script edit reaches GPU snapshot");
    }
    auto doc = ScenePersistence::capture(w,testAssets());
    auto restored = SceneDocument::fromJson(Json::parse(doc.json().dump()));
    check(restored.json() == doc.json(),"All light parameters and hierarchy round trip");
    World loaded;
    auto directory = std::filesystem::path(AFTERLIGHT_ROOT)/"build"/("lights-"+newPersistentId());
    AssetManager local{Project::create(directory, "Physical lights").content()};
    registerEngineAssets(local);
    auto saved = ScenePersistence::save(w,local,AssetPath("/Game/Lights"),"Lights");
    ScenePersistence::load(loaded,local,saved.path);
    auto other = loaded.snapshot({},0,0,0);
    check(other.lights.size() == 5,"Scene load restores five light components");
    for (size_t i=0;i<5;++i) {
        check(other.lights[i].shape == frame.lights[i].shape,"Shape parameters survive save/load");
        check(glm::distance(vec3(other.lights[i].directionType),vec3(frame.lights[i].directionType)) < .0001f,
              "Light orientation survives save/load");
    }
    auto legacy = doc.json();
    legacy["version"] = 6;
    auto imported = SceneDocument::fromJson(legacy);
    close(imported.entities[1].components.find<LightComponent>()->intensity,40*12*Pi,1.e-6,"Legacy intensity conversion");
    auto parent = w.registry().view<Transform>()[0];
    w.setEnabled(parent,false);
    check(w.snapshot({},0,0,0).lights.empty(),"Disabled rig stops all light types");
    w.setEnabled(parent,true);
    auto id = frame.lightEntities[0];
    w.remove<LightComponent>(id);
    check(w.snapshot({},0,0,0).lights.size()==4,"Removing component updates light topology");
}
int main() {
    try { physics(); components(); std::cout << "PASS physical light integration and ECS\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
