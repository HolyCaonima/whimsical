#pragma once
#include "Asset.h"
#include <filesystem>

namespace afterlight {
class Project {
    std::filesystem::path root_;
    std::string id_, name_;
    AssetPath startupMap_;
    std::vector<AssetPath> scripts_;
    std::vector<AssetPath> hostScripts_;
    bool runOnStartup_ = true;

  public:
    // Opens either the project directory or its .project file.
    explicit Project(const std::filesystem::path&);
    static Project create(const std::filesystem::path& directory, const std::string& name);
    const std::filesystem::path& root() const {
        return root_;
    }
    std::filesystem::path content() const {
        return root_ / "Content";
    }
    const std::string& id() const {
        return id_;
    }
    const std::string& name() const {
        return name_;
    }
    const AssetPath& startupMap() const {
        return startupMap_;
    }
    const std::vector<AssetPath>& scripts() const {
        return scripts_;
    }
    const std::vector<AssetPath>& hostScripts() const {
        return hostScripts_;
    }
    bool runOnStartup() const {
        return runOnStartup_;
    }
};
} // namespace afterlight
