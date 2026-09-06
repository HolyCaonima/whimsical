#pragma once
#include "Project.h"
#include <functional>
#include <set>
#include <thread>

namespace afterlight {
class AssetManager {
  public:
    using Loader =
        std::function<std::shared_ptr<Asset>(AssetManager&, const AssetHeader&, const std::string&)>;
    explicit AssetManager(Project);
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    const Project& project() const {
        return project_;
    }
    void registerLoader(std::string type, Loader);
    void scan(); // Transactionally rebuild descriptors; never loads payloads.
    AssetRef reference(const AssetPath&) const;
    AssetRef resolve(const AssetRef&) const;
    const AssetHeader& descriptor(const AssetPath&) const;
    size_t size() const {
        return entries_.size();
    }
    std::shared_ptr<const Asset> load(const AssetPath&);
    std::shared_ptr<const Asset> load(const AssetRef&);
    template <class T> std::shared_ptr<const T> load(const AssetPath& path) {
        auto asset = std::dynamic_pointer_cast<const T>(load(path));
        if (!asset)
            throw std::invalid_argument("Wrong asset type: " + path.string());
        return asset;
    }
    template <class T> std::shared_ptr<const T> load(const AssetRef& ref) {
        return load<T>(resolve(ref).path);
    }
    void clearCache(); // Existing shared references and immutable Frames remain valid.
    AssetRef save(const AssetPath&, AssetHeader, const std::string& payload);

  private:
    struct Entry {
        AssetHeader header;
        std::filesystem::path file;
    };
    Project project_;
    std::thread::id owner_ = std::this_thread::get_id();
    std::map<AssetPath, Entry> entries_;
    std::map<std::string, AssetPath> ids_;
    std::map<std::string, Loader> loaders_;
    std::map<std::string, std::shared_ptr<const Asset>> cache_;
    std::set<std::string> loading_;
    void checkThread() const;
    std::filesystem::path contained(const std::filesystem::path&) const;
    std::filesystem::path externalFile(const Entry&) const;
    std::string externalPayload(const Entry&) const;
};
} // namespace afterlight
