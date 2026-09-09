#include "ContentMounts.h"
#include "Asset.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace whimsical {
namespace {
bool inside(const std::filesystem::path& root, const std::filesystem::path& file) {
    auto a = root.native(), b = file.native();
    while (!a.empty() && (a.back() == L'\\' || a.back() == L'/'))
        a.pop_back();
    return b.size() >= a.size() &&
           CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(a.size()), TRUE) == CSTR_EQUAL &&
           (b.size() == a.size() || b[a.size()] == L'\\' || b[a.size()] == L'/');
}
void requireActive(const ContentSourceRef& source) {
    if (!source || !source->active())
        throw std::invalid_argument("Content mount is no longer active");
}
std::string normalized(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.find_first_of(":|\0", 0, 3) != std::string::npos)
        throw std::invalid_argument("Invalid Content file path: " + path);
    auto p = std::filesystem::u8path(path).lexically_normal();
    if (p.empty() || p == "." || p.has_root_path() || *p.begin() == "..")
        throw std::invalid_argument("File escapes its Content: " + path);
    return p.generic_u8string();
}
} // namespace
std::string ContentFile::uri() const {
    requireActive(source);
    return "/__content/" + source->id + "/" + relative;
}
std::filesystem::path ContentFile::physical() const {
    requireActive(source);
    auto path = std::filesystem::weakly_canonical(source->root / std::filesystem::u8path(relative));
    if (!inside(source->root, path) || path == source->root)
        throw std::invalid_argument("File escapes its Content: " + relative);
    return path;
}
ContentMounts::~ContentMounts() {
    for (auto& entry : sources_)
        entry.second->active_ = false;
}
void ContentMounts::validateAlias(const std::string& alias) {
    if (alias.size() < 2 || alias[0] != '/' ||
        alias.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 1) == 1 ||
        alias.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-", 1) !=
            std::string::npos)
        throw std::invalid_argument("Mount must be one virtual root, e.g. /Game or /Library");
}
ContentSourceRef ContentMounts::mount(const std::string& alias, const std::filesystem::path& directory,
                                      bool writable, bool shared) {
    validateAlias(alias);
    auto root = std::filesystem::canonical(directory);
    if (!std::filesystem::is_directory(root))
        throw std::invalid_argument("Content must be a directory");
    for (const auto& entry : sources_) {
        const auto& s = *entry.second;
        if (_stricmp(alias.c_str(), s.mount.c_str()) == 0)
            throw std::invalid_argument("Mount alias already exists: " + alias);
        if (inside(s.root, root) || inside(root, s.root))
            throw std::invalid_argument("Content roots must be distinct and must not overlap");
    }
    auto source = std::make_shared<ContentSource>(newPersistentId(), alias, root, writable, shared);
    sources_.emplace(alias, source);
    return source;
}
void ContentMounts::unmount(const std::string& alias) {
    auto found = sources_.find(alias);
    if (found == sources_.end())
        throw std::invalid_argument("Unknown mount: " + alias);
    found->second->active_ = false;
    sources_.erase(found);
}
bool ContentMounts::contains(const std::string& alias) const {
    return sources_.count(alias) != 0;
}
ContentSourceRef ContentMounts::source(const std::string& alias) const {
    auto found = sources_.find(alias);
    if (found == sources_.end())
        throw std::invalid_argument("Unknown mount: " + alias);
    return found->second;
}
ContentSourceRef ContentMounts::byId(const std::string& id) const {
    for (const auto& entry : sources_)
        if (entry.second->id == id)
            return entry.second;
    throw std::invalid_argument("Stale or unknown Content identity: " + id);
}
std::vector<ContentSourceRef> ContentMounts::sources() const {
    std::vector<ContentSourceRef> result;
    for (const auto& entry : sources_)
        result.push_back(entry.second);
    return result;
}
ContentFile ContentMounts::file(ContentSourceRef source, const std::string& path) {
    requireActive(source);
    ContentFile result{std::move(source), normalized(path)};
    (void)result.physical();
    return result;
}
ContentFile ContentMounts::locate(const std::string& path) const {
    const auto slash = path.find('/', 1);
    if (path.empty() || path[0] != '/' || slash == std::string::npos)
        throw std::invalid_argument("Expected a mounted file path: " + path);
    return file(source(path.substr(0, slash)), path.substr(slash + 1));
}
bool ContentMounts::isUri(const std::string& path) {
    return path.rfind("/__content/", 0) == 0;
}
ContentFile ContentMounts::fromUri(const std::string& uri) const {
    if (!isUri(uri))
        throw std::invalid_argument("Expected a Content resource URI");
    const auto slash = uri.find('/', 11);
    if (slash == std::string::npos)
        throw std::invalid_argument("Invalid Content resource URI");
    return file(byId(uri.substr(11, slash - 11)), uri.substr(slash + 1));
}
ContentFile ContentMounts::relative(const ContentFile& owner, const std::string& path) const {
    requireActive(owner.source);
    if (isUri(path)) {
        auto target = fromUri(path);
        if (target.source != owner.source)
            throw std::invalid_argument("Cross-Content resource dependency");
        return target;
    }
    if (path.rfind("/Game/", 0) == 0)
        return file(owner.source, path.substr(6));
    if (!path.empty() && path[0] == '/') {
        auto target = locate(path);
        if (target.source != owner.source)
            throw std::invalid_argument("Cross-Content resource dependency");
        return target;
    }
    return file(owner.source,
                (std::filesystem::u8path(owner.relative).parent_path() / std::filesystem::u8path(path))
                    .generic_u8string());
}
std::string ContentMounts::read(const ContentFile& file) {
    std::ifstream stream(file.physical(), std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot read Content file: " + file.relative);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    if (stream.bad())
        throw std::runtime_error("Content read failed: " + file.relative);
    return bytes.str();
}
void ContentMounts::write(const ContentFile& file, const std::string& bytes) {
    if (!file.source->writable)
        throw std::invalid_argument("Content is read-only: " + file.source->mount);
    auto target = file.physical();
    std::filesystem::create_directories(target.parent_path());
    auto temporary = target;
    temporary += "." + newPersistentId() + ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary);
        stream.write(bytes.data(), std::streamsize(bytes.size()));
        stream.close();
        if (!stream)
            throw std::runtime_error("Content write failed");
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot commit Content file: " + std::to_string(GetLastError()));
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
}
} // namespace whimsical
