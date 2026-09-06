#include "AssetManager.h"
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace afterlight {
static AssetHeader readHeader(std::istream& file) {
    std::string magic, header;
    std::getline(file, magic);
    std::getline(file, header);
    if (!magic.empty() && magic.back() == '\r')
        magic.pop_back();
    if (magic != "ALAS1" || !file)
        throw std::invalid_argument("Invalid .asset envelope");
    return AssetHeader::fromJson(Json::parse(header));
}
static std::string readBytes(std::istream& file) {
    std::ostringstream bytes;
    bytes << file.rdbuf();
    if (file.bad())
        throw std::runtime_error("Asset read failed");
    return bytes.str();
}
AssetManager::AssetManager(Project project) : project_(std::move(project)) {
    registerLoader("Data", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        auto a = std::make_shared<DataAsset>();
        a->data = Json::parse(bytes);
        return a;
    });
    registerLoader("Script", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        auto a = std::make_shared<ScriptAsset>();
        a->source = bytes;
        return a;
    });
    registerLoader("Binary", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        auto a = std::make_shared<BinaryAsset>();
        a->bytes = bytes;
        return a;
    });
    scan();
}
void AssetManager::checkThread() const {
    if (owner_ != std::this_thread::get_id())
        throw std::logic_error("AssetManager belongs to the simulation thread");
}
void AssetManager::registerLoader(std::string type, Loader loader) {
    checkThread();
    if (!loaders_.emplace(std::move(type), std::move(loader)).second)
        throw std::invalid_argument("Asset loader already registered");
}
std::filesystem::path AssetManager::contained(const std::filesystem::path& file) const {
    auto root = std::filesystem::canonical(project_.content());
    auto path = std::filesystem::weakly_canonical(file);
    auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw std::invalid_argument("Asset escapes Project Content: " + file.string());
    return path;
}
std::filesystem::path AssetManager::externalFile(const Entry& e) const {
    std::filesystem::path relative(e.header.source);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::invalid_argument("External asset source must be relative to its header");
    auto file = contained(e.file.parent_path() / relative);
    if (!std::filesystem::is_regular_file(file))
        throw std::runtime_error("Missing external asset payload");
    return file;
}
std::string AssetManager::externalPayload(const Entry& entry) const {
    std::ifstream file(externalFile(entry), std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open external asset payload");
    return readBytes(file);
}
void AssetManager::scan() {
    checkThread();
    std::map<AssetPath, Entry> entries;
    std::map<std::string, AssetPath> ids;
    std::set<std::string> foldedPaths;
    for (const auto& file : std::filesystem::recursive_directory_iterator(project_.content())) {
        if (!file.is_regular_file() || file.path().extension() != ".asset")
            continue;
        auto relative = file.path().lexically_relative(project_.content());
        relative.replace_extension();
        AssetPath path("/Game/" + relative.generic_string());
        std::string folded = path.string();
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        std::ifstream stream(contained(file.path()), std::ios::binary);
        Entry e{readHeader(stream), file.path()};
        if (e.header.storage == PayloadStorage::External) {
            (void)externalFile(e);
            if (stream.peek() != EOF)
                throw std::invalid_argument("Header-only asset must not contain a payload");
        }
        if (!foldedPaths.insert(folded).second || !ids.emplace(e.header.id, path).second)
            throw std::invalid_argument("Duplicate asset path or ID: " + path.string());
        entries.emplace(path, std::move(e));
    }
    entries_ = std::move(entries);
    ids_ = std::move(ids);
    cache_.clear();
}
const AssetHeader& AssetManager::descriptor(const AssetPath& path) const {
    checkThread();
    return entries_.at(path).header;
}
AssetRef AssetManager::reference(const AssetPath& path) const {
    return {descriptor(path).id, path};
}
AssetRef AssetManager::resolve(const AssetRef& ref) const {
    checkThread();
    return reference(ids_.at(ref.id));
}
std::shared_ptr<const Asset> AssetManager::load(const AssetRef& ref) {
    return load(resolve(ref).path);
}
std::shared_ptr<const Asset> AssetManager::load(const AssetPath& path) {
    checkThread();
    const auto& entry = entries_.at(path);
    auto found = cache_.find(entry.header.id);
    if (found != cache_.end())
        return found->second;
    const auto& loader = loaders_.at(entry.header.type);
    if (!loading_.insert(entry.header.id).second)
        throw std::invalid_argument("Cyclic asset dependency: " + path.string());
    try {
        std::ifstream file(contained(entry.file), std::ios::binary);
        auto header = readHeader(file);
        if (!(header.json() == entry.header.json()))
            throw std::runtime_error("Asset descriptor changed; rescan the project");
        std::string bytes;
        if (header.storage == PayloadStorage::External) {
            if (file.peek() != EOF)
                throw std::invalid_argument("External asset cannot contain an inline payload");
            bytes = externalPayload(entry);
        } else
            bytes = readBytes(file);
        auto asset = loader(*this, header, bytes);
        if (!asset)
            throw std::logic_error("Asset loader returned null");
        asset->header_ = std::move(header);
        asset->path_ = path;
        cache_.emplace(entry.header.id, asset);
        loading_.erase(entry.header.id);
        return asset;
    } catch (...) {
        loading_.erase(entry.header.id);
        throw;
    }
}
void AssetManager::clearCache() {
    checkThread();
    cache_.clear();
}
AssetRef AssetManager::save(const AssetPath& path, AssetHeader header, const std::string& payload) {
    checkThread();
    auto old = entries_.find(path);
    if (old != entries_.end() &&
        (old->second.header.id != header.id || old->second.header.type != header.type))
        throw std::invalid_argument("Cannot overwrite a different asset identity/type");
    auto id = ids_.find(header.id);
    if (id != ids_.end() && id->second != path)
        throw std::invalid_argument("Asset ID already registered at another path");
    header = AssetHeader::fromJson(header.json());
    auto target = contained(project_.content() / (path.string().substr(6) + ".asset"));
    if (old == entries_.end() && std::filesystem::exists(target))
        throw std::invalid_argument("Asset file already exists; rescan or use its registered spelling");
    Entry entry{header, target};
    const auto& loader = loaders_.at(header.type);
    if (header.storage == PayloadStorage::External && !payload.empty())
        throw std::invalid_argument("External asset cannot contain an inline payload");
    // Both storage modes feed the same type decoder; validate before committing the header.
    (void)loader(*this, header,
                 header.storage == PayloadStorage::External ? externalPayload(entry) : payload);
    std::filesystem::create_directories(target.parent_path());
    auto temporary = target;
    temporary += "." + newPersistentId() + ".tmp";
    try {
        std::ofstream file(temporary, std::ios::binary);
        file << "ALAS1\n" << header.json().dump() << '\n';
        file.write(payload.data(), std::streamsize(payload.size()));
        file.close();
        if (!file)
            throw std::runtime_error("Asset write failed");
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot commit asset file: " + std::to_string(GetLastError()));
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
    entries_[path] = std::move(entry);
    ids_[header.id] = path;
    cache_.clear(); // Dependencies may have changed; live references keep the old generation.
    return {header.id, path};
}
} // namespace afterlight
