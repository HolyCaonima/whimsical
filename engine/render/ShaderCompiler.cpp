#include "ShaderCompiler.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <regex>

namespace whimsical {
static std::string read(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot read shader file: " + file.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static void replace(std::string& source, const std::string& key, const std::string& value) {
    auto position = source.find(key);
    if (position == std::string::npos)
        throw std::runtime_error("Missing shader link point: " + key);
    source.replace(position, key.size(), value);
}
// Included source, including the pinned SDK, participates in program caching.
static std::string expandIncludes(const std::string& source, const std::filesystem::path& directory) {
    static const std::regex include(R"(^\s*#include\s+["<]([^">]+)[">]\s*$)");
    std::istringstream lines(source);
    std::ostringstream result;
    std::string line;
    while (std::getline(lines, line)) {
        std::smatch match;
        if (std::regex_match(line, match, include)) {
            const auto& name = match[1].str();
            auto path = directory / name;
            if (name.rfind("Rtxdi/", 0) == 0)
                path = std::filesystem::path(WHIMSICAL_RTXDI_INCLUDES) / name;
            else if (name.rfind("generated/", 0) == 0)
                path = std::filesystem::path(WHIMSICAL_SHADERS) / name;
            result << expandIncludes(read(path), path.parent_path());
        } else result << line << '\n';
    }
    return result.str();
}
std::string ShaderCompiler::surfaceLibrary(const ShaderSet& shaders, bool raster) {
    std::ostringstream code;
    code << std::setprecision(9) << std::showpoint;
    const char* types[] = {"", "float", "vec2", "vec3", "vec4"};
    const char* swizzles[] = {"", ".x", ".xy", ".xyz", ""};
    for (size_t i = 0; i < shaders.size(); ++i) {
        const auto& shader = *shaders[i];
        // Each source is a function body, so authored local symbols never collide
        // when independent Shader assets are linked into the same compute module.
        if (!shader.properties.empty()) {
            code << "struct Properties_" << i << " {\n";
            for (const auto& p : shader.properties)
                code << types[int(p.type)] << " " << p.name << ";\n";
            code << "};\n";
        }
        if (!shader.textures.empty()) {
            code << "struct Textures_" << i << " {\n";
            for (const auto& t : shader.textures)
                code << "int " << t << ";\n";
            code << "};\n";
        }
        code << "SurfaceData EvaluateSurface_" << i << "(MaterialContext ctx) {\n";
        if (!shader.properties.empty())
            code << "Properties_" << i << " properties;\n";
        for (size_t p = 0; p < shader.properties.size(); ++p)
            code << "properties." << shader.properties[p].name << " = materials[ctx.material].properties["
                 << p << "]" << swizzles[int(shader.properties[p].type)] << ";\n";
        if (!shader.textures.empty())
            code << "Textures_" << i << " textures;\n";
        for (size_t t = 0; t < shader.textures.size(); ++t)
            code << "textures." << shader.textures[t] << " = materials[ctx.material].textures[" << t / 4
                 << "][" << t % 4 << "];\n";
        code << "#line 1 " << i + 1 << "\n" << shader.source << "\n}\n#line 1 0\n";
    }
    code << "SurfaceData EvaluateSurface(MaterialContext ctx) {\n";
    if (raster)
        code << "return EvaluateSurface_0(ctx);\n";
    else {
        code << "switch(materials[ctx.material].info.x) {\n";
        for (size_t i = 0; i < shaders.size(); ++i)
            code << "case " << i << "u: return EvaluateSurface_" << i << "(ctx);\n";
        code << "}\nreturn DefaultSurface(ctx);\n";
    }
    code << "}\n";
    return code.str();
}
std::string ShaderCompiler::passSource(const std::filesystem::path& directory, const std::string& pass,
                                       const ShaderSet& shaders) {
    auto common = read(directory / "common.glsl");
    replace(common, "#include \"surface.glsl\"", read(directory / "surface.glsl"));
    replace(common, "#include \"surface_link.glsl\"",
            surfaceLibrary(shaders, std::filesystem::path(pass).extension() == ".frag"));
    auto source = read(directory / pass);
    replace(source, "#include \"common.glsl\"", common);
    return expandIncludes(source, directory);
}
const std::vector<uint32_t>& ShaderCompiler::compile(const std::string& pass, const ShaderSet& shaders) {
    auto source = passSource(WHIMSICAL_SHADER_SOURCES, pass, shaders);
    try {
        return compiler_.compile(pass, source);
    } catch (const std::exception& error) {
        std::string assets;
        for (size_t i = 0; i < shaders.size(); ++i)
            assets += "\nsource " + std::to_string(i + 1) + ": " + shaders[i]->reference().path.string();
        throw std::runtime_error(std::string(error.what()) + assets);
    }
}
} // namespace whimsical