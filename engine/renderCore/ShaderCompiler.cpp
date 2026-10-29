#include "ShaderCompiler.h"
#include "Glslang.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace whimsical::rc {
const std::vector<uint32_t>& ShaderCompiler::compile(const std::string& name, const std::string& source) {
    return *compile({{name, source}}).front();
}
std::vector<const std::vector<uint32_t>*> ShaderCompiler::compile(
    const std::vector<std::pair<std::string, std::string>>& sources) {
    std::lock_guard<std::mutex> lock(mutex_);
    struct Job {
        std::string key;
        const std::string *name, *source;
        WhimsicalShaderStage stage;
        bool compiled = false;
        std::vector<uint32_t> words;
        std::string log;
    };
    std::vector<std::string> keys;
    std::vector<Job> jobs;
    for (const auto& [name, source] : sources) {
        const auto extension = std::filesystem::path(name).extension().string();
        WhimsicalShaderStage stage;
        if (extension == ".comp")
            stage = WhimsicalCompute;
        else if (extension == ".vert")
            stage = WhimsicalVertex;
        else if (extension == ".frag")
            stage = WhimsicalFragment;
        else
            throw std::invalid_argument("Shader name must specify .comp, .vert or .frag");
        keys.push_back(extension + '\n' + source);
        if (programs_.count(keys.back()) ||
            std::any_of(jobs.begin(), jobs.end(), [&](const Job& job) { return job.key == keys.back(); }))
            continue;
        jobs.push_back({keys.back(), &name, &source, stage});
    }
    // Independent programs compile concurrently, at most one per hardware thread.
    std::atomic<size_t> next{0};
    const auto work = [&] {
        for (size_t i; (i = next++) < jobs.size();) {
            auto& job = jobs[i];
            job.compiled = whimsicalCompileGlsl(job.stage, job.source->c_str(), &job,
                                                [](void* user, const uint32_t* words, size_t count, const char* log) {
                                                    auto& job = *static_cast<Job*>(user);
                                                    if (words)
                                                        job.words.assign(words, words + count);
                                                    else
                                                        job.log = log;
                                                });
        }
    };
    std::vector<std::thread> workers;
    for (size_t i = 1; i < std::min<size_t>(jobs.size(), std::thread::hardware_concurrency()); ++i)
        workers.emplace_back(work);
    work();
    for (auto& worker : workers)
        worker.join();
    for (const auto& job : jobs)
        if (!job.compiled)
            throw std::runtime_error("Shader compilation failed: " + *job.name + "\n" + job.log);
    for (auto& job : jobs)
        programs_.emplace(std::move(job.key), std::move(job.words));
    std::vector<const std::vector<uint32_t>*> result;
    for (const auto& key : keys)
        result.push_back(&programs_.find(key)->second);
    return result;
}
} // namespace whimsical::rc
