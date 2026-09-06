#include "Project.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace afterlight {
Project::Project(const std::filesystem::path& path) {
    auto file = std::filesystem::is_directory(path) ? path / ".project" : path;
    if (file.filename() != ".project")
        throw std::invalid_argument("Expected project directory or .project");
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open project: " + file.string());
    std::stringstream text;
    text << stream.rdbuf();
    const auto j = Json::parse(text.str());
    if (j.at("version").uint() != 1)
        throw std::invalid_argument("Unsupported project version");
    id_ = j.at("id").string();
    validatePersistentId(id_);
    name_ = j.at("name").string();
    if (!j.at("startupMap").null())
        startupMap_ = AssetPath(j.at("startupMap").string());
    for (const auto& script : j.at("scripts").elements())
        scripts_.emplace_back(script.string());
    root_ = std::filesystem::canonical(file).parent_path();
    if (!std::filesystem::is_directory(content()))
        throw std::runtime_error("Project has no Content directory");
}
Project Project::create(const std::filesystem::path& directory, const std::string& name) {
    if (std::filesystem::exists(directory / ".project"))
        throw std::runtime_error("Project already exists");
    std::filesystem::create_directories(directory / "Content");
    Json j{{"version", 1},
           {"id", newPersistentId()},
           {"name", name},
           {"startupMap", Json()},
           {"scripts", Json::array()}};
    std::ofstream file(directory / ".project", std::ios::binary);
    file << j.dump() << '\n';
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write .project");
    return Project(directory);
}
} // namespace afterlight
