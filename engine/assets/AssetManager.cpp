#include "AssetManager.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace afterlight {
namespace {
AssetHeader readHeader(std::istream& file) {
    std::string magic, header;
    std::getline(file, magic);
    std::getline(file, header);
    if (!magic.empty() && magic.back() == '\r')
        magic.pop_back();
    if (magic != "ALAS1" || !file)
        throw std::invalid_argument("Invalid .asset envelope");
    return AssetHeader::fromJson(Json::parse(header));
}
std::string readBytes(std::istream& file) {
    std::ostringstream bytes;
    bytes << file.rdbuf();
    if (file.bad())
        throw std::runtime_error("Asset read failed");
    return bytes.str();
}
} // namespace
AssetManager::Scope::Scope(AssetManager& manager, ContentSourceRef source, bool closed)
    : manager_(manager), previous_(manager.context_), closed_(manager.closed_) {
    manager.checkThread();
    if (manager.closed_ && (!closed || source != manager.context_))
        throw std::invalid_argument("Cannot escape a Content dependency scope");
    manager.context_ = std::move(source);
    manager.closed_ = closed;
}
AssetManager::Scope::~Scope() {
    manager_.context_ = previous_;
    manager_.closed_ = closed_;
}
AssetManager::AssetManager() {
    registerLoader(
        "Data",
        [](AssetManager&, const AssetHeader&, const std::string& bytes) {
            auto a = std::make_shared<DataAsset>();
            a->data = Json::parse(bytes);
            return a;
        },
        Encoding::Json);
    registerLoader(
        "Script",
        [](AssetManager&, const AssetHeader&, const std::string& bytes) {
            auto a = std::make_shared<ScriptAsset>();
            a->source = bytes;
            return a;
        },
        Encoding::Text);
    registerLoader("Binary", [](AssetManager&, const AssetHeader&, const std::string& bytes) {
        auto a = std::make_shared<BinaryAsset>();
        a->bytes = bytes;
        return a;
    });
}
AssetManager::AssetManager(const std::filesystem::path& content) : AssetManager() {
    mount("/Game", content);
}
void AssetManager::checkThread() const {
    if (owner_ != std::this_thread::get_id())
        throw std::logic_error("AssetManager belongs to the simulation thread");
}
void AssetManager::checkMutation() const {
    checkThread();
    if (closed_)
        throw std::logic_error("Cannot mutate mounts or indices during dependency resolution");
}
ContentSourceRef AssetManager::mount(const std::string& alias, const std::filesystem::path& root,
                                     bool writable) {
    checkMutation();
    auto source = mounts_->mount(alias, root, writable);
    try {
        stores_.emplace(source->id, scanned(source));
    } catch (...) {
        mounts_->unmount(alias);
        throw;
    }
    return source;
}
void AssetManager::unmount(const std::string& alias) {
    checkMutation();
    auto source = mounts_->source(alias);
    stores_.erase(source->id);
    mounts_->unmount(alias);
}
void AssetManager::registerLoader(std::string type, Loader loader, Encoding encoding) {
    checkMutation();
    if (!loaders_.emplace(std::move(type), Codec{std::move(loader), encoding}).second)
        throw std::invalid_argument("Asset loader already registered");
}
ContentSourceRef AssetManager::origin(const AssetPath& path) const {
    checkThread();
    auto alias = path.string().substr(0, path.string().find('/', 1));
    auto source = alias == "/Game" && context_ ? context_ : mounts_->source(alias);
    if (!source->active())
        throw std::invalid_argument("Content mount is no longer active");
    if (closed_ && source != context_)
        throw std::invalid_argument("Cross-Content asset dependency");
    return source;
}
ContentSourceRef AssetManager::origin(const AssetRef& ref) const {
    checkThread();
    auto source = ref.source.empty() ? origin(ref.path) : mounts_->byId(ref.source);
    if (closed_ && source != context_)
        throw std::invalid_argument("Cross-Content asset reference");
    return source;
}
ContentFile AssetManager::file(const std::string& path) const {
    checkThread();
    if (ContentMounts::isUri(path)) {
        auto result = mounts_->fromUri(path);
        if (closed_ && result.source != context_)
            throw std::invalid_argument("Cross-Content file reference");
        return result;
    }
    auto slash = path.find('/', 1);
    if (slash == std::string::npos)
        throw std::invalid_argument("Expected a mounted file path");
    auto source = origin(AssetPath(path.substr(0, slash) + "/file"));
    return ContentMounts::file(source, path.substr(slash + 1));
}
AssetPath AssetManager::local(const AssetPath& path) const {
    return AssetPath("/Game" + path.string().substr(path.string().find('/', 1)));
}
AssetManager::Store& AssetManager::store(ContentSourceRef source) {
    return stores_.at(source->id);
}
const AssetManager::Store& AssetManager::store(ContentSourceRef source) const {
    return stores_.at(source->id);
}
ContentFile AssetManager::externalFile(const Entry& e) const {
    std::filesystem::path relative = std::filesystem::u8path(e.header.source);
    if (relative.empty() || relative.has_root_path())
        throw std::invalid_argument("External asset source must be relative to its header");
    return mounts_->relative(e.file, e.header.source);
}
AssetManager::Store AssetManager::scanned(ContentSourceRef source) const {
    Store result;
    result.source = source;
    std::set<std::string> foldedPaths;
    for (const auto& f : std::filesystem::recursive_directory_iterator(source->root)) {
        if (!f.is_regular_file() || f.path().extension() != ".asset")
            continue;
        auto relative = f.path().lexically_relative(source->root);
        auto file = ContentMounts::file(source, relative.generic_u8string());
        relative.replace_extension();
        AssetPath path("/Game/" + relative.generic_u8string());
        std::string folded = path.string();
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        std::ifstream stream(file.physical(), std::ios::binary);
        Entry e{readHeader(stream), file};
        if (e.header.storage == PayloadStorage::External) {
            if (!std::filesystem::is_regular_file(externalFile(e).physical()))
                throw std::runtime_error("Missing external asset payload");
            if (stream.peek() != EOF)
                throw std::invalid_argument("Header-only asset must not contain a payload");
        }
        if (!foldedPaths.insert(folded).second || !result.ids.emplace(e.header.id, path).second)
            throw std::invalid_argument("Duplicate asset path or ID within " + source->mount + ": " +
                                        path.string());
        result.entries.emplace(path, std::move(e));
    }
    return result;
}
void AssetManager::scan() {
    checkMutation();
    std::map<std::string, Store> next;
    for (auto source : mounts_->sources())
        next.emplace(source->id, scanned(source));
    stores_ = std::move(next);
}
void AssetManager::scan(const std::string& alias) {
    checkMutation();
    auto source = mounts_->source(alias);
    auto next = scanned(source);
    stores_.at(source->id) = std::move(next);
}
AssetRef AssetManager::bound(const Store& s, const AssetPath& path) const {
    return {s.entries.at(path).header.id, AssetPath(s.source->mount + path.string().substr(5)), s.source->id};
}
std::vector<AssetRef> AssetManager::browse(const std::string& alias) const {
    const auto& s = store(origin(AssetPath(alias + "/browse")));
    std::vector<AssetRef> refs;
    for (const auto& entry : s.entries)
        refs.push_back(bound(s, entry.first));
    return refs;
}
size_t AssetManager::size() const {
    checkThread();
    size_t count = 0;
    for (const auto& s : stores_)
        count += s.second.entries.size();
    return count;
}
AssetHeader AssetManager::descriptor(const AssetPath& path) {
    auto source = origin(path);
    Scope scope(*this, source);
    auto header = store(source).entries.at(local(path)).header;
    header.metadata = references(header.metadata, false);
    return header;
}
AssetPath AssetManager::qualify(const AssetPath& path) const {
    return AssetPath(origin(path)->mount + local(path).string().substr(5));
}
AssetRef AssetManager::reference(const AssetPath& path) const {
    return bound(store(origin(path)), local(path));
}
AssetRef AssetManager::resolve(const AssetRef& ref) const {
    const auto& s = store(origin(ref));
    return bound(s, s.ids.at(ref.id));
}
Json AssetManager::references(const Json& value, bool persistent) const {
    if (std::holds_alternative<Json::Object>(value.value())) {
        if (value.contains("id") && value.contains("path")) {
            auto ref = resolve(AssetRef::fromJson(value));
            if (persistent) {
                ref.path = local(ref.path);
                ref.source.clear();
            }
            return ref.json();
        }
        Json result = Json::object();
        for (const auto& item : value.members())
            result[item.first] = references(item.second, persistent);
        return result;
    }
    if (std::holds_alternative<Json::Array>(value.value())) {
        Json result = Json::array();
        for (const auto& item : value.elements())
            result.push(references(item, persistent));
        return result;
    }
    return value;
}
AssetManager::Document AssetManager::readEntry(const AssetRef& input) {
    auto ref = resolve(input);
    auto source = origin(ref);
    Scope scope(*this, source);
    const auto& entry = store(source).entries.at(local(ref.path));
    std::ifstream stream(entry.file.physical(), std::ios::binary);
    auto header = readHeader(stream);
    if (!(header.json() == entry.header.json()))
        throw std::runtime_error("Asset descriptor changed; rescan its Content");
    std::string bytes;
    if (header.storage == PayloadStorage::External) {
        if (stream.peek() != EOF)
            throw std::invalid_argument("External asset cannot contain an inline payload");
        bytes = ContentMounts::read(externalFile(entry));
    } else
        bytes = readBytes(stream);
    auto encoding = loaders_.at(header.type).encoding;
    header.metadata = references(header.metadata, false);
    if (encoding == Encoding::Json)
        bytes = references(Json::parse(bytes), false).dump();
    return {ref, header, bytes, encoding};
}
AssetManager::Document AssetManager::read(const AssetRef& ref) {
    auto doc = readEntry(ref);
    Scope scope(*this, origin(doc.ref));
    (void)loaders_.at(doc.header.type).load(*this, doc.header, doc.payload);
    return doc;
}
std::shared_ptr<const Asset> AssetManager::load(const AssetPath& path) {
    return load(reference(path));
}
std::shared_ptr<const Asset> AssetManager::load(const AssetRef& input) {
    auto ref = resolve(input);
    auto source = origin(ref);
    Scope scope(*this, source);
    auto& s = store(source);
    auto found = s.cache.find(ref.id);
    if (found != s.cache.end())
        return found->second;
    if (!s.loading.insert(ref.id).second)
        throw std::invalid_argument("Cyclic asset dependency: " + ref.path.string());
    try {
        auto doc = readEntry(ref);
        auto asset = loaders_.at(doc.header.type).load(*this, doc.header, doc.payload);
        if (!asset)
            throw std::logic_error("Asset loader returned null");
        asset->header_ = std::move(doc.header);
        asset->path_ = ref.path;
        asset->source_ = ref.source;
        s.cache.emplace(ref.id, asset);
        s.loading.erase(ref.id);
        return asset;
    } catch (...) {
        s.loading.erase(ref.id);
        throw;
    }
}
void AssetManager::clearCache() {
    checkMutation();
    for (auto& s : stores_)
        s.second.cache.clear();
}
AssetRef AssetManager::save(const AssetPath& path, AssetHeader header, const std::string& bytes) {
    return saveImpl(path, std::move(header), bytes, false);
}
AssetRef AssetManager::save(const AssetRef& ref, AssetHeader header, const std::string& bytes) {
    auto current = resolve(ref);
    Scope scope(*this, origin(current));
    return saveImpl(current.path, std::move(header), bytes, false);
}
AssetRef AssetManager::write(const AssetPath& path, AssetHeader header, const std::string& bytes) {
    return saveImpl(path, std::move(header), bytes, true);
}
AssetRef AssetManager::write(const AssetRef& ref, AssetHeader header, const std::string& bytes) {
    auto current = resolve(ref);
    Scope scope(*this, origin(current));
    return saveImpl(current.path, std::move(header), bytes, true);
}
AssetRef AssetManager::saveImpl(const AssetPath& path, AssetHeader header, const std::string& payload,
                                bool writeExternal) {
    auto source = origin(path);
    Scope scope(*this, source);
    auto& s = store(source);
    auto key = local(path);
    if (!s.loading.empty())
        throw std::logic_error("Cannot save during asset decoding");
    if (!source->writable)
        throw std::invalid_argument("Content is read-only");
    auto old = s.entries.find(key);
    if (old != s.entries.end() &&
        (old->second.header.id != header.id || old->second.header.type != header.type))
        throw std::invalid_argument("Cannot overwrite a different asset identity/type");
    auto id = s.ids.find(header.id);
    if (id != s.ids.end() && id->second != key)
        throw std::invalid_argument("Asset ID already registered at another path");
    header = AssetHeader::fromJson(header.json());
    auto target = ContentMounts::file(source, key.string().substr(6) + ".asset");
    if (old == s.entries.end() && std::filesystem::exists(target.physical()))
        throw std::invalid_argument("Asset file already exists; rescan or use its registered spelling");
    Entry entry{header, target};
    bool external = header.storage == PayloadStorage::External;
    if (external && !writeExternal && !payload.empty())
        throw std::invalid_argument("External asset cannot contain an inline payload");
    auto bytes = external && !writeExternal ? ContentMounts::read(externalFile(entry)) : payload;
    const auto& codec = loaders_.at(header.type);
    auto runtimeHeader = header;
    runtimeHeader.metadata = references(header.metadata, false);
    if (codec.encoding == Encoding::Json)
        bytes = references(Json::parse(bytes), false).dump();
    (void)codec.load(*this, runtimeHeader, bytes);
    header.metadata = references(runtimeHeader.metadata, true);
    if (codec.encoding == Encoding::Json)
        bytes = references(Json::parse(bytes), true).dump();
    entry.header = header;
    // All dependency validation precedes either write. Each individual file commits atomically.
    if (external && (writeExternal || codec.encoding == Encoding::Json))
        ContentMounts::write(externalFile(entry), bytes);
    ContentMounts::write(target, "ALAS1\n" + header.json().dump() + "\n" + (external ? "" : bytes));
    s.entries[key] = std::move(entry);
    s.ids[header.id] = key;
    s.cache.clear();
    return bound(s, key);
}
} // namespace afterlight
