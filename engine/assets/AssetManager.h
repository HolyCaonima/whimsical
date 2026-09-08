#pragma once
#include "Asset.h"
#include "ContentMounts.h"
#include <functional>
#include <set>
#include <thread>

namespace afterlight {
class AssetManager {
  public:
    using Loader =
        std::function<std::shared_ptr<Asset>(AssetManager&, const AssetHeader&, const std::string&)>;
    enum class Encoding { Raw, Text, Json };
    // Owner scopes allow local and explicitly shared dependencies; caller scopes supply /Game origin.
    class Scope {
        AssetManager& manager_;
        ContentSourceRef previous_;
        bool closed_;

      public:
        Scope(AssetManager&, ContentSourceRef, bool closed = true);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
    struct Document {
        AssetRef ref;
        AssetHeader header;
        std::string payload;
        Encoding encoding;
    };
    AssetManager();
    explicit AssetManager(const std::filesystem::path& content);
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    std::shared_ptr<const ContentMounts> mounts() const {
        return mounts_;
    }
    ContentSourceRef mount(const std::string& alias, const std::filesystem::path&, bool writable = true,
                           bool shared = false);
    void unmount(const std::string& alias);
    ContentSourceRef origin(const AssetPath&) const;
    ContentSourceRef origin(const AssetRef&) const;
    ContentFile file(const std::string&) const;
    void registerLoader(std::string type, Loader, Encoding = Encoding::Raw);
    Encoding encoding(const std::string& type) const {
        return loaders_.at(type).encoding;
    }
    void scan();
    void scan(const std::string& alias);
    std::vector<AssetRef> browse(const std::string& alias) const;
    AssetPath qualify(const AssetPath&) const;
    AssetRef reference(const AssetPath&) const;
    AssetRef resolve(const AssetRef&) const;
    AssetHeader descriptor(const AssetPath&);
    size_t size() const;
    std::shared_ptr<const Asset> load(const AssetPath&);
    std::shared_ptr<const Asset> load(const AssetRef&);
    template <class T, class Ref> std::shared_ptr<const T> load(const Ref& ref) {
        auto asset = std::dynamic_pointer_cast<const T>(load(ref));
        if (!asset)
            throw std::invalid_argument("Wrong asset type");
        return asset;
    }
    void clearCache();
    Document read(const AssetRef&);
    Document read(const AssetPath& path) {
        return read(reference(path));
    }
    AssetRef save(const AssetPath&, AssetHeader, const std::string& payload);
    AssetRef save(const AssetRef&, AssetHeader, const std::string& payload);
    // External payload editing is explicit; save() preserves header-only semantics.
    AssetRef write(const AssetRef&, AssetHeader, const std::string& payload);
    AssetRef write(const AssetPath&, AssetHeader, const std::string& payload);

  private:
    struct Entry {
        AssetHeader header;
        ContentFile file;
    };
    struct Store {
        ContentSourceRef source;
        std::map<AssetPath, Entry> entries;
        std::map<std::string, AssetPath> ids;
        std::map<std::string, std::shared_ptr<const Asset>> cache;
        std::set<std::string> loading;
    };
    struct Codec {
        Loader load;
        Encoding encoding;
    };
    std::shared_ptr<ContentMounts> mounts_ = std::make_shared<ContentMounts>();
    std::map<std::string, Store> stores_;
    std::map<std::string, Codec> loaders_;
    ContentSourceRef context_;
    bool closed_ = false;
    std::thread::id owner_ = std::this_thread::get_id();
    void checkThread() const;
    void checkMutation() const;
    Store scanned(ContentSourceRef) const;
    Store& store(ContentSourceRef);
    const Store& store(ContentSourceRef) const;
    AssetPath local(const AssetPath&) const;
    AssetRef bound(const Store&, const AssetPath&) const;
    ContentFile externalFile(const Entry&) const;
    Json references(const Json&, bool persistent) const;
    Document readEntry(const AssetRef&);
    AssetRef saveImpl(const AssetPath&, AssetHeader, const std::string&, bool writeExternal);
};
} // namespace afterlight
