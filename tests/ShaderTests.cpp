#include "TestProject.h"
#include "assets/MaterialAsset.h"
#include "render/MaterialBindings.h"
#include "render/RenderResources.h"
#include "render/graph/ShaderAccess.h"
#include <iostream>
#include <cstring>
#include <fstream>

using namespace afterlight;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
// Reflection reports shader operations. Passes separately declare output coverage.
enum class Operation { Binding, Read, Write, ReadWrite };
static Operation usageOf(const rg::Registry& registry, const std::vector<rg::ShaderAccess>& accesses,
                         rg::ResourceRef ref, rg::Access site, const char* what) {
    for (const auto& access : accesses)
        if (access.binding == registry.view(ref).binding && access.access == site)
            return access.writes ? (access.reads ? Operation::ReadWrite : Operation::Write)
                                 : (access.reads ? Operation::Read : Operation::Binding);
    throw std::runtime_error(std::string("Shader access not derived: ") + what);
}
static bool untouched(const rg::Registry& registry, const std::vector<rg::ShaderAccess>& accesses,
                      rg::ResourceRef ref) {
    if (!registry.view(ref))
        return true;
    for (const auto& access : accesses)
        if (access.binding == registry.view(ref).binding)
            return false;
    return true;
}
template <class F> static void rejects(F&& f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}
// Compile ordinary, non-inlined GLSL function wrappers. Both argument directions and
// the size-only binding must survive reflection through nested function calls.
static void functionResources() {
    rg::Registry registry;
    rg::ResourceId ids[3];
    for (int i = 0; i < 3; ++i) {
        rg::Declaration d;
        d.name = "argument" + std::to_string(i);
        d.view.type = rg::BindingType::StorageImage;
        ids[i] = registry.declare(d);
    }
    std::ifstream file(CONTRACT_SPIRV, std::ios::binary | std::ios::ate);
    check(bool(file), "Missing function reflection fixture");
    std::vector<uint32_t> code(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), std::streamsize(code.size() * 4));
    auto accesses = rg::reflect(registry, code.data(), code.size());
    check(usageOf(registry, accesses, ids[0], rg::Access::Compute, "wrapped input") == Operation::Read &&
              usageOf(registry, accesses, ids[1], rg::Access::Compute, "wrapped output") ==
                  Operation::Write &&
              usageOf(registry, accesses, ids[2], rg::Access::Compute, "size only") == Operation::Binding,
          "Functions must retain resource provenance and distinguish shape from contents");
    // An extension access outside the analyser's vocabulary must fail, not disappear.
    for (size_t at = 5; at < code.size(); at += code[at] >> 16)
        if ((code[at] & 0xffff) == 99) {
            code[at] = (code[at] & 0xffff0000) | 65000;
            break;
        }
    rejects([&] { rg::reflect(registry, code.data(), code.size()); },
            "Unknown resource operations must be diagnosed");
}

int main() {
    try {
        functionResources();
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
        // The operations used to validate each pass come from the module it will run,
        // against the registry that assigned the binding numbers it was compiled with.
        RenderResources r(false, 0);
        auto reflected = [&](const char* pass, const ShaderCompiler::ShaderSet& set) {
            const auto& code = compiler.compile(pass, set);
            return rg::reflect(r.registry, code.data(), code.size());
        };
        auto lighting = reflected("lighting.comp", shaders);
        using rg::Access, rg::previous;
        // Initial sampling only ever stores reservoirs, but a pass writes two of the four
        // rotating layers, so the contents that reach it have to survive.
        check(usageOf(r.registry, lighting, r.di.reservoirs, Access::Compute, "lighting reservoirs") ==
                  Operation::Write,
              "Reflection reports writes without inventing coverage");
        check(usageOf(r.registry, lighting, r.shading.rawDiffuse, Access::Compute, "lighting diffuse") ==
                      Operation::Write &&
                  usageOf(r.registry, lighting, r.gi.candidate, Access::Compute, "lighting GI") ==
                      Operation::Write,
              "A shader that only stores has write access");
        check(usageOf(r.registry, lighting, r.gbuffer.position, Access::Compute, "lighting surface") ==
                      Operation::Read &&
                  usageOf(r.registry, lighting, r.scene.tlas, Access::Trace, "lighting rays") ==
                      Operation::Read,
              "Reads reached through linked library code must be derived");
        check(untouched(r.registry, lighting, r.di.gradient) &&
                  untouched(r.registry, lighting, r.output.swapchain),
              "Only the resources a shader reaches may be declared");
        auto gradient = reflected("di_gradient.comp", shaders);
        // The gradient is measured per stratum and asks the image its own size; querying a
        // resource's shape is not consuming its contents.
        check(usageOf(r.registry, gradient, r.di.gradient, Access::Compute, "gradient") == Operation::Write,
              "A size query must not imply a content read");
        // The replay reads last frame's surface, and the bridge it goes through reads this
        // frame's on the way, so both halves are named and both have to be declared.
        check(usageOf(r.registry, gradient, previous(r.di.luminance), Access::Compute,
                      "replayed luminance") == Operation::Read &&
                  usageOf(r.registry, gradient, previous(r.gbuffer.position), Access::Compute,
                          "replayed surface") == Operation::Read &&
                  usageOf(r.registry, gradient, r.gbuffer.position, Access::Compute, "current surface") ==
                      Operation::Read,
              "Which half of a history pair a shader names must be derived");
        auto raster = reflected("gbuffer.frag", {shaders[0]});
        check(usageOf(r.registry, raster, r.scene.materials, Access::Graphics, "material parameters") ==
                  Operation::Read,
              "A raster shader's reads must be declared at the graphics stage");
        // A pass binds several modules and unions what they do.
        rg::merge(raster, lighting);
        check(usageOf(r.registry, raster, r.scene.materials, Access::Graphics, "merged parameters") ==
                      Operation::Read &&
                  usageOf(r.registry, raster, r.shading.rawDiffuse, Access::Compute, "merged diffuse") ==
                      Operation::Write,
              "Merging modules must keep each stage's accesses");
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
