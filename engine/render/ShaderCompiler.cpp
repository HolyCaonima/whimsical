#include "ShaderCompiler.h"
#include "assets/Asset.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
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
    auto key = pass + '\n' + source;
    auto found = programs_.find(key);
    if (found != programs_.end())
        return found->second;
    if (directory_.empty()) {
        directory_ = std::filesystem::path(WHIMSICAL_SHADERS) / ("runtime-" + newPersistentId());
        std::filesystem::create_directories(directory_);
    }
    auto input = directory_ / (std::to_string(programs_.size()) + "." + pass);
    auto output = input;
    output += ".spv";
    auto log = input;
    log += ".log";
    std::ofstream(input, std::ios::binary) << source;
    // Launch the compiler directly; asset text never passes through a shell.
    std::wstring command = L"\"" + std::filesystem::path(WHIMSICAL_GLSLANG).wstring() +
                           L"\" --target-env vulkan1.2 -V \"" + input.wstring() + L"\" -o \"" +
                           output.wstring() + L"\"";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE diagnostic = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (diagnostic == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create Shader compiler diagnostics");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = diagnostic;
    PROCESS_INFORMATION process{};
    BOOL launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &startup, &process);
    if (!launched) {
        CloseHandle(diagnostic);
        throw std::runtime_error("Cannot launch glslang: " + std::to_string(GetLastError()));
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 0;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(diagnostic);
    if (status) {
        std::string assets;
        for (size_t i = 0; i < shaders.size(); ++i)
            assets += "\nsource " + std::to_string(i + 1) + ": " + shaders[i]->reference().path.string();
        throw std::runtime_error("Shader compilation failed: " + pass + assets + "\n" + read(log));
    }
    auto bytes = read(output);
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return programs_.emplace(std::move(key), std::move(words)).first->second;
}
} // namespace whimsical
