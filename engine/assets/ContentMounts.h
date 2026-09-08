#pragma once
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace afterlight {
class ContentMounts;
// A mount is a lease on one independent Content. Reusing its alias creates a new identity.
class ContentSource {
    friend class ContentMounts;
    std::atomic<bool> active_{true};

  public:
    const std::string id, mount;
    const std::filesystem::path root;
    const bool writable;
    ContentSource(std::string identity, std::string name, std::filesystem::path directory, bool writes)
        : id(std::move(identity)), mount(std::move(name)), root(std::move(directory)), writable(writes) {}
    bool active() const {
        return active_.load();
    }
};
using ContentSourceRef = std::shared_ptr<const ContentSource>;
struct ContentFile {
    ContentSourceRef source;
    std::string relative;
    std::string uri() const;
    std::filesystem::path physical() const;
};

// The common file mapping for asset envelopes, external payloads, and UI resources.
// It contains no project configuration, asset decoders, or script execution policy.
class ContentMounts {
    std::map<std::string, std::shared_ptr<ContentSource>> sources_;

  public:
    ContentMounts() = default;
    ContentMounts(const ContentMounts&) = delete;
    ContentMounts& operator=(const ContentMounts&) = delete;
    ~ContentMounts();
    ContentSourceRef mount(const std::string& alias, const std::filesystem::path&, bool writable = true);
    void unmount(const std::string& alias);
    bool contains(const std::string& alias) const;
    ContentSourceRef source(const std::string& alias) const;
    ContentSourceRef byId(const std::string&) const;
    std::vector<ContentSourceRef> sources() const;
    ContentFile locate(const std::string& virtualPath) const;
    ContentFile fromUri(const std::string&) const;
    static bool isUri(const std::string&);
    // A dependent file resolves /Game against its owner, never against a global mount.
    ContentFile relative(const ContentFile& owner, const std::string& path) const;
    static ContentFile file(ContentSourceRef, const std::string& relative);
    static std::string read(const ContentFile&);
    static void write(const ContentFile&, const std::string& bytes);
    static void validateAlias(const std::string&);
};
} // namespace afterlight
