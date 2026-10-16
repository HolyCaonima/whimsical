#include "ShaderCompiler.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

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
    return *compile({{name, source}}).front();
}
std::vector<const std::vector<uint32_t>*> ShaderCompiler::compile(
    const std::vector<std::pair<std::string, std::string>>& sources) {
    std::lock_guard<std::mutex> lock(mutex_);
    struct Job {
        std::string key, name, stage;
        const std::string* source;
        std::filesystem::path output, log;
        HANDLE process = nullptr;
    };
    std::vector<std::string> keys;
    std::vector<Job> jobs;
    for (const auto& [name, source] : sources) {
        const auto stage = std::filesystem::path(name).extension().string();
        if (stage != ".comp" && stage != ".vert" && stage != ".frag")
            throw std::invalid_argument("Shader name must specify .comp, .vert or .frag");
        keys.push_back(stage + '\n' + source);
        if (programs_.count(keys.back()) ||
            std::any_of(jobs.begin(), jobs.end(), [&](const Job& job) { return job.key == keys.back(); }))
            continue;
        jobs.push_back({keys.back(), name, stage, &source});
    }
    if (!jobs.empty() && directory_.empty()) {
        static std::atomic<uint64_t> sequence{0};
        directory_ = std::filesystem::path(WHIMSICAL_SHADER_CACHE) /
                     ("runtime-" + std::to_string(GetCurrentProcessId()) + "-" +
                      std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence));
        std::filesystem::create_directories(directory_);
    }
    // A glslang process spends most of its time starting up, so independent
    // programs overlap; at most one process per hardware thread runs at once.
    const size_t width = std::max(1u, std::thread::hardware_concurrency()), first = programs_.size();
    size_t launched = 0, finished = 0;
    std::string failure;
    const auto finish = [&] {
        auto& job = jobs[finished++];
        WaitForSingleObject(job.process, INFINITE);
        DWORD status = 0;
        GetExitCodeProcess(job.process, &status);
        CloseHandle(job.process);
        if (status) {
            if (failure.empty())
                failure = "Shader compilation failed: " + job.name + "\n" + read(job.log);
            return;
        }
        auto bytes = read(job.output);
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        programs_.emplace(job.key, std::move(words));
    };
    while (launched < jobs.size() && failure.empty()) {
        if (launched - finished == width)
            finish();
        auto& job = jobs[launched];
        const auto input = directory_ / (std::to_string(first + launched) + job.stage);
        job.output = input;
        job.output += ".spv";
        job.log = input;
        job.log += ".log";
        std::ofstream(input, std::ios::binary) << *job.source;
        // Compiler input never goes through a shell, including shader-authored source text.
        std::wstring command = L"\"" + std::filesystem::path(WHIMSICAL_GLSLANG).wstring() +
                               L"\" --target-env vulkan1.2 -V \"" + input.wstring() + L"\" -o \"" +
                               job.output.wstring() + L"\"";
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE diagnostic = CreateFileW(job.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (diagnostic == INVALID_HANDLE_VALUE) {
            failure = "Cannot create shader compiler diagnostics";
            break;
        }
        // Other threads also launch processes and open inheritable files (the CRT default);
        // the child inherits only its diagnostics, or it could hold another job's input open.
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<char> attributeStorage(attributeBytes);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes);
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &diagnostic,
                                  sizeof(diagnostic), nullptr, nullptr);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = diagnostic;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                      &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(diagnostic);
        if (!started) {
            failure = "Cannot launch glslang: " + std::to_string(GetLastError());
            break;
        }
        CloseHandle(process.hThread);
        job.process = process.hProcess;
        ++launched;
    }
    while (finished < launched)
        finish();
    if (!failure.empty())
        throw std::runtime_error(failure);
    std::vector<const std::vector<uint32_t>*> result;
    for (const auto& key : keys)
        result.push_back(&programs_.find(key)->second);
    return result;
}
} // namespace whimsical::rc
