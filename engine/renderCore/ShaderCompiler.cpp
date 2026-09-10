#include "ShaderCompiler.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace whimsical::rc {
namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read shader file: " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace
const std::vector<uint32_t>& ShaderCompiler::compile(const std::string& name, const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto stage = std::filesystem::path(name).extension().string();
    if (stage != ".comp" && stage != ".vert" && stage != ".frag")
        throw std::invalid_argument("Shader name must specify .comp, .vert or .frag");
    auto key = stage + '\n' + source;
    auto found = programs_.find(key);
    if (found != programs_.end())
        return found->second;
    if (directory_.empty()) {
        static std::atomic<uint64_t> sequence{0};
        directory_ = std::filesystem::path(WHIMSICAL_SHADER_CACHE) /
                     ("runtime-" + std::to_string(GetCurrentProcessId()) + "-" +
                      std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence));
        std::filesystem::create_directories(directory_);
    }
    auto input = directory_ / (std::to_string(programs_.size()) + stage);
    auto output = input;
    output += ".spv";
    auto log = input;
    log += ".log";
    std::ofstream(input, std::ios::binary) << source;
    // Compiler input never goes through a shell, including shader-authored source text.
    std::wstring command = L"\"" + std::filesystem::path(WHIMSICAL_GLSLANG).wstring() +
                           L"\" --target-env vulkan1.2 -V \"" + input.wstring() + L"\" -o \"" +
                           output.wstring() + L"\"";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE diagnostic = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (diagnostic == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create shader compiler diagnostics");
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
    if (status)
        throw std::runtime_error("Shader compilation failed: " + name + "\n" + read(log));
    auto bytes = read(output);
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return programs_.emplace(std::move(key), std::move(words)).first->second;
}
} // namespace whimsical::rc
