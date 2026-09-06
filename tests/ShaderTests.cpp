#include "TestProject.h"
#include "assets/MaterialAsset.h"
#include "render/MaterialBindings.h"
#include <iostream>
#include <cstring>

using namespace afterlight;
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
int main() {
    try {
        auto& assets = testAssets();
        ShaderCompiler::ShaderSet shaders;
        for (const auto* name : {"Standard", "Paving", "Foliage"})
            shaders.push_back(assets.load<ShaderAsset>(AssetPath(std::string("/Game/shaders/") + name)));
        MaterialDefinition definition;
        definition.shader = shaders[0]->reference();
        auto first = definition.resolve(assets);
        check(first.properties[0] == vec4(.5, .5, .5, 0) && !first.textures[0],
              "Schema defaults must resolve");
        definition.properties = {{"roughness", .72}};
        auto second = definition.resolve(assets);
        auto bindings = MaterialBindings::build({first, second});
        check(bindings.shaders.size() == 1 && bindings.materials[0].info.x == bindings.materials[1].info.x,
              "Material values must not create Shader variants");
        check(bindings.materials[1].properties[1].x == .72f, "Renderer must pack schema properties");
        check(MaterialDefinition::capture(second, assets).resolve(assets) == second,
              "Inline instances must round-trip");
        definition.properties = {{"roughnes", .7}};
        rejects([&] { definition.resolve(assets); }, "Unknown properties must fail at asset resolution");
        definition.properties = {{"baseColor", Json::array({1, 2})}};
        rejects([&] { definition.resolve(assets); }, "Property dimensions must match the Shader");
        auto metadata = shaders[0]->header().metadata;
        metadata["materialModel"] = "unknown";
        rejects([&] { ShaderAsset::decode(metadata, shaders[0]->source); },
                "Unsupported models must not silently use PBR");
        metadata = shaders[0]->header().metadata;
        metadata["renderState"]["surface"] = "transparent";
        rejects([&] { ShaderAsset::decode(metadata, shaders[0]->source); },
                "Unsupported blending must fail explicitly");
        metadata = shaders[0]->header().metadata;
        metadata["properties"].push(metadata.at("properties").at(0));
        rejects([&] { ShaderAsset::decode(metadata, shaders[0]->source); },
                "Duplicate schema names must fail");
        auto linen = assets.load<MaterialAsset>(AssetPath("/Game/materials/Honeybud/wood"))->parameters;
        bindings = MaterialBindings::build({linen, linen});
        check(bindings.textures.size() == 3 &&
                  bindings.materials[0].textures == bindings.materials[1].textures,
              "Texture bindings must be shared by the renderer");
        definition.shader = shaders[1]->reference();
        definition.properties = Json::object();
        auto paving = definition.resolve(assets);
        auto ordered = MaterialBindings::build({first, paving});
        auto reordered = MaterialBindings::build({paving, first});
        check(ordered.shaders == reordered.shaders, "Material reorder must retain Shader dispatch IDs");
        assets.clearCache();
        definition.shader = shaders[0]->reference();
        auto reloaded = definition.resolve(assets);
        check(reloaded.shader != first.shader && reloaded.shader->header().id == first.shader->header().id,
              "Reload must retain asset identity and create a new immutable generation");
        check(MaterialBindings::build({first, reloaded}).shaders ==
                  MaterialBindings::build({reloaded, first}).shaders,
              "Mixed generations must retain dispatch IDs on Material reorder");
        auto definitions = ShaderCompiler::surfaceLibrary(shaders, false);
        check(definitions.find(shaders[1]->source) != std::string::npos,
              "Ray linkage must contain the authored paving evaluation");
        ShaderCompiler compiler;
        for (const auto& shader : shaders) {
            auto& code = compiler.compile("gbuffer.frag", {shader});
            check(!code.empty() && code[0] == 0x07230203, "Raster Shader must compile to SPIR-V");
        }
        for (const auto* pass : ShaderCompiler::surfacePasses)
            check(!compiler.compile(pass, shaders).empty(), "Linked ray-query passes must compile");
        auto count = compiler.compilationCount();
        second.properties[1].x = .2f;
        for (const auto& material : {first, second})
            compiler.compile("gbuffer.frag", {material.shader});
        check(compiler.compilationCount() == count, "Material edits must reuse compiled programs");
        // A schema unrelated to the example PBR inputs must work in every pass.
        Json customMetadata{
            {"materialModel", "metallicRoughness"},
            {"properties",
             Json::array({{{"name", "tint"}, {"type", "vec4"}, {"default", Json::array({.2, .4, .8, 1})}},
                          {{"name", "frequency"}, {"type", "float"}, {"default", 4}}})},
            {"textures", Json::array({"mask"})},
            {"renderState", {{"surface", "masked"}, {"cull", "back"}, {"alphaCutoff", .4}}}};
        auto custom = ShaderAsset::decode(customMetadata, R"(
SurfaceData s = DefaultSurface(ctx);
s.albedo = properties.tint.rgb;
s.emission = vec3(.1 * sin(ctx.position.x * properties.frequency));
s.opacity = SampleTexture(ctx, textures.mask, ctx.uv).r * properties.tint.a;
return s;
)");
        compiler.compile("gbuffer.frag", {custom});
        auto linked = shaders;
        linked.push_back(custom);
        for (const auto* pass : ShaderCompiler::surfacePasses)
            compiler.compile(pass, linked);
        check(compiler.compilationCount() == count + 1 + ShaderCompiler::surfacePasses.size(),
              "New Shader must compile its raster pass and relink ray passes");
        custom->source += "\n// new source generation\n";
        compiler.compile("gbuffer.frag", {custom});
        check(compiler.compilationCount() == count + 2 + ShaderCompiler::surfacePasses.size(), "Source changes must invalidate the program cache");
        auto broken = ShaderAsset::decode(shaders[0]->header().metadata, "this is invalid GLSL;");
        rejects([&] { compiler.compile("gbuffer.frag", {broken}); }, "Compiler errors must be surfaced");
        std::cout << "Shader assets, instance bindings and " << count << " surface programs verified\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
