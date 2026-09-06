#pragma once
#include "Animation.h"
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>

namespace afterlight::animation {
// Loader registration belongs to the engine composition root, not the animation evaluator.
class Library {
  public:
    using Loader = std::function<std::shared_ptr<const Asset>(const std::filesystem::path&)>;
    void registerLoader(std::string extension, Loader loader) {
        loaders_[std::move(extension)] = std::move(loader);
    }
    std::shared_ptr<const Asset> load(const std::filesystem::path& path) {
        auto key = std::filesystem::weakly_canonical(path);
        auto cached = assets_.find(key);
        if (cached != assets_.end())
            return cached->second;
        auto loader = loaders_.find(key.extension().string());
        if (loader == loaders_.end())
            throw std::invalid_argument("No animation asset loader for " + key.extension().string());
        auto asset = loader->second(key);
        assets_.emplace(key, asset);
        return asset;
    }

  private:
    std::map<std::string, Loader> loaders_;
    std::map<std::filesystem::path, std::shared_ptr<const Asset>> assets_;
};
} // namespace afterlight::animation
